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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA.
 */

#include "compat.h"

#include <sys/types.h>
#include <dirent.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>

#include "dvdauthor.h"
#include "da-internal.h"
#include "dvdvm.h"



// with this enabled, extra PGC commands will be generated to allow
// jumping/calling to a wider number of destinations
bool jumppad = false;

// with this enabled, all 16 general purpose registers can be used, but
// prohibits certain convenience features, like multiple commands on a button
bool allowallreg = false;

/* video/audio/subpicture attribute keywords -- note they are all unique to allow
  xxx_ANY attribute setting to work */
static const char * const vmpegdesc[4]={"","mpeg1","mpeg2",0};
static const char * const vresdesc[6]={"","720xfull","704xfull","352xfull","352xhalf",0};
static const char * const vformatdesc[4]={"","ntsc","pal",0};
static const char * const vaspectdesc[4]={"","4:3","16:9",0};
static const char * const vwidescreendesc[5]={"","noletterbox","nopanscan","crop",0};
// taken from mjpegtools, also GPL
const static char * const vratedesc[16] = /* descriptions of frame-rate codes */
  {
    "0x0",
    "24000.0/1001.0 (NTSC 3:2 pulldown converted FILM)",
    "24.0 (NATIVE FILM)",
    "25.0 (PAL/SECAM VIDEO / converted FILM)",
    "30000.0/1001.0 (NTSC VIDEO)",
    "30.0",
    "50.0 (PAL FIELD RATE)",
    "60000.0/1001.0 (NTSC FIELD RATE)",
    "60.0",
  /* additional rates copied from FFmpeg, really just to fill out array */
    "15.0",
    "5.0",
    "10.0",
    "12.0",
    "15.0",
    "0xe",
    "0xf"
  };
static const char * const aformatdesc[6]={"","ac3","mp2","pcm","dts",0};
  /* audio formats */
static const char * const aquantdesc[6]={"","16bps","20bps","24bps","drc",0};
static const char * const adolbydesc[3]={"","surround",0};
static const char * const alangdesc[4]={"","nolang","lang",0};
static const char * const achanneldesc[10]={"","1ch","2ch","3ch","4ch","5ch","6ch","7ch","8ch",0};
static const char * const asampledesc[4]={"","48khz","96khz",0};
  /* audio sample rates */
static const char * const acontentdesc[6] =
    {"", "normal", "impaired", "comments1", "comments2", 0};
    /* audio content types */

const char * const entries[9]={"","","title","root","subtitle","audio","angle","ptt",0};
  /* entry menu types */

const char * const pstypes[3]={"VTS","VTSM","VMGM"};

static const char * const smodedesc[6]={"","normal","widescreen","letterbox","panscan",0};
  /* subpicture usage modes */
static const char * const scontentdesc[17] =
    {
        "", "normal", "large", "children",
        "", "normal_cc", "large_cc", "children_cc",
        "", "forced", "", "",
        "", "director", "large_director", "children_director",
        0
    };
    /* subpicture content types */

static const int default_colors[16]={ /* default contents for new colour tables */
    COLOR_UNUSED,
    COLOR_UNUSED,
    COLOR_UNUSED,
    COLOR_UNUSED,

    COLOR_UNUSED,
    COLOR_UNUSED,
    COLOR_UNUSED,
    COLOR_UNUSED,

    COLOR_UNUSED,
    COLOR_UNUSED,
    COLOR_UNUSED,
    COLOR_UNUSED,

    COLOR_UNUSED,
    COLOR_UNUSED,
    COLOR_UNUSED,
    COLOR_UNUSED
};

static const int ratedenom[9]={0,90090,90000,90000,90090,90000,90000,90090,90000};
  /* corresponding to vratedesc, adjustment to clock units per second
    to convert nominal to actual frame rate */
static const int evenrate[9]={0,    24,   24,   25,   30,   30,   50,   60,   60};
  /* corresponding to vratedesc, nominal frame rate */

bool delete_output_dir = false;

static int getratecode(const struct vobgroup *va)
  /* returns the frame rate code if specified, else the default. */
{
    if (va->vd.vframerate != 0)
        return va->vd.vframerate;
    else if (va->vd.vformat != VF_NONE || default_video_format != VF_NONE)
      {
      /* fudge it for calls from menu PGC-generation routines with no video present */
        return (va->vd.vformat != VF_NONE ? va->vd.vformat : default_video_format) == VF_PAL ? VR_PAL : VR_NTSC;
      }
    else
      {
#if defined(DEFAULT_VIDEO_FORMAT)
#    if DEFAULT_VIDEO_FORMAT == 1
        fprintf(stderr, "WARN: defaulting frame rate to NTSC\n");
        return VR_NTSC;
#    elif DEFAULT_VIDEO_FORMAT == 2
        fprintf(stderr, "WARN: defaulting frame rate to PAL\n");
        return VR_PAL;
#    endif
#else
        fprintf(stderr, "ERR:  cannot determine default frame rate--no video format specified\n");
        exit(1);
#endif
      } /*if*/
} /*getratecode*/

int getratedenom(const struct vobgroup *va)
  /* returns the frame rate divider for the frame rate if specified, else the default. */
  {
    return ratedenom[getratecode(va)];
  } /*getratedenom*/

pts_t getframepts(const struct vobgroup *va)
  /* returns the number of exact clock units per frame. */
  {
    const int rc = getratecode(va);
    return ratedenom[rc] / evenrate[rc];
  } /*getframepts*/

static int tobcd(int v)
  /* separates the two decimal digits of v (assumed in range [0 .. 99]) into two hex nibbles.
    This is used for encoding cell and PGC playback times. */
  {
    return (v / 10) * 16 + v % 10;
  } /*tobcd*/

static unsigned int buildtimehelper(const struct vobgroup *va, int64_t num, int64_t denom)
  /* returns a BCD-encoded representation hhmmssff of num/denom seconds including
    the frame rate. */
  {
    int hr, min, sec, fr, rc;
    int64_t frate;

    if (denom == 90090)
      {
        frate = 30;
        rc = 3;
      }
    else
      {
        frate = 25;
        rc = 1;
      } /*if*/
    num += denom / (frate * 2) + 1; /* so duration will be rounded to nearest whole frame time */
    sec = num / denom; /* seconds */
    min = sec / 60;
    hr = tobcd(min / 60); /* hours */
    min = tobcd(min % 60); /* minutes */
    sec = tobcd(sec % 60); /* seconds */
    num %= denom;
    fr = tobcd(num * frate / denom); /* frame number within second--note tens digit will be <= 3 */
    return
            hr << 24
        |
            min << 16
        |
            sec << 8
        |
            fr
        |
            rc << 6;
  } /*buildtimehelper*/

unsigned int buildtimeeven(const struct vobgroup *va, int64_t num)
  /* returns a BCD-encoded representation hhmmssff of num/denom seconds, where
    denom is computed according to va->vd.vframerate. This is used for encoding
    cell and PGC playback times. I think these BCD-encoded fields are designed
    to be easy for the player to convert to a a form that can be displayed to
    the user, they're not going to be used for any other computations in the
    player. */
  {
    const int rc = getratecode(va);
    return
        buildtimehelper(va, num, ratedenom[rc]);
  } /*buildtimeeven*/

int getaudch(const struct vobgroup *va, int a)
  /* returns an index into a vob.audch array, with the audio format in the top two bits
    and the channel id in the bottom three bits. */
  {
    if (!va->ad[a].aid)
        return
            -1;
    return
        va->ad[a].aid - 1 + (va->ad[a].aformat - 1) * 8;
  } /*getaudch*/

void write8(unsigned char *p,unsigned char d0,unsigned char d1,unsigned char d2,unsigned char d3,unsigned char d4,unsigned char d5,unsigned char d6,unsigned char d7)
/* stores 8 bytes beginning at address p. */
{
    p[0]=d0;
    p[1]=d1;
    p[2]=d2;
    p[3]=d3;
    p[4]=d4;
    p[5]=d5;
    p[6]=d6;
    p[7]=d7;
}

void write4(unsigned char *p,unsigned int v)
/* inserts a four-byte integer in big-endian format beginning at address p. */
{
    p[0]=(v>>24)&255;
    p[1]=(v>>16)&255;
    p[2]=(v>>8)&255;
    p[3]=v&255;
}

void write2(unsigned char *p,unsigned int v)
/* inserts a two-byte integer in big-endian format beginning at address p. */
{
    p[0]=(v>>8)&255;
    p[1]=v&255;
}

unsigned int read4(const unsigned char *p)
/* extracts a four-byte integer in big-endian format beginning at address p. */
{
    return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3];
}

unsigned int read2(const unsigned char *p)
/* extracts a two-byte integer in big-endian format beginning at address p. */
{
    return (p[0]<<8)|p[1];
}

static int findsubpmode(const char *m)
  /* returns the code corresponding to the specified subpicture usage mode name, or
    -1 if unrecognized. */
  {
    int i;
    for (i = 0; i < 4; i++)
        if (!strcasecmp(smodedesc[i+1], m))
            return
                i;
    return
        -1;
  } /*findsubpmode*/

static int warnupdate
  (
    int * oldval,
    int newval,
    int * warnval, /* value previously warned about, if any */
    const char * desc, /* explanatory text for warning message */
    const char * const * lookup /* for converting values to symbolic form for messages */
  )
  /* updates *oldval to newval if not yet set (i.e. = 0), does nothing if it is already newval,
    otherwise if it was set to something else, outputs a warning (if *warnval is not already
    newval) and sets *warnval to newval. Returns 1 iff such a mismatch was found, else 0. */
  {
    if (oldval[0] == 0)
      {
        oldval[0] = newval;
        return
            0;
      }
    else if (oldval[0] == newval)
        return
            0;
    else if (warnval[0] != newval) /* not already warned about this value */
      {
        fprintf
          (
            stderr,
            "WARN: attempt to update %s from %s to %s; skipping\n",
            desc,
            lookup[oldval[0]],
            lookup[newval]
          );
        warnval[0] = newval; /* to reduce number of warnings */
      } /*if*/
    return
        1;
  } /*warnupdate*/

static int scanandwarnupdate
  (
    int * oldval,
    const char * newval, /* symbolic new value */
    int * warnval,
    const char * desc, /* explanatory text for warning message */
    const char * const * lookup /* table from which to return index matching newval */
  )
  /* updates *oldval to the index of the entry matching the name newval in the array lookup if
    found and *oldval was not yet set (i.e. = 0). Does nothing if it was already set to the value,
    otherwise if it was set to something else, outputs a warning and sets *warnval to the
    new value. Returns 0 if newval could not be recognized, 1 if *oldval was updated or was
    already the right value, and 2 if the warning was output. */
  {
    int i;
    for (i = 1; lookup[i]; i++)
        if (!strcasecmp(newval, lookup[i]))
            return
                warnupdate(oldval, i, warnval, desc, lookup) + 1;
    return
        0;
  } /*scanandwarnupdate*/

int vobgroup_set_video_framerate(struct vobgroup *va, int rate /* [0 .. 15] */)
  /* sets the video frame rate code (should be VR_PAL or VR_NTSC only). Returns 1 if
    the framerate was already set to something different, else 0. */
  {
    if (!va->vd.vframerate && rate != VR_PAL && rate != VR_NTSC)
      {
        fprintf(stderr, "WARN: not a valid DVD frame rate: 0x%02x\n", rate);
        rate = VR_NTSC; /* or something */
      } /*if*/
    return warnupdate(&va->vd.vframerate, rate, &va->vdwarn.vframerate, "frame rate", vratedesc);
  } /*vobgroup_set_video_framerate*/

#define ATTRMATCH(a) (attr==0 || attr==(a))
  /* does the attribute code match either the specified value or the xxx_ANY value */

int vobgroup_set_video_attr(struct vobgroup *va,int attr,const char *s)
  /* sets the specified video attribute (might be VIDEO_ANY) to the specified keyword value.
    Returns 1 if the attribute was already set to a different value, else 0.
    Aborts the program on an unrecognizable value. */
  {
    int w;

    if( ATTRMATCH(VIDEO_MPEG) ) {
        w=scanandwarnupdate(&va->vd.vmpeg,s,&va->vdwarn.vmpeg,"mpeg format",vmpegdesc);
        if(w) return w-1;
    }

    if( ATTRMATCH(VIDEO_FORMAT) ) {
        w=scanandwarnupdate(&va->vd.vformat,s,&va->vdwarn.vformat,"tv format",vformatdesc);
        if(w) return w-1;
    }

    if( ATTRMATCH(VIDEO_ASPECT) ) {
        w=scanandwarnupdate(&va->vd.vaspect,s,&va->vdwarn.vaspect,"aspect ratio",vaspectdesc);
        if(w) return w-1;
    }

    if( ATTRMATCH(VIDEO_WIDESCREEN) ) {
        w=scanandwarnupdate(&va->vd.vwidescreen,s,&va->vdwarn.vwidescreen,"widescreen conversion",vwidescreendesc);
        if(w) return w-1;
    }

    if (ATTRMATCH(VIDEO_CAPTION))
      {
        w = va->vd.vcaption;
        if (!strcasecmp(s, "field1"))
            va->vd.vcaption |= 1;
        else if (!strcasecmp(s, "field2"))
            va->vd.vcaption |= 2;
        else
          {
            fprintf(stderr, "ERR:  Cannot parse video caption '%s'\n", s);
            exit(1);
          } /*if*/
        return w;
      } /*if*/

    if (ATTRMATCH(VIDEO_RESOLUTION) && strstr(s, "x"))
      {
        const int splitpos = strstr(s, "x") - s;
        const char * const s1 = strndup(s, splitpos);
        const char * const s2 = s + splitpos + 1;
        const int h = strtounsigned(s1, "horizontal resolution");
        int v, r, w;

        if (isdigit(s2[0]))
            v = strtounsigned(s2, "vertical resolution");
        else if (!strcasecmp(s2, "full") || !strcasecmp(s2, "high"))
            v = 384;
        else
            v = 383;

        if (h > 704)
            r = VS_720H;
        else if (h > 352)
            r = VS_704H;
        else if (v >= 384)
            r = VS_352H;
        else
            r = VS_352L;
        w = warnupdate(&va->vd.vres, r, &va->vdwarn.vres, "resolution", vresdesc);

        if (va->vd.vformat == VF_NONE)
          {
            if (v % 5 == 0)
                va->vd.vformat = VF_NTSC;
            else if (v % 9 == 0)
                va->vd.vformat = VF_PAL;
          } /*if*/
        free((char *)s1);
        return w;
      } /*if*/

    fprintf(stderr,"ERR:  Cannot parse video option '%s'\n",s);
    exit(1);
  } /*vobgroup_set_video_attr*/

int audiodesc_set_audio_attr(struct audiodesc *ad,struct audiodesc *adwarn,int attr,const char *s)
  /* sets the specified audio attribute (might be AUDIO_ANY) to the specified keyword value.
    Returns 1 if the attribute was already set to a different value, else 0.
    Aborts the program on an unrecognizable value. */
{
    int w;

    if (ATTRMATCH(AUDIO_FORMAT)) {
        w=scanandwarnupdate(&ad->aformat,s,&adwarn->aformat,"audio format",aformatdesc);
        if(w) return w-1;
    }

    if (ATTRMATCH(AUDIO_QUANT)) {
        w=scanandwarnupdate(&ad->aquant,s,&adwarn->aquant,"audio quantization",aquantdesc);
        if(w) return w-1;
    }

    if (ATTRMATCH(AUDIO_DOLBY)) {
        w=scanandwarnupdate(&ad->adolby,s,&adwarn->adolby,"surround",adolbydesc);
        if(w) return w-1;
    }

    if (ATTRMATCH(AUDIO_ANY)) {
        w=scanandwarnupdate(&ad->alangpresent,s,&adwarn->alangpresent,"audio language",alangdesc);
        if(w) return w-1;
    }

    if (ATTRMATCH(AUDIO_CHANNELS)) {
        w=scanandwarnupdate(&ad->achannels,s,&adwarn->achannels,"number of channels",achanneldesc);
        if(w) return w-1;
    }

    if (ATTRMATCH(AUDIO_SAMPLERATE)) {
        w=scanandwarnupdate(&ad->asample,s,&adwarn->asample,"sampling rate",asampledesc);
        if(w) return w-1;
    }

    if (ATTRMATCH(AUDIO_LANG) && 2==strlen(s)) {
        w=warnupdate(&ad->alangpresent,AL_LANG,&adwarn->alangpresent,"audio language",alangdesc);
          /* turn on lang */
        if(ad->lang[0] || ad->lang[1])
            w=1; /* language code already set */
        ad->lang[0]=tolower(s[0]); /* note I don't actually validate the language code */
        ad->lang[1]=tolower(s[1]);
        return w;
    }

    if (ATTRMATCH(AUDIO_CONTENT))
      {
        w = scanandwarnupdate(&ad->acontent, s, &adwarn->acontent, "audio content type", acontentdesc);
        if(w)
            return w - 1;
      } /*if*/

    fprintf(stderr,"ERR:  Cannot parse audio option '%s'\n",s);
    exit(1);
}

static int vobgroup_set_audio_attr(struct vobgroup *va,int attr,const char *s,int ch)
  {
    if (ch >= va->numaudiotracks) /* assert ch = va->numaudiotracks + 1 */
        va->numaudiotracks = ch + 1; /* new audio track */
    return
        audiodesc_set_audio_attr(&va->ad[ch], &va->adwarn[ch], attr, s);
  } /*vobgroup_set_audio_attr*/

static int vobgroup_set_subpic_attr(struct vobgroup *va,int attr,const char *s,int ch)
  /* sets the specified subpicture attribute (might be SPU_ANY) to the specified keyword value.
    Returns 1 if the attribute was already set to a different value, else 0.
    Aborts the program on an unrecognizable value. */
{
    int w;

    if (ch >= va->numsubpicturetracks) /* assert ch = va->numsubpicturetracks + 1 */
        va->numsubpicturetracks = ch + 1;

    if (ATTRMATCH(SPU_ANY)) {
        w=scanandwarnupdate(&va->sp[ch].slangpresent,s,&va->spwarn[ch].slangpresent,"subpicture language",alangdesc);
          /* fixme: note this lang/nolang setting can only be specified on the command line,
            not the XML file */
        if(w) return w-1;
    }

    if(ATTRMATCH(SPU_LANG) && 2==strlen(s)) {
        w=warnupdate(&va->sp[ch].slangpresent,AL_LANG,&va->spwarn[ch].slangpresent,"subpicture language",alangdesc);
          /* turn on lang */
        if(va->sp[ch].lang[0] || va->sp[ch].lang[1])
            w=1; /* language code already set */
        va->sp[ch].lang[0]=tolower(s[0]); /* note I don't actually validate the language code */
        va->sp[ch].lang[1]=tolower(s[1]);
        return w;
    }

    if (ATTRMATCH(SPU_CONTENT))
      {
        w = scanandwarnupdate(&va->sp[ch].scontent, s, &va->spwarn[ch].scontent, "subpicture content type", scontentdesc);
        if(w)
            return w - 1;
      } /*if*/
    fprintf(stderr,"ERR:  Cannot parse subpicture option '%s' on track %d\n",s,ch);
    exit(1);
}

static int vobgroup_set_subpic_stream(struct vobgroup *va, int ch, const char *m, int id)
  /* adds a mapping for the subpicture stream numbered ch (in order of appearance) with
    mode name m to the substream with ID id. */
  {
    int mid;
    if (ch >= va->numsubpicturetracks) /* assert ch = va->numsubpicturetracks + 1 */
        va->numsubpicturetracks = ch + 1;
    mid = findsubpmode(m);
    if (mid < 0)
      {
        fprintf(stderr, "ERR:  Cannot parse subpicture stream mode '%s'\n", m);
        exit(1);
      } /*if*/
    if (va->sp[ch].idmap[mid] && va->sp[ch].idmap[mid] != 128 + id)
      {
        fprintf(stderr, "ERR:  Subpicture stream already defined for subpicture %d mode %s\n", ch, m);
        exit(1);
      } /*if*/
    va->sp[ch].idmap[mid] = 128 + id;

    return 0;
  } /*vobgroup_set_subpic_stream*/

static void inferattr(int *a,int def)
  /* defaults *a to def if not already set. */
{
    if( a[0]!=0 ) return;
    a[0]=def;
}

int getsubpmask(const struct videodesc *vd)
/* returns a 4-bit mask specifying the default usage of a subpicture stream, with
  meaning as follows:
    3  2  1  0
    |  |  |  \ narrowscreen footage
    |  |  \widescreen footage, crop on narrowscreen display
    |  \widescreen footage, letterbox on narrowscreen display
    \widescreen footage, pan&scan on narrowscreen display
*/
  {
    int mask = 0;
    if (vd->vaspect == VA_16x9)
        mask |= 14; /* widescreen => allow pan&scan, letterbox and crop */
    else
        mask |= 1;
    switch (vd->vwidescreen)
      {
    case VW_NOLETTERBOX:
        mask &= -1-4; /* clear letterbox bit */
    break;
    case VW_NOPANSCAN:
        mask &= -1-8; /* clear pan&scan bit */
    break;
    case VW_CROP:
        mask |= 2; /* redundant? crop bit already set for widescreen */
    break;
      } /*switch*/
    return
        mask;
  } /*getsubpmask*/

static void set_video_format_attr
  (
    struct vobgroup * va,
    vtypes pstype
  )
  {
#if defined(DEFAULT_VIDEO_FORMAT)
#    if DEFAULT_VIDEO_FORMAT == 1
        inferattr(&va->vd.vformat, default_video_format || VF_NTSC);
#    elif DEFAULT_VIDEO_FORMAT == 2
        inferattr(&va->vd.vformat, default_video_format || VF_PAL);
#    endif
#else
    inferattr(&va->vd.vformat, default_video_format);
    if (va->vd.vformat == 0)
      {
        fprintf(stderr, "ERR:  no video format specified for %s\n", pstypes[pstype]);
        exit(1);
      } /*if*/
#endif
  } /*set_video_format_attr*/

static void setattr
  (
    struct vobgroup * va,
    vtypes pstype
  )
  /* matches up video, audio and subpicture tracks that the user specified with
    those actually found. */
  {
    int i;

  /* warn user about defaulting settings not already determined */
    if (va->vd.vmpeg == VM_NONE)
        fprintf(stderr, "WARN: video mpeg version was not autodetected\n");
    if (va->vd.vres == VS_NONE)
        fprintf(stderr, "WARN: video resolution was not autodetected\n");
    if (va->vd.vformat == VF_NONE)
        fprintf(stderr, "WARN: video format was not autodetected\n");
    if (va->vd.vaspect == VA_NONE)
        fprintf(stderr, "WARN: aspect ratio was not autodetected\n");
  /* default the undetermined settings */
    inferattr(&va->vd.vmpeg, VM_MPEG2);
    inferattr(&va->vd.vres,  VS_720H);
    set_video_format_attr(va, pstype);
    inferattr(&va->vd.vaspect, VA_4x3);
  /* check for incompatible combinations of aspect/widescreen settings */
    if (va->vd.vaspect == VA_4x3)
      {
        if (va->vd.vwidescreen == VW_NOLETTERBOX || va->vd.vwidescreen == VW_NOPANSCAN)
          {
            fprintf
              (
                stderr,
                "ERR:  widescreen conversion should not be set to either noletterbox or"
                    " nopanscan for 4:3 source material.\n"
              );
            exit(1);
          } /*if*/
      }
    else
      {
        if (va->vd.vwidescreen == VW_CROP)
          {
            fprintf
              (
                stderr,
                "ERR:  widescreen conversion should not be set to crop for 16:9 source material.\n"
              );
            exit(1);
          } /*if*/
      } /*if*/

    for (i = 0; i < 32; i++) /* 8 audio streams * 4 formats */
      {
      /* collect all the appropriate audio tracks for this vobgroup, matching up
        the descriptions specified by the user with those actually found in the
        movie files */
        const int id = (i >> 2) + 1;
        const int afmt = (i & 3) + 1; // note this does not follow the normal stream order
          /* afmt = 1 => AC3, 2 => MPEG audio, 3 => PCM, 4 => DTS, in order of preference */
        int j;
        bool fnd;
        struct audchannel *fad = 0;
        int matchidx, bestmatchcount;

        fnd = false;
        for (j = 0; j < va->numvobs; j++)
          {
            fad = &va->vobs[j]->audch[id - 1 + (afmt - 1) * 8];
            if (fad->numaudpts) /* audio track actually present */
              {
                fnd = true;
                break;
              } /*if*/
          } /*for*/
        if (!fnd)
            continue;

        // do we already know about this stream?
        fnd = false;
        for (j = 0; j < va->numaudiotracks; j++)
            if (va->ad[j].aformat == afmt && va->ad[j].aid == id)
              {
                fnd = true;
                break;
              } /*if; for*/
        if (fnd)
            continue;

        bestmatchcount = -1;
        matchidx = -1;
        // maybe we know about this type of stream but haven't matched the id yet?
        for (j = 0; j < va->numaudiotracks; j++)
            if (va->ad[j].aid == 0)
              {
                int thismatchcount = 0; /* will exceed bestmatchcount first time, at least */
#define ACMPV(setting, val) \
              /* how does va->ad[j] match fad->ad, not counting undetermined values? Let me count the ways... */ \
                if (va->ad[j].setting != 0 && val != 0 && va->ad[j].setting != val) \
                    continue; /* mismatch on determined value */ \
                if (va->ad[j].setting == val) \
                    thismatchcount++;
#define ACMP(setting) ACMPV(setting, fad->ad.setting)
                ACMPV(aformat, afmt)
                ACMP(aquant)
                ACMP(adolby)
                ACMP(achannels)
                ACMP(asample)
#undef ACMP
#undef ACMPV
                if (thismatchcount > bestmatchcount)
                  {
                    bestmatchcount = thismatchcount;
                    matchidx = j;
                  } /*if*/
                fnd = true;
              } /*if; for*/
        if (!fnd)
          {
            // guess we need to add this stream
            j = va->numaudiotracks++; /* new entry */
          }
        else
            j = matchidx; /* replace previous best match */
        va->ad[j].aformat = afmt;
        va->ad[j].aid = id;
        (void)warnupdate(&va->ad[j].aquant,
                   fad->ad.aquant,
                   &va->adwarn[j].aquant,
                   "audio quantization",
                   aquantdesc);
        (void)warnupdate(&va->ad[j].adolby,
                   fad->ad.adolby,
                   &va->adwarn[j].adolby,
                   "surround",
                   adolbydesc);
        (void)warnupdate(&va->ad[j].achannels,
                   fad->ad.achannels,
                   &va->adwarn[j].achannels,
                   "number of channels",
                   achanneldesc);
        (void)warnupdate(&va->ad[j].asample,
                   fad->ad.asample,
                   &va->adwarn[j].asample,
                   "sampling rate",
                   asampledesc);
      } /*for*/
    for (i = 0; i < va->numaudiotracks; i++)
      {
      /* fill in the blanks in the tracks I've just collected */
        if (va->ad[i].aformat == AF_NONE)
          {
            fprintf(stderr, "WARN: audio stream %d was not autodetected\n", i);
          } /*if*/
        inferattr(&va->ad[i].aformat, AF_MP2);
        switch(va->ad[i].aformat)
          {
        case AF_AC3:
        case AF_DTS:
            inferattr(&va->ad[i].aquant, AQ_DRC);
            inferattr(&va->ad[i].achannels, 6);
        break;
        case AF_MP2:
            inferattr(&va->ad[i].aquant, AQ_20);
            inferattr(&va->ad[i].achannels, 2);
        break;
        case AF_PCM:
            inferattr(&va->ad[i].achannels, 2);
            inferattr(&va->ad[i].aquant, AQ_16);
        break;
          } /*switch*/
        inferattr(&va->ad[i].asample, AS_48KHZ);
      } /*for*/

    for (i = 0; i < va->numallpgcs; i++)
      {
        int j, k, l, m, mask;
        bool used;
        struct pgc * const pgc = va->allpgcs[i];

        mask = getsubpmask(&va->vd);

        // If any of the subpicture streams were manually set for this PGC, assume
        // they all were set and don't try to infer anything (in case a VOB is used
        // by multiple PGC's, but only some subpictures are exposed in one PGC and others
        // in the other PGC)
        for (m = 0; m < 4; m++)
            if (pgc->subpmap[0][m])
                goto noinfer;

        for (j = 0; j < pgc->numsources; j++)
          {
            const struct vob * const vob = pgc->sources[j]->vob;

            for (k = 0; k < 32; k++)
              {
                // Does this subpicture stream exist in the VOB?  if not, skip
                if (!vob->audch[k + 32].numaudpts)
                    continue;
                // Is this subpicture stream already referenced by the subpicture table?  if so, skip
                for (l = 0; l < 32; l++)
                    for (m = 0; m < 4; m++)
                        if (pgc->subpmap[l][m] == 128 + k)
                            goto handled;
                // Is this subpicture id defined by the vobgroup?
                used = false;
                for (l = 0; l < va->numsubpicturetracks; l++)
                    for (m = 0; m < 4; m++)
                        if (va->sp[l].idmap[m] == 128 + k && pgc->subpmap[l][m] == 0)
                          {
                            pgc->subpmap[l][m] = 128 + k;
                            used = true; // keep looping in case it's referenced multiple times
                          } /*if; for; for*/
                if (used)
                    continue;
                // Find a subpicture slot that is not used
                for (l = 0; l < 32; l++)
                  {
                    // Is this slot defined by the vobgroup?  If so, it can't be used
                    if (l < va->numsubpicturetracks)
                      {
                        for (m = 0; m < 4; m++)
                            if (va->sp[l].idmap[m])
                                goto next;
                      } /*if*/
                    // Is this slot defined by the pgc?  If so, it can't be used
                    for (m = 0; m < 4; m++)
                        if (pgc->subpmap[l][m])
                            goto next;
                    break;
next: ;
                  } /*for*/
                assert(l < 32);
                // Set all the appropriate stream ids
                for (m = 0; m < 4; m++)
                    pgc->subpmap[l][m] = (mask & 1 << m) != 0 ? 128 + k : 127;
handled: ;
              } /*for*/
          } /*for*/
noinfer:
        for (m = 0; m < 4; m++)
          {
            if ((mask & 1 << m) != 0)
                continue;
            for (l = 0; l < 32; l++)
              {
                int mainid = -1;
                for (m = 0; m < 4; m++)
                  {
                    if ((mask & 1 << m) == 0 && (pgc->subpmap[l][m] & 128) == 128)
                      {
                        fprintf
                          (
                            stderr,
                            "WARN: PGC %d has the subtitle set for stream %d, mode %s"
                                " which is illegal given the video characteristics.  Forcibly"
                                " removing.\n",
                            i,
                            l,
                            smodedesc[m + 1]
                          );
                        pgc->subpmap[l][m] = 127;
                      } /*if*/
                    if (pgc->subpmap[l][m] & 128)
                      {
                        if (mainid == -1)
                            mainid = pgc->subpmap[l][m] & 127;
                        else if (mainid >= 0 && mainid != (pgc->subpmap[l][m] & 127))
                            mainid = -2;
                      } /*if*/
                  } /*for*/
                // if no streams are set for this subpicture, ignore it
                if (mainid == -1)
                    continue;
                // if any streams aren't set that should be (because of the mask), then set them to the main stream
                for (m = 0; m < 4; m++)
                  {
                    if ((mask & 1 << m) == 0)
                        continue;
                    if ((pgc->subpmap[l][m] & 128) == 0)
                      {
                        if (mainid < 0)
                            fprintf
                              (
                                stderr,
                                "WARN:  Cannot infer the stream id for subpicture %d mode %s"
                                    " in PGC %d; please manually specify.\n",
                                l,
                                smodedesc[m+1],
                                i
                              );
                        else
                            pgc->subpmap[l][m] = 128 + mainid;
                      } /*if*/
                  } /*for*/
              } /*for*/
          } /*for*/
      } /*for*/

    for (i = 0; i < 32; i++)
      {
        int j, k;
        bool fnd;
        fnd = false;
        for (j = 0; j < va->numallpgcs; j++)
            for (k = 0; k < 4; k++)
                if (va->allpgcs[j]->subpmap[i][k])
                    fnd = true;
        if (!fnd)
            continue;
        // guess we need to add this stream
        if (i >= va->numsubpicturetracks)
            va->numsubpicturetracks = i + 1;
      } /*for*/

    if (va->numsubpicturetracks > 1 && pstype != VTYPE_VTS)
      {
        fprintf
          (
            stderr,
            "WARN: Too many subpicture tracks for a menu; 1 is allowed, %d are present."
                "  Perhaps you want different <stream> tags for normal/widescreen/letterbox/panscan"
                " within one <subpicture> tag instead of multiple <subpicture> tags?\n",
            va->numsubpicturetracks
          );
      } /*if*/

    fprintf(stderr, "INFO: Generating %s with the following video attributes:\n", pstypes[pstype]);
    fprintf(stderr, "INFO: MPEG version: %s\n", vmpegdesc[va->vd.vmpeg]);
    fprintf(stderr, "INFO: TV standard: %s\n", vformatdesc[va->vd.vformat]);
    fprintf(stderr, "INFO: Aspect ratio: %s\n", vaspectdesc[va->vd.vaspect]);
    fprintf(stderr, "INFO: Resolution: %dx%d\n",
            va->vd.vres != VS_720H ? (va->vd.vres == VS_704H ? 704 : 352) : 720,
            (va->vd.vres == VS_352L ? 240 : 480) * (va->vd.vformat == VF_PAL ? 6 : 5) / 5);
    for (i = 0; i < va->numaudiotracks; i++)
      {
        fprintf
          (
            stderr,
            "INFO: Audio ch %d format: %s/%s,  %s %s",
            i,
            aformatdesc[va->ad[i].aformat],
            achanneldesc[va->ad[i].achannels],
            asampledesc[va->ad[i].asample],
            aquantdesc[va->ad[i].aquant]
          );
        if (va->ad[i].adolby == AD_SURROUND)
            fprintf(stderr, ", surround");
        if (va->ad[i].alangpresent == AL_LANG)
            fprintf(stderr, ", '%c%c'", va->ad[i].lang[0], va->ad[i].lang[1]);
        fprintf(stderr, "\n");
        if (!va->ad[i].aid)
            fprintf(stderr, "WARN: Audio ch %d is not used!\n", i);
      } /*for*/
  } /*setattr*/

int findcellvobu(const struct vob *va,int cellid)
/* finds the element of array va that includes the cell with ID cellid. */
{
    int l=0,h=va->numvobus-1;
    if( h<l )
        return 0;
    cellid=(cellid&255)|(va->vobid*256);
    if( cellid<va->vobu[0].vobcellid )
        return 0;
    if( cellid>va->vobu[h].vobcellid )
        return h+1;
    while(l<h) { /* search by binary chop */
        int m=(l+h)/2;
        if( cellid<=va->vobu[m].vobcellid )
            h=m;
        else
            l=m+1;
    }
    return l;
}

pts_t getcellpts(const struct vob *va,int cellid)
/* returns the duration of the specified cell. */
{
    int s=findcellvobu(va,cellid),e=findcellvobu(va,cellid+1);
    if( s==e ) return 0;
    return va->vobu[e-1].sectpts[1]-va->vobu[s].sectpts[0];
}

int findvobu(const struct vob *va,pts_t pts,int l,int h)
/* finds the element of array va, within indexes l and h, that includes time pts. */
{
    // int l=0,h=va->numvobus-1;

    if( h<l )
        return l-1;
    if( pts<va->vobu[l].sectpts[0] )
        return l-1;
    if( pts>=va->vobu[h].sectpts[1] )
        return h+1;
    while(l<h) { /* search by binary chop */
        int m=(l+h+1)/2;
        if( pts < va->vobu[m].sectpts[0] )
            h=m-1;
        else
            l=m;
    }
    return l;
}

pts_t getptsspan(const struct pgc *ch)
  /* returns the duration of the specified PGC. */
{
    int s,c,ci;
    pts_t ptsspan=0;

    for( s=0; s<ch->numsources; s++ ) {
        const struct source * const sc = ch->sources[s];
        for( c=0; c<sc->numcells; c++ ) {
            const struct cell *const cl = &sc->cells[c];
            for( ci=cl->scellid; ci<cl->ecellid; ci++ )
                ptsspan+=getcellpts(sc->vob,ci);
        }
    }
    return ptsspan;
}

static char *makevtsdir(const char *s)
  /* returns the full pathname of the VIDEO_TS subdirectory within s if non-NULL,
    else returns NULL. */
{
    static const char * subdir = "/VIDEO_TS";
    char * fbuf;
    if (s != NULL)
      {
        fbuf = malloc(strlen(s) + strlen(subdir) + 1);
        sprintf(fbuf, "%s%s", s, subdir);
      }
    else
      {
        fbuf = NULL;
      } /*if*/
    return
        fbuf;
}

// jumppad requires the existence of a menu to operate
// if no languages exist, create an english one
static void jp_force_menu(struct menugroup *mg, vtypes type)
  {
    struct pgcgroup *pg;

    if (!jumppad)
        return;
    if (mg->numgroups)
        return;
    fprintf(stderr, "WARN: The use of jumppad requires a menu; creating a dummy ENGLISH menu\n");
    pg = pgcgroup_new(type);
    menugroup_add_pgcgroup(mg, "en", pg);
  } /*jp_force_menu*/

static void ScanIfo(struct toc_summary *ts, const char *ifo)
  /* scans another existing VTS IFO file and puts info about it
    into *ts for inclusion in the VMG. */
  {
    static unsigned char buf[2048];
    struct vtsdef *vd;
    int i,first;
    FILE * const h = fopen(ifo, "rb");
    if (!h)
      {
        fprintf(stderr, "ERR:  cannot open %s: %s\n", ifo, strerror(errno));
        exit(1);
      } /*if*/
    if (ts->numvts + 1 >= MAXVTS)
      {
      /* shouldn't occur */
        fprintf(stderr,"ERR:  Too many VTSs\n");
        exit(1);
      } /*if*/
    fread(buf, 1, 2048, h);
    vd = &ts->vts[ts->numvts]; /* where to put new entry */
    if (read4(buf + 0xc0) != 0) /* start sector of menu VOB */
        vd->hasmenu = true;
    else
        vd->hasmenu = false;
    vd->numsectors = read4(buf + 0xc) + 1; /* last sector of title set (last sector of BUP) */
    memcpy(vd->vtscat, buf + 0x22, 4); /* VTS category */
    memcpy(vd->vtssummary, buf + 0x100, 0x300); /* attributes of streams in VTS and VTSM */
    fread(buf, 1, 2048, h); // VTS_PTT_SRPT is 2nd sector
    // we only need to read the 1st sector of it because we only need the
    // pgc pointers
    vd->numtitles = read2(buf); /* nr titles */
    vd->numchapters = (int *)malloc(sizeof(int) * vd->numtitles);
      /* array of nr chapters in each title */
    first = 8 + vd->numtitles * 4; /* offset to VTS_PTT for first title */
    for (i = 0; i < vd->numtitles - 1; i++)
      {
        const int n = read4(buf + 12 + i * 4); /* offset to VTS_PTT for next title */
        vd->numchapters[i] = (n - first) / 4; /* difference from previous offset gives nr chapters for this title */
        first = n;
      } /*for*/
    vd->numchapters[i] = (read4(buf + 4) /* end address (last byte of last VTS_PTT) */ + 1 - first) / 4;
      /* nr chapters for last title */
    fclose(h);
    ts->numvts++;
  } /*ScanIfo*/

static void forceaddentry(struct pgcgroup *va, int entry)
  /* gives the first PGC in va the specified entry type, if this is not present already. */
  {
    if (!va->numpgcs && !jumppad)
        return;
    if (!(va->allentries & entry)) /* entry not already present */
      {
        if (va->numpgcs)
            va->pgcs[0]->entries |= entry;
        va->allentries |= entry;
        va->numentries++;
      } /*if*/
  } /*forceaddentry*/

static void checkaddentry(struct pgcgroup *va, int entry)
  /* gives the first PGC in va the specified entry type, if this is not present already
    and it has at least one PGC. */
  {
    if (va->numpgcs)
        forceaddentry(va, entry);
  } /*checkaddentry*/

static int getvtsnum(const char *fbase)
  /* returns the next unused titleset number within output directory fbase. */
  {
    static char realfbase[1000];
    int i;
    if (!fbase)
        return 1;
    for (i = 1; i <= 99; i++)
      {
        FILE *h;
        snprintf(realfbase, sizeof realfbase, "%s/VIDEO_TS/VTS_%02d_0.IFO", fbase, i);
        h = fopen(realfbase, "rb");
        if (!h)
            break; /* doesn't look like this number is in use */
        fclose(h);
      } /*for*/
    fprintf(stderr, "STAT: Picking VTS %02d\n", i);
    return i;
  } /*getvtsnum*/

static void deletedir(const char * fbase)
  /* deletes any existing output directory structure. Note for safety I only look for
    names matching limited patterns. */
  {
    static char dirname[1000], subname[1000];
    DIR  * subdir;
    snprintf(dirname, sizeof dirname, "%s/VIDEO_TS", fbase);
    subdir = opendir(dirname);
    if (subdir == NULL && errno != ENOENT)
      {
        fprintf(stderr, "ERR:  cannot open dir for deleting %s: %s\n", dirname, strerror(errno));
        exit(1);
      } /*if*/
    if (subdir != NULL)
      {
        for (;;)
          {
            const struct dirent * const entry = readdir(subdir);
            if (entry == NULL)
                break;
            if
              (
                    strlen(entry->d_name) == 12
                &&
                    entry->d_name[8] == '.'
                &&
                    (
                        !strcmp(entry->d_name + 9, "IFO")
                    ||
                        !strcmp(entry->d_name + 9, "BUP")
                    ||
                        !strcmp(entry->d_name + 9, "VOB")
                    )
                &&
                    (
                        !strncmp(entry->d_name, "VIDEO_TS", 8)
                    ||
                            !strncmp(entry->d_name, "VTS_", 4)
                        &&
                            entry->d_name[6] == '_'
                    )
              )
              {
                snprintf(subname, sizeof subname, "%s/%s", dirname, entry->d_name);
                if (unlink(subname))
                  {
                    fprintf(stderr, "ERR:  cannot delete file %s: %s\n", subname, strerror(errno));
                    exit(1);
                  } /*if*/
              } /*if*/
          } /*for*/
        closedir(subdir);
      } /*if*/
    if (rmdir(dirname) && errno != ENOENT)
      {
        fprintf(stderr, "ERR:  cannot delete dir %s: %s\n", dirname, strerror(errno));
        exit(1);
      } /*if*/
    snprintf(dirname, sizeof dirname, "%s/AUDIO_TS", fbase);
    if (rmdir(dirname) && errno != ENOENT)
      {
        fprintf(stderr, "ERR:  cannot delete dir %s: %s\n", dirname, strerror(errno));
        exit(1);
      } /*if*/
    errno = 0;
  } /*deletedir*/

static void initdir(const char * fbase)
  /* creates the top-level DVD-video subdirectories within the output directory,
    if they don't already exist. */
  {
    static char realfbase[1000];
    if (fbase)
      {
        if (delete_output_dir)
          {
            deletedir(fbase);
            delete_output_dir = false; /* only do on first call */
          } /*if*/
        if (mkdir(fbase, 0777) && errno != EEXIST)
          {
            fprintf(stderr, "ERR:  cannot create dir %s: %s\n", fbase, strerror(errno));
            exit(1);
          } /*if*/
        snprintf(realfbase, sizeof realfbase, "%s/VIDEO_TS", fbase);
        if (mkdir(realfbase, 0777) && errno != EEXIST)
          {
            fprintf(stderr, "ERR:  cannot create dir %s: %s\n", realfbase, strerror(errno));
            exit(1);
          } /*if*/
        snprintf(realfbase, sizeof realfbase, "%s/AUDIO_TS", fbase);
        if (mkdir(realfbase, 0777) && errno != EEXIST)
          {
            fprintf(stderr, "ERR:  cannot create dir %s: %s\n", realfbase, strerror(errno));
            exit(1);
          } /*if*/
      } /*if*/
    errno = 0;
  } /*initdir*/

static struct colorinfo *colorinfo_new()
{
    struct colorinfo *ci=malloc(sizeof(struct colorinfo));
    ci->refcount=1;
    memcpy(ci->color,default_colors,16*sizeof(int));
    return ci;
}

static void colorinfo_free(struct colorinfo *ci)
  {
    if (ci)
      {
        ci->refcount--;
        if (!ci->refcount)
            free(ci);
      } /*if*/
  } /*colorinfo_free*/

static struct vob *vob_new(const char *fname,struct pgc *progchain)
{
    struct vob *v=malloc(sizeof(struct vob));
    memset(v,0,sizeof(struct vob));
    v->fname=strdup(fname);
    v->progchain=progchain;
    return v;
}

static void vob_free(struct vob *v)
  {
    if (v)
      {
        int i;
        free(v->fname);
        free(v->vobu);
        for (i = 0; i < 64; i++)
            free(v->audch[i].audpts);
        free(v);
      } /*if*/
  } /*vob_free*/

static struct vobgroup *vobgroup_new()
{
    struct vobgroup *vg=malloc(sizeof(struct vobgroup));
    memset(vg,0,sizeof(struct vobgroup));
    return vg;
}

static void vobgroup_free(struct vobgroup *vg)
  {
    if (vg)
      {
        int i;
        free(vg->allpgcs);
        if (vg->vobs)
          {
            for (i = 0; i < vg->numvobs; i++)
                vob_free(vg->vobs[i]);
            free(vg->vobs);
          } /*if*/
        free(vg);
      } /*if*/
  } /*vobgroup_free*/

static void vobgroup_addvob(struct vobgroup *pg, struct pgc *p, struct source *s)
  {
    const bool forcenew = p->numbuttons != 0;
    if (!forcenew)
      {
      /* Reuse a previously-created vob element with the same input file name,
        if one can be found. This is not tried if buttons are present--is that
        because of colour-remapping issues? */
        int i;
        for (i = 0; i < pg->numvobs; i++)
            if (!strcmp(pg->vobs[i]->fname, s->fname) && pg->vobs[i]->progchain->numbuttons == 0)
              {
                s->vob = pg->vobs[i];
                return;
              } /*if*/
      } /*if*/
    pg->vobs = realloc(pg->vobs, (pg->numvobs + 1) * sizeof(struct vob *));
    s->vob = pg->vobs[pg->numvobs++] = vob_new(s->fname, p);
  } /*vobgroup_addvob*/

static void pgcgroup_pushci(struct pgcgroup *p, bool warn)
  /* shares colorinfo structures among all pgc elements that have sources
    which were allocated the same vob structures. */
  {
    int i, j, ii, jj;
    for (i = 0; i < p->numpgcs; i++)
      {
        if (!p->pgcs[i]->colors)
            continue;
        for (j = 0; j < p->pgcs[i]->numsources; j++)
          {
            const struct vob * const v = p->pgcs[i]->sources[j]->vob;
            for (ii = 0; ii < p->numpgcs; ii++)
                for (jj = 0; jj < p->pgcs[ii]->numsources; jj++)
                    if (v == p->pgcs[ii]->sources[jj]->vob)
                      {
                        if (!p->pgcs[ii]->colors)
                          {
                            p->pgcs[ii]->colors = p->pgcs[i]->colors;
                            p->pgcs[ii]->colors->refcount++;
                          }
                        else if (p->pgcs[ii]->colors != p->pgcs[i]->colors && warn)
                          {
                            fprintf
                              (
                                stderr,
                                "WARN: Conflict in colormap between PGC %d and %d\n",
                                i, ii
                              );
                          } /*if*/
                      } /*if; for; for*/
          } /*for*/
      } /*for*/
  } /*pgcgroup_pushci*/

static void pgcgroup_createvobs(struct pgcgroup *p, struct vobgroup *v)
  /* appends p->pgcs onto v->allpgcs and builds the struct vob arrays in the vobgroups. */
  {
    int i, j;
    v->allpgcs = (struct pgc **)realloc(v->allpgcs, (v->numallpgcs + p->numpgcs) * sizeof(struct pgc *));
    memcpy(v->allpgcs + v->numallpgcs, p->pgcs, p->numpgcs * sizeof(struct pgc *));
    v->numallpgcs += p->numpgcs;
    for (i = 0; i < p->numpgcs; i++)
        for (j = 0; j < p->pgcs[i]->numsources; j++)
            vobgroup_addvob(v, p->pgcs[i], p->pgcs[i]->sources[j]);
    pgcgroup_pushci(p, false);
    for (i = 0; i < p->numpgcs; i++)
        if (!p->pgcs[i]->colors)
          {
            p->pgcs[i]->colors = colorinfo_new();
            pgcgroup_pushci(p, false);
          } /*if; for*/
    pgcgroup_pushci(p, true);
  } /*pgcgroup_createvobs*/

static void validatesummary(struct pgcgroup *va)
/* merges the info for all pgcs and validates the collected settings for a pgcgroup. */
{
    int i,allowedentries;
    bool err = false;

    switch (va->pstype)
      {
    case VTYPE_VTSM:
        allowedentries = 0xf8; /* all entry menu types allowed except title */
    break;
    case VTYPE_VMGM:
        allowedentries = 4; /* title entry menu type only */
    break;
    case VTYPE_VTS:
    default:
        allowedentries = 0; /* no entry menu types */
    break;
      } /*switch*/

    for( i=0; i<va->numpgcs; i++ ) {
        struct pgc *p=va->pgcs[i];
      /* why is this being done? let user specify it if they want
        if( !p->posti && p->numsources ) {
            struct source *s=p->sources[p->numsources-1];
            s->cells[s->numcells-1].pauselen=255;
        } */
        if( va->allentries & p->entries ) {
          /* this pgc adds entry menus already seen */
            int j;

            for( j=0; j<8; j++ )
                if( va->allentries & p->entries & (1<<j) )
                    fprintf(stderr,"ERR:  Multiple definitions for entry %s, 2nd occurrence in PGC #%d\n",entries[j],i);
            err = true;
        }
        if (va->pstype != VTYPE_VTS && (p->entries & ~allowedentries) != 0)
          {
          /* disallowed entry menu types present--report them */
            int j;
            for (j = 0; j < 8; j++)
                if (p->entries & (~allowedentries) & (1 << j))
                    fprintf
                      (
                        stderr,
                        "ERR:  Entry %s is not allowed for menu type %s\n",
                        entries[j],
                        pstypes[va->pstype]
                      );
            err = true;
          } /*if*/
        va->allentries|=p->entries;
        if( p->numsources ) {
            int j;
            bool first;
            first = true;
            for( j=0; j<p->numsources; j++ ) {
                if( !p->sources[j]->numcells )
                    fprintf(stderr,"WARN: Source has no cells (%s) in PGC %d\n",p->sources[j]->fname,i);
                else if( first ) {
                    if( p->sources[j]->cells[0].ischapter!=CELL_CHAPTER_PROGRAM ) {
                        fprintf(stderr,"WARN: First cell is not marked as a chapter in PGC %d, setting chapter flag\n",i);
                        p->sources[j]->cells[0].ischapter=CELL_CHAPTER_PROGRAM;
                    }
                    first = false;
                }
            }
        }
    }
    for( i=1; i<256; i<<=1 )
        if( va->allentries&i )
            va->numentries++;
    if(err)
        exit(1);
}

static void statement_free(struct vm_statement *s)
  {
    if (s)
      {
        free(s->s1);
        free(s->s2);
        free(s->s3);
        free(s->s4);
        statement_free(s->param);
        statement_free(s->next);
        free(s);
      } /*if*/
  } /*statement_free*/

struct source *source_new()
{
    struct source *v=malloc(sizeof(struct source));
    memset(v,0,sizeof(struct source));
    return v;
}

static void source_free(struct source *s)
  {
    if (s)
      {
        int i;
        free(s->fname);
        if (s->cells)
          {
            for (i = 0; i < s->numcells; i++)
                statement_free(s->cells[i].commands);
            free(s->cells);
          } /*if*/
        // vob is a reference created by vobgroup_addvob
        free(s);
      } /*if*/
  } /*source_free*/

int source_add_cell(struct source *v,double starttime,double endtime,cell_chapter_types chap,int pause,const char *cmd)
{
    struct cell *c;

    v->cells=realloc(v->cells,(v->numcells+1)*sizeof(struct cell)); /* space for a new cell */
    c=v->cells+v->numcells; /* the newly-added cell */
    v->numcells++;
    c->startpts=starttime*90000+.5;
    c->endpts=endtime*90000+.5;
    c->ischapter=chap;
    c->pauselen=pause;
    if( cmd )
        c->commands=vm_parse(cmd);
    else
        c->commands=0;
    return 0;
}

void source_set_filename(struct source *v,const char *s)
{
    v->fname=strdup(s);
}

static void button_freecontents(struct button *b)
  {
    if (b)
      {
        int i;
        free(b->name);
        statement_free(b->commands);
        for (i = 0; i < b->numstream; i++)
          {
            free(b->stream[i].up);
            free(b->stream[i].down);
            free(b->stream[i].left);
            free(b->stream[i].right);
          } /*for*/
      } /*if*/
  } /*button_freecontents*/

struct pgc *pgc_new()
{
    struct pgc *p=malloc(sizeof(struct pgc));
    memset(p,0,sizeof(struct pgc));
    return p;
}

void pgc_free(struct pgc *p)
  {
    if (p)
      {
        int i;
        if (p->sources)
          {
            for (i = 0; i < p->numsources; i++)
                source_free(p->sources[i]);
            free(p->sources);
          } /*if*/
        if (p->buttons)
          {
            for (i = 0; i < p->numbuttons; i++)
                button_freecontents(p->buttons + i);
            free(p->buttons);
          } /*if*/
        statement_free(p->prei);
        statement_free(p->posti);
        colorinfo_free(p->colors);
        // don't free the pgcgroup; it's an upward reference
        free(p);
      } /*if*/
  } /*pgc_free*/

void pgc_set_pre(struct pgc *p,const char *cmd)
{
    assert(!p->prei);
    p->prei=vm_parse(cmd); // this will initialize prei
}

void pgc_set_post(struct pgc *p,const char *cmd)
{
    assert(!p->posti);
    p->posti=vm_parse(cmd); // this will initialize posti
}

void pgc_set_color(struct pgc *p,int index,int color)
{
    assert(index>=0 && index<16);
    if( !p->colors ) p->colors=colorinfo_new();
    p->colors->color[index]=color;
}

void pgc_set_stilltime(struct pgc *p,int still)
{
    p->pauselen=still;
}

int pgc_set_subpic_stream(struct pgc *p,int ch,const char *m,int id)
  /* adds a mapping for the subpicture stream numbered ch (in order of appearance) with
    mode name m to the substream with ID id. */
{
    int mid;

    mid=findsubpmode(m);
    if( mid<0 ) {
        fprintf(stderr,"ERR:  Cannot parse subpicture stream mode '%s'\n",m);
        exit(1);
    }

    if( p->subpmap[ch][mid] && p->subpmap[ch][mid]!=128+id ) {
        fprintf(stderr,"ERR:  Subpicture stream already defined for subpicture %d mode %s\n",ch,m);
        exit(1);
    }
    p->subpmap[ch][mid]=128+id;

    return 0;
}

void pgc_add_entry(struct pgc *p, vtypes vtype, const char *entry)
  {
    int i;
    if (vtype != VTYPE_VTS)
      {
        for (i = 2; i < 8; i++)
            if (!strcasecmp(entry, entries[i]))
              {
                int v = 1 << i;
                if (p->entries & v)
                  {
                    fprintf
                      (
                        stderr,
                        "ERR:  Defined entry '%s' multiple times for the same PGC\n",
                        entry
                      );
                    exit(1);
                  } /*if*/
                p->entries |= 1 << i;
                return;
              } /*if*/
      }
    else
      {
        if (!strcasecmp(entry, "notitle"))
          {
            p->entries |= 1; /* anything nonzero */
            return;
          } /*if*/
      } /*if*/
    fprintf(stderr,"ERR:  Unknown entry '%s'\n",entry);
    exit(1);
  } /*pgc_add_entry*/

void pgc_add_source(struct pgc *p,struct source *v)
{
    if( !v->fname ) {
        fprintf(stderr,"ERR:  source has no filename\n");
        exit(1);
    }
    p->sources=(struct source **)realloc(p->sources,(p->numsources+1)*sizeof(struct source *));
    p->sources[p->numsources++]=v;
}

int pgc_add_button(struct pgc *p,const char *name,const char *cmd)
  /* adds a new button definition, optional name, and associated commands to a PGC. */
  {
    struct button *bs;
    if (p->numbuttons == 36)
      {
        fprintf(stderr, "ERR:  Limit of up to 36 buttons\n");
        exit(1);
      } /*if*/
    p->buttons = (struct button *)realloc(p->buttons, (p->numbuttons + 1) * sizeof(struct button));
    bs = &p->buttons[p->numbuttons++];
    memset(bs, 0, sizeof(struct button));
  /* note stream-specific info (including spatial and auto-action info) is initially empty */
    if (name)
        bs->name = strdup(name);
    else
      {
      /* make up a sequentially-assigned name */
        char nm[10];
        snprintf(nm, sizeof nm, "%d", p->numbuttons);
        bs->name = strdup(nm);
      } /*if*/
    bs->commands = vm_parse(cmd);
    return 0;
  } /*pgc_add_button*/

struct pgcgroup *pgcgroup_new(vtypes type)
  {
    struct pgcgroup *ps=malloc(sizeof(struct pgcgroup));
    memset(ps,0,sizeof(struct pgcgroup));
    ps->pstype=type;
    if (type == VTYPE_VTS)
        ps->pg_vg = vobgroup_new();
    return ps;
  }

void pgcgroup_free(struct pgcgroup *pg)
  {
    if (pg)
      {
        int i;
        if (pg->pgcs)
          {
            for (i = 0; i < pg->numpgcs; i++)
                pgc_free(pg->pgcs[i]);
            free(pg->pgcs);
          } /*if*/
        vobgroup_free(pg->pg_vg);
        free(pg);
      } /*if*/
  } /*pgcgroup_free*/

void pgcgroup_add_pgc(struct pgcgroup *ps,struct pgc *p)
  /* adds a new PGC to the specified pgcgroup. */
  {
    ps->pgcs = (struct pgc **)realloc(ps->pgcs, (ps->numpgcs + 1) * sizeof(struct pgc *));
    ps->pgcs[ps->numpgcs++] = p;
    assert(p->pgcgroup == 0); /* should not already be in any group! */
    p->pgcgroup = ps;
  } /*pgcgroup_add_pgc*/

int pgcgroup_set_video_attr(struct pgcgroup *va,int attr,const char *s)
{
    return vobgroup_set_video_attr(va->pg_vg,attr,s);
}

int pgcgroup_set_audio_attr(struct pgcgroup *va,int attr,const char *s,int ch)
{
    return vobgroup_set_audio_attr(va->pg_vg,attr,s,ch);
}

int pgcgroup_set_subpic_attr(struct pgcgroup *va,int attr,const char *s,int ch)
{
    return vobgroup_set_subpic_attr(va->pg_vg,attr,s,ch);
}

int pgcgroup_set_subpic_stream(struct pgcgroup *va,int ch,const char *m,int id)
{
    return vobgroup_set_subpic_stream(va->pg_vg,ch,m,id);
}

struct menugroup *menugroup_new()
{
    struct menugroup *mg=malloc(sizeof(struct menugroup));
    memset(mg,0,sizeof(struct menugroup));
    mg->mg_vg = vobgroup_new();
    return mg;
}

void menugroup_free(struct menugroup *mg)
  {
    if (mg)
      {
        int i;
        if (mg->groups)
          {
            for (i = 0; i < mg->numgroups; i++)
                pgcgroup_free(mg->groups[i].pg);
            free(mg->groups);
          } /*if*/
        vobgroup_free(mg->mg_vg);
        free(mg);
      } /*if*/
  } /*menugroup_free*/

void menugroup_add_pgcgroup(struct menugroup *mg, const char *lang, struct pgcgroup *pg)
  {
    mg->groups = (struct langgroup *)realloc(mg->groups, (mg->numgroups + 1) * sizeof(struct langgroup));
    if (strlen(lang) != 2)
      {
        fprintf(stderr, "ERR:  Menu language '%s' is not two letters.\n", lang);
        exit(1);
      } /*if*/
  /* fixme: I don't check if there's already a langgroup with this language code? */
    mg->groups[mg->numgroups].lang[0] = tolower(lang[0]);
    mg->groups[mg->numgroups].lang[1] = tolower(lang[1]);
    mg->groups[mg->numgroups].lang[2] = 0;
    mg->groups[mg->numgroups].pg = pg;
    mg->numgroups++;
  } /*menugroup_add_pgcgroup*/

int menugroup_set_video_attr(struct menugroup *va,int attr,const char *s)
{
    return vobgroup_set_video_attr(va->mg_vg,attr,s);
}

int menugroup_set_audio_attr(struct menugroup *va,int attr,const char *s,int ch)
{
    return vobgroup_set_audio_attr(va->mg_vg,attr,s,ch);
}

int menugroup_set_subpic_attr(struct menugroup *va,int attr,const char *s,int ch)
{
    return vobgroup_set_subpic_attr(va->mg_vg,attr,s,ch);
}

int menugroup_set_subpic_stream(struct menugroup *va,int ch,const char *m,int id)
{
    return vobgroup_set_subpic_stream(va->mg_vg,ch,m,id);
}

void dvdauthor_enable_jumppad()
{
    if( allowallreg ) {
        fprintf(stderr,"ERR:  Cannot enable both allgprm and jumppad\n");
        exit(1);
    }
    jumppad = true;
}

void dvdauthor_enable_allgprm()
{
    if( jumppad ) {
        fprintf(stderr,"ERR:  Cannot enable both allgprm and jumppad\n");
        exit(1);
    }
    allowallreg = true;
}

void dvdauthor_vmgm_gen(struct pgc *fpc, struct menugroup *menus, const char *fbase)
  /* generates the VMG, taking into account all already-generated titlesets. */
  {
    DIR *d;
    struct dirent *de;
    char *vtsdir;
    int i;
    static struct toc_summary ts; /* static avoids having to initialize it! */
    static char fbuf[1000];
    static char ifonames[101][14];
    struct workset ws;

    if (!fbase) // can't really make a vmgm without titlesets
        return;
    ws.titlesets = &ts;
    ws.menus = menus;
    ws.titles = 0;
    jp_force_menu(menus, VTYPE_VMGM);
    for (i = 0; i < menus->numgroups; i++)
      {
        validatesummary(menus->groups[i].pg);
        pgcgroup_createvobs(menus->groups[i].pg, menus->mg_vg);
        forceaddentry(menus->groups[i].pg, 4); /* entry=title */
      } /*for*/
    fprintf(stderr, "INFO: dvdauthor creating table of contents\n");
    initdir(fbase);
    // create base entry, if not already existing
    memset(&ts, 0, sizeof(struct toc_summary));
    vtsdir = makevtsdir(fbase);
    for (i = 0; i < 101; i++)
        ifonames[i][0] = 0; /* mark all name entries as unused */
    d = opendir(vtsdir);
    while ((de = readdir(d)) != 0)
      {
      /* look for existing titlesets */
        i = strlen(de->d_name);
        if
          (
                i == 12
             &&
                !strcasecmp(de->d_name + i - 6, "_0.IFO")
             &&
                !strncasecmp(de->d_name, "VTS_", 4)
             /* name is of form VTS_nn_0.IFO */
          )
          {
            i = (de->d_name[4] - '0') * 10 + (de->d_name[5] - '0');
            if (ifonames[i][0]) /* title set nr already seen!? will actually never happen */
              {
                fprintf(stderr, "ERR:  Two different names for the same titleset: %s and %s\n",
                    ifonames[i], de->d_name);
                exit(1);
              } /*if*/
            if (!i)
              {
                fprintf(stderr,"ERR:  Cannot have titleset #0 (%s)\n", de->d_name);
                exit(1);
              } /*if*/
            strcpy(ifonames[i], de->d_name);
          } /*if*/
      } /*while*/
    closedir(d);
    for (i = 1; i <= 99; i++)
      {
        if (!ifonames[i][0])
            break;
        snprintf(fbuf, sizeof fbuf, "%s/%s", vtsdir, ifonames[i]);
        fprintf(stderr, "INFO: Scanning %s\n",fbuf);
        ScanIfo(&ts, fbuf); /* collect info about existing titleset for inclusion in new VMG IFO */
      } /*for*/
    for (; i <= 99; i++) /* look for discontiguously-assigned title nrs (error) */
        if (ifonames[i][0])
          {
            fprintf(stderr, "ERR:  Titleset #%d (%s) does not immediately follow the last titleset\n",i,ifonames[i]);
            exit(1);
          } /*if; for*/
    if (!ts.numvts)
      {
        fprintf(stderr, "ERR:  No .IFO files to process\n");
        exit(1);
      } /*if*/
    if (menus->mg_vg->numvobs != 0)
      {
        fprintf(stderr, "INFO: Creating menu for TOC\n");
        snprintf(fbuf, sizeof fbuf, "%s/VIDEO_TS.VOB", vtsdir);
        FindVobus(fbuf, menus->mg_vg, VTYPE_VMGM);
        MarkChapters(menus->mg_vg);
        setattr(menus->mg_vg, VTYPE_VMGM);
        fprintf(stderr, "\n");
        FixVobus(fbuf, menus->mg_vg, &ws, VTYPE_VMGM);
      }
    else
      /* unconditional because there will always be at least one PGC,
        namely the FPC (explicit or default) */
      {
        set_video_format_attr(menus->mg_vg, VTYPE_VMGM); /* for the sake of buildtimeeven */
      } /*if*/
  /* (re)generate VMG IFO */
    snprintf(fbuf, sizeof fbuf, "%s/VIDEO_TS.IFO", vtsdir);
    TocGen(&ws, fpc, fbuf);
    snprintf(fbuf, sizeof fbuf, "%s/VIDEO_TS.BUP", vtsdir); /* same thing again, backup copy */
    TocGen(&ws, fpc, fbuf);
    for (i = 0; i < ts.numvts; i++)
        if (ts.vts[i].numchapters)
            free(ts.vts[i].numchapters);
    free(vtsdir);
  } /*dvdauthor_vmgm_gen*/

void dvdauthor_vts_gen(struct menugroup *menus, struct pgcgroup *titles, const char *fbase)
  /* generates a VTS (titleset). */
  {
    int vtsnum, i;
    static char realfbase[1000];
    struct workset ws;

    fprintf(stderr, "INFO: dvdauthor creating VTS\n");
    initdir(fbase);
    ws.titlesets = 0;
    ws.menus = menus;
    ws.titles = titles;
    jp_force_menu(menus, VTYPE_VTSM);
    for (i = 0; i < menus->numgroups; i++)
      {
        validatesummary(menus->groups[i].pg);
        pgcgroup_createvobs(menus->groups[i].pg, menus->mg_vg);
        forceaddentry(menus->groups[i].pg, 0x80); /* entry=ptt? */
        checkaddentry(menus->groups[i].pg, 0x08); /* entry=root */
      } /*for*/
    validatesummary(titles);
    pgcgroup_createvobs(titles, titles->pg_vg);
    if (titles->numpgcs == 0)
      {
        fprintf(stderr, "ERR:  no titles defined\n");
        exit(1);
      } /*if*/
    vtsnum = getvtsnum(fbase);
    if (fbase)
      {
        snprintf(realfbase, sizeof realfbase, "%s/VIDEO_TS/VTS_%02d", fbase, vtsnum);
        fbase = realfbase;
      } /*if*/
    if (menus->mg_vg->numvobs != 0)
      {
        FindVobus(fbase, menus->mg_vg, VTYPE_VTSM);
        MarkChapters(menus->mg_vg);
        setattr(menus->mg_vg, VTYPE_VTSM);
      }
    else if (menus->mg_vg->numallpgcs != 0)
      {
        set_video_format_attr(menus->mg_vg, VTYPE_VTSM); /* for the sake of buildtimeeven */
      } /*if*/
    FindVobus(fbase, titles->pg_vg, VTYPE_VTS);
    MarkChapters(titles->pg_vg);
    setattr(titles->pg_vg, VTYPE_VTS);
    if (!menus->mg_vg->numvobs) // for undefined menus, we'll just copy the video type of the title
      {
        menus->mg_vg->vd = titles->pg_vg->vd;
      } /*if*/
    fprintf(stderr, "\n");
    WriteIFOs(fbase, &ws);
    if (menus->mg_vg->numvobs)
        FixVobus(fbase, menus->mg_vg, &ws, VTYPE_VTSM);
    FixVobus(fbase, titles->pg_vg, &ws, VTYPE_VTS);
  } /*dvdauthor_vts_gen*/
