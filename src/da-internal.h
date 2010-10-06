/*
    Lower-level definitions for building DVD authoring structures
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

#ifndef __DA_INTERNAL_H_
#define __DA_INTERNAL_H_

#include "common.h"

enum {VM_NONE=0,VM_MPEG1=1,VM_MPEG2=2}; /* values for videodesc.vmpeg */
enum {VS_NONE=0,VS_720H=1,VS_704H=2,VS_352H=3,VS_352L=4}; /* values for videodesc.vres */
enum {VA_NONE=0,VA_4x3=1,VA_16x9=2}; /* values for videodesc.vaspect */
enum {VW_NONE=0,VW_NOLETTERBOX=1,VW_NOPANSCAN=2,VW_CROP=3}; /* values for videodesc.vwidescreen */
enum {VR_NONE=0,VR_NTSCFILM=1,VR_FILM=2,VR_PAL=3,VR_NTSC=4,VR_30=5,VR_PALFIELD=6,VR_NTSCFIELD=7,VR_60=8}; /* values for videodesc.vframerate */

enum {AF_NONE=0,AF_AC3=1,AF_MP2=2,AF_PCM=3,AF_DTS=4}; /* values for audiodesc.aformat */
enum {AQ_NONE=0,AQ_16=1,AQ_20=2,AQ_24=3,AQ_DRC=4}; /* values for audiodesc.aquant */
enum {AD_NONE=0,AD_SURROUND=1}; /* values for audiodesc.adolby */
enum {AL_NONE=0,AL_NOLANG=1,AL_LANG=2}; /* values for audiodesc.alangpresent and subpicdesc.slangpresent */
enum {AS_NONE=0,AS_48KHZ=1,AS_96KHZ=2}; /* values for audiodesc.asample */
enum /* values for audiodesc.acontent */
  {
    ACONTENT_UNSPEC = 0, /* unspecified */
    ACONTENT_NORMAL = 1, /* normal */
    ACONTENT_IMPAIRED = 2, /* visually impaired */
    ACONTENT_COMMENTS1 = 3, /* director's comments */
    ACONTENT_COMMENTS2 = 4, /* alternate director's comments */
  };
enum /* values for subpicdesc.scontent */
  {
    SCONTENT_UNSPEC = 0, /* unspecified */
    SCONTENT_NORMAL = 1, /* normal */
    SCONTENT_LARGE = 2, /* large */
    SCONTENT_CHILDREN = 3, /* children */
    SCONTENT_NORMAL_CC = 5, /* normal captions */
    SCONTENT_LARGE_CC = 6, /* large captions */
    SCONTENT_CHILDREN_CC = 7, /* children's captions */
    SCONTENT_FORCED = 9, /* forced */
    SCONTENT_DIRECTOR = 13, /* director comments */
    SCONTENT_LARGE_DIRECTOR = 14, /* large director comments */
    SCONTENT_CHILDREN_DIRECTOR = 15, /* director comments for children */
  };

typedef int64_t pts_t; /* timestamp in units of 90kHz clock */

struct vobuinfo { /* describes a VOBU in a VOB */
    int sector; /* starting sector number within input file */
    int lastsector; /* ending sector number within input file */
    int fsect; /* sector number within output VOB file */
    int fnum; /* number of VOB file within titleset */
    int vobcellid; /* cell ID in low byte, VOB ID in rest */
    int firstvobuincell,lastvobuincell,hasseqend,hasvideo;
    pts_t videopts[2],sectpts[2],firstvideopts;
    int numref; /* nr entries in lastrefsect */
    int firstIfield;
    int numfields;
    int lastrefsect[3]; // why on earth do they want the LAST sector of the ref (I, P) frame?
    unsigned char sectdata[0x26]; // PACK and system header, so we don't have to reread it
};

struct colorinfo { /* a colour table for subpictures */
    int refcount; /* shared structure */
    int color[16];
};

struct videodesc { /* describes a video stream, info from a <video> tag */
    int vmpeg,vres,vformat,vaspect,vwidescreen,vframerate,vcaption;
};

struct audiodesc { /* describes an audio stream, info from an <audio> tag */
    int aformat;
    int aquant;
    int adolby;
    int achannels;
    int alangpresent;
    int asample;
    int aid;
    char lang[2];
    int acontent;
};

struct subpicdesc {
  /* describes a <subpicture> track at the pgcgroup level. This groups one or more
    streams, being alternative representations of the subpicture for different modes. */
    int slangpresent;
    char lang[2];
    int scontent;
    unsigned char idmap[4];
      /* stream ID for each of normal, widescreen, letterbox, and panscan respectively,
        (128 | id) if defined, else 0 */
};

struct cell {
  /* describes a cell within a source video file--generated from <cell> tags & "chapters"
    attributes, or by default if none of these */
    pts_t startpts,endpts;
    cell_chapter_types ischapter; // 1 = chapter&program, 2 = program only, 0 = neither
    int pauselen;
    int scellid; /* ID assigned to cell */
    int ecellid; /* ID assigned to next cell */
    struct vm_statement *commands;
};

struct source { /* describes an input video file, corresponding to a single <vob> directive */
    char *fname; /* name of file */
    int numcells; /* nr elements in cells */
    struct cell *cells; /* array */
    struct vob *vob; /* pointer to created vob */
};

struct audpts { /* describes a packet in an audio stream */
    pts_t pts[2]; /* start and end time of packet */
    int asect; /* sector number in source file */
};

struct audchannel { /* describes information collected from an audio stream */
    struct audpts *audpts; /* array */
    int numaudpts; /* used portion of audpts array */
    int maxaudpts; /* allocated size of audpts array */
    struct audiodesc ad,adwarn; // use for quant and channels
};

struct vob { /* one entry created for each source in each pgc */
    char *fname; /* name of input file, copied from source */
    int numvobus; /* used portion of vobu array */
    int maxvobus; /* allocated size of vobu array */
    int vobid,numcells;
    struct pgc *progchain; /* backpointer to PGC, used for colorinfo and buttons */
    struct vobuinfo *vobu; /* array of VOBUs in the VOB */
    struct audchannel audch[64]; /* vob-wide audio and subpicture mapping */
      /* index meaning:
        0-31: top two bits are the audio type (0 => AC3, 1 => MPEG, 2 => PCM, 3 => DTS),
            bottom 3 bits are the channel id
        32-63: bottom five bits are subpicture id */
    unsigned char buttoncoli[24]; /* 3 groups of SL_COLI (button colour) info for PCI packets */
};

struct buttoninfo { /* describes a button within a single subpicture stream */
    int substreamid; /* substream ID as specified to spumux */
    bool autoaction; /* true for auto-action button */
    int x1,y1,x2,y2; /* button bounds */
    char *up,*down,*left,*right; /* names of neighbouring buttons */
    int grp;
};

#define MAXBUTTONSTREAM 3
struct button { /* describes a button including versions across different subpicture streams */
    char *name; /* button name */
    struct vm_statement *commands; /* associated commands */
    struct buttoninfo stream[MAXBUTTONSTREAM]; /* stream-specific descriptions */
    int numstream; /* nr of stream entries actually used */
};

struct pgc { /* describes a program chain corresponding to a <pgc> directive */
    int numsources; /* length of sources array */
    int numbuttons; /* length of buttons array */
    int numchapters,numprograms,numcells,pauselen;
    int entries; /* bit mask of applicable menu entry types, or, in a titleset, nonzero if non-title PGC */
    struct source **sources; /* array of <vob> directives seen */
    struct button *buttons; /* array */
    struct vm_statement *prei,*posti;
    struct colorinfo *colors;
    struct pgcgroup *pgcgroup; /* back-pointer to containing pgcgroup */
    unsigned char subpmap[32][4];
      /* per-PGC explicit mapping of subpicture streams to alternative display modes for same
        <subpicture> track. Each entry is (128 | id) if present; 127 if not present. */
};

struct pgcgroup { /* common info across a set of menus or a set of titles (<menus> and <titles> directives) */
    vtypes pstype; // 0 - vts, 1 - vtsm, 2 - vmgm
    struct pgc **pgcs; /* array[numpgcs] of pointers */
    int numpgcs;
    int allentries; /* mask of entry types present */
    int numentries; /* number of entry types present */
    struct vobgroup *vg; /* only for pstype==VTYPE_VTS, otherwise shared menugroup.vg field is used */
};

struct langgroup { /* contents of a <menus> directive */
    char lang[3]; /* value of the "lang" attribute */
    struct pgcgroup *pg;
};

struct menugroup { /* contents specific to all collections of <menus> directives, either VTSM or VMGM */
    int numgroups; /* length of groups array */
    struct langgroup *groups; /* array, one entry per <menus> directive */
    struct vobgroup *vg; /* common among all groups[i]->pg elements */
      /* fixme: I don't think this works right with multiple <menus> ,,, </menus> sections,
        which the XML does allow */
};

struct vobgroup { /* contents of a menuset or titleset (<menus> or <titles>) */
    int numaudiotracks; /* nr <audio> tags seen = size of used part of ad/adwarn arrays */
    int numsubpicturetracks; /* nr <subpicture> tags seen = size of used part of sp/spwarn arrays */
    int numvobs; /* size of vobs array */
    int numallpgcs; /* size of allpgcs array */
    struct pgc **allpgcs; /* array of pointers to PGCs */
    struct vob **vobs; /* array of pointers to VOBs */
    struct videodesc vd; /* describes the video stream, one <video> tag only */
    struct videodesc vdwarn; /* for saving attribute value mismatches */
    struct audiodesc ad[8]; /* describes the audio streams, one per <audio> tag */
    struct audiodesc adwarn[8]; /* for saving attribute value mismatches */
    struct subpicdesc sp[32]; /* describes the subpicture streams, one per <subpicture> tag */
    struct subpicdesc spwarn[32]; /* for saving attribute value mismatches */
};

struct vtsdef { /* describes a VTS */
    bool hasmenu;
    int numtitles; /* length of numchapters array */
    int *numchapters; /* number of chapters in each title */
    int numsectors;
    char vtssummary[0x300]; /* copy of VTS attributes (bytes 0x100 onwards of VTS IFO) */
    char vtscat[4]; /* VTS_CAT (copy of bytes 0x22 .. 0x25 of VTS IFO) */
};

// keeps TT_SRPT within 1 sector
#define MAXVTS 170

struct toc_summary {
    struct vtsdef vts[MAXVTS];
    int numvts;
};

struct workset {
    const struct toc_summary *titlesets;
    const struct menugroup *menus;
    const struct pgcgroup *titles;
};

/* following implemented in dvdauthor.c */

extern const char * const entries[]; /* PGC menu entry types */
extern bool
    jumppad, /* reserve registers and set up code to allow convenient jumping between titlesets */
    allowallreg; /* don't reserve any registers for convenience purposes */
extern const char * const pstypes[]; /* names of PGC types, indexed by vtypes values */

void write8(unsigned char *p,unsigned char d0,unsigned char d1,
            unsigned char d2,unsigned char d3,
            unsigned char d4,unsigned char d5,
            unsigned char d6,unsigned char d7);
void write4(unsigned char *p,unsigned int v);
void write2(unsigned char *p,unsigned int v);
unsigned int read4(const unsigned char *p);
unsigned int read2(const unsigned char *p);
int getsubpmask(const struct videodesc *vd);
int getratedenom(const struct vobgroup *va);
int findvobu(const struct vob *va,pts_t pts,int l,int h);
pts_t getptsspan(const struct pgc *ch);
pts_t getframepts(const struct vobgroup *va);
unsigned int buildtimeeven(const struct vobgroup *va,int64_t num);
int getaudch(const struct vobgroup *va,int a);
int findcellvobu(const struct vob *va,int cellid);
pts_t getcellpts(const struct vob *va,int cellid);
int vobgroup_set_video_attr(struct vobgroup *va,int attr,const char *s);
int vobgroup_set_video_framerate(struct vobgroup *va,int rate);
int audiodesc_set_audio_attr(struct audiodesc *ad,struct audiodesc *adwarn,int attr,const char *s);

/* following implemented in dvdcompile.c */

unsigned char *vm_compile
  (
    const unsigned char *obuf, /* start of buffer for computing instruction numbers for branches */
    unsigned char *buf, /* where to insert new compiled code */
    const struct workset *ws,
    const struct pgcgroup *curgroup,
    const struct pgc *curpgc,
    const struct vm_statement *cs,
    vtypes ismenu
  );
  /* compiles the parse tree cs into actual VM instructions. */
void vm_optimize(const unsigned char *obuf, unsigned char *buf, unsigned char **end);
  /* does various peephole optimizations on the part of obuf from buf to *end. */
struct vm_statement *vm_parse(const char *b);

/* following implemented in dvdifo.c */

void WriteIFOs(const char *fbase,const struct workset *ws);
void TocGen(const struct workset *ws,const struct pgc *fpc,const char *fname);

/* following implemented in dvdpgc.c */

int CreatePGC(FILE *h,const struct workset *ws,vtypes ismenu);

/* following implemented in dvdvob.c */

int FindVobus(const char *fbase,struct vobgroup *va,vtypes ismenu);
void MarkChapters(struct vobgroup *va);
void FixVobus(const char *fbase,const struct vobgroup *va,const struct workset *ws,vtypes ismenu);
int calcaudiogap(const struct vobgroup *va,int vcid0,int vcid1,int ach);

#endif
