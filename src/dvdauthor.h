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

struct menugroup;
struct pgcgroup;
struct pgc;
struct source;
struct cell;

struct pgc *pgc_new();
int pgc_add_button(struct pgc *p,const char *name,const char *cmd);
void pgc_add_entry(struct pgc *p,char *entry);
void pgc_add_source(struct pgc *p,struct source *v);
void pgc_set_pre(struct pgc *p,const char *cmd);
void pgc_set_post(struct pgc *p,const char *cmd);
void pgc_set_color(struct pgc *p,int index,int color);
#if 0
void pgc_set_buttongroup(struct pgc *p,int index,unsigned char *map);
#endif
void pgc_set_stilltime(struct pgc *p,int still);

enum { VIDEO_ANY=0, VIDEO_MPEG, VIDEO_FORMAT, VIDEO_ASPECT, VIDEO_RESOLUTION, VIDEO_WIDESCREEN, VIDEO_FRAMERATE, VIDEO_CAPTION };
enum { AUDIO_ANY=0, AUDIO_FORMAT, AUDIO_QUANT, AUDIO_DOLBY, AUDIO_LANG, AUDIO_CHANNELS, AUDIO_SAMPLERATE };
enum { SPU_ANY=0, SPU_LANG };

struct pgcgroup *pgcgroup_new(int type);
void pgcgroup_add_pgc(struct pgcgroup *ps,struct pgc *p);
int pgcgroup_set_video_attr(struct pgcgroup *va,int attr,char *s);
int pgcgroup_set_audio_attr(struct pgcgroup *va,int attr,char *s,int ch);
int pgcgroup_set_subpic_attr(struct pgcgroup *va,int attr,char *s,int ch);

struct menugroup *menugroup_new();
void menugroup_add_pgcgroup(struct menugroup *mg,char *lang,struct pgcgroup *pg);
int menugroup_set_video_attr(struct menugroup *va,int attr,char *s);
int menugroup_set_audio_attr(struct menugroup *va,int attr,char *s,int ch);
int menugroup_set_subpic_attr(struct menugroup *va,int attr,char *s,int ch);

struct source *source_new();
int source_add_cell(struct source *v,double starttime,double endtime,int chap,int pause,const char *cmd);
void source_set_filename(struct source *v,const char *s);

void dvdauthor_enable_jumppad();
void dvdauthor_vts_gen(struct menugroup *menus,struct pgcgroup *titles,char *fbase);
void dvdauthor_vmgm_gen(struct pgc *fpc,struct menugroup *menus,char *fbase);

#ifdef __cplusplus
}
#endif


#endif
