/*
    Parsing of spumux XML control files
*/
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


static void printtime(char *b,int t)
{
    sprintf(b,"%d:%02d:%02d.%03d",
            (t/90/1000/60/60),
            (t/90/1000/60)%60,
            (t/90/1000)%60,
            (t/90)%1000);
}

static unsigned int parsetime(const char *t)
  /* parses a time as [[hh:]mm:]ss[.cc], returning the value in 90kHz clock units. */
  {
    int tf = 1; /* haven't seen decimal point yet */
    int rt = 0; /* accumulation of all componetns except last */
    int n = 0; /* value of last copmonent accumulated here */
    int nd = 0; /* multiplier for next digit of n */
    while (*t)
      {
        if (isdigit(*t))
          {
            if (nd < 10000)
              {
                n = n * 10 + t[0] - '0';
                nd *= 10;
              } /*if*/
          }
        else if (*t == ':')
          {
            assert(tf);
            rt = rt * 60 + n;
            n = 0;
            nd = 1;
          }
        else if (*t == '.' || *t == ',')
          {
          /* on to fractions of a second */
            assert(tf);
            rt = rt * 60 + n;
            n = 0;
            nd = 1;
            tf = 0;
          } /*if*/
        t++;
      } /*while*/
    if (tf)
        return (rt * 60 + n) * 90000;
    else
        return rt * 90000 + 90000 * n / nd;
  } /*parsetime*/

static int
    had_stream = 0, /* whether I've seen <stream> */
    had_textsub = 0; /* whether I've seen <textsub> */
static stinfo *st=0; /* current <spu> directive collected here */
static button *curbutton=0;
static char * filename = 0;

static void stream_begin()
  {
    if (had_stream)
      {
        fprintf(stderr,"ERR:  Only one stream is currently allowed.\n");
        exit(1);
      } /*if*/
    had_stream = 1;
  } /*stream_begin*/

static void spu_begin()
{
    st = malloc(sizeof(stinfo));
    memset(st, 0, sizeof(stinfo));
}

static void spu_image(const char *v)        { st->img.fname=utf8tolocal(v); }
static void spu_highlight(const char *v)    { st->hlt.fname=utf8tolocal(v); }
static void spu_select(const char *v)       { st->sel.fname=utf8tolocal(v); }
static void spu_start(const char *v)        { st->spts         = parsetime(v); }
static void spu_end(const char *v)          { st->sd           = parsetime(v); }
static void spu_outlinewidth(const char *v) { st->outlinewidth = strtounsigned(v, "spu outlinewidth");      }
static void spu_xoffset(const char *v)      { st->x0 = strtounsigned(v, "spu xoffset");                }
static void spu_yoffset(const char *v)      { st->y0 = strtounsigned(v, "spu yoffset");                }

static void spu_force(const char *v)
{
    st->forced = xml_ison(v);
    if (st->forced == -1)
      {
        fprintf(stderr, "ERR:  Cannot parse 'force' value '%s'\n", v);
        exit(1);
      } /*if*/
}

static void spu_transparent(const char *v)
{
    int c = 0;
    sscanf(v, "%x", &c);
    st->transparentc.r = c>>16;
    st->transparentc.g = c>>8;
    st->transparentc.b = c;
    st->transparentc.t = 255;
}

static void spu_autooutline(const char *v)
{
    if (!strcmp(v,"infer"))
        st->autooutline = 1;
    else
      {
        fprintf(stderr, "ERR:  Unknown autooutline type %s\n", v);
        exit(1);
      } /*if*/
}

static void spu_autoorder(const char *v)
{
    if (!strcmp(v,"rows"))
        st->autoorder = 0;
    else if (!strcmp(v,"columns"))
        st->autoorder = 1;
    else
      {
        fprintf(stderr, "ERR:  Unknown autoorder type %s\n", v);
        exit(1);
      } /*if*/
}

static void spu_complete()
{
    if (!st->sd) /* no end time specified */
        st->sd = -1; /* default to indefinite */
    else
      {
        if (st->sd <= st->spts)
          {
            char stime[50], etime[50];
            printtime(stime, st->spts);
            printtime(etime, st->sd);
            fprintf(stderr, "ERR:  sub has end (%s)<=start (%s), skipping\n", etime, stime);
            skip++;
            return;
          } /*if*/
        st->sd -= st->spts;
      } /*if*/
    spus = realloc(spus, (numspus + 1) * sizeof(stinfo *));
    spus[numspus++] = st;
    st = 0;
}

static void button_begin()
{
    st->buttons = realloc(st->buttons, (st->numbuttons + 1) * sizeof(button));
    curbutton = &st->buttons[st->numbuttons++];
    memset(curbutton, 0, sizeof(button));
    curbutton->r.x0 = -1;
    curbutton->r.y0 = -1;
    curbutton->r.x1 = -1;
    curbutton->r.y1 = -1;
}

static void action_begin()
{
    button_begin();
    curbutton->autoaction = 1;
}

static void button_label(const char *v) { curbutton->name  = strdup(v); }
static void button_up(const char *v)    { curbutton->up    = strdup(v); }
static void button_down(const char *v)  { curbutton->down  = strdup(v); }
static void button_left(const char *v)  { curbutton->left  = strdup(v); }
static void button_right(const char *v) { curbutton->right = strdup(v); }
static void button_x0(const char *v)    { curbutton->r.x0  = strtounsigned(v, "button x0");   }
static void button_y0(const char *v)    { curbutton->r.y0  = strtounsigned(v, "button y0");   }
static void button_x1(const char *v)    { curbutton->r.x1  = strtounsigned(v, "button x1");   }
static void button_y1(const char *v)    { curbutton->r.y1  = strtounsigned(v, "button y1");   }

static void textsub_filename(const char *v)
{
    filename = utf8tolocal(v); /* won't leak, because I won't be called more than once */
}

static void textsub_characterset(const char *v)
{
#ifdef HAVE_ICONV
    sub_cp = strdup(v); /* won't leak, because I won't be called more than once */
#else
    fprintf(stderr, "ERR:  <textsub> characterset attribute cannot be interpreted without iconv\n");
    exit(1);
#endif /*HAVE_ICONV*/
}

void textsub_h_alignment(const char *v)
{
    if (!strcmp(v, "left"))
        h_sub_alignment = H_SUB_ALIGNMENT_LEFT;
    else if (!strcmp(v, "right"))
        h_sub_alignment = H_SUB_ALIGNMENT_RIGHT;
    else if (!strcmp(v, "center"))
        h_sub_alignment = H_SUB_ALIGNMENT_CENTER;
    else if (!strcmp(v, "default"))
        h_sub_alignment = H_SUB_ALIGNMENT_DEFAULT;
    else
      {
        fprintf(stderr, "ERR:  Unknown horizontal-alignment type %s\n", v);
        exit(1);
      } /*if*/
}

void textsub_v_alignment(const char *v)
{
    if (!strcmp(v, "top"))
        v_sub_alignment = V_SUB_ALIGNMENT_TOP;
    else if (!strcmp(v, "center"))
        v_sub_alignment = V_SUB_ALIGNMENT_CENTER;
    else if (!strcmp(v, "bottom"))
        v_sub_alignment = V_SUB_ALIGNMENT_BOTTOM;
    else
      {
        fprintf(stderr, "ERR:  Unknown vertical-alignment type %s\n", v);
        exit(1);
      } /*if*/
}

static void textsub_begin()
  {
    if (had_textsub)
      {
        fprintf(stderr,"ERR:  Only one textsub is currently allowed.\n");
        exit(1);
      } /*if*/
    had_textsub = 1;
  } /*textsub_begin*/

static void textsub_complete()
  /* called on a </textsub> tag to load and parse the subtitles. */
  {
    unsigned long pts = 0;
    unsigned long subtitle_end;
    textsub_subtitle_type textsub_subtitle;
    if (filename == NULL)
      {
        fprintf(stderr, "ERR: Filename of subtitle file missing");
        exit(1);
      }
    else
      {
        if (textsub_init(filename, movie_fps, movie_width, movie_height) == NULL)
          {
            fprintf(stderr, "ERR: Couldn't load file %s.\n", filename);
            exit(1);
          } /*if*/
        filename = NULL; /* belongs to textsub_subdata now */
        have_textsub = 1;
        subtitle_end = textsub_subdata->subtitles[textsub_subdata->sub_num - 1].end;
        for (pts = 0; pts < subtitle_end; pts++)
          {
          /* scan the entire duration of the <textsub> tag, and each time a new
            subtitle entry appears, append a description of it to the spus array */
            textsub_subtitle = textsub_find_sub(pts);
            if (textsub_subtitle.valid) /* another subtitle entry appears */
              {
                st = malloc(sizeof(stinfo));
                memset(st, 0, sizeof(stinfo));
                if (!textsub_subdata->sub_uses_time)
                  {
                  /* start and end are in frame numbers */
                    st->spts = textsub_subtitle.start * 90000.0 / movie_fps;
                    st->sd =
                            (textsub_subtitle.end - textsub_subtitle.start)
                        *
                            90000.0
                        /
                            movie_fps;
                  }
                else
                  {
                  /* start and end are in hundredths of a second */
                    st->spts = textsub_subtitle.start * 900;
                    st->sd = (textsub_subtitle.end - textsub_subtitle.start) * 900;
                  } /*if*/
                st->sub_title = vo_sub;
                spus = realloc(spus, (numspus + 1) * sizeof(stinfo *));
                spus[numspus++] = st;
              } /*if*/
          } /*for*/
        free(filename);
        filename = NULL;
      } /*if*/
  } /*textsub_complete*/

static void textsub_l_margin(const char *v)     { sub_left_margin=strtounsigned(v, "textsub left-margin");        }
static void textsub_r_margin(const char *v)     { sub_right_margin=strtounsigned(v, "textsub right-margin");       }
static void textsub_b_margin(const char *v)     { sub_bottom_margin=strtounsigned(v, "textsub bottom-margin");      }
static void textsub_t_margin(const char *v)     { sub_top_margin=strtounsigned(v, "textsub top-margin");         }
static void textsub_font(const char *v)         { sub_font=strdup(v);             }
static void textsub_sub_fps(const char *v)      { sub_fps=atof(v);                }
static void textsub_movie_fps(const char *v)    { movie_fps=atof(v);              }
static void textsub_movie_width(const char* v)  { movie_width=strtounsigned(v, "textsub movie-width");            }
static void textsub_movie_height(const char* v) { movie_height=strtounsigned(v, "textsub movie-height");           }
static void textsub_fontsize(const char *v)     { text_font_scale_factor=atof(v); }

static void textsub_force(const char *v)
{
    text_forceit = xml_ison(v);
    if (text_forceit == -1)
      {
        fprintf(stderr,"ERR:  Cannot parse 'force' value '%s'\n",v);
        exit(1);
      } /*if*/
}

enum { /* parse states */
    SPU_BEGIN=0, /* initial state must be 0 */
    SPU_ROOT, /* expect <stream> */
    SPU_STREAM, /* expect <spu> or <textsub> */
    SPU_SPU, /* within <spu>, expect <button> or <action> */
    SPU_NOSUB /* not expecting subtags */
};

static struct elemdesc spu_elems[]={
    {"subpictures",SPU_BEGIN,SPU_ROOT,0,0},
    {"stream",SPU_ROOT,SPU_STREAM,stream_begin,0},
    {"spu",SPU_STREAM,SPU_SPU,spu_begin,spu_complete},
    {"button",SPU_SPU,SPU_NOSUB,button_begin,0},
    {"action",SPU_SPU,SPU_NOSUB,action_begin,0},
    {"textsub",SPU_STREAM,SPU_NOSUB,textsub_begin,textsub_complete},
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
    {"action","up",button_up},
    {"action","down",button_down},
    {"action","left",button_left},
    {"action","right",button_right},
    {"action","x0",button_x0},
    {"action","y0",button_y0},
    {"action","x1",button_x1},
    {"action","y1",button_y1},
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
    {"textsub","force",textsub_force},
    {0,0,0}
};

int spumux_parse(const char *fname)
{
   return readxml(fname,spu_elems,spu_attrs);
}
