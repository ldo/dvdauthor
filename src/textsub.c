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
unsigned char *textsub_image_buffer;

bool textsub_init
  (
    const char * textsub_filename
  )
  /* loads subtitles from textsub_filename and sets up structures for rendering
    the text. */
  {
    const size_t image_buffer_size =
        sizeof(uint8_t) * 3 * movie_height * movie_width;
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

void textsub_render(const subtitle_elt * sub)
  /* does the actual rendering of a previously-loaded subtitle. */
  {
    memset(textsub_image_buffer, 128, sizeof(uint8_t) * 3 * movie_height * movie_width);
      /* fill with transparent colour, which happens to be 50% grey */
    vo_update_osd(sub);
  } /*textsub_render*/

void textsub_finish()
  {
    vo_finish_osd();
    free(textsub_image_buffer);
  } /*textsub_finish*/
