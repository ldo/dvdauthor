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

#ifndef __DA_INTERNAL_H_
#define __DA_INTERNAL_H_

enum {VM_NONE=0,VM_MPEG1=1,VM_MPEG2=2};
enum {VS_NONE=0,VS_720H=1,VS_704H=2,VS_352H=3,VS_352L=4};
enum {VF_NONE=0,VF_NTSC=1,VF_PAL=2};
enum {VA_NONE=0,VA_4x3=1,VA_16x9=2};
enum {VD_NONE=0,VD_LETTERBOX=1,VD_PANSCAN=2};
enum {VR_NONE=0,VR_NTSCFILM=1,VR_FILM=2,VR_PAL=3,VR_NTSC=4,VR_30=5,VR_PALFIELD=6,VR_NTSCFIELD=7,VR_60=8};
enum {AF_NONE=0,AF_AC3=1,AF_MP2=2,AF_PCM=3,AF_DTS=4};
enum {AQ_NONE=0,AQ_16=1,AQ_20=2,AQ_24=3,AQ_DRC=4};
enum {AD_NONE=0,AD_SURROUND=1};
enum {AL_NONE=0,AL_NOLANG=1,AL_LANG=2};
enum {AS_NONE=0,AS_48KHZ=1,AS_96KHZ=2};

enum {COMPILE_PRE=0,COMPILE_CELL=1,COMPILE_POST=2};

typedef int64_t pts_t;

struct vobuinfo {
    int sector,lastsector,fsect,fnum,vobcellid,firstvobuincell,lastvobuincell,hasseqend,hasvideo;
    pts_t videopts[2],sectpts[2],firstvideopts;
    int numref, firstIfield, numfields, lastrefsect[3]; // why on earth do they want the LAST sector of the ref (I, P) frame?
    unsigned char *sectdata; // so we don't have to reread it
};

struct colorinfo {
    int colors[16];
};

struct videodesc {
    int vmpeg,vres,vformat,vaspect,vdisallow,vframerate,vcaption;
};

struct audiodesc {
    int aformat,aquant,adolby;
    int achannels,alangp,aid,asample;
    char lang[2];
};

struct subpicdesc {
    int slangp,sid;
    char lang[2];
};

struct cell {
    pts_t startpts,endpts;
    int ischapter,pauselen; // ischapter: 1 = chapter&program, 2=program only
    int scellid,ecellid;
    struct vm_statement *cs;
};

struct source {
    char *fname;
    int numcells;
    struct cell *cells;
    struct vob *vob;
};

struct audpts {
    pts_t pts[2];
    int sect;
};

struct audchannel {
    struct audpts *audpts;
    int numaudpts,maxaudpts;
    struct audiodesc ad,adwarn; // use for quant and channels
};

struct vob {
    char *fname;
    int numvobus,maxvobus;
    int vobid,numcells;
    struct pgc *p; // used for colorinfo and buttons
    struct vobuinfo *vi;
    // 0-31: top two bits are the audio type, bottom 3 bits are the channel id
    // 32-63: bottom five bits are subpicture id
    struct audchannel audch[64];
    unsigned char buttoncoli[24];
};

struct button {
    char *name;
    int autoaction;
    int x1,y1,x2,y2;
    char *up,*down,*left,*right;
    int grp;
    struct vm_statement *cs;
};

struct pgc {
    int numsources, numbuttons;
    int numchapters,numprograms,numcells,entries,pauselen;
    struct source **sources;
    struct button *buttons;
    struct vm_statement *prei,*posti;
    struct colorinfo *ci;
};

struct pgcgroup {
    int pstype; // 0 - vts, 1 - vtsm, 2 - vmgm
    struct pgc **pgcs;
    int numpgcs,allentries,numentries;
    struct vobgroup *vg; // only valid for pstype==0
};

struct langgroup {
    char lang[3];
    struct pgcgroup *pg;
};

struct menugroup {
    int numgroups;
    struct langgroup *groups;
    struct vobgroup *vg;
};

struct vobgroup {
    int numaudiotracks, numsubpicturetracks, numvobs, numallpgcs;
    struct pgc **allpgcs;
    struct vob **vobs;
    struct videodesc vd,vdwarn;
    struct audiodesc ad[8],adwarn[8];
    struct subpicdesc sp[32],spwarn[32];
};

struct vtsdef {
    int hasmenu,numtitles,*numchapters,numsectors;
    char vtssummary[0x300],vtscat[4];
};

// keeps TT_SRPT within 1 sector
#define MAXVTS 170

struct toc_summary {
    struct vtsdef vts[MAXVTS];
    int numvts;
};

struct workset {
    struct toc_summary *ts;
    struct menugroup *menus;
    struct pgcgroup *titles;
    int curmenu;
};

extern char *entries[];
extern int jumppad;
extern char *pstypes[];

void write8(unsigned char *p,unsigned char d0,unsigned char d1,
            unsigned char d2,unsigned char d3,
            unsigned char d4,unsigned char d5,
            unsigned char d6,unsigned char d7);
void write4(unsigned char *p,unsigned int v);
void write2(unsigned char *p,unsigned int v);
unsigned int read4(unsigned char *p);
unsigned int read2(unsigned char *p);
int getratedenom(struct vobgroup *va);
int findvobu(struct vob *va,pts_t pts,int l,int h);
pts_t getptsspan(struct pgc *ch);
pts_t getframepts(struct vobgroup *va);
unsigned int buildtimeeven(struct vobgroup *va,int64_t num);
unsigned int getaudch(struct vobgroup *va,int a);
int findcellvobu(struct vob *va,int cellid);
pts_t getcellpts(struct vob *va,int cellid);
int vobgroup_set_video_attr(struct vobgroup *va,int attr,char *s);
int vobgroup_set_video_framerate(struct vobgroup *va,int rate);
int audiodesc_set_audio_attr(struct audiodesc *ad,struct audiodesc *adwarn,int attr,char *s);

unsigned char *vm_compile(unsigned char *obuf,unsigned char *buf,struct workset *ws,int thispgc,struct vm_statement *cs,int ismenu,int prepost);
struct vm_statement *vm_parse(const char *b);

void WriteIFOs(char *fbase,struct workset *ws);
void TocGen(struct workset *ws,struct pgc *fpc,char *fname);

int CreatePGC(FILE *h,struct workset *ws,int ismenu);

int FindVobus(char *fbase,struct vobgroup *va,int ismenu);
void MarkChapters(struct vobgroup *va);
void FixVobus(char *fbase,struct vobgroup *va,int ismenu);
int calcaudiogap(struct vobgroup *va,int vcid0,int vcid1,int ach);

#endif
