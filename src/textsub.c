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



#include "subconfig.h"
#include "subglobals.h"
#include "subreader.h"
#include "subrender.h"
#include "subfont.h"
#include "textsub.h"


static const char RCSID[]="$Id: //depot/dvdauthor/src/textsub.c#6 $";

char* dvdsub_lang="";
/* dvdsub_lang (char) indicates subtitle language (user parameter) */
float sub_delay=0.0;
/* sub_delay (float) contains delay for subtitles in 10 msec intervals (optional user parameter, 0.0 for no delay)*/
float sub_fps=0.0;
/* sub_fps (float)contains subtitle frames per second, only applicable when we have no timed subs (detection from
   video stream, 0.0 for setting taken over from fps otherwise subtitle fps)*/
int suboverlap_enabled=1;
/* suboverlap_enabled (int) indicates overlap if the user forced it (suboverlap_enabled == 2) or
   the user didn't forced no-overlapsub and the format is Jacosub or Ssa.
   this is because usually overlapping subtitles are found in these formats,
   while in others they are probably result of bad timing (set by subtile file type if initialized on 1)*/
int sub_utf8=0;
/* sub_utf8 (int) is a flag which indicates the characterset encoding: 0=initial 1=utf8 dictated by filename
   extension ".utf", ".utf8" or "utf-8" or 2: set by sub_cp being validated is a valid ICONV "from character-set"
   (set by filename or valid sub_cp when initialized on 0) */
float font_factor=0.75;
int verbose=0;
float osd_font_scale_factor = 6.0;
float subtitle_font_radius = 0.0;     /*2.0*/
float subtitle_font_thickness = 3.0;  /*2.0*/
int subtitle_autoscale = 0; /* 0=no autoscale 1=video height 2=video width 3=diagonal */
int sub_bg_color=8; /* subtitles background color */
int sub_bg_alpha=0;
int sub_justify=1;

/*-----------------25-11-03 1:29--------------------
 * Start of minimum set of variables that should be user configurable
 * --------------------------------------------------*/
char* sub_cp="ISO8859-1";    /* sub_cp (char) contains "from character-set" for ICONV like ISO8859-1 and UTF-8, */
                          /* "to character-set" is set to UTF-8 (optional user parameter, NULL for non-applicable)*/
float text_font_scale_factor = 28.0; /* font size in font units */
int h_sub_alignment = 1;  /* Horizontal alignmeent 0=center, 1=left, 2=right, 4=subtitle default */
int sub_alignment=2;      /* Vertical alignment 0=top, 1=center, 2=bottom */
int sub_left_margin=60;   /* Size of left horizontal non-display area in pixel units */
int sub_right_margin=60;  /* Size of right horizontal non-display area in pixel units */
int sub_bottom_margin=30; /* Size of bottom horizontal non-display area in pixel units */
int sub_top_margin=20;    /* Size of top horizontal non-display area in pixel units */
char *sub_font="arial.ttf"; /* Name of true type font, windows OS apps will look in \windows\fonts others in home dir */
/*-----------------25-11-03 1:31--------------------
 * End of mimum set of variables that should be user configurable
 * --------------------------------------------------*/

float movie_fps=25.0;
int movie_width=720;
int movie_height=574;
sub_data *textsub_subdata;
subtitle *textsub_subs;
subtitle *vo_sub;
unsigned char *image_buffer;
char *font_name;
int current_sub;
font_desc_t* vo_font;
char *filename=NULL;
int sub_max_chars;
int sub_max_lines;
int sub_max_font_height;
int sub_max_bottom_font_height;
int sub_last;
int sub_num_of_subtitles;
char *img_name;

sub_data * textsub_init(char *textsub_filename, float textsub_movie_fps, float textsub_movie_width, float textsub_movie_height)
{
  vo_sub=NULL;
  font_name=NULL;
  current_sub=-1;
  vo_font=NULL;
  sub_last=1;
  sub_max_chars=0;
  sub_max_lines=0;
  sub_max_font_height=0;
  sub_max_bottom_font_height=0;
  sub_num_of_subtitles=0;
  movie_fps=textsub_movie_fps;
  movie_width=textsub_movie_width;
  movie_height=textsub_movie_height;
#ifdef HAVE_FREETYPE
  if (!vo_font)
	init_freetype();
#endif
  vo_init_osd();
#ifdef ICONV
  if (sub_cp)
    if (!strcmp(sub_cp,""))
      sub_cp=NULL;
#endif
  if (dvdsub_lang)
    if (!strcmp(dvdsub_lang,""))
      dvdsub_lang=NULL;
  image_buffer=malloc(sizeof(u_int8_t)*3*textsub_movie_height*textsub_movie_width*3);
  memset(image_buffer,128,sizeof(u_int8_t)*3*textsub_movie_height*textsub_movie_width*3);
  if ( image_buffer==NULL)
  {
    fprintf(stderr,"ERR: Failed to allocate memory\n");
	exit(1);
  }
  textsub_subdata=sub_read_file(textsub_filename,textsub_movie_fps);
  vo_update_osd(textsub_movie_width,textsub_movie_height);
  vo_osd_changed(OSDTYPE_SUBTITLE);
  if (textsub_subdata!=NULL)
    textsub_subs=textsub_subdata->subtitles;
  return(textsub_subdata);
}

void textsub_dump_file()
{
  list_sub_file(textsub_subdata);
}

textsub_subtitle_type *textsub_find_sub(unsigned long text_sub_pts)
{
  textsub_subtitle_type *tsub;

  tsub=(textsub_subtitle_type*)malloc(sizeof(textsub_subtitle_type));
  tsub->valid=0;
  tsub->start=-1;
  find_sub(textsub_subdata,text_sub_pts);
  if ( (vo_sub)&& (current_sub!=sub_last))
  {
    if ( h_sub_alignment!=SUB_ALIGNMENT_DEFAULT)
	{
	  vo_sub->alignment=h_sub_alignment;
	}
	sub_num_of_subtitles++;
	sub_last=current_sub;
	tsub->start=vo_sub->start;
	tsub->end=vo_sub->end;
	tsub->valid=1;
  }
  return (tsub);
}

/* extern char *draw_image(int p_w, int p_h, unsigned char* p_planes,unsigned int p_stride); */

void textsub_render(subtitle* sub)
{
  vo_sub=sub;
  vo_osd_changed(OSDTYPE_SUBTITLE);
  memset(image_buffer,128,sizeof(u_int8_t)*3*movie_height*movie_width*3);
  vo_update_osd(movie_width,movie_height);
/*  draw_image(movie_width,movie_height,image_buffer,movie_width*3); */
}

void textsub_statistics()
{
  fprintf(stderr,"\nStatistics:\n");
  fprintf(stderr,"- Processed %d subtitles.\n",sub_num_of_subtitles);
  fprintf(stderr,"- The longest display line had %d characters.\n",sub_max_chars-1);
  fprintf(stderr,"- The maximum number of displayed lines was %d.\n",sub_max_lines);
  fprintf(stderr,"- The normal display height of the font %s was %d.\n",sub_font,sub_max_font_height);
  fprintf(stderr,"- The bottom display height of the font %s was %d.\n",sub_font,sub_max_bottom_font_height);
  fprintf(stderr,"- The biggest subtitle box had %d bytes.\n",max_sub_size);
}

void textsub_finish()
{
  free(image_buffer);
#ifdef HAVE_FREETYPE
  if (vo_font) free_font_desc(vo_font);
    vo_font = NULL;
  done_freetype();
#endif
}

#ifdef TEXTSUB_DEBUG

static int framenum = 0;

struct pngdata {
	FILE * fp;
	png_structp png_ptr;
	png_infop info_ptr;
	enum {OK,ERROR} status;
};

static struct pngdata create_png (char * fname, int image_width, int image_height, int swapped)
{
    struct pngdata png;

    /*png_structp png_ptr = png_create_write_struct
       (PNG_LIBPNG_VER_STRING, (png_voidp)user_error_ptr,
        user_error_fn, user_warning_fn);*/
    //png_byte *row_pointers[image_height];
    png.png_ptr = png_create_write_struct
       (PNG_LIBPNG_VER_STRING, NULL,
        NULL, NULL);
    png.info_ptr = png_create_info_struct(png.png_ptr);

    if (!png.png_ptr) {
       if(verbose > 1) fprintf(stderr,"ERR: PNG Failed to init png pointer\n");
       png.status = ERROR;
       return png;
    }

    if (!png.info_ptr) {
       if(verbose > 1) fprintf(stderr,"ERR: PNG Failed to init png infopointer\n");
       png_destroy_write_struct(&png.png_ptr,
         (png_infopp)NULL);
       png.status = ERROR;
       return png;
    }

    if (setjmp(png.png_ptr->jmpbuf)) {
	if(verbose > 1) fprintf(stderr,"ERR: PNG Internal error!\n");
        png_destroy_write_struct(&png.png_ptr, &png.info_ptr);
        fclose(png.fp);
        png.status = ERROR;
        return png;
    }

    png.fp = fopen (fname, "wb");
    if (png.fp == NULL) {
	fprintf(stderr,"ERR: PNG Error opening %s for writing!\n", strerror(errno));
       	png.status = ERROR;
       	return png;
    }

    if(verbose > 1) fprintf(stderr,"INFO: PNG Init IO\n");
    png_init_io(png.png_ptr, png.fp);

    /* set the zlib compression level */
    png_set_compression_level(png.png_ptr, 0);


    /*png_set_IHDR(png_ptr, info_ptr, width, height,
       bit_depth, color_type, interlace_type,
       compression_type, filter_type)*/
    png_set_IHDR(png.png_ptr, png.info_ptr, image_width, image_height,
       8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
       PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

    if(verbose > 1) fprintf(stderr,"INFO: PNG Write Info\n");
    png_write_info(png.png_ptr, png.info_ptr);

    if(swapped) {
    	if(verbose > 1) fprintf(stderr,"INFO: PNG Set BGR Conversion\n");
    	png_set_bgr(png.png_ptr);
    }

    png.status = OK;
    return png;
}

static u_int8_t destroy_png(struct pngdata png) {

    if(verbose > 1) fprintf(stderr,"INFO: PNG Write End\n");
    png_write_end(png.png_ptr, png.info_ptr);

    if(verbose > 1) fprintf("INFO: PNG Destroy Write Struct\n");
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
	    fprintf(stderr,"ERR: PNG Error in create_png\n");
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
  subtitle *last_sub;
  textsub_subtitle_type *textsub_subtitle;
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
  if(textsub_init(filename,movie_fps,movie_width,movie_height)==NULL)
  {
    fprintf(stderr,"ERR: Couldn't load file %s.\n",filename);
    exit(1);
  }
  if (argc==3)
    textsub_dump_file();
  last_sub=(&textsub_subs[textsub_subdata->sub_num-1]);
  for ( pts=0;pts<last_sub->end;pts++)
  {
	textsub_subtitle=textsub_find_sub(pts);
	if ( textsub_subtitle->image!=NULL)
	  if (draw_image(movie_width,movie_height,textsub_subtitle->image,movie_width*3)!=NULL)
	  {
	    st=malloc(sizeof(stinfo));
        memset(st,0,sizeof(stinfo));
	    strcpy(st->img.fname,img_name);
	    st->spts=textsub_subtitle->start;
	    st->sd=(textsub_subtitle->end)-(textsub_subtitle->start);
	    spus=realloc(spus,(numspus+1)*sizeof(stinfo *));
        spus[numspus++]=st;
	  }
  }
  textsub_finish();
  textsub_statistics();
  exit(0);
}
#endif
