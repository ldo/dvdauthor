/*
 * Copyright (C) 2002, 2003 Jan Panteltje <panteltje@yahoo.com>
 * With many changes by Scott Smith (trckjunky@users.sourceforge.net)
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
#include "subglobals.h"

typedef struct
{
    unsigned char r, g, b, t; // t=255 means opaque, t=0 means transparent
} palt;

typedef struct {
    unsigned char *fname;
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
	subtitle *sub_title;
} stinfo;

#define SUB_BUFFER_MAX		53220
#define SUB_BUFFER_HEADROOM     1024

extern unsigned char *sub;
extern int debug;
extern int have_textsub;
extern int have_transparent;
extern int transparent_color;

extern stinfo **spus;
extern int numspus;

extern int skip;

int calcY(palt *p);
int calcCr(palt *p);
int calcCb(palt *p);

int findmasterpal(stinfo *s,palt *p);

// subgen-parse-xml

int spumux_parse(const char *fname);

// subgen-encode

int dvd_encode(stinfo *s);
int svcd_encode(stinfo *s);
int cvd_encode(stinfo *s);

// subgen-image

int process_subtitle(stinfo *s);
void image_init();
void image_shutdown();
