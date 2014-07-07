/*Daala video codec
Copyright (c) 2013 Daala project contributors.  All rights reserved.

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


#if !defined(_accounting_H)
# define _accounting_H (1)

# include <stdio.h>
# include "internal.h"
# include "state.h"

# define OD_ACCT_BLOCK (1)

enum od_acct_category {
  OD_ACCT_CAT_TECHNIQUE = 0,
  OD_ACCT_CAT_PLANE = 1,
  OD_ACCT_NCATS
};

typedef enum od_acct_category od_acct_category;

enum od_acct_technique {
  OD_ACCT_TECH_UNKNOWN = 0,
  OD_ACCT_TECH_FRAME = 1,
  OD_ACCT_TECH_BLOCK_SIZE = 2,
  OD_ACCT_TECH_INTRA_MODE = 3,
  OD_ACCT_TECH_DC_COEFF = 4,
  OD_ACCT_TECH_AC_COEFFS = 5,
  OD_ACCT_TECH_MOTION_VECTORS = 6,
  OD_ACCT_NTECHS
};

typedef enum od_acct_technique od_acct_technique;

enum od_acct_plane {
  OD_ACCT_PLANE_UNKNOWN = 0,
  OD_ACCT_PLANE_FRAME = 1,
  OD_ACCT_PLANE_LUMA = 2,
  OD_ACCT_PLANE_CB = 3,
  OD_ACCT_PLANE_CR = 4,
  OD_ACCT_PLANE_ALPHA = 5,
  OD_ACCT_NPLANES
};

typedef enum od_acct_plane od_acct_plane;

/*typedef enum od_acct_band od_acct_band;*/

#if defined(OD_ACCOUNTING)
# define OD_ACCT_UPDATE(acct, frac_bits, cat, value) \
 od_acct_update(acct, frac_bits, cat, value)
#else
# define OD_ACCT_UPDATE(acct, frac_bits, cat, value)
#endif

#define OD_ACCT_SIZE (OD_ACCT_NTECHS*OD_ACCT_NPLANES)

typedef struct od_acct od_acct;

struct od_acct {
  FILE *fp;
  od_state *s;
  ogg_uint32_t last_frac_bits;
  unsigned int state[OD_ACCT_NCATS];
  ogg_uint32_t frac_bits[OD_ACCT_SIZE];
#ifdef OD_ACCT_BLOCK
# define OD_ACCT_BLOCK_BUFSIZE 512*512
  ogg_uint32_t block_bits[3][OD_ACCT_BLOCK_BUFSIZE];
  ogg_uint32_t block_bits_pos;
#endif
};

#ifdef OD_ACCT_BLOCK
# define OD_ACCT_BLOCK_STRIDE 512
#endif

void od_acct_init(od_acct *acct, od_state *s);
void od_acct_clear(od_acct *acct);
void od_acct_reset(od_acct *acct);
void od_acct_update_frac_bits(od_acct *acct, ogg_uint32_t frac_bits);
void od_acct_set_category(od_acct *acct, od_acct_category cat,
 unsigned int value);
void od_acct_update(od_acct *acct, ogg_uint32_t frac_bits,
 od_acct_category cat, unsigned int value);
#ifdef OD_ACCT_BLOCK
void od_acct_start_block(od_acct *acct, ogg_uint32_t frac_bits);
void od_acct_finish_block(od_acct *acct, ogg_uint32_t frac_bits,
 int pli, int bx, int by);
#endif
void od_acct_print(od_acct *acct, FILE *_fp);
void od_acct_write(od_acct *acct, ogg_int64_t cur_time);

#endif
