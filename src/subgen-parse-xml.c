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

#include "subglobals.h"
#include "subreader.h"
#include "subgen.h"
#include "readxml.h"


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
    bool tf = true; /* haven't seen decimal point yet */
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
            tf = false;
          } /*if*/
        t++;
      } /*while*/
    if (tf)
        return (rt * 60 + n) * 90000;
    else
        return rt * 90000 + 90000 * n / nd;
  } /*parsetime*/

static bool
    had_stream = false, /* whether I've seen <stream> */
    had_spu = false, /* whether I've seen <spu> */
    had_textsub = false; /* whether I've seen <textsub> */
static stinfo *curspu = 0; /* current <spu> directive collected here */
static button *curbutton=0;
static char * filename = 0;

static void stream_begin()
  {
    if (had_stream)
      {
        fprintf(stderr, "ERR:  Only one stream is currently allowed.\n");
        exit(1);
      } /*if*/
    had_stream = true;
  } /*stream_begin*/

static void stream_video_format(const char *v)
  {
    if (!strcasecmp(v, "NTSC"))
      {
        default_video_format = VF_NTSC;
      }
    else if (!strcasecmp(v, "PAL"))
      {
        default_video_format = VF_PAL;
      }
    else
      {
        fprintf(stderr, "ERR:  unrecognized video format \"%s\"\n", v);
        exit(1);
      } /*if*/
  } /*stream_video_format*/

static void spu_begin()
  {
    if (had_textsub)
      {
        fprintf(stderr, "ERR:  cannot have both <textsub> and <spu>\n");
        exit(1);
      } /*if*/
    curspu = malloc(sizeof(stinfo));
    memset(curspu, 0, sizeof(stinfo));
    had_spu = true;
  }

static void spu_image(const char *v)        { curspu->img.fname=localize_filename(v); }
static void spu_highlight(const char *v)    { curspu->hlt.fname=localize_filename(v); }
static void spu_select(const char *v)       { curspu->sel.fname=localize_filename(v); }
static void spu_start(const char *v)        { curspu->spts         = parsetime(v); }
static void spu_end(const char *v)          { curspu->sd           = parsetime(v); }
static void spu_outlinewidth(const char *v) { curspu->outlinewidth = strtounsigned(v, "spu outlinewidth");      }
static void spu_xoffset(const char *v)      { curspu->x0 = strtounsigned(v, "spu xoffset");                }
static void spu_yoffset(const char *v)      { curspu->y0 = strtounsigned(v, "spu yoffset");                }

static void spu_force(const char *v)
{
    curspu->forced = xml_ison(v, "spu force");
}

static void spu_transparent(const char *v)
{
    curspu->transparentc = parse_color(v, "transparency");
}

static void spu_autooutline(const char *v)
{
    if (!strcmp(v, "infer"))
        curspu->autooutline = true;
    else
      {
        fprintf(stderr, "ERR:  Unknown autooutline type %s\n", v);
        exit(1);
      } /*if*/
}

static void spu_autoorder(const char *v)
{
    if (!strcmp(v, "rows"))
        curspu->autoorder = false;
    else if (!strcmp(v, "columns"))
        curspu->autoorder = true;
    else
      {
        fprintf(stderr, "ERR:  Unknown autoorder type %s\n", v);
        exit(1);
      } /*if*/
}

static void spu_complete()
{
    if (!curspu->sd) /* no end time specified */
        curspu->sd = -1; /* default to indefinite */
    else
      {
        if (curspu->sd <= curspu->spts)
          {
            char stime[50], etime[50];
            printtime(stime, curspu->spts);
            printtime(etime, curspu->sd);
            fprintf(stderr, "ERR:  sub has end (%s)<=start (%s), skipping\n", etime, stime);
            nr_subtitles_skipped++;
            return;
          } /*if*/
        curspu->sd -= curspu->spts;
      } /*if*/
    spus = realloc(spus, (numspus + 1) * sizeof(stinfo *));
    spus[numspus++] = curspu;
    curspu = 0;
}

static void button_begin()
{
    curspu->buttons = realloc(curspu->buttons, (curspu->numbuttons + 1) * sizeof(button));
    curbutton = &curspu->buttons[curspu->numbuttons++];
    memset(curbutton, 0, sizeof(button));
    curbutton->r.x0 = -1;
    curbutton->r.y0 = -1;
    curbutton->r.x1 = -1;
    curbutton->r.y1 = -1;
}

static void action_begin()
{
    button_begin();
    curbutton->autoaction = true;
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
    filename = localize_filename(v); /* won't leak, because I won't be called more than once */
}

static void textsub_characterset(const char *v)
{
#ifdef HAVE_ICONV
    subtitle_charset = v[0] != 0 ? strdup(v) : NULL; /* won't leak, because I won't be called more than once */
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
    if (had_spu)
      {
        fprintf(stderr, "ERR:  cannot have both <spu> and <textsub>\n");
        exit(1);
      } /*if*/
    if (had_textsub)
      {
        fprintf(stderr,"ERR:  Only one textsub is currently allowed.\n");
        exit(1);
      } /*if*/
    had_textsub = true;
  } /*textsub_begin*/

static void textsub_complete()
  /* called on a </textsub> tag to load and parse the subtitles. */
  {
    int i;
    if (filename == NULL)
      {
        fprintf(stderr, "ERR:  Filename of subtitle file missing");
        exit(1);
      } /*if*/
    textsub_subdata = sub_read_file(filename, movie_fps);
      /* fixme: sub_free never called! */
    if (textsub_subdata == NULL)
      {
        fprintf(stderr, "ERR:  Couldn't load file %s.\n", filename);
        exit(1);
      } /*if*/
    filename = NULL; /* belongs to textsub_subdata now */
    have_textsub = true;
    numspus = textsub_subdata->sub_num;
    spus = realloc(spus, textsub_subdata->sub_num * sizeof(stinfo *));
      /* fixme: need to make sure user doesn't specify both <textsub> and <spu> */
    for (i = 0; i < textsub_subdata->sub_num; ++i)
      {
        subtitle_elt * const thissub = textsub_subdata->subtitles + i;
        stinfo * const newspu = malloc(sizeof(stinfo));
        memset(newspu, 0, sizeof(stinfo));
        if (!textsub_subdata->sub_uses_time)
          {
          /* start and end are in frame numbers */
            newspu->spts = thissub->start * 90000.0 / movie_fps;
            newspu->sd =
                    (thissub->end - thissub->start)
                *
                    90000.0
                /
                    movie_fps;
          }
        else
          {
          /* start and end are in hundredths of a second */
            newspu->spts = thissub->start * 900;
            newspu->sd = (thissub->end - thissub->start) * 900;
          } /*if*/
        newspu->sub_title = thissub;
        spus[i] = newspu;
      } /*for*/
    free(filename);
    filename = NULL;
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

static void textsub_aspect(const char * v)
  {
    if (!strcmp(v, "16:9"))
      {
        widescreen = true;
      }
    else if (!strcmp(v, "4:3"))
      {
        widescreen = false;
      }
    else
      {
        fprintf(stderr,"ERR:  unrecognized aspect \"%s\" not 16:9 or 4:3.\n", v);
        exit(1);
      } /*if*/
  } /*textsub_aspect*/

static void textsub_fontsize(const char *v)     { text_font_scale_factor=atof(v); }

static void textsub_fill_color(const char *v)
  {
    subtitle_fill_color = parse_color(v, "text fill");
  } /*textsub_fill_color*/

static void textsub_outline_color(const char *v)
  {
    subtitle_outline_color = parse_color(v, "text outline");
  } /*textsub_outline_color*/

static void textsub_outline_thickness(const char *v)
  {
    subtitle_font_thickness = atof(v);
  } /*textsub_outline_thickness*/

static void textsub_shadow_offset(const char *v)
  {
    char * dx, * dy, * junk;
    dx = str_extract_until(&v, ", ");
    dy = str_extract_until(&v, ", ");
    junk = str_extract_until(&v, ", ");
    if (dy == NULL || junk != NULL)
      {
        fprintf(stderr,"ERR:  shadow offset must consist of exactly 2 numbers.\n");
        exit(1);
      } /*if*/
    subtitle_shadow_dx = strtosigned(dx, "text shadow x-offset");
    subtitle_shadow_dy = strtosigned(dy, "text shadow y-offset");
    free(dx);
    free(dy);
    free(junk);
  } /*textsub_shadow_offset*/

static void textsub_shadow_color(const char *v)
  {
    subtitle_shadow_color = parse_color(v, "text shadow");
  } /*textsub_shadow_color*/

static void textsub_force(const char *v)
{
    text_forceit = xml_ison(v, "textsub force");
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
    {"subpictures","format",stream_video_format},
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
    {"textsub","fill-color",textsub_fill_color},
    {"textsub","outline-color",textsub_outline_color},
    {"textsub","outline-thickness",textsub_outline_thickness},
    {"textsub","shadow-offset",textsub_shadow_offset},
    {"textsub","shadow-color",textsub_shadow_color},
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
    {"textsub","aspect",textsub_aspect},
    {"textsub","force",textsub_force},
    {0,0,0}
};

int spumux_parse(const char *fname)
{
   return readxml(fname,spu_elems,spu_attrs);
}
