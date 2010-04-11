/*
    Top-level loading and rendering of subtitle files for spumux
*/
/* Copyright (C) 2003 Sjef van Gool (svangool@hotmail.com)
 *
 * This module uses various parts of the MPLAYER project (http://www.mplayerhq.hu)
 * Copyright (C) 2000 - 2003 various authors of the MPLAYER project
 * With many changes by Sjef van Gool (svangool@hotmail.com) November 2003
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307
 * USA
 */

/*
 * vo_png.c, Portable Network Graphics Renderer for Mplayer
 *
 * Copyright 2001 by Felix Buenemann <atmosfear@users.sourceforge.net>
 *
 * Uses libpng (which uses zlib), so see according licenses.
 *
 */

#include "config.h"

#include "compat.h"

#undef TEXTSUB_DEBUG

#ifdef TEXTSUB_DEBUG
#include "subparsexml.h"
#include <png.h>
#include <errno.h>
#endif



#include "subglobals.h"
#include "subreader.h"
#include "subrender.h"
#include "subfont.h"
#include "textsub.h"



float sub_delay=0.0;
/* sub_delay (float) contains delay for subtitles in 10 msec intervals (optional user parameter, 0.0 for no delay)*/
float sub_fps=0.0;
/* sub_fps (float)contains subtitle frames per second, only applicable when we have no timed subs (detection from
   video stream, 0.0 for setting taken over from fps otherwise subtitle fps)*/
int suboverlap_enabled=1;
/* suboverlap_enabled (int) indicates overlap if the user forced it (suboverlap_enabled == 2) or
   the user didn't forced no-overlapsub and the format is Jacosub or Ssa.
   this is because usually overlapping subtitles are found in these formats,
   while in others they are probably result of bad timing (set by subtile file type if
   initialized on 1) */
 /* never set to any other value */
int sub_utf8 = 0;
/* sub_utf8 (int) is a flag which indicates the characterset encoding: 0=initial 1=utf8
  dictated by filename extension ".utf", ".utf8" or "utf-8" or 2: set subtitle_charset being
  validated is a valid ICONV "from character-set" (set by filename or valid subtitle_charset
  when initialized on 0) */
float font_factor=0.75;
int verbose=0;
float subtitle_font_thickness = 3.0;  /*2.0*/
int subtitle_autoscale = AUTOSCALE_NONE;
int sub_bg_color=8; /* subtitles background color */
int sub_bg_alpha=0;
int sub_justify=1;

/*-----------------25-11-03 1:29--------------------
 * Start of minimum set of variables that should be user configurable
 *
 * 23-04-05   Added the text_forceit default value (by Pierre Dumuid)
 * --------------------------------------------------*/
char* subtitle_charset = NULL;
  /* subtitle_charset (char) contains "from character-set" for ICONV like ISO8859-1 and UTF-8, */
  /* "to character-set" is set to UTF-8. If not specified, then defaults to locale */
float text_font_scale_factor = 28.0; /* font size in font units */
bool text_forceit = false;     /* Forcing of the subtitles */
int h_sub_alignment = H_SUB_ALIGNMENT_LEFT;  /* Horizontal alignment 0=center, 1=left, 2=right, 4=subtitle default */
int v_sub_alignment = V_SUB_ALIGNMENT_BOTTOM;      /* Vertical alignment 0=top, 1=center, 2=bottom */
int sub_left_margin=60;   /* Size of left horizontal non-display area in pixel units */
int sub_right_margin=60;  /* Size of right horizontal non-display area in pixel units */
int sub_bottom_margin=30; /* Size of bottom horizontal non-display area in pixel units */
int sub_top_margin=20;    /* Size of top horizontal non-display area in pixel units */
char *sub_font = /* Name of true type font, windows OS apps will look in \windows\fonts others in home dir */
#if HAVE_FONTCONFIG
    "arial"
#else
    "arial.ttf"
#endif
;
/*-----------------25-11-03 1:31--------------------
 * End of mimum set of variables that should be user configurable
 * --------------------------------------------------*/

float movie_fps=25.0; /* fixme: should perhaps depend on video format */
int movie_width=720;
int movie_height=574; /* fixme: should perhaps depend on video format */
sub_data *textsub_subdata;
subtitle_elt *vo_sub;
unsigned char *textsub_image_buffer;
int current_sub;

static int sub_last;
int sub_num_of_subtitles;

bool textsub_init
  (
    const char * textsub_filename
  )
  /* loads subtitles from textsub_filename and sets up structures for rendering
    the text. */
  {
    const size_t image_buffer_size =
        sizeof(uint8_t) * 3 * movie_height * movie_width;
    vo_sub = NULL;
    current_sub = -1;
    sub_last = 1;
    sub_num_of_subtitles = 0;
    vo_init_osd();
#ifdef ICONV
    if (subtitle_charset && !strcmp(subtitle_charset, ""))
        subtitle_charset = NULL;
#endif
    textsub_image_buffer = malloc(image_buffer_size);
      /* fixme: not freed from previous call! */
    if (textsub_image_buffer == NULL)
     {
        fprintf(stderr, "ERR:  Failed to allocate memory\n");
        exit(1);
      } /*if*/
    textsub_subdata = sub_read_file(textsub_filename, movie_fps);
      /* fixme: sub_free never called! */
    return textsub_subdata != NULL;
  } /*textsub_init*/

void textsub_dump_file()
  /* not used anywhere (except in obsolete code below). */
  {
    list_sub_file(textsub_subdata);
  } /*textsub_dump_file*/

textsub_subtitle_type textsub_find_sub(unsigned long text_sub_pts)
  /* looks for the subtitle entry covering the specified time, returning it
    if it exists and is not the same as was returned for the previous call.
    Kind of a roundabout way of finding durations of successive subtitles. */
  {
    textsub_subtitle_type result;
    result.valid = 0;
    result.start = -1;
    result.end = -1;
    find_sub(textsub_subdata, text_sub_pts);
    if (vo_sub && current_sub != sub_last)
      {
        sub_num_of_subtitles++;
        sub_last = current_sub;
        result.start = vo_sub->start;
        result.end = vo_sub->end;
        result.valid = 1;
      } /*if*/
    return result;
  } /*textsub_find_sub*/

/* extern char *draw_image(int p_w, int p_h, unsigned char* p_planes,unsigned int p_stride); */

void textsub_render(subtitle_elt * sub)
  /* does the actual rendering of a previously-loaded subtitle. */
  {
    vo_sub = sub;
    memset(textsub_image_buffer, 128, sizeof(uint8_t) * 3 * movie_height * movie_width);
      /* fill with default transparent colour */
    vo_update_osd();
/*  draw_image(movie_width, movie_height, textsub_image_buffer, movie_width * 3); */
  } /*textsub_render*/

void textsub_finish()
  {
    vo_finish_osd();
    free(textsub_image_buffer);
  } /*textsub_finish*/

#ifdef TEXTSUB_DEBUG
/* test program to save text rendering to a PNG file */
/* this probably will not compile any more */

static int framenum = 0;
static char *img_name;

struct pngdata {
    FILE * fp;
    png_structp png_ptr;
    png_infop info_ptr;
    enum {OK,ERROR} status;
};

static struct pngdata create_png
  (
    const char * fname,
    int image_width,
    int image_height,
    int swapped
  )
  /* creates and opens a PNG file and returns a structure ready for writing its contents. */
  {
    struct pngdata png;

    //png_byte *row_pointers[image_height];
    png.png_ptr = png_create_write_struct
      (
        /*user_png_ver =*/ PNG_LIBPNG_VER_STRING,
        /*error_ptr =*/ NULL,
        /*error_fn =*/ NULL,
        /*warn_fn =*/ NULL
      );
    png.info_ptr = png_create_info_struct(png.png_ptr);
    if (!png.png_ptr) {
        if(verbose > 1)
            fprintf(stderr,"ERR:  PNG Failed to init png pointer\n");
        png.status = ERROR;
        return png;
    }
    if (!png.info_ptr) {
        if(verbose > 1)
            fprintf(stderr,"ERR:  PNG Failed to init png infopointer\n");
        png_destroy_write_struct
          (
            /*png_ptr_ptr =*/ &png.png_ptr,
            /*info_ptr_ptr -*/ (png_infopp)NULL
          );
        png.status = ERROR;
        return png;
    }
    if (setjmp(png.png_ptr->jmpbuf)) {
        if(verbose > 1)
            fprintf(stderr,"ERR:  PNG Internal error!\n");
        png_destroy_write_struct(&png.png_ptr, &png.info_ptr);
        fclose(png.fp);
        png.status = ERROR;
        return png;
    }

    png.fp = fopen(fname, "wb");
    if (png.fp == NULL) {
        fprintf(stderr,"ERR:  PNG Error opening %s for writing!\n", strerror(errno));
        png.status = ERROR;
        return png;
    }

    if(verbose > 1)
        fprintf(stderr,"INFO: PNG Init IO\n");
    png_init_io(png.png_ptr, png.fp);

    /* set the zlib compression level */
    png_set_compression_level(png.png_ptr, 0);

    png_set_IHDR
      (
        /*png_ptr =*/ png.png_ptr,
        /*info_ptr =*/ png.info_ptr,
        /*width =*/ image_width,
        /*height =*/ image_height,
        /*bit_depth =*/ 8,
        /*color_type =*/ PNG_COLOR_TYPE_RGB,
        /*interlace_method =*/ PNG_INTERLACE_NONE,
        /*compression_method =*/ PNG_COMPRESSION_TYPE_DEFAULT,
        /*filter_method =*/ PNG_FILTER_TYPE_DEFAULT
      );

    if(verbose > 1)
        fprintf(stderr,"INFO: PNG Write Info\n");
    png_write_info(png.png_ptr, png.info_ptr);

    if(swapped) {
        if(verbose > 1)
            fprintf(stderr,"INFO: PNG Set BGR Conversion\n");
        png_set_bgr(png.png_ptr);
    }

    png.status = OK;
    return png;
  } /*pngdata create_png*/

static uint8_t destroy_png(struct pngdata png) {

    if(verbose > 1) fprintf(stderr,"INFO: PNG Write End\n");
    png_write_end(png.png_ptr, png.info_ptr);

    if(verbose > 1) fprintf(stderr,"INFO: PNG Destroy Write Struct\n");
    png_destroy_write_struct(&png.png_ptr, &png.info_ptr);

    fclose (png.fp);

    return 0;
}

char *draw_image(int p_w, int p_h, unsigned char* p_planes,unsigned int p_stride)
{
    char buf[100];
    int k;
    struct pngdata png;
    png_byte *row_pointers[p_h];

    img_name=NULL;
    snprintf (buf, 100, "%08d.png", ++framenum);

    png = create_png(buf, p_w, p_h, 0);

    if(png.status){
        fprintf(stderr,"ERR:  PNG Error in create_png\n");
        return NULL;
    }

    if(verbose > 1) fprintf(stderr,"INFO: PNG Creating Row Pointers\n");
    for ( k = 0; k < p_h; k++ )
    row_pointers[k] = p_planes+p_stride*k;

    //png_write_flush(png.png_ptr);
    //png_set_flush(png.png_ptr, nrows);

    if(verbose > 1) fprintf(stderr,"INFO: PNG Writing Image Data\n");
    png_write_image(png.png_ptr, row_pointers);

    destroy_png(png);
    img_name=malloc(100);
    strcpy(img_name,buf);
    return img_name;
}
int main(int argc, char **argv)
{
  typedef struct
  {
    unsigned char r, g, b, t;
  } palt;

  typedef struct {
    unsigned char fname[256];
    unsigned char *img;
    palt pal[256];
    int numpal,width,height;
  } pict;

  typedef struct {
    int x0,y0,x1,y1;
  } rectangle;

  typedef struct {
    char *name;
    int autoaction;
    rectangle r;
    char *up,*down,*left,*right;
    int grp;
  } button;

  typedef struct {
    unsigned int x0, y0, xd, yd; // x0,y0 -- start, xd,yd -- dimension (size)
    int spts, sd, forced, numbuttons, numpal; // start pts, subtitle duration
    int autooutline,outlinewidth,autoorder;
    pict img,hlt,sel;
    unsigned char *fimg;
    palt pal[4],masterpal[16],transparentc;
    int numgroups,groupmap[3][4];
    button *buttons;
  } stinfo;

  unsigned long pts=0;
  subtitle_elt *last_sub;
  textsub_subtitle_type textsub_subtitle;
  stinfo **spus=NULL;
  stinfo *st=NULL;
  int numspus=0;

  if(argc<2)
  {
    fprintf(stderr,"\nUsage: textsub filename.sub [Dump]\n");
    exit(1);
  }
  if (textsub_parse(argv[1]))
  {
    exit(1);
  }
  if(!textsub_init(filename,movie_fps,movie_width,movie_height))
  {
    fprintf(stderr,"ERR:  Couldn't load file %s.\n",filename);
    exit(1);
  }
  if (argc==3)
    textsub_dump_file();
  last_sub=(&textsub_subdata->subtitles[textsub_subdata->sub_num-1]);
  for ( pts=0;pts<last_sub->end;pts++)
  {
    textsub_subtitle = textsub_find_sub(pts);
    if ( textsub_subtitle.image!=NULL)
      if (draw_image(movie_width,movie_height,textsub_subtitle.image,movie_width*3)!=NULL)
      {
        st=malloc(sizeof(stinfo));
        memset(st,0,sizeof(stinfo));
        strcpy(st->img.fname,img_name);
        st->spts=textsub_subtitle.start;
        st->sd=(textsub_subtitle.end)-(textsub_subtitle.start);
        spus=realloc(spus,(numspus+1)*sizeof(stinfo *));
        spus[numspus++]=st;
      }
  }
  textsub_finish();
  textsub_statistics();
  exit(0);
}
#endif
