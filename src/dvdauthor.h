/*
    Higher-level definitions for building DVD authoring structures
*/
/*
 * Copyright (C) 2002 Scott Smith (trckjunky@users.sourceforge.net)
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

#ifndef __DVDAUTHOR_H_
#define __DVDAUTHOR_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "common.h"

extern int default_video_format; /* defined in dvdcli.c */

typedef enum /* type of menu/title */
  { /* note assigned values cannot be changed */
    VTYPE_VTS = 0, /* title in titleset */
    VTYPE_VTSM = 1, /* menu in titleset */
    VTYPE_VMGM = 2, /* menu in VMG */
  } vtypes;

#define COLOR_UNUSED 0x1000000
  /* special value indicating unused colour-table entry, different from all
    possible 24-bit colour values */

/* types fully defined in da-internal.h */
struct menugroup;
struct pgcgroup;
struct pgc;
struct source;
struct cell;

extern bool delete_output_dir;
  /* whether to delete any existing output directory structure
    before creating a new one */

struct pgc *pgc_new();
void pgc_free(struct pgc *p);
int pgc_add_button(struct pgc *p,const char *name,const char *cmd);
void pgc_add_entry(struct pgc *p, vtypes vtype,const char *entry);
void pgc_add_source(struct pgc *p,struct source *v);
void pgc_set_pre(struct pgc *p,const char *cmd);
void pgc_set_post(struct pgc *p,const char *cmd);
void pgc_set_color(struct pgc *p,int index,int color);
#if 0
void pgc_set_buttongroup(struct pgc *p,int index,unsigned char *map);
#endif
void pgc_set_stilltime(struct pgc *p,int still);
int pgc_set_subpic_stream(struct pgc *p,int ch,const char *m,int id);

/* used to indicate which video/audio/subpicture attribute is being set to a particular
  keyword value, or xxx_ANY (= 0) to match whichever one I can */
enum { VIDEO_ANY=0, VIDEO_MPEG, VIDEO_FORMAT, VIDEO_ASPECT, VIDEO_RESOLUTION, VIDEO_WIDESCREEN, VIDEO_FRAMERATE, VIDEO_CAPTION };
enum { AUDIO_ANY=0, AUDIO_FORMAT, AUDIO_QUANT, AUDIO_DOLBY, AUDIO_LANG, AUDIO_CHANNELS, AUDIO_SAMPLERATE, AUDIO_CONTENT };
enum { SPU_ANY=0, SPU_LANG, SPU_CONTENT };

struct pgcgroup *pgcgroup_new(vtypes type);
void pgcgroup_free(struct pgcgroup *pg);
void pgcgroup_add_pgc(struct pgcgroup *ps,struct pgc *p);
int pgcgroup_set_video_attr(struct pgcgroup *va,int attr,const char *s);
int pgcgroup_set_audio_attr(struct pgcgroup *va,int attr,const char *s,int ch);
int pgcgroup_set_subpic_attr(struct pgcgroup *va,int attr,const char *s,int ch);
int pgcgroup_set_subpic_stream(struct pgcgroup *va,int ch,const char *m,int id);

struct menugroup *menugroup_new();
void menugroup_free(struct menugroup *mg);
void menugroup_add_pgcgroup(struct menugroup *mg,const char *lang,struct pgcgroup *pg);
int menugroup_set_video_attr(struct menugroup *va,int attr,const char *s);
int menugroup_set_audio_attr(struct menugroup *va,int attr,const char *s,int ch);
int menugroup_set_subpic_attr(struct menugroup *va,int attr,const char *s,int ch);
int menugroup_set_subpic_stream(struct menugroup *va,int ch,const char *m,int id);

struct source *source_new();
int source_add_cell(struct source *v,double starttime,double endtime,cell_chapter_types chap,int pause,const char *cmd);
void source_set_filename(struct source *v,const char *s);

void dvdauthor_enable_jumppad();
void dvdauthor_enable_allgprm();
void dvdauthor_vts_gen(struct menugroup *menus,struct pgcgroup *titles,const char *fbase);
void dvdauthor_vmgm_gen(struct pgc *fpc,struct menugroup *menus,const char *fbase);

#ifdef __cplusplus
}
#endif


#endif
