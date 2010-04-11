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

#include "config.h"
#include "compat.h"

#include "subglobals.h"
#include "subreader.h"
#include "subrender.h"
#include "subfont.h"
#include "textsub.h"

sub_data *textsub_subdata;
unsigned char *textsub_image_buffer;
size_t textsub_image_buffer_size;

bool textsub_init
  (
    const char * textsub_filename
  )
  /* loads subtitles from textsub_filename and sets up structures for rendering
    the text. */
  {
    textsub_image_buffer_size = sizeof(uint8_t) * 3 * movie_height * movie_width;
    vo_init_osd();
#ifdef ICONV
    if (subtitle_charset && !strcmp(subtitle_charset, ""))
        subtitle_charset = NULL;
#endif
    textsub_image_buffer = malloc(textsub_image_buffer_size);
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
    memset(textsub_image_buffer, 128, textsub_image_buffer_size);
      /* fill with transparent colour, which happens to be 50% grey */
    vo_update_osd(sub);
  } /*textsub_render*/

void textsub_finish()
  {
    vo_finish_osd();
    free(textsub_image_buffer);
  } /*textsub_finish*/
