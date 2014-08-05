/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*Daala video codec
Copyright (c) 2006-2010 Daala project contributors.  All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS”
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
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <getopt.h>
#include "../src/logging.h"
#include "daala/daalaenc.h"
#if defined(_WIN32)
# include <fcntl.h>
# include <io.h>
#endif
#if defined(_MSC_VER)
static double rint(double _x){
  return (int)(_x<0?_x-0.5:_x+0.5);
}
#endif



typedef struct av_input av_input;



struct av_input{
  int               has_video;
  FILE             *video_infile;
  int               video_pic_w;
  int               video_pic_h;
  int               video_fps_n;
  int               video_fps_d;
  int               video_par_n;
  int               video_par_d;
  int               video_interlacing;
  char              video_chroma_type[16];
  int               video_nplanes;
  daala_plane_info  video_plane_info[OD_NPLANES_MAX];
  od_img            video_img;
  int               video_cur_img;
};



static int y4m_parse_tags(av_input *_avin,char *_tags){
  int   got_w;
  int   got_h;
  int   got_fps;
  int   got_interlacing;
  int   got_par;
  int   got_chroma;
  int   tmp_video_fps_n;
  int   tmp_video_fps_d;
  int   tmp_video_par_n;
  int   tmp_video_par_d;
  char *p;
  char *q;
  got_w=got_h=got_interlacing=got_chroma=0;
  got_fps=_avin->video_fps_n>0&&_avin->video_fps_d>0;
  got_par=_avin->video_par_n>=0&&_avin->video_par_d>=0;
  for(p=_tags;;p=q){
    /*Skip any leading spaces.*/
    while(*p==' ')p++;
    /*If that's all we have, stop.*/
    if(p[0]=='\0')break;
    /*Find the end of this tag.*/
    for(q=p+1;*q!='\0'&&*q!=' ';q++);
    /*Process the tag.*/
    switch(p[0]){
      case 'W':{
        if(sscanf(p+1,"%d",&_avin->video_pic_w)!=1)return -1;
        got_w=1;
      }break;
      case 'H':{
        if(sscanf(p+1,"%d",&_avin->video_pic_h)!=1)return -1;
        got_h=1;
      }break;
      case 'F':{
        if(sscanf(p+1,"%d:%d",&tmp_video_fps_n,&tmp_video_fps_d)!=2)return -1;
        got_fps=1;
      }break;
      case 'I':{
        _avin->video_interlacing=p[1];
        got_interlacing=1;
      }break;
      case 'A':{
        if(sscanf(p+1,"%d:%d",&tmp_video_par_n,&tmp_video_par_d)!=2)return -1;
        got_par=1;
      }break;
      case 'C':{
        if(q-p>16)return -1;
        memcpy(_avin->video_chroma_type,p+1,q-p-1);
        _avin->video_chroma_type[q-p-1]='\0';
        got_chroma=1;
      }break;
      /*Ignore unknown tags.*/
    }
  }
  if(!got_w||!got_h||!got_fps||!got_interlacing||!got_par)return -1;
  /*Chroma-type is not specified in older files, e.g., those generated by
     mplayer.*/
  if(!got_chroma)strcpy(_avin->video_chroma_type,"420");
  /*Update fps and aspect ration fields only if not specified on the command
     line.*/
  if(_avin->video_fps_n<=0||_avin->video_fps_d<=0){
    _avin->video_fps_n=tmp_video_fps_n;
    _avin->video_fps_d=tmp_video_fps_d;
  }
  if(_avin->video_par_n<0||_avin->video_par_d<0){
    _avin->video_par_n=tmp_video_par_n;
    _avin->video_par_d=tmp_video_par_d;
  }
  return 0;
}

static void id_y4m_file(av_input *_avin,const char *_file,FILE *_test){
  od_img        *img;
  unsigned char  buf[128];
  int            ret;
  int            pli;
  int            bi;
  for(bi=0;bi<127;bi++){
    ret=fread(buf+bi,1,1,_test);
    if(ret<1)return;
    if(buf[bi]=='\n')break;
  }
  if(bi>=127){
    fprintf(stderr,"Error parsing '%s' header; not a YUV4MPEG2 file?\n",_file);
    exit(1);
  }
  buf[bi]='\0';
  if(memcmp(buf,"MPEG",4))return;
  if(buf[4]!='2'){
    fprintf(stderr,
     "Incorrect YUV input file version; YUV4MPEG2 required.\n");
    exit(1);
  }
  ret=y4m_parse_tags(_avin,(char *)buf+5);
  if(ret<0){
    fprintf(stderr,"Error parsing YUV4MPEG2 header fields in '%s'.\n",_file);
    exit(1);
  }
  if(_avin->video_interlacing!='p'){
    fprintf(stderr,"Interlaced input is not currently supported.\n");
    exit(1);
  }
  _avin->video_infile=_test;
  _avin->has_video=1;
  fprintf(stderr,"File '%s' is %ix%i %0.03f fps %s video.\n",
   _file,_avin->video_pic_w,_avin->video_pic_h,
    (double)_avin->video_fps_n/_avin->video_fps_d,_avin->video_chroma_type);
  /*Allocate buffers for the image data.*/
  /*TODO: Specify chroma offsets.*/
  _avin->video_plane_info[0].xdec=0;
  _avin->video_plane_info[0].ydec=0;
  if(strcmp(_avin->video_chroma_type,"444")==0){
    _avin->video_nplanes=3;
    _avin->video_plane_info[1].xdec=0;
    _avin->video_plane_info[1].ydec=0;
    _avin->video_plane_info[2].xdec=0;
    _avin->video_plane_info[2].ydec=0;
  }
  else if(strcmp(_avin->video_chroma_type,"444alpha")==0){
    _avin->video_nplanes=4;
    _avin->video_plane_info[1].xdec=0;
    _avin->video_plane_info[1].ydec=0;
    _avin->video_plane_info[2].xdec=0;
    _avin->video_plane_info[2].ydec=0;
    _avin->video_plane_info[3].xdec=0;
    _avin->video_plane_info[3].ydec=0;
  }
  else if(strcmp(_avin->video_chroma_type,"422")==0){
    _avin->video_nplanes=3;
    _avin->video_plane_info[1].xdec=1;
    _avin->video_plane_info[1].ydec=0;
    _avin->video_plane_info[2].xdec=1;
    _avin->video_plane_info[2].ydec=0;
  }
  else if(strcmp(_avin->video_chroma_type,"411")==0){
    _avin->video_nplanes=3;
    _avin->video_plane_info[1].xdec=2;
    _avin->video_plane_info[1].ydec=0;
    _avin->video_plane_info[2].xdec=2;
    _avin->video_plane_info[2].ydec=0;
  }
  else if(strcmp(_avin->video_chroma_type,"420")==0||
   strcmp(_avin->video_chroma_type,"420jpeg")==0){
    _avin->video_nplanes=3;
    _avin->video_plane_info[1].xdec=1;
    _avin->video_plane_info[1].ydec=1;
    _avin->video_plane_info[2].xdec=1;
    _avin->video_plane_info[2].ydec=1;
  }
  else if(strcmp(_avin->video_chroma_type,"420mpeg2")==0){
    _avin->video_nplanes=3;
    _avin->video_plane_info[1].xdec=1;
    _avin->video_plane_info[1].ydec=1;
    _avin->video_plane_info[2].xdec=1;
    _avin->video_plane_info[2].ydec=1;
  }
  else if(strcmp(_avin->video_chroma_type,"420paldv")==0){
    _avin->video_nplanes=3;
    _avin->video_plane_info[1].xdec=1;
    _avin->video_plane_info[1].ydec=1;
    _avin->video_plane_info[2].xdec=1;
    _avin->video_plane_info[2].ydec=1;
  }
  else if(strcmp(_avin->video_chroma_type,"mono")==0){
    _avin->video_nplanes=1;
  }
  else{
    fprintf(stderr,"Unknown chroma sampling type: '%s'.\n",
     _avin->video_chroma_type);
    exit(1);
  }
  img=&_avin->video_img;
  img->nplanes=_avin->video_nplanes;
  img->width=_avin->video_pic_w;
  img->height=_avin->video_pic_h;
  for(pli=0;pli<img->nplanes;pli++){
    od_img_plane *iplane;
    iplane=img->planes+pli;
    iplane->xdec=_avin->video_plane_info[pli].xdec;
    iplane->ydec=_avin->video_plane_info[pli].ydec;
    iplane->xstride=1;
    iplane->ystride=(_avin->video_pic_w+(1<<iplane->xdec)-1)>>iplane->xdec;
    iplane->data=_ogg_malloc(iplane->ystride*
     ((_avin->video_pic_h+(1<<iplane->ydec)-1)>>iplane->ydec));
  }
}

static void id_file(av_input *_avin,const char *_file){
  unsigned char  buf[4];
  FILE          *test;
  int            ret;
  if(!strcmp(_file,"-"))test=stdin;
  else{
    test=fopen(_file,"rb");
    if(test==NULL){
      fprintf(stderr,"Unable to open input file '%s'\n",_file);
      exit(1);
    }
  }
  ret=fread(buf,1,4,test);
  if(ret<4){
    fprintf(stderr,"EOF determining file type of file '%s'\n",_file);
    exit(1);
  }
  if(!memcmp(buf,"YUV4",4)){
    if(_avin->has_video){
      fprintf(stderr,
       "Multiple YUV4MPEG2 files specified on the command line.\n");
      exit(1);
    }
    id_y4m_file(_avin,_file,test);
    if(!_avin->has_video){
      fprintf(stderr,"Error parsing YUV4MPEG2 file.\n");
      exit(1);
    }
  }
  else{
    fprintf(stderr,
     "Input file '%s' is not a YUV4MPEG2 file.\n",_file);
  }
}

int fetch_and_process_video(av_input *_avin,ogg_page *_page,
 ogg_stream_state *_vo,daala_enc_ctx *_dd,int _video_ready,
 int *_limit){
  ogg_packet op;
  while(!_video_ready){
    size_t ret;
    char   frame[6];
    char   c;
    int    last;
    if(ogg_stream_pageout(_vo,_page)>0)return 1;
    else if(ogg_stream_eos(_vo)||(_limit && (*_limit)<0))return 0;
    ret=fread(frame,1,6,_avin->video_infile);
    if(ret==6){
      od_img *img;
      int     pli;
      if(memcmp(frame,"FRAME",5)!=0){
        fprintf(stderr,"Loss of framing in YUV input data.\n");
        exit(1);
      }
      if(frame[5]!='\n'){
        int bi;
        for(bi=0;bi<121;bi++){
          if(fread(&c,1,1,_avin->video_infile)==1&&c=='\n')break;
        }
        if(bi>=121){
          fprintf(stderr,"Error parsing YUV frame header.\n");
          exit(1);
        }
      }
      /*Read the frame data.*/
      img=&_avin->video_img;
      for(pli=0;pli<img->nplanes;pli++){
        od_img_plane *iplane;
        size_t        plane_sz;
        iplane=img->planes+pli;
        plane_sz=((_avin->video_pic_w+(1<<iplane->xdec)-1)>>iplane->xdec)*
         ((_avin->video_pic_h+(1<<iplane->ydec)-1)>>iplane->ydec);
        ret=fread(iplane->data/*+(_avin->video_pic_y>>iplane->ydec)*iplane->ystride+
         (_avin->video_pic_x>>iplane->xdec)*/,1,plane_sz,_avin->video_infile);
        if(ret!=plane_sz){
          fprintf(stderr,"Error reading YUV frame data.\n");
          exit(1);
        }
      }
      if(_limit){
        last=(*_limit)==0;
        (*_limit)--;
      }
      else last=0;
    }
    else last=1;
    /*Pull the packets from the previous frame, now that we know whether or not
       we can read the current one.
      This is used to set the e_o_s bit on the final packet.*/
    while(daala_encode_packet_out(_dd,last,&op))ogg_stream_packetin(_vo,&op);
    /*Submit the current frame for encoding.*/
    if(!last)daala_encode_img_in(_dd,&_avin->video_img,0);
  }
  return _video_ready;
}

static const char *OPTSTRING="o:a:A:v:V:s:S:f:F:h:k:l:";

static const struct option OPTIONS[]={
  {"output",required_argument,NULL,'o'},
  {"video-quality",required_argument,NULL,'v'},
  {"video-rate-target",required_argument,NULL,'V'},
  {"keyframe-rate",required_argument,NULL,'k'},
  {"serial",required_argument,NULL,'s'},
  {"limit",required_argument,NULL,'l'},
  {"help",no_argument,NULL,'h'},
  {NULL,0,NULL,0}
};

static void usage(void){
  fprintf(stderr,
   "Usage: encoder_example [options] video_file\n\n"
   "Options:\n\n"
   "  -o --output <filename.ogg>     file name for encoded output;\n"
   "                                 If this option is not given, the\n"
   "                                 compressed data is sent to stdout.\n\n"
   "  -v --video-quality <n>         Daala quality selector from 0 to 511.\n"
   "                                 511 yields the smallest files, but\n"
   "                                 lowest video quality; 1 yields the\n"
   "                                 highest quality, but large files;\n"
   "                                 0 is lossless.\n\n"
   "  -k --keyframe-rate <n>         Frequence of keyframes in output.\n\n"
   "  -V --video-rate-target <n>     bitrate target for Daala video;\n"
   "                                 use -v and not -V if at all possible,\n"
   "                                 as -v gives higher quality for a given\n"
   "                                 bitrate.\n\n"
   "  -s --serial <n>                Specify a serial number for the stream.\n"
   "  -l --limit <n>                 Maximum number of frames to encode.\n"
   " encoder_example accepts only uncompressed YUV4MPEG2 video.\n\n");
  exit(1);
}

int main(int _argc,char **_argv){
  FILE             *outfile;
  av_input          avin;
  ogg_stream_state  vo;
  ogg_page          og;
  ogg_packet        op;
  daala_enc_ctx    *dd;
  daala_info        di;
  daala_comment     dc;
  ogg_int64_t       video_bytesout;
  double            time_base;
  int               c;
  int               loi;
  int               ret;
  int               video_kbps;
  int               video_q;
  int               video_r;
  int               video_keyframe_rate;
  int               video_ready;
  int               pli;
  int               fixedserial;
  unsigned int      serial;
  int               limit;
  int               interactive;
  daala_log_init();
#if defined(_WIN32)
  _setmode(_fileno(stdin),_O_BINARY);
  _setmode(_fileno(stdout),_O_BINARY);
  interactive = _isatty(_fileno(stderr));
#else
  interactive = isatty(fileno(stderr));
#endif
  outfile=stdout;
  memset(&avin,0,sizeof(avin));
  avin.video_fps_n=-1;
  avin.video_fps_d=-1;
  avin.video_par_n=-1;
  avin.video_par_d=-1;
  video_q=10;
  video_keyframe_rate=256;
  video_r=-1;
  video_bytesout=0;
  fixedserial=0;
  limit=-1;
  while((c=getopt_long(_argc,_argv,OPTSTRING,OPTIONS,&loi))!=EOF){
    switch(c){
      case 'o':{
        outfile=fopen(optarg,"wb");
        if(outfile==NULL){
          fprintf(stderr,"Unable to open output file '%s'\n",optarg);
          exit(1);
        }
      }break;
      case 'k':{
        video_keyframe_rate=atoi(optarg);
        if(video_keyframe_rate<1||video_keyframe_rate>1000){
          fprintf(stderr,"Illegal video keyframe rate (use 1 through 1000)\n");
          exit(1);
        }
      }break;
      case 'v':{
        video_q=(int)rint(atof(optarg)*1);
        if(video_q<0||video_q>511){
          fprintf(stderr,"Illegal video quality (use 0 through 511)\n");
          exit(1);
        }
        video_r=0;
      }break;
      case 'V':{
        video_r=(int)rint(atof(optarg)*1000);
        if(video_r<45000||video_r>2000000){
          fprintf(stderr,
           "Illegal video bitrate (use 45kbps through 2000kbps)\n");
          exit(1);
        }
        video_q=0;
      }break;
      case 's':{
        if(sscanf(optarg,"%u",&serial)!=1){
          serial=0;
        }
        else{
          fixedserial=1;
        }
      }break;
      case 'l':{
        limit=atoi(optarg);
        if(limit<1){
          fprintf(stderr,
           "Illegal maximum frame limit (must be greater than 0)\n");
          exit(1);
        }
      }break;
      case 'h':
      default:{
        usage();
      }
    }
  }
  /*Assume anything following the options must be a file name.*/
  for(;optind<_argc;optind++)id_file(&avin,_argv[optind]);
  if(!avin.has_video){
    fprintf(stderr,"No video files submitted for compression.\n");
    exit(1);
  }
  if (!fixedserial) {
    srand(time(NULL));
    serial=rand();
  }
  ogg_stream_init(&vo,serial);
  daala_info_init(&di);
  di.pic_width=avin.video_pic_w;
  di.pic_height=avin.video_pic_h;
  di.timebase_numerator=avin.video_fps_n;
  di.timebase_denominator=avin.video_fps_d;
  di.frame_duration=1;
  di.pixel_aspect_numerator=avin.video_par_n;
  di.pixel_aspect_denominator=avin.video_par_d;
  di.nplanes=avin.video_nplanes;
  memcpy(di.plane_info,avin.video_plane_info,
   di.nplanes*sizeof(*di.plane_info));
  di.keyframe_rate = video_keyframe_rate;
  /*TODO: Other crap.*/
  dd=daala_encode_create(&di);
  daala_comment_init(&dc);
  /*Set up encoder.*/
  daala_encode_ctl(dd, OD_SET_QUANT, &video_q, sizeof(int));
  /*Write the bitstream header packets with proper page interleave.*/
  /*The first packet for each logical stream will get its own page
     automatically.*/
  if(daala_encode_flush_header(dd,&dc,&op)<=0){
    fprintf(stderr,"Internal Daala library error.\n");
    exit(1);
  }
  ogg_stream_packetin(&vo,&op);
  if(ogg_stream_pageout(&vo,&og)!=1){
    fprintf(stderr,"Internal Ogg library error.\n");
    exit(1);
  }
  if(fwrite(og.header,1,og.header_len,outfile)<(size_t)og.header_len){
    fprintf(stderr,"Could not complete write to file.\n");
    exit(1);
  }
  if(fwrite(og.body,1,og.body_len,outfile)<(size_t)og.body_len){
    fprintf(stderr,"Could not complete write to file.\n");
    exit(1);
  }
  /*Create and buffer the remaining Daala headers.*/
  for(;;){
    ret=daala_encode_flush_header(dd,&dc,&op);
    if(ret<0){
      fprintf(stderr,"Internal Daala library error.\n");
      exit(1);
    }
    else if(!ret)break;
    ogg_stream_packetin(&vo,&op);
  }
  for(;;){
    ret=ogg_stream_flush(&vo,&og);
    if(ret<0){
      fprintf(stderr,"Internal Ogg library error.\n");
      exit(1);
    }
    else if(!ret)break;
    if(fwrite(og.header,1,og.header_len,outfile)<(size_t)og.header_len){
      fprintf(stderr,"Could not write header to file.\n");
      exit(1);
    }
    if(fwrite(og.body,1,og.body_len,outfile)<(size_t)og.body_len){
      fprintf(stderr,"Could not write body to file.\n");
      exit(1);
    }
  }
  /*Setup complete.
     Main compression loop.*/
  fprintf(stderr,"Compressing...\n");
  video_ready=0;
  for(;;){
    ogg_page video_page;
    double   video_time;
    size_t bytes_written;
    video_ready=fetch_and_process_video(&avin,&video_page,
     &vo,dd,video_ready,limit>=0 ? &limit : NULL);
    /*TODO: Fetch the next video page.*/
    /*If no more pages are available, we've hit the end of the stream.*/
    if(!video_ready)break;
    video_time=daala_granule_time(dd,ogg_page_granulepos(&video_page));
    bytes_written=fwrite(video_page.header,1,video_page.header_len,outfile);
    if(bytes_written<(size_t)video_page.header_len){
      fprintf(stderr,"Could not write page header to file.\n");
      exit(1);
    }
    video_bytesout+=bytes_written;
    bytes_written=fwrite(video_page.body,1,video_page.body_len,outfile);
    if(bytes_written<(size_t)video_page.body_len){
      fprintf(stderr,"Could not write page body to file.\n");
      exit(1);
    }
    video_bytesout+=bytes_written;
    video_ready=0;
    if(video_time==-1)continue;
    video_kbps=(int)rint(video_bytesout*8*0.001/video_time);
    time_base=video_time;
    if (interactive) {
      fprintf(stderr, "\r");
    }
    else {
      fprintf(stderr, "\n");
    }
    fprintf(stderr,
     "     %i:%02i:%02i.%02i video: %ikbps          ",
     (int)time_base/3600,((int)time_base/60)%60,(int)time_base%60,
     (int)(time_base*100-(long)time_base*100),video_kbps);
  }
  ogg_stream_clear(&vo);
  daala_encode_free(dd);
  daala_comment_clear(&dc);
  for(pli=0;pli<avin.video_img.nplanes;pli++){
    _ogg_free(avin.video_img.planes[pli].data);
  }
  if(outfile!=NULL&&outfile!=stdout)fclose(outfile);
  fprintf(stderr,"\r    \ndone.\n\r");
  if(avin.video_infile!=NULL&&avin.video_infile!=stdin){
    fclose(avin.video_infile);
  }
  return 0;
}
