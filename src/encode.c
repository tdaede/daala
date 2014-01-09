/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*Daala video codec
Copyright (c) 2006-2013 Daala project contributors.  All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.*/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "encint.h"
#if defined(OD_ENCODER_CHECK)
# include "decint.h"
#endif
#include "generic_code.h"
#include "filter.h"
#include "dct.h"
#include "intra.h"
#include "logging.h"
#include "pvq.h"
#include "pvq_code.h"
#include "block_size.h"
#include "block_size_enc.h"
#include "logging.h"
#include "tf.h"
#include "metrics.h"
#include "state.h"
#if defined(OD_X86ASM)
# include "x86/x86int.h"
#endif

static double mode_bits = 0;
static double mode_count = 0;

void od_enc_opt_vtbl_init_c(od_enc_ctx *enc) {
  enc->opt_vtbl.mc_compute_sad_4x4_xstride_1 =
   od_mc_compute_sad_4x4_xstride_1_c;
  enc->opt_vtbl.mc_compute_sad_8x8_xstride_1 =
   od_mc_compute_sad_8x8_xstride_1_c;
  enc->opt_vtbl.mc_compute_sad_16x16_xstride_1 =
   od_mc_compute_sad_16x16_xstride_1_c;
}

static void od_enc_opt_vtbl_init(od_enc_ctx *enc) {
#if defined(OD_X86ASM)
  od_enc_opt_vtbl_init_x86(enc);
#else
  od_enc_opt_vtbl_init_c(enc);
#endif
}

static int od_enc_init(od_enc_ctx *enc, const daala_info *info) {
  int i;
  int ret;
  ret = od_state_init(&enc->state, info);
  if (ret < 0) return ret;
  od_enc_opt_vtbl_init(enc);
  oggbyte_writeinit(&enc->obb);
  od_ec_enc_init(&enc->ec, 65025);
  enc->packet_state = OD_PACKET_INFO_HDR;
  for (i = 0; i < OD_NPLANES_MAX; i++) enc->scale[i] = 10;
  enc->mvest = od_mv_est_alloc(enc);
  return 0;
}

static void od_enc_clear(od_enc_ctx *enc) {
  od_mv_est_free(enc->mvest);
  od_ec_enc_clear(&enc->ec);
  oggbyte_writeclear(&enc->obb);
  od_state_clear(&enc->state);
}

daala_enc_ctx *daala_encode_create(const daala_info *info) {
  od_enc_ctx *enc;
  if (info == NULL) return NULL;
  enc = (od_enc_ctx *)_ogg_malloc(sizeof(*enc));
  if (od_enc_init(enc, info) < 0) {
    _ogg_free(enc);
    return NULL;
  }
#if defined(OD_ENCODER_CHECK)
  enc->dec = daala_decode_alloc(info, NULL);
#endif
  return enc;
}

void daala_encode_free(daala_enc_ctx *enc) {
  if (enc != NULL) {
#if defined(OD_ENCODER_CHECK)
    if (enc->dec != NULL) {
      daala_decode_free(enc->dec);
    }
#endif
    od_enc_clear(enc);
    _ogg_free(enc);
  }
}

int daala_encode_ctl(daala_enc_ctx *enc, int req, void *buf, size_t buf_sz) {
  (void)buf;
  (void)buf_sz;
  switch (req) {
    case OD_SET_QUANT:
    {
      int i;
      OD_ASSERT(enc);
      OD_ASSERT(buf);
      OD_ASSERT(buf_sz == sizeof(*enc->scale));
      for (i = 0; i < OD_NPLANES_MAX; i++) enc->scale[i] = *(int *)buf;
      return OD_SUCCESS;
    }
    default: return OD_EIMPL;
  }
}

static void od_img_plane_copy_pad8(od_img_plane *dst_p,
 int plane_width, int plane_height, od_img_plane *src_p,
 int pic_width, int pic_height) {
  unsigned char *dst_data;
  ptrdiff_t dstride;
  int y;
  dstride = dst_p->ystride;
  /*If we have _no_ data, just encode a dull green.*/
  if (pic_width == 0 || pic_height == 0) {
    dst_data = dst_p->data;
    for (y = 0; y < plane_height; y++) {
      memset(dst_data, 0, plane_width*sizeof(*dst_data));
      dst_data += dstride;
    }
  }
  /*Otherwise, copy what we do have, and add our own padding.*/
  else {
    unsigned char *src_data;
    unsigned char *dst;
    ptrdiff_t sxstride;
    ptrdiff_t systride;
    int x;
    /*Step 1: Copy the data we do have.*/
    sxstride = src_p->xstride;
    systride = src_p->ystride;
    dst_data = dst_p->data;
    src_data = src_p->data;
    dst = dst_data;
    for (y = 0; y < pic_height; y++) {
      if (sxstride == 1) memcpy(dst, src_data, pic_width);
      else for (x = 0; x < pic_width; x++) dst[x] = *(src_data + sxstride*x);
      dst += dstride;
      src_data += systride;
    }
    /*Step 2: Perform a low-pass extension into the padding region.*/
    /*Right side.*/
    for (x = pic_width; x < plane_width; x++) {
      dst = dst_data + x - 1;
      for (y = 0; y < pic_height; y++) {
        dst[1] = (2*dst[0] + (dst - (dstride & -(y > 0)))[0]
         + (dst + (dstride & -(y + 1 < pic_height)))[0] + 2) >> 2;
        dst += dstride;
      }
    }
    /*Bottom.*/
    dst = dst_data + dstride*pic_height;
    for (y = pic_height; y < plane_height; y++) {
      for (x = 0; x < plane_width; x++) {
        dst[x] = (2*(dst - dstride)[x] + (dst - dstride)[x - (x > 0)]
         + (dst - dstride)[x + (x + 1 < plane_width)] + 2) >> 2;
      }
      dst += dstride;
    }
  }
}

/*Extend the edge into the padding.*/
static void od_img_plane_edge_ext8(od_img_plane *dst_p,
 int plane_width, int plane_height, int horz_padding, int vert_padding) {
  ptrdiff_t dstride;
  unsigned char *dst_data;
  unsigned char *dst;
  int x;
  int y;
  dstride = dst_p->ystride;
  dst_data = dst_p->data;
  /*Left side.*/
  for (y = 0; y < plane_height; y++) {
    dst = dst_data + dstride * y;
    for (x = 1; x <= horz_padding; x++) {
      (dst-x)[0] = dst[0];
    }
  }
  /*Right side.*/
  for (y = 0; y < plane_height; y++) {
    dst = dst_data + plane_width - 1 + dstride * y;
    for (x = 1; x <= horz_padding; x++) {
      dst[x] = dst[0];
    }
  }
  /*Top.*/
  dst = dst_data - horz_padding;
  for (y = 0; y < vert_padding; y++) {
    for (x = 0; x < plane_width + 2 * horz_padding; x++) {
      (dst - dstride)[x] = dst[x];
    }
    dst -= dstride;
  }
  /*Bottom.*/
  dst = dst_data - horz_padding + plane_height * dstride;
  for (y = 0; y < vert_padding; y++) {
    for (x = 0; x < plane_width + 2 * horz_padding; x++) {
      dst[x] = (dst - dstride)[x];
    }
    dst += dstride;
  }
}

struct od_mb_enc_ctx {
  generic_encoder model_dc[OD_NPLANES_MAX];
  generic_encoder model_g[OD_NPLANES_MAX];
  generic_encoder model_ym[OD_NPLANES_MAX];
  ogg_int32_t adapt[OD_NSB_ADAPT_CTXS];
  signed char *modes;
  od_coeff *c;
  od_coeff **d;
  /* holds a TF'd copy of the transform coefficients in 4x4 blocks */
  od_coeff *tf;
  od_coeff *md;
  od_coeff *mc;
  od_coeff *l;
  int run_pvq[OD_NPLANES_MAX];
  int ex_dc[OD_NPLANES_MAX];
  int ex_g[OD_NPLANES_MAX];
  int is_keyframe;
  int nk;
  int k_total;
  int sum_ex_total_q8;
  int ncount;
  int count_total_q8;
  int count_ex_total_q8;
  ogg_uint16_t mode_p0[OD_INTRA_NMODES];
};
typedef struct od_mb_enc_ctx od_mb_enc_ctx;

void od_band_encode(od_ec_enc *ec, int qg, int theta, int max_theta,
 const int *y, int n, int k, generic_encoder *model, int *adapt, int *exg,
 int *ext) {
  int adapt_curr[OD_NSB_ADAPT_CTXS] = { 0 };
  int speed = 5;
  generic_encode(ec, model, qg, exg, 2);
  if (theta >= 0 && max_theta > 0)
    generic_encode(ec, model, theta, ext, 2);
  pvq_encoder(ec, y, n - (theta >= 0), k, adapt_curr, adapt);
  if (adapt_curr[OD_ADAPT_K_Q8] > 0) {
    adapt[OD_ADAPT_K_Q8] += 256*adapt_curr[OD_ADAPT_K_Q8] -
     adapt[OD_ADAPT_K_Q8]>>speed;
    adapt[OD_ADAPT_SUM_EX_Q8] += adapt_curr[OD_ADAPT_SUM_EX_Q8] -
     adapt[OD_ADAPT_SUM_EX_Q8]>>speed;
  }
  if (adapt_curr[OD_ADAPT_COUNT_Q8] > 0) {
    adapt[OD_ADAPT_COUNT_Q8] += adapt_curr[OD_ADAPT_COUNT_Q8]-
     adapt[OD_ADAPT_COUNT_Q8]>>speed;
    adapt[OD_ADAPT_COUNT_EX_Q8] += adapt_curr[OD_ADAPT_COUNT_EX_Q8]-
     adapt[OD_ADAPT_COUNT_EX_Q8]>>speed;
  }
}

void od_single_band_encode(daala_enc_ctx *enc, od_mb_enc_ctx *ctx, int ln,
 int pli, int bx, int by, int has_ur) {
  ogg_int32_t adapt_curr[OD_NSB_ADAPT_CTXS];
  int n;
  int n2;
  int xdec;
  int ydec;
  int w;
  int frame_width;
  signed char *modes;
  od_coeff *c;
  od_coeff *d;
  od_coeff *tf;
  od_coeff *md;
  od_coeff *mc;
  od_coeff *l;
  int x;
  int y;
  od_coeff pred[16*16];
  od_coeff predt[16*16];
  int cblock[16*16];
  od_coeff scalar_out[16*16];
  int zzi;
  int vk;
  int run_pvq;
  int scale;
#ifndef USE_PSEUDO_ZIGZAG
  unsigned char const *zig;
#endif
#if defined(OD_OUTPUT_PRED)
  od_coeff preds[16*16];
#endif
#if defined(OD_METRICS)
  ogg_int64_t pvq_frac_bits;
  ogg_int64_t dc_frac_bits;
  ogg_int64_t intra_frac_bits;
#endif
  OD_ASSERT(ln >= 0 && ln <= 2);
  n = 1 << (ln + 2);
  /* The new PVQ is only supported on 8x8 for now. */
  run_pvq = ctx->run_pvq[pli] && (n == 4 || n == 8);
  n2 = n*n;
  bx <<= ln;
  by <<= ln;
#ifndef USE_PSEUDO_ZIGZAG
  zig = OD_DCT_ZIGS[ln];
#endif
  xdec = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].xdec;
  ydec = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].ydec;
  frame_width = enc->state.frame_width;
  w = frame_width >> xdec;
  modes = ctx->modes;
  c = ctx->c;
  d = ctx->d[pli];
  tf = ctx->tf;
  md = ctx->md;
  mc = ctx->mc;
  l = ctx->l;
  vk = 0;
  /*Apply forward transform(s).*/
  (*OD_FDCT_2D[ln])(d + (by << 2)*w + (bx << 2), w,
   c + (by << 2)*w + (bx << 2), w);
  if (!ctx->is_keyframe) {
    (*OD_FDCT_2D[ln])(md + (by << 2)*w + (bx << 2), w,
     mc + (by << 2)*w + (bx << 2), w);
  }
  if (ctx->is_keyframe) {
    if (bx > 0 && by > 0) {
      if (pli == 0) {
        ogg_uint16_t mode_cdf[OD_INTRA_NMODES];
        ogg_uint32_t mode_dist[OD_INTRA_NMODES];
        int m_l;
        int m_ul;
        int m_u;
        int mode;
        od_coeff *coeffs[4];
        int strides[4];
        /*Search predictors from the surrounding blocks.*/
        coeffs[0] = tf + ((by - (1 << ln)) << 2)*w + ((bx - (1 << ln)) << 2);
        coeffs[1] = tf + ((by - (1 << ln)) << 2)*w + ((bx - (0 << ln)) << 2);
        coeffs[2] = tf + ((by - (1 << ln)) << 2)*w + ((bx + (1 << ln)) << 2);
        coeffs[3] = tf + ((by - (0 << ln)) << 2)*w + ((bx - (1 << ln)) << 2);
        if (!has_ur) {
          coeffs[2] = coeffs[1];
        }
        strides[0] = w;
        strides[1] = w;
        strides[2] = w;
        strides[3] = w;
        m_l = modes[by*(w >> 2) + bx - 1];
        m_ul = modes[(by - 1)*(w >> 2) + bx - 1];
        m_u = modes[(by - 1)*(w >> 2) + bx];
        od_intra_pred_cdf(mode_cdf, OD_INTRA_PRED_PROB_4x4[pli],
         ctx->mode_p0, OD_INTRA_NMODES, m_l, m_ul, m_u);
        (*OD_INTRA_DIST[ln])(mode_dist, d + (by << 2)*w + (bx << 2), w,
         coeffs, strides);
        /*Lambda = 1*/
        mode = od_intra_pred_search(mode_cdf, mode_dist,
         OD_INTRA_NMODES, 256);
        (*OD_INTRA_GET[ln])(pred, coeffs, strides, mode);
#if defined(OD_METRICS)
        intra_frac_bits = od_ec_enc_tell_frac(&enc->ec);
#endif
        od_ec_encode_cdf_unscaled(&enc->ec, mode, mode_cdf, OD_INTRA_NMODES);
#if defined(OD_METRICS)
        enc->state.bit_metrics[OD_METRIC_INTRA] +=
         od_ec_enc_tell_frac(&enc->ec) - intra_frac_bits;
#endif
        mode_bits -= M_LOG2E*log(
         (mode_cdf[mode] - (mode == 0 ? 0 : mode_cdf[mode - 1]))/
         (float)mode_cdf[OD_INTRA_NMODES - 1]);
        mode_count++;
        for (y = 0; y < (1 << ln); y++) {
          for (x = 0; x < (1 << ln); x++) {
            modes[(by + y)*(w >> 2) + bx + x] = mode;
          }
        }
        od_intra_pred_update(ctx->mode_p0, OD_INTRA_NMODES, mode, m_l, m_ul,
         m_u);
      }
      else {
        int mode;
        mode = modes[(by << ydec)*(frame_width >> 2) + (bx << xdec)];
        od_chroma_pred(pred, d, l, w, bx, by, ln, xdec, ydec,
         enc->state.bsize, enc->state.bstride,
         OD_INTRA_CHROMA_WEIGHTS_Q8[mode]);
      }
    }
    else {
      int nsize;
      for (zzi = 0; zzi < n2; zzi++) pred[zzi] = 0;
      nsize = ln;
      /*444/420 only right now.*/
      OD_ASSERT(xdec == ydec);
      if (bx > 0) {
        int noff;
        nsize = OD_BLOCK_SIZE4x4(enc->state.bsize, enc->state.bstride,
         (bx - 1) << xdec, by << ydec);
        nsize = OD_MAXI(nsize - xdec, 0);
        noff = 1 << nsize;
        /*Because of the quad-tree structure we can always find our neighbors
           starting offset by rounding to a multiple of his size.*/
        OD_ASSERT(!(bx & (noff - 1)));
        pred[0] = d[((by & ~(noff - 1)) << 2)*w + ((bx - noff) << 2)];
      }
      else if (by > 0) {
        int noff;
        nsize = OD_BLOCK_SIZE4x4(enc->state.bsize, enc->state.bstride,
         bx << xdec, (by - 1) << ydec);
        nsize = OD_MAXI(nsize - xdec, 0);
        noff = 1 << nsize;
        OD_ASSERT(!(by & (noff - 1)));
        pred[0] = d[((by - noff) << 2)*w + ((bx & ~(noff - 1)) << 2)];
      }
      /*Rescale DC for correct transform size.*/
      if (nsize > ln) pred[0] >>= (nsize - ln);
      else if (nsize < ln) pred[0] <<= (ln - nsize);
      if (pli == 0) {
        for (y = 0; y < (1 << ln); y++) {
          for (x = 0; x < (1 << ln); x++) {
            modes[(by + y)*(w >> 2) + bx + x] = 0;
          }
        }
      }
    }
  }
  else {
    int ci;
    ci = 0;
    for (y = 0; y < n; y++) {
      for (x = 0; x < n; x++) {
        pred[ci++] = md[(y + (by << 2))*w + (x + (bx << 2))];
      }
    }
  }
#if defined(OD_OUTPUT_PRED)
  for (zzi = 0; zzi < n2; zzi++) preds[zzi] = pred[zzi];
#endif
  /* Change ordering for encoding. */
#ifdef USE_PSEUDO_ZIGZAG
  od_band_pseudo_zigzag(cblock,  n, &d[((by << 2))*w + (bx << 2)], w,
   !run_pvq);
  od_band_pseudo_zigzag(predt,  n, &pred[0], n, !run_pvq);
#else
  /*Zig-zag*/
  for (y = 0; y < n; y++) {
    for (x = 0; x < n; x++) {
      cblock[zig[y*n + x]] = d[((by << 2) + y)*w + (bx << 2) + x];
      predt[zig[y*n + x]] = pred[y*n + x];
    }
  }
#endif
  scale = OD_MAXI(enc->scale[pli], 1);
  scalar_out[0] = OD_DIV_R0(cblock[0] - predt[0], scale);
#if defined(OD_METRICS)
  dc_frac_bits = od_ec_enc_tell_frac(&enc->ec);
#endif
  generic_encode(&enc->ec, ctx->model_dc + pli, abs(scalar_out[0]),
   ctx->ex_dc + pli, 0);
  if (scalar_out[0]) od_ec_enc_bits(&enc->ec, scalar_out[0] < 0, 1);
#if defined(OD_METRICS)
  enc->state.bit_metrics[OD_METRIC_DC] += od_ec_enc_tell_frac(&enc->ec) -
   dc_frac_bits;
#endif
  scalar_out[0] = scalar_out[0]*scale;
  scalar_out[0] += predt[0];
  if (run_pvq) {
    int theta[4];
    int max_theta[4];
    int qg[4];
    int k[4];
    int *adapt;
    int *exg;
    int *ext;
    int predflags8;
    int i;
    generic_encoder *model;
    adapt = enc->state.pvq_adapt;
    exg = enc->state.pvq_exg;
    ext = enc->state.pvq_ext;
    model = &enc->state.pvq_gain_model;
    if (n == 4) {
      qg[0] = pvq_theta(cblock+1, predt+1, 15, scale, scalar_out+1,
       &theta[0], &max_theta[0], &k[0]);
      od_ec_encode_bool_q15(&enc->ec, theta[0] != -1, PRED4_PROB);
      od_band_encode(&enc->ec, qg[0], theta[0], max_theta[0], scalar_out+1,
       15, k[0], model, adapt, exg, ext);
    }
    else {
      qg[0] = pvq_theta(cblock+1, predt+1, 15, scale, scalar_out+1,
       &theta[0], &max_theta[0], &k[0]);
      qg[1] = pvq_theta(cblock+16, predt+16, 8, scale, scalar_out+16,
       &theta[1], &max_theta[1], &k[1]);
      qg[2] = pvq_theta(cblock+24, predt+24, 8, scale, scalar_out+24,
       &theta[2], &max_theta[2], &k[2]);
      qg[3] = pvq_theta(cblock+32, predt+32, 32, scale, scalar_out+32,
       &theta[3], &max_theta[3], &k[3]);
      predflags8 = 8*(theta[0] != -1) + 4*(theta[1] != -1) + 2*(theta[2] != -1)
       + (theta[3] != -1);
      od_ec_encode_cdf_q15(&enc->ec, predflags8, pred8_cdf, 16);
      od_band_encode(&enc->ec, qg[0], theta[0], max_theta[0], scalar_out+1,
       15, k[0], model, adapt, exg, ext);
      od_band_encode(&enc->ec, qg[1], theta[1], max_theta[1], scalar_out+16,
       8, k[1], model, adapt, exg+1, ext+1);
      od_band_encode(&enc->ec, qg[2], theta[2], max_theta[2], scalar_out+24,
       8, k[2], model, adapt, exg+2, ext+2);
      od_band_encode(&enc->ec, qg[3], theta[3], max_theta[3], scalar_out+32,
       32, k[3], model, adapt, exg+3, ext+3);
    }
    for (zzi = 1; zzi < n2; zzi++) scalar_out[zzi] = cblock[zzi];
    for (i = 0; i < OD_NSB_ADAPT_CTXS; i++) adapt_curr[i] = 0;
  }
  else {
    vk = 0;
    for (zzi = 1; zzi < n2; zzi++) {
      scalar_out[zzi] = OD_DIV_R0(cblock[zzi] - predt[zzi], scale);
      vk += abs(scalar_out[zzi]);
    }
#if defined(OD_METRICS)
    pvq_frac_bits = od_ec_enc_tell_frac(&enc->ec);
#endif
    generic_encode(&enc->ec, ctx->model_g + pli, vk,
     ctx->ex_g + pli, 0);
    pvq_encoder(&enc->ec, scalar_out + 1, n2 - 1, vk, adapt_curr, ctx->adapt);
#if defined(OD_METRICS)
    enc->state.bit_metrics[OD_METRIC_PVQ] += od_ec_enc_tell_frac(&enc->ec) -
     pvq_frac_bits;
#endif
    for (zzi = 1; zzi < n2; zzi++) {
      scalar_out[zzi] = scalar_out[zzi]*scale + predt[zzi];
    }
  }
#ifdef USE_PSEUDO_ZIGZAG
  od_band_pseudo_dezigzag(&d[((by << 2))*w + (bx << 2)], w, scalar_out, n,
   !run_pvq);
#else
  /*De-zigzag*/
  for (y = 0; y < n; y++) {
    for (x = 0; x < n; x++) {
      d[((by << 2) + y)*w + (bx << 2) + x] = scalar_out[zig[y*n + x]];
    }
  }
#endif
  /* Update the TF'd luma plane. */
  if (ctx->is_keyframe && pli == 0) {
    od_convert_block_down(tf + (by << 2)*w + (bx << 2), w,
     d + (by << 2)*w + (bx << 2), w, ln, 0);
  }
  if (adapt_curr[OD_ADAPT_K_Q8] >= 0) {
    ctx->nk++;
    ctx->k_total += adapt_curr[OD_ADAPT_K_Q8];
    ctx->sum_ex_total_q8 += adapt_curr[OD_ADAPT_SUM_EX_Q8];
  }
  if (adapt_curr[OD_ADAPT_COUNT_Q8] >= 0) {
    ctx->ncount++;
    ctx->count_total_q8 += adapt_curr[OD_ADAPT_COUNT_Q8];
    ctx->count_ex_total_q8 += adapt_curr[OD_ADAPT_COUNT_EX_Q8];
  }
  /*Apply the inverse transform.*/
#if !defined(OD_OUTPUT_PRED)
  (*OD_IDCT_2D[ln])(c + (by << 2)*w + (bx << 2), w, d + (by << 2)*w
   + (bx << 2), w);
#else
# if 0
  /*Output the resampled luma plane.*/
  if (pli != 0) {
    for (y = 0; y < n; y++) {
      for (x = 0; x < n; x++) {
        preds[y*n + x] = l[((by << 2) + y)*w + (bx << 2) + x] >> xdec;
      }
    }
  }
# endif
  (*OD_IDCT_2D[ln])(c + (by << 2)*w + (bx << 2), w, preds, n);
#endif
}

static void od_32x32_encode(daala_enc_ctx *enc, od_mb_enc_ctx *ctx, int ln,
 int pli, int bx, int by, int has_ur) {
  bx <<= 1;
  by <<= 1;
  od_single_band_encode(enc, ctx, ln - 1, pli, bx + 0, by + 0, 1);
  od_single_band_encode(enc, ctx, ln - 1, pli, bx + 1, by + 0, has_ur);
  od_single_band_encode(enc, ctx, ln - 1, pli, bx + 0, by + 1, 1);
  od_single_band_encode(enc, ctx, ln - 1, pli, bx + 1, by + 1, 0);
}

typedef void (*od_enc_func)(daala_enc_ctx *enc, od_mb_enc_ctx *ctx, int ln,
 int pli, int bx, int by, int has_ur);

const od_enc_func OD_ENCODE_BLOCK[OD_NBSIZES + 2] = {
  od_single_band_encode,
  od_single_band_encode,
  od_single_band_encode,
  od_32x32_encode
};

static void od_encode_block(daala_enc_ctx *enc, od_mb_enc_ctx *ctx, int pli,
 int bx, int by, int l, int xdec, int ydec, int has_ur) {
  int od;
  int d;
  /*This code assumes 4:4:4 or 4:2:0 input.*/
  OD_ASSERT(xdec == ydec);
  od = OD_BLOCK_SIZE4x4(enc->state.bsize,
   enc->state.bstride, bx << l, by << l);
  d = OD_MAXI(od, xdec);
  OD_ASSERT(d <= l);
  if (d == l) {
    d -= xdec;
    /*Construct the luma predictors for chroma planes.*/
    if (ctx->l != NULL) {
      int w;
      int frame_width;
      OD_ASSERT(pli > 0);
      frame_width = enc->state.frame_width;
      w = frame_width >> xdec;
      od_resample_luma_coeffs(ctx->l + (by << (2 + d))*w + (bx << (2 + d)), w,
       ctx->d[0] + (by << (2 + l))*frame_width + (bx << (2 + l)),
       frame_width, xdec, ydec, d, od);
    }
    (*OD_ENCODE_BLOCK[d])(enc, ctx, d, pli, bx, by, has_ur);
  }
  else {
    l--;
    bx <<= 1;
    by <<= 1;
    od_encode_block(enc, ctx, pli, bx + 0, by + 0, l, xdec, ydec, 1);
    od_encode_block(enc, ctx, pli, bx + 1, by + 0, l, xdec, ydec, has_ur);
    od_encode_block(enc, ctx, pli, bx + 0, by + 1, l, xdec, ydec, 1);
    od_encode_block(enc, ctx, pli, bx + 1, by + 1, l, xdec, ydec, 0);
  }
}

static void od_encode_mv(daala_enc_ctx *enc, od_mv_grid_pt *mvg, int vx,
 int vy, int level, int mv_res, int width, int height) {
  static const int ex[5] = { 628, 1382, 1879, 2119, 2102 };
  static const int ey[5] = { 230, 525, 807, 1076, 1332 };
  int pred[2];
  int ox;
  int oy;
  od_state_get_predictor(&enc->state, pred, vx, vy, level, mv_res);
  ox = (mvg->mv[0] >> mv_res) - pred[0];
  oy = (mvg->mv[1] >> mv_res) - pred[1];
  /*Interleave positive and negative values.*/
  ox = (ox << 1) ^ OD_SIGNMASK(ox);
  oy = (oy << 1) ^ OD_SIGNMASK(oy);
  laplace_encode(&enc->ec, ox, ex[level] >> mv_res, width << (4-level));
  laplace_encode(&enc->ec, oy, ey[level] >> mv_res, height << (4-level));
}

int daala_encode_img_in(daala_enc_ctx *enc, od_img *img, int duration) {
  int refi;
  int nplanes;
  int pli;
  int frame_width;
  int frame_height;
  int pic_width;
  int pic_height;
  int i;
  int j;
  int k;
  int m;
  BlockSizeComp *bs;
  int nhsb;
  int nvsb;
  od_mb_enc_ctx mbctx;
#if defined(OD_METRICS)
  ogg_int64_t tot_frac_bits;
  ogg_int64_t bs_frac_bits;
  ogg_int64_t mv_frac_bits;
  for (i = 0; i < OD_METRIC_COUNT; i++) {
    enc->state.bit_metrics[i] = 0;
  }
#endif
  if (enc == NULL || img == NULL) return OD_EFAULT;
  if (enc->packet_state == OD_PACKET_DONE) return OD_EINVAL;
  /*Check the input image dimensions to make sure they're compatible with the
     declared video size.*/
  nplanes = enc->state.info.nplanes;
  if (img->nplanes != nplanes) return OD_EINVAL;
  for (pli = 0; pli < nplanes; pli++) {
    if (img->planes[pli].xdec != enc->state.info.plane_info[pli].xdec
     || img->planes[pli].ydec != enc->state.info.plane_info[pli].ydec) {
      return OD_EINVAL;
    }
  }
  frame_width = enc->state.frame_width;
  frame_height = enc->state.frame_height;
  pic_width = enc->state.info.pic_width;
  pic_height = enc->state.info.pic_height;
  nhsb = enc->state.nhsb;
  nvsb = enc->state.nvsb;
  if (img->width != frame_width || img->height != frame_height) {
    /*The buffer does not match the frame size.
      Check to see if it matches the picture size.*/
    if (img->width != pic_width || img->height != pic_height) {
      /*It doesn't; we don't know how to handle it yet.*/
      return OD_EINVAL;
    }
  }
  /* Copy and pad the image. */
  for (pli = 0; pli < nplanes; pli++) {
    od_img_plane plane;
    int plane_width;
    int plane_height;
    *&plane = *(img->planes + pli);
    plane_width = ((pic_width + (1 << plane.xdec) - 1) >> plane.xdec);
    plane_height = ((pic_height + (1 << plane.ydec) - 1) >>
     plane.ydec);
    od_img_plane_copy_pad8(&enc->state.io_imgs[OD_FRAME_INPUT].planes[pli],
     frame_width >> plane.xdec, frame_height >> plane.ydec,
     &plane, plane_width, plane_height);
    od_img_plane_edge_ext8(enc->state.io_imgs[OD_FRAME_INPUT].planes + pli,
     frame_width >> plane.xdec, frame_height >> plane.ydec,
     OD_UMV_PADDING >> plane.xdec, OD_UMV_PADDING >> plane.ydec);
  }
#if defined(OD_DUMP_IMAGES)
  if (od_logging_active(OD_LOG_GENERIC, OD_LOG_DEBUG)) {
    daala_info *info;
    od_img img;
    info = &enc->state.info;
    /*Modify the image offsets to include the padding.*/
    *&img = *(enc->state.io_imgs+OD_FRAME_INPUT);
    for (pli = 0; pli < nplanes; pli++) {
      img.planes[pli].data -= (OD_UMV_PADDING>>info->plane_info[pli].xdec)
       +img.planes[pli].ystride*(OD_UMV_PADDING>>info->plane_info[pli].ydec);
    }
    img.width += OD_UMV_PADDING<<1;
    img.height += OD_UMV_PADDING<<1;
    od_state_dump_img(&enc->state, &img, "pad");
  }
#endif
  /* Check if the frame should be a keyframe. */
  mbctx.is_keyframe = (enc->state.cur_time %
   (enc->state.info.keyframe_rate) == 0) ? 1 : 0;
  /*Update the buffer state.*/
  if (enc->state.ref_imgi[OD_FRAME_SELF] >= 0) {
    enc->state.ref_imgi[OD_FRAME_PREV] =
     enc->state.ref_imgi[OD_FRAME_SELF];
    /*TODO: Update golden frame.*/
    if (enc->state.ref_imgi[OD_FRAME_GOLD] < 0) {
      enc->state.ref_imgi[OD_FRAME_GOLD] =
       enc->state.ref_imgi[OD_FRAME_SELF];
      /*TODO: Mark keyframe timebase.*/
    }
  }
  /*Select a free buffer to use for this reference frame.*/
  for (refi = 0; refi == enc->state.ref_imgi[OD_FRAME_GOLD]
   || refi == enc->state.ref_imgi[OD_FRAME_PREV]
   || refi == enc->state.ref_imgi[OD_FRAME_NEXT]; refi++);
  enc->state.ref_imgi[OD_FRAME_SELF] = refi;
  memcpy(&enc->state.input, img, sizeof(enc->state.input));
  /*We must be a keyframe if we don't have a reference.*/
  mbctx.is_keyframe |= !(enc->state.ref_imgi[OD_FRAME_PREV] >= 0);
  /*Initialize the entropy coder.*/
  od_ec_enc_reset(&enc->ec);
#if defined(OD_METRICS)
  tot_frac_bits = od_ec_enc_tell_frac(&enc->ec);
#endif
  /*Write a bit to mark this as a data packet.*/
  od_ec_encode_bool_q15(&enc->ec, 0, 16384);
  /*Code the keyframe bit.*/
  od_ec_encode_bool_q15(&enc->ec, mbctx.is_keyframe, 16384);
  OD_LOG((OD_LOG_ENCODER, OD_LOG_INFO, "is_keyframe=%d", mbctx.is_keyframe));
  /*TODO: Incrment frame count.*/
  /*Motion estimation and compensation.*/
  if (!mbctx.is_keyframe) {
#if defined(OD_DUMP_IMAGES) && defined(OD_ANIMATE)
    enc->state.ani_iter = 0;
#endif
    OD_LOG((OD_LOG_ENCODER, OD_LOG_INFO, "Predicting frame %i:",
     (int)daala_granule_basetime(enc, enc->state.cur_time)));
    od_mv_est(enc->mvest, OD_FRAME_PREV, 452);
    od_state_mc_predict(&enc->state, OD_FRAME_PREV);
    /*Do edge extension here because the block-size analysis needs to read
      outside the frame, but otherwise isn't read from.*/
    for (pli = 0; pli < nplanes; pli++) {
      od_img_plane plane;
      *&plane = *(enc->state.io_imgs[OD_FRAME_REC].planes + pli);
      od_img_plane_edge_ext8(&plane, frame_width >> plane.xdec,
       frame_height >> plane.ydec, OD_UMV_PADDING >> plane.xdec,
       OD_UMV_PADDING >> plane.ydec);
    }
#if defined(OD_DUMP_IMAGES)
    /*Dump reconstructed frame.*/
    /*od_state_dump_img(&enc->state,enc->state.io_imgs + OD_FRAME_REC,"rec");*/
    od_state_fill_vis(&enc->state);
    od_state_dump_img(&enc->state, &enc->state.vis_img, "vis");
#endif
  }
  /*Block size switching.*/
  od_state_init_border_as_32x32(&enc->state);
  /* Allocate a blockSizeComp for scratch space and then calculate the block sizes
     eventually store them in bsize. */
  bs = _ogg_malloc(sizeof(BlockSizeComp));
  od_log_matrix_uchar(OD_LOG_GENERIC, OD_LOG_INFO, "bimg ",
   enc->state.io_imgs[OD_FRAME_INPUT].planes[0].data -
   16*enc->state.io_imgs[OD_FRAME_INPUT].planes[0].ystride - 16,
   enc->state.io_imgs[OD_FRAME_INPUT].planes[0].ystride, (nvsb + 1)*32);
#if defined(OD_METRICS)
  bs_frac_bits = od_ec_enc_tell_frac(&enc->ec);
#endif
  for (i = 0; i < nvsb; i++) {
    unsigned char *bimg;
    unsigned char *rimg;
    int istride;
    int rstride;
    int bstride;
    int kf;
    kf = mbctx.is_keyframe;
    bstride = enc->state.bstride;
    istride = enc->state.io_imgs[OD_FRAME_INPUT].planes[0].ystride;
    rstride = kf ? 0 : enc->state.io_imgs[OD_FRAME_REC].planes[0].ystride;
    bimg = enc->state.io_imgs[OD_FRAME_INPUT].planes[0].data + i*istride*32;
    rimg = enc->state.io_imgs[OD_FRAME_REC].planes[0].data + i*rstride*32;
    for (j = 0; j < nhsb; j++) {
      int bsize[4][4];
      unsigned char *state_bsize;
      state_bsize = &enc->state.bsize[i*4*enc->state.bstride + j*4];
      process_block_size32(bs, bimg + j*32, istride,
       kf ? NULL : rimg + j*32, rstride, bsize);
      /* Grab the 4x4 information returned from process_block_size32 in bsize
         and store it in the od_state bsize. */
      for (k = 0; k < 4; k++) {
        for (m = 0; m < 4; m++) {
#if 0
          state_bsize[k*bstride + m] = OD_MINI(bsize[k][m], 2);
#else
          state_bsize[k*bstride + m] = 0;
#endif
        }
      }
      od_block_size_encode(&enc->ec, &state_bsize[0], bstride);
    }
  }
#if defined(OD_METRICS)
  enc->state.bit_metrics[OD_METRIC_BLOCK_SWITCHING] =
   od_ec_enc_tell_frac(&enc->ec) - bs_frac_bits;
#endif
  od_log_matrix_uchar(OD_LOG_GENERIC, OD_LOG_INFO, "bsize ", enc->state.bsize,
   enc->state.bstride, (nvsb + 1)*4);
  for (i = 0; i < nvsb*4; i++) {
    for (j = 0; j < nhsb*4; j++) {
      OD_LOG_PARTIAL((OD_LOG_GENERIC, OD_LOG_INFO, "%d ",
       enc->state.bsize[i*enc->state.bstride + j]));
    }
    OD_LOG_PARTIAL((OD_LOG_GENERIC, OD_LOG_INFO, "\n"));
  }
  _ogg_free(bs);
  /*Code the motion vectors.*/
  if (!mbctx.is_keyframe) {
    int nhmvbs;
    int nvmvbs;
    int vx;
    int vy;
    od_img *mvimg;
    int width;
    int height;
    int mv_res;
    od_mv_grid_pt *mvp;
    od_mv_grid_pt **grid;
#if defined(OD_METRICS)
    mv_frac_bits = od_ec_enc_tell_frac(&enc->ec);
#endif
    nhmvbs = (enc->state.nhmbs + 1) << 2;
    nvmvbs = (enc->state.nvmbs + 1) << 2;
    mvimg = enc->state.io_imgs + OD_FRAME_REC;
    mv_res = enc->state.mv_res;
    OD_ASSERT(0 <= mv_res && mv_res < 3);
    od_ec_enc_uint(&enc->ec, mv_res, 3);
    width = (mvimg->width + 32) << (3 - mv_res);
    height = (mvimg->height + 32) << (3 - mv_res);
    grid = enc->state.mv_grid;
    /*Level 0.*/
    for (vy = 0; vy <= nvmvbs; vy += 4) {
      for (vx = 0; vx <= nhmvbs; vx += 4) {
        mvp = &grid[vy][vx];
        od_encode_mv(enc, mvp, vx, vy, 0, mv_res, width, height);
      }
    }
    /*Level 1.*/
    for (vy = 2; vy <= nvmvbs; vy += 4) {
      for (vx = 2; vx <= nhmvbs; vx += 4) {
        int p_invalid;
        p_invalid = od_mv_level1_prob(grid, vx, vy);
        mvp = &(grid[vy][vx]);
        od_ec_encode_bool_q15(&enc->ec, mvp->valid, p_invalid);
        if (mvp->valid) {
          od_encode_mv(enc, mvp, vx, vy, 1, mv_res, width, height);
        }
      }
    }
    /*Level 2.*/
    for (vy = 0; vy <= nvmvbs; vy += 2) {
      for (vx = 2*((vy & 3) == 0); vx <= nhmvbs; vx += 4) {
        mvp = &grid[vy][vx];
        if ((vy-2 < 0 || grid[vy-2][vx].valid)
         && (vx-2 < 0 || grid[vy][vx-2].valid)
         && (vy+2 > nvmvbs || grid[vy+2][vx].valid)
         && (vx+2 > nhmvbs || grid[vy][vx+2].valid)) {
          od_ec_encode_bool_q15(&enc->ec, mvp->valid, 13684);
          if (mvp->valid) {
            od_encode_mv(enc, mvp, vx, vy, 2, mv_res, width, height);
          }
          else {
            OD_ASSERT(!mvp->valid);
          }
        }
      }
    }
    /*Level 3.*/
    for (vy = 1; vy <= nvmvbs; vy += 2) {
      for (vx = 1; vx <= nhmvbs; vx += 2) {
        mvp = &grid[vy][vx];
        if (grid[vy-1][vx-1].valid && grid[vy-1][vx+1].valid
         && grid[vy+1][vx+1].valid && grid[vy+1][vx-1].valid) {
          od_ec_encode_bool_q15(&enc->ec, mvp->valid, 16384);
          if (mvp->valid) {
            od_encode_mv(enc, mvp, vx, vy, 3, mv_res, width, height);
          }
          else {
            OD_ASSERT(!mvp->valid);
          }
        }
      }
    }
    /*Level 4.*/
    for (vy = 2; vy <= nvmvbs - 2; vy += 1) {
      for (vx = 3 - (vy & 1); vx <= nhmvbs - 2; vx += 2) {
        mvp = &grid[vy][vx];
        if (grid[vy-1][vx].valid && grid[vy][vx-1].valid
         && grid[vy+1][vx].valid && grid[vy][vx+1].valid) {
          od_ec_encode_bool_q15(&enc->ec, mvp->valid, 16384);
          if (mvp->valid) {
            od_encode_mv(enc, mvp, vx, vy, 4, mv_res, width, height);
          }
          else {
            OD_ASSERT(!mvp->valid);
          }
        }
      }
    }
#if defined(OD_METRICS)
    enc->state.bit_metrics[OD_METRIC_MV] =
     od_ec_enc_tell_frac(&enc->ec) - mv_frac_bits;
#endif
  }
  {
    od_coeff *ctmp[OD_NPLANES_MAX];
    od_coeff *dtmp[OD_NPLANES_MAX];
    od_coeff *mctmp[OD_NPLANES_MAX];
    od_coeff *mdtmp[OD_NPLANES_MAX];
    od_coeff *ltmp[OD_NPLANES_MAX];
    od_coeff *lbuf[OD_NPLANES_MAX];
    int xdec;
    int ydec;
    int sby;
    int sbx;
    int mi;
    int h;
    int w;
    int y;
    int x;
    /*Initialize the data needed for each plane.*/
    mbctx.modes = _ogg_calloc((frame_width >> 2)*(frame_height >> 2),
     sizeof(*mbctx.modes));
    for (mi = 0; mi < OD_INTRA_NMODES; mi++) {
      mbctx.mode_p0[mi] = 32768/OD_INTRA_NMODES;
    }
    for (pli = 0; pli < nplanes; pli++) {
      generic_model_init(&mbctx.model_dc[pli]);
      generic_model_init(&mbctx.model_g[pli]);
      generic_model_init(&mbctx.model_ym[pli]);
      mbctx.ex_dc[pli] = pli > 0 ? 8 : 32768;
      mbctx.ex_g[pli] = 8;
      xdec = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].xdec;
      ydec = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].ydec;
      w = frame_width >> xdec;
      h = frame_height >> ydec;
      /* Set this to 1 to enable the new (experimental, encode-only) PVQ
         implementation */
      mbctx.run_pvq[pli] = 0;
      od_ec_enc_uint(&enc->ec, enc->scale[pli], 512);
      /*If the scale is zero, force scalar.*/
      if (!enc->scale[pli]) mbctx.run_pvq[pli] = 0;
      else od_ec_encode_bool_q15(&enc->ec, mbctx.run_pvq[pli], 16384);
      ctmp[pli] = _ogg_calloc(w*h, sizeof(*ctmp[pli]));
      dtmp[pli] = _ogg_calloc(w*h, sizeof(*dtmp[pli]));
      if (pli == 0) {
        mbctx.tf = _ogg_calloc(w*h, sizeof(*mbctx.tf));
      }
      mctmp[pli] = _ogg_calloc(w*h, sizeof(*mctmp[pli]));
      mdtmp[pli] = _ogg_calloc(w*h, sizeof(*mdtmp[pli]));
      /*We predict chroma planes from the luma plane.  Since chroma can be
        subsampled, we cache subsampled versions of the luma plane in the
        frequency domain.  We can share buffers with the same subsampling.*/
      if (pli > 0) {
        int plj;
        if (xdec || ydec) {
          for (plj = 1; plj < pli; plj++) {
            if (xdec == enc->state.io_imgs[OD_FRAME_INPUT].planes[plj].xdec
             && ydec == enc->state.io_imgs[OD_FRAME_INPUT].planes[plj].ydec) {
              ltmp[pli] = NULL;
              lbuf[pli] = ltmp[plj];
            }
          }
          if (plj >= pli) {
            lbuf[pli] = ltmp[pli] = _ogg_calloc(w*h, sizeof(*ltmp[pli]));
          }
        }
        else {
          ltmp[pli] = NULL;
          lbuf[pli] = ctmp[pli];
        }
      }
      else lbuf[pli] = ltmp[pli] = NULL;
      od_adapt_row_init(&enc->state.adapt_sb[pli]);
    }
    for (pli = 0; pli < nplanes; pli++) {
      xdec = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].xdec;
      ydec = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].ydec;
      w = frame_width >> xdec;
      h = frame_height >> ydec;
      /*Collect the image data needed for this plane.*/
      {
        unsigned char *data;
        unsigned char *mdata;
        int ystride;
        data = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].data;
        mdata = enc->state.io_imgs[OD_FRAME_REC].planes[pli].data;
        ystride = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].ystride;
        for (y = 0; y < h; y++) {
          for (x = 0; x < w; x++) {
            ctmp[pli][y*w + x] = data[ystride*y + x] - 128;
            if (!mbctx.is_keyframe) {
              mctmp[pli][y*w + x] = mdata[ystride*y + x] - 128;
            }
          }
        }
      }
      /*Apply the prefilter across the entire image.*/
      for (sby = 0; sby < nvsb; sby++) {
        for (sbx = 0; sbx < nhsb; sbx++) {
          od_apply_prefilter(ctmp[pli], w, sbx, sby, 3, enc->state.bsize,
           enc->state.bstride, xdec, ydec, (sbx > 0 ? OD_LEFT_EDGE : 0) |
           (sby < nvsb - 1 ? OD_BOTTOM_EDGE : 0));
          if (!mbctx.is_keyframe) {
            od_apply_prefilter(mctmp[pli], w, sbx, sby, 3, enc->state.bsize,
             enc->state.bstride, xdec, ydec, (sbx > 0 ? OD_LEFT_EDGE : 0) |
             (sby < nvsb - 1 ? OD_BOTTOM_EDGE : 0));
          }
        }
      }
    }
    for (sby = 0; sby < nvsb; sby++) {
      ogg_int32_t adapt_hmean[OD_NPLANES_MAX][OD_NSB_ADAPT_CTXS];
      for (pli = 0; pli < nplanes; pli++) {
        od_adapt_hmean_init(&enc->state.adapt_sb[pli], adapt_hmean[pli]);
      }
      for (sbx = 0; sbx < nhsb; sbx++) {
        for (pli = 0; pli < nplanes; pli++) {
          od_adapt_ctx *adapt_sb;
          mbctx.c = ctmp[pli];
          mbctx.d = dtmp;
          mbctx.mc = mctmp[pli];
          mbctx.md = mdtmp[pli];
          mbctx.l = lbuf[pli];
          xdec = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].xdec;
          ydec = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].ydec;
          w = frame_width >> xdec;
          h = frame_height >> ydec;
          mbctx.nk = mbctx.k_total = mbctx.sum_ex_total_q8 = 0;
          mbctx.ncount = mbctx.count_total_q8 = mbctx.count_ex_total_q8 = 0;
          adapt_sb = &enc->state.adapt_sb[pli];
          /*Need to update this to decay based on superblocks width.*/
          od_adapt_get_stats(adapt_sb, sbx, adapt_hmean[pli], mbctx.adapt);
          od_encode_block(enc, &mbctx, pli, sbx, sby, 3, xdec, ydec,
           sby > 0 && sbx < nhsb - 1);
          if (mbctx.nk > 0) {
            mbctx.adapt[OD_ADAPT_K_Q8] =
             OD_DIVU_SMALL(mbctx.k_total << 8, mbctx.nk);
            mbctx.adapt[OD_ADAPT_SUM_EX_Q8] =
             OD_DIVU_SMALL(mbctx.sum_ex_total_q8, mbctx.nk);
          }
          else {
            mbctx.adapt[OD_ADAPT_K_Q8] = OD_ADAPT_NO_VALUE;
            mbctx.adapt[OD_ADAPT_SUM_EX_Q8] = OD_ADAPT_NO_VALUE;
          }
          if (mbctx.ncount > 0) {
            mbctx.adapt[OD_ADAPT_COUNT_Q8] =
             OD_DIVU_SMALL(mbctx.count_total_q8, mbctx.ncount);
            mbctx.adapt[OD_ADAPT_COUNT_EX_Q8] =
             OD_DIVU_SMALL(mbctx.count_ex_total_q8, mbctx.ncount);
          }
          else {
            mbctx.adapt[OD_ADAPT_COUNT_Q8] = OD_ADAPT_NO_VALUE;
            mbctx.adapt[OD_ADAPT_COUNT_EX_Q8] = OD_ADAPT_NO_VALUE;
          }
          od_adapt_forward(adapt_sb, sbx, adapt_hmean[pli], mbctx.adapt);
        }
      }
      for (pli = 0; pli < nplanes; pli++) {
        od_adapt_row_backward(&enc->state.adapt_sb[pli]);
      }
    }
    for (pli = 0; pli < nplanes; pli++) {
      xdec = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].xdec;
      ydec = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].ydec;
      w = frame_width >> xdec;
      h = frame_height >> ydec;
      /*Apply the postfilter across the entire image.*/
      for (sby = 0; sby < nvsb; sby++) {
        for (sbx = 0; sbx < nhsb; sbx++) {
          od_apply_postfilter(ctmp[pli], w, sbx, sby, 3, enc->state.bsize,
           enc->state.bstride, xdec, ydec, (sby > 0 ? OD_TOP_EDGE : 0) |
           (sbx < nhsb - 1 ? OD_RIGHT_EDGE : 0));
        }
      }
      {
        unsigned char *data;
        int ystride;
        data = enc->state.io_imgs[OD_FRAME_REC].planes[pli].data;
        ystride = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].ystride;
        for (y = 0; y < h; y++) {
          for (x = 0; x < w; x++) {
            data[ystride*y + x] = OD_CLAMP255(ctmp[pli][y*w + x] + 128);
          }
        }
      }
    }
    for (pli = nplanes; pli-- > 0;) {
      _ogg_free(ltmp[pli]);
      _ogg_free(dtmp[pli]);
      _ogg_free(ctmp[pli]);
      _ogg_free(mctmp[pli]);
      _ogg_free(mdtmp[pli]);
    }
    _ogg_free(mbctx.modes);
    _ogg_free(mbctx.tf);
  }
#if defined(OD_DUMP_IMAGES)
  /*Dump YUV*/
  {
    char *ptr;
    ptr = getenv("OD_DUMP_IMAGES_SUFFIX");
    if (!ptr) {
      ptr="out";
    }
    od_state_dump_yuv(&enc->state, enc->state.io_imgs + OD_FRAME_REC, ptr);
  }
#endif
#if defined(OD_LOGGING_ENABLED)
  for (pli = 0; pli < nplanes; pli++) {
    unsigned char *data;
    ogg_int64_t enc_sqerr;
    ogg_uint32_t npixels;
    int ystride;
    int xdec;
    int ydec;
    int w;
    int h;
    int x;
    int y;
    enc_sqerr = 0;
    data = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].data;
    ystride = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].ystride;
    xdec = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].xdec;
    ydec = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].ydec;
    w = frame_width >> xdec;
    h = frame_height >> ydec;
    npixels = w*h;
    for (y = 0; y < h; y++) {
      unsigned char *rec_row;
      unsigned char *inp_row;
      rec_row = enc->state.io_imgs[OD_FRAME_REC].planes[pli].data +
       enc->state.io_imgs[OD_FRAME_REC].planes[pli].ystride*y;
      inp_row = data + ystride*y;
      for (x = 0; x < w; x++) {
        int inp_val;
        int diff;
        inp_val = inp_row[x];
        diff = inp_val - rec_row[x];
        enc_sqerr += diff*diff;
      }
    }
    OD_LOG((OD_LOG_ENCODER, OD_LOG_DEBUG,
     "Encoded Plane %i, Squared Error: %12lli  Pixels: %6u  PSNR:  %5.4f",
     pli, (long long)enc_sqerr, npixels,
     10*log10(255*255.0*npixels/enc_sqerr)));
  }
#endif
  OD_LOG((OD_LOG_ENCODER, OD_LOG_INFO,
   "mode bits: %f/%f=%f", mode_bits, mode_count, mode_bits/mode_count));
  enc->packet_state = OD_PACKET_READY;
  od_state_upsample8(&enc->state,
   enc->state.ref_imgs + enc->state.ref_imgi[OD_FRAME_SELF],
   enc->state.io_imgs + OD_FRAME_REC);
#if defined(OD_DUMP_IMAGES)
  /*Dump reference frame.*/
  /*od_state_dump_img(&enc->state,
   enc->state.ref_img + enc->state.ref_imigi[OD_FRAME_SELF], "ref");*/
#endif
  if (enc->state.info.frame_duration == 0) enc->state.cur_time += duration;
  else enc->state.cur_time += enc->state.info.frame_duration;
#if defined(OD_METRICS)
  enc->state.bit_metrics[OD_METRIC_TOTAL] =
   od_ec_enc_tell_frac(&enc->ec) - tot_frac_bits;
  write_metrics(enc->state.cur_time, &enc->state.bit_metrics[0]);
#endif
  return 0;
}

#if defined(OD_ENCODER_CHECK)
static void daala_encoder_check(daala_enc_ctx *ctx, od_img *img,
 ogg_packet *op) {
  int pli;
  od_img dec_img;
  OD_ASSERT(ctx->dec);

  if (daala_decode_packet_in(ctx->dec, &dec_img, op) < 0) {
    fprintf(stderr,"decode failed!\n");
    return;
  }

  OD_ASSERT(img->nplanes == dec_img.nplanes);
  for (pli = 0; pli < img->nplanes; pli++) {
    int plane_width;
    int plane_height;
    int xdec;
    int ydec;
    int i;
    OD_ASSERT(img->planes[pli].xdec == dec_img.planes[pli].xdec);
    OD_ASSERT(img->planes[pli].ydec == dec_img.planes[pli].ydec);
    OD_ASSERT(img->planes[pli].ystride == dec_img.planes[pli].ystride);

    xdec = dec_img.planes[pli].xdec;
    ydec = dec_img.planes[pli].ydec;
    plane_width = ctx->dec->state.frame_width >> xdec;
    plane_height = ctx->dec->state.frame_height >> ydec;
    for (i = 0; i < plane_height; i++) {
      if (memcmp(img->planes[pli].data + img->planes[pli].ystride * i,
       dec_img.planes[pli].data + dec_img.planes[pli].ystride * i,
       plane_width))
        fprintf(stderr,"pixel mismatch in row %d\n", i);
      }
    }
}
#endif

int daala_encode_packet_out(daala_enc_ctx *enc, int last, ogg_packet *op) {
  ogg_uint32_t nbytes;
  if (enc == NULL || op == NULL) return OD_EFAULT;
  else if (enc->packet_state <= 0 || enc->packet_state == OD_PACKET_DONE) {
    return 0;
  }
  op->packet = od_ec_enc_done(&enc->ec, &nbytes);
  op->bytes = nbytes;
  OD_LOG((OD_LOG_ENCODER, OD_LOG_INFO, "Output Bytes: %ld", op->bytes));
  op->b_o_s = 0;
  op->e_o_s = last;
  op->packetno = 0;
  op->granulepos = enc->state.cur_time;
  if (last) enc->packet_state = OD_PACKET_DONE;
  else enc->packet_state = OD_PACKET_EMPTY;

#if defined(OD_ENCODER_CHECK)
  /*Compare reconstructed frame against decoded frame.*/
  daala_encoder_check(enc, enc->state.io_imgs + OD_FRAME_REC, op);
#endif

  return 1;
}
