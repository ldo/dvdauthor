/*
 * Copyright (C) 2003 Scott Smith (trckjunky@users.sourceforge.net)
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

#include <assert.h>
#include <ctype.h>

#include "subgen.h"
#include "readxml.h"
#include "textsub.h"

static const char RCSID[]="$Id: //depot/dvdauthor/src/subgen-parse-xml.c#14 $";

static void printtime(char *b,int t)
{
    sprintf(b,"%d:%02d:%02d.%03d",
            (t/90/1000/60/60),
            (t/90/1000/60)%60,
            (t/90/1000)%60,
            (t/90)%1000);
}

static unsigned int parsetime(char *t)
{
    int tf=1;
    int rt=0,n=0,nd=0;

    while(*t) {
        if(isdigit(*t)) {
            if( nd<10000 ) {
                n=n*10+t[0]-'0';
                nd*=10;
            }
        } else if( *t == ':' ) {
            assert(tf);
            rt=rt*60+n;
            n=0;
            nd=1;
        } else if( *t == '.' || *t == ',' ) {
            assert(tf);
            rt=rt*60+n;
            n=0;
            nd=1;
            tf=0;
        }
        t++;
    }
    if( tf )
        return (rt*60+n)*90000;
    else
        return rt*90000+90000*n/nd;
}

#if 0
#endif




static int had_stream=0;
static stinfo *st=0;
static button *curbutton=0;

void stream_begin()
{
    if(had_stream) {
        fprintf(stderr,"ERR:  Only one stream is currently allowed.\n");
        exit(1);
    }
    had_stream=1;
}

void spu_begin()
{
    st=malloc(sizeof(stinfo));
    memset(st,0,sizeof(stinfo));
}

void spu_image(char *v)        { st->img.fname=utf8tolocal(v); }
void spu_highlight(char *v)    { st->hlt.fname=utf8tolocal(v); }
void spu_select(char *v)       { st->sel.fname=utf8tolocal(v); }
void spu_start(char *v)        { st->spts         = parsetime(v); }
void spu_end(char *v)          { st->sd           = parsetime(v); }
void spu_outlinewidth(char *v) { st->outlinewidth = atoi(v);      }
void spu_xoffset(char *v)      { st->x0 = atoi(v);                }
void spu_yoffset(char *v)      { st->y0 = atoi(v);                }

void spu_force(char *v)
{
    st->forced = xml_ison(v);
    if( st->forced==-1 ) {
        fprintf(stderr,"ERR:  Cannot parse 'force' value '%s'\n",v);
        exit(1);
    }
}

void spu_transparent(char *v)
{
    int c=0;

    sscanf(v,"%x",&c);
    st->transparentc.r=c>>16;
    st->transparentc.g=c>>8;
    st->transparentc.b=c;
    st->transparentc.t=255;
}

void spu_autooutline(char *v)
{
    if( !strcmp(v,"infer") )
        st->autooutline=1;
    else {
        fprintf(stderr,"ERR:  Unknown autooutline type %s\n",v);
        exit(1);
    }
}

void spu_autoorder(char *v)
{
    if(!strcmp(v,"rows"))
        st->autoorder=0;
    else if(!strcmp(v,"columns"))
        st->autoorder=1;
    else {
        fprintf(stderr,"ERR:  Unknown autoorder type %s\n",v);
        exit(1);
    }
}

void spu_complete()
{
    if (!st->sd)
        st->sd = -1;
    else
    {
        if (st->sd <= st->spts)
        {
            char stime[50],etime[50];
            printtime(stime,st->spts);
            printtime(etime,st->sd);
            fprintf(stderr, "ERR:  sub has end (%s)<=start (%s), skipping\n",etime,stime);
            skip++;
            return;
        }
        st->sd -= st->spts;
    }
    spus=realloc(spus,(numspus+1)*sizeof(stinfo *));
    spus[numspus++]=st;
    st=0;
}

void button_begin()
{
    st->buttons=realloc(st->buttons,(st->numbuttons+1)*sizeof(button));
    curbutton=&st->buttons[st->numbuttons++];
    memset(curbutton,0,sizeof(button));
    curbutton->r.x0=-1;
    curbutton->r.y0=-1;
    curbutton->r.x1=-1;
    curbutton->r.y1=-1;
}

void action_begin()
{
    button_begin();
    curbutton->autoaction=1;
}

void button_label(char *v) { curbutton->name  = strdup(v); }
void button_up(char *v)    { curbutton->up    = strdup(v); }
void button_down(char *v)  { curbutton->down  = strdup(v); }
void button_left(char *v)  { curbutton->left  = strdup(v); }
void button_right(char *v) { curbutton->right = strdup(v); }
void button_x0(char *v)    { curbutton->r.x0  = atoi(v);   }
void button_y0(char *v)    { curbutton->r.y0  = atoi(v);   }
void button_x1(char *v)    { curbutton->r.x1  = atoi(v);   }
void button_y1(char *v)    { curbutton->r.y1  = atoi(v);   }

void textsub_filename(char *v)
{
    filename=utf8tolocal(v);
}

void textsub_characterset(char *v)
{
#ifdef HAVE_ICONV
    sub_cp=strdup(v);
#endif
}

void textsub_h_alignment(char *v)
{
    if(!strcmp(v,"left"))
        h_sub_alignment=1;
    else if(!strcmp(v,"right"))
        h_sub_alignment=2;
    else if(!strcmp(v,"center"))
        h_sub_alignment=0;
	else if(!strcmp(v,"default"))
        h_sub_alignment=4;

    else {
        fprintf(stderr,"ERR:  Unknown horizontal-alignment type %s\n",v);
        exit(1);}
}

void textsub_v_alignment(char *v)
{
    if(!strcmp(v,"top"))
        sub_alignment=0;
    else if(!strcmp(v,"center"))
        sub_alignment=1;
    else if(!strcmp(v,"bottom"))
        sub_alignment=2;
	else {
        fprintf(stderr,"ERR:  Unknown vertical-alignment type %s\n",v);
        exit(1);}
}

void textsub_complete()
{
    unsigned long pts=0;
    subtitle *last_sub;
    textsub_subtitle_type *textsub_subtitle;

    if (filename==NULL)
    {
        fprintf(stderr,"ERR: Filename of subtitle file missing");
        exit(1);
    }
    else
    {
        if(textsub_init(filename,movie_fps,movie_width,movie_height)==NULL)
        {
            fprintf(stderr,"ERR: Couldn't load file %s.\n",filename);
            exit(1);
        }
        have_textsub=1;
        last_sub=(&textsub_subs[textsub_subdata->sub_num-1]);
        for ( pts=0;pts<last_sub->end;pts++)
        {
            textsub_subtitle=textsub_find_sub(pts);
            if (textsub_subtitle->valid)
            {
                st=malloc(sizeof(stinfo));
                memset(st,0,sizeof(stinfo));
                if( !textsub_subdata->sub_uses_time) {
                    st->spts=textsub_subtitle->start*90000.0/movie_fps;
                    st->sd=((textsub_subtitle->end)-(textsub_subtitle->start))*90000.0/movie_fps;
                } else {
                    st->spts=textsub_subtitle->start*900;
                    st->sd=((textsub_subtitle->end)-(textsub_subtitle->start))*900;
                }
                st->sub_title=vo_sub;
                if ( have_transparent)
                {
                    st->transparentc.r=transparent_color>>16;
                    st->transparentc.g=transparent_color>>8;
                    st->transparentc.b=transparent_color;
                    st->transparentc.t=255;
                }
                spus=realloc(spus,(numspus+1)*sizeof(stinfo *));
                spus[numspus++]=st;
            }
        }
    }
}

void textsub_l_margin(char *v)     { sub_left_margin=atoi(v);        }
void textsub_r_margin(char *v)     { sub_right_margin=atoi(v);       }
void textsub_b_margin(char *v)     { sub_bottom_margin=atoi(v);      }
void textsub_t_margin(char *v)     { sub_top_margin=atoi(v);         }
void textsub_font(char *v)         { sub_font=strdup(v);             }
void textsub_sub_fps(char *v)      { sub_fps=atof(v);                }
void textsub_movie_fps(char *v)    { movie_fps=atof(v);              }
void textsub_movie_width(char* v)  { movie_width=atoi(v);            }
void textsub_movie_height(char* v) { movie_height=atoi(v);           }
void textsub_fontsize(char *v)     { text_font_scale_factor=atof(v); }

void textsub_transparent(char *v)
{
    sscanf(v,"%x",&transparent_color);
    have_transparent=1;
}


enum {
    SPU_BEGIN=0,
    SPU_ROOT,
    SPU_STREAM,
    SPU_SPU,
    SPU_NOSUB
};

static struct elemdesc spu_elems[]={
    {"subpictures",SPU_BEGIN,SPU_ROOT,0,0},
    {"stream",SPU_ROOT,SPU_STREAM,stream_begin,0},
    {"spu",SPU_STREAM,SPU_SPU,spu_begin,spu_complete},
    {"button",SPU_SPU,SPU_NOSUB,button_begin,0},
    {"action",SPU_SPU,SPU_NOSUB,action_begin,0},
    {"textsub",SPU_STREAM,SPU_NOSUB,0,textsub_complete},
    {0,0,0,0,0}
};

static struct elemattr spu_attrs[]={
    {"spu","image",spu_image},
    {"spu","highlight",spu_highlight},
    {"spu","select",spu_select},
    {"spu","start",spu_start},
    {"spu","end",spu_end},
    {"spu","transparent",spu_transparent},
    {"spu","autooutline",spu_autooutline},
    {"spu","outlinewidth",spu_outlinewidth},
    {"spu","autoorder",spu_autoorder},
    {"spu","force",spu_force},
    {"spu","xoffset",spu_xoffset},
    {"spu","yoffset",spu_yoffset},
    {"button","name",button_label},
    {"button","up",button_up},
    {"button","down",button_down},
    {"button","left",button_left},
    {"button","right",button_right},
    {"button","x0",button_x0},
    {"button","y0",button_y0},
    {"button","x1",button_x1},
    {"button","y1",button_y1},
    {"action","name",button_label},
    {"textsub","filename",textsub_filename},
    {"textsub","characterset",textsub_characterset},
    {"textsub","fontsize",textsub_fontsize},
    {"textsub","horizontal-alignment",textsub_h_alignment},
    {"textsub","vertical-alignment",textsub_v_alignment},
    {"textsub","left-margin",textsub_l_margin},
    {"textsub","right-margin",textsub_r_margin},
    {"textsub","bottom-margin",textsub_b_margin},
    {"textsub","top-margin",textsub_t_margin},
    {"textsub","font",textsub_font},
    {"textsub","subtitle-fps",textsub_sub_fps},
    {"textsub","movie-fps",textsub_movie_fps},
    {"textsub","movie-width",textsub_movie_width},
    {"textsub","movie-height",textsub_movie_height},
    {"textsub","transparent",textsub_transparent},
    {0,0,0}
};

int spumux_parse(const char *fname)
{
   return readxml(fname,spu_elems,spu_attrs);
}
