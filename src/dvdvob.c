/*
    dvdauthor -- generation of .VOB files
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

#include "config.h"

#include "compat.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>

#include "dvdauthor.h"
#include "da-internal.h"



struct colorremap /* for remapping colours to indexes into a common palette */
  {
    int newcolors[16]; /* bit 24 is set to indicate a colour needs remapping */
    int state,curoffs,maxlen,nextoffs,skip,ln_ctli; /* state of SPU parser machine (procremap) */
    struct colorinfo *origmap; /* colours merged into common indexes into this palette */
  };

struct vscani {
    int lastrefsect; /* flag that last sector should be recorded as a reference sector */
    int firstgop; /* 1 => looking for first GOP, 2 => found first GOP, 0 => don't bother looking any more */
    int firsttemporal; /* first temporal sequence number seen in current sequence */
    int lastadjust; /* temporal sequence reset */
    int adjustfields; /* fixme: not used for anything? */
};

static pts_t timeline[19]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
                           20,60,120,240};
  /* various time steps for VOBU offsets needed in DSI packet, in units of half a second */

#define BIGWRITEBUFLEN (16*2048)
static unsigned char bigwritebuf[BIGWRITEBUFLEN];
static int writebufpos=0;
static int writefile=-1;

static unsigned char videoslidebuf[15]={255,255,255,255, 255,255,255, 0,0,0,0, 0,0,0,0};


/* The following are variants for the ways I've seen DVD's encoded */

// Grosse Pointe Blank uses exactly 1/2 second for the FF/REW records
// Bullitt uses 1/2 video second (i.e. in NTSC, 45045 PTS)
#define DVD_FFREW_HALFSEC 45000
// #define DVD_FFREW_HALFSEC (getratedenom(va)>>1)

static pts_t calcpts
  (
    const struct vobgroup *va,
    int cancomplain,
    int *didcomplain,
    pts_t *align,
    pts_t basepts,
    int nfields /* count of prior fields */
  )
  /* returns basepts aligned to a whole number of fields, offset by *align. */
  {
    // I assume pts should round down?  That seems to be how mplex deals with it
    // also see later comment
    const pts_t fpts = getframepts(va);
    const int bpframe = (basepts * 2 - *align + fpts / 2) / fpts; /* nearest whole field number */
    if ((*align + bpframe * fpts) / 2 != basepts)
      {
        if (!*didcomplain)
          {
            if (cancomplain)
                fprintf(stderr, "WARN: Video PTS does not line up on a multiple of a field.\n");
            *didcomplain = 1;
          } /*if*/
        *align = basepts * 2; /* assume this will avoid further warnings */
      }
    else
        nfields += bpframe;
    return (*align + nfields * fpts) / 2;
  } /*calcpts*/

static int findnextvideo(const struct vob *va, int cur, int dir)
  // find next (dir=1) or previous(dir=-1) vobu with video
  {
    int i, numvobus;
    numvobus = va->numvobus;
    switch(dir)
      {
    case 1:  // forward
        for (i = cur+1; i < numvobus; i++)
            if(va->vobu[i].hasvideo)
                return i;
        return -1;
    case -1: // backward
        for (i = cur-1; i > -1; i--)
            if(va->vobu[i].hasvideo)
                return i;
        return -1;
    default:
        // ??
        return -1;
      } /*switch*/
  } /*findnextvideo*/

static int findaudsect(const struct vob *va, int aind, pts_t pts0, pts_t pts1)
  /* finds the audpts entry, starting from aind, that includes the time pts0 .. pts1,
    or -1 if not found. */
  {
    const struct audchannel * const ach = &va->audch[aind];
    int l = 0, h = ach->numaudpts - 1;

    if (h < l)
        return -1;
    while (h > l)
      {
        const int m =(l + h + 1) / 2; /* binary search */
        if (pts0 < ach->audpts[m].pts[0])
            h = m - 1;
        else
            l = m;
      } /*while*/
    if (ach->audpts[l].pts[0] > pts1)
        return -1;
    return ach->audpts[l].asect;
  } /*findaudsect*/

static int findvobubysect(const struct vob *va, int sect)
  /* returns the index of the VOBU that spans the specified sector. */
  {
    int l = 0, h = va->numvobus - 1;
    if (h < 0)
        return -1;
    if (sect < va->vobu[0].sector)
        return -1;
    while (l < h)
      {
        const int m = (l + h + 1) / 2; /* binary search */
        if (sect < va->vobu[m].sector)
            h = m - 1;
        else
            l = m;
      } /*while*/
    return l;
  } /*findvobubysect*/

static int findspuidx(const struct vob *va, int ach, pts_t pts0)
  /* returns the index of the subpicture packet spanning the specified time. */
  {
    int l = 0, h = va->audch[ach].numaudpts - 1;
    if (h < l)
        return -1;
    while (h > l)
      {
        const int m = (l + h + 1) / 2; /* binary search */
        if (pts0 < va->audch[ach].audpts[m].pts[0])
            h = m - 1;
        else
            l = m;
      } /*while*/
    return l;
  } /*findspuidx*/

static unsigned int getsect
  (
    const struct vob * va,
    int curvobunum, /* the VOBU number I'm jumping from */
    int jumpvobunum, /* the VOBU number I'm jumping to */
    int skip, /* whether to set the skipping-more-than-one-VOBU bit */
    unsigned notfound /* what to return if there is no matching VOBU */
  )
  /* computes relative VOBU offsets needed at various points in a DSI packet,
    including the mask bit that indicates a backward jump, and optionally the
    one indicating skipping multiple video VOBUs as well. */
  {
    if (skip)
      {
        int l, h, i;
        // only set skip bit if one of the VOBU's from here to there contain video
      /* hmm, this page <http://www.mpucoder.com/DVD/dsi_pkt.html> doesn't say
        it has to contain video */
        if (curvobunum < jumpvobunum)
          {
            l = curvobunum + 1;
            h = jumpvobunum - 1;
          }
        else
          {
            l = jumpvobunum + 1;
            h = curvobunum - 1;
          } /*if*/
        for (i = l; i <= h; i++)
            if (va->vobu[i].hasvideo)
                break; /* found an in-between VOBU containing video */
        if (i <= h)
            skip = 0x40000000;
        else
            skip = 0;
      } /*if*/
    if
      (
          jumpvobunum < 0
      ||
          jumpvobunum >= va->numvobus
      || 
          va->vobu[jumpvobunum].vobcellid != va->vobu[curvobunum].vobcellid
            /* never cross cells */
      )
        return
            notfound | skip;
    return
            abs(va->vobu[jumpvobunum].sector - va->vobu[curvobunum].sector)
        |
            (va->vobu[jumpvobunum].hasvideo ? 0x80000000 : 0)
        |
            skip;
  } /*getsect*/

static pts_t readscr(const unsigned char *buf)
  /* returns the timestamp as found in the pack header. This is actually supposed to
    be units of a 27MHz clock, but I ignore the extra precision and truncate it to
    the usual 90kHz clock units. */
  {
    return
            ((pts_t)(buf[0] & 0x38)) << 27 /* SCR 32 .. 30 */
        |
            (buf[0] & 3) << 28 /* SCR 29 .. 28 */
        |
            buf[1] << 20 /* SCR 27 .. 20 */
        |
            (buf[2] & 0xf8) << 12 /* SCR 19 .. 15 */
        |
            (buf[2] & 3) << 13 /* SCR 14 .. 13 */
        |
            buf[3] << 5 /* SCR 12 .. 5 */
        |
            (buf[4] & 0xf8) >> 3; /* SCR 4 .. 0 */
          /* ignore SCR_ext */
  } /*readscr*/

static void writescr(unsigned char *buf, pts_t scr)
  /* writes a new timestamp for a pack header, ignoring the additional 27MHz
    precision. */
  {
    buf[0] = ((scr >> 27) & 0x38) | ((scr >> 28) & 3) | 68;
    buf[1] = scr >> 20;
    buf[2] = ((scr >> 12) & 0xf8) | ((scr >> 13) & 3) | 4;
    buf[3] = scr >> 5;
    buf[4] = ((scr << 3) & 0xf8) | (buf[4] & 7);
  } /*writescr*/

static pts_t readpts(const unsigned char *buf)
  /* reads a timestamp from a PES header as expressed in 90kHz clock units. */
  {
    int a1, a2, a3;
    a1 = (buf[0] & 0xe) >> 1;
    a2 = ((buf[1] << 8) | buf[2]) >> 1;
    a3 = ((buf[3] << 8) | buf[4]) >> 1;
    return
            ((pts_t)a1) << 30
        |
            a2 << 15
        |
            a3;
  } /*readpts*/

static void writepts(unsigned char *buf, pts_t pts)
  /* writes a timestamp to a PES header as expressed in 90kHz clock units. */
  {
    buf[0] = ((pts >> 29) & 0xe) | (buf[0] & 0xf1);
      // this preserves the PTS / DTS / PTSwDTS top bits
    write2(buf + 1, (pts >> 14) | 1);
    write2(buf + 3, (pts << 1) | 1);
  } /*writepts*/

static int findbutton(const struct pgc *pg, const char *dest, int dflt)
  /* returns the index of the button with name dest, or dflt if not found or no name specified. */
  {
    int i;
    if (!dest)
        return dflt;
    for (i = 0; i < pg->numbuttons; i++)
        if (!strcmp(pg->buttons[i].name,dest))
            return i + 1;
    return dflt;
  } /*findbutton*/

static void transpose_ts(unsigned char *buf, pts_t tsoffs)
  /* adjusts the timestamps in the specified PACK header and its constituent packets
    by the specified amount. */
  {
    // pack scr
    if
      (
            buf[0] == 0
        &&
            buf[1] == 0
        &&
            buf[2] == 1
        &&
            buf[3] == 0xba /* PACK header */
      )
    {
        writescr(buf + 4, readscr(buf + 4) + tsoffs);
        // video/audio?
        // pts?
        if
          (
                buf[14] == 0
            &&
                buf[15] == 0
            &&
                buf[16] == 1
            &&
                (
                    buf[17] == 0xbd /* private stream 1 */
                ||
                    buf[17] >= 0xc0 && buf[17] <= 0xef /* audio or video stream */
                )
            &&
                (buf[21] & 128) /* PTS present */
          )
          {
            writepts(buf + 23, readpts(buf + 23) + tsoffs);
            // dts?
            if (buf[21] & 64) /* decoder timestamp present */
              {
                writepts(buf + 28, readpts(buf + 28) + tsoffs);
              } /*if*/
          } /*if*/
    }
  } /*transpose_ts*/

static int mpa_valid(const unsigned char *b)
{
    const unsigned int v = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
    int t;

    // sync, mpeg1, layer2, 48khz
    if ((v & 0xFFFE0C00) != 0xFFFC0400)
        return 0;
    // bitrate 1..14
    t = (v >> 12) & 15;
    if (t == 0 || t == 15)
        return 0;
    // emphasis reserved
    if ((v & 3) == 2)
        return 0;
    return 1;
}

static int mpa_len(const unsigned char *b)
{
    static int bitratetable[16]={0,32,48,56,64,80,96,112,128,160,192,224,256,320,384,0};
    int padding=(b[2]>>1)&1;
    int bitrate=bitratetable[(b[2]>>4)&15];
    
    return 3*bitrate+padding; // 144*bitrate/sampling; 144/48=3
}

static void writeflush()
  /* writes out the data buffered so far. */
  {
    if (!writebufpos)
        return;
    if (writefile != -1)
      {
        if (write(writefile, bigwritebuf, writebufpos) != writebufpos)
          {
            fprintf(stderr,"ERR:  Error writing data\n"); /* fixme: report error code? */
            exit(1);
          } /*if*/
      } /*if*/
    writebufpos = 0;
  } /*writeflush*/

static unsigned char *writegrabbuf()
{
    unsigned char *buf;
    if( writebufpos == BIGWRITEBUFLEN )
        writeflush();
    buf=bigwritebuf+writebufpos;
    writebufpos+=2048;
    return buf;
}

static void writeundo()
  /* drops the last sector from the output buffer. */
  {
    writebufpos -= 2048;
  } /*writeundo*/

static void writeclose()
  /* flushes and closes the output file. */
  {
    writeflush();
    if (writefile != -1)
      {
        close(writefile);
        writefile = -1;
      } /*if*/
  } /*writeclose*/

static void writeopen(const char *newname)
  /* opens an output file for writing. */
  {
    writefile = open(newname, O_CREAT | O_WRONLY | O_BINARY, 0666);
    if (writefile < 0)
      {
        fprintf(stderr, "ERR:  Error %d opening %s: %s\n", errno, newname, strerror(errno));
        exit(1);
      } /*if*/
  }

static void closelastref(struct vobuinfo *thisvi, struct vscani *vsi, int cursect)
  /* collects another end-sector of another reference frame, if I don't have enough already. */
  {
    if (vsi->lastrefsect && thisvi->numref < 3)
      {
        thisvi->lastrefsect[thisvi->numref++] = cursect;
        vsi->lastrefsect = 0;
      } /*if*/
  } /*closelastref*/

// this function is allowed to update buf[7] and guarantee it will end up
// in the output stream
// prevbytesect is the sector for the byte immediately preceding buf[0]
static void scanvideoptr
  (
    struct vobgroup *va,
    unsigned char *buf,
    struct vobuinfo *thisvi,
    int prevbytesect,
    struct vscani *vsi
  )
  {
    if
      (
            buf[0] == 0
        &&
            buf[1] == 0
        &&
            buf[2] == 1
      )
      {
        switch(buf[3])
          {
        case 0: /* picture header */
          {
            const int ptype = (buf[5] >> 3) & 7; /* frame type, 1 => I, 2 => P, 3 => B, 4 => D */
            const int temporal = (buf[4] << 2) | (buf[5] >> 6); /* temporal sequence number */
            closelastref(thisvi, vsi, prevbytesect);
            if (vsi->firsttemporal == -1)
                vsi->firsttemporal = temporal;
            vsi->lastadjust = (temporal < vsi->firsttemporal);
            if (ptype == 1 || ptype == 2) // I or P frame
                vsi->lastrefsect = 1; /* it's a reference frame */
            if (va->vd.vmpeg == VM_MPEG1)
              {
                thisvi->numfields += 2;
                if (vsi->lastadjust && vsi->firstgop == 2)
                    thisvi->firstIfield += 2;
              } /*if*/

            // fprintf(stderr,"INFO: frame type %d, tempref=%d, prevsect=%d\n",ptype,temporal,prevbytesect);
        break;
          } /*case 0*/

        case 0xb3: /* sequence header */
          {
          /* collect information about video attributes */
            int hsize, vsize, aspect, framerate, newaspect;
            char sizestring[30];
            closelastref(thisvi, vsi, prevbytesect);
            hsize = (buf[4] << 4) | (buf[5] >> 4);
            vsize = ((buf[5] << 8) & 0xf00) | buf[6];
            aspect = buf[7] >> 4;
            framerate = buf[7] & 0xf;

            vobgroup_set_video_framerate(va, framerate);
            switch(framerate)
              {
            case 1: // 24000/1001
            case 4: // 30000/1001
            case 7: // 60000/1001
                vobgroup_set_video_attr(va, VIDEO_FORMAT, "ntsc");
            break;

            case 3: // 25
            case 6: // 50
                vobgroup_set_video_attr(va, VIDEO_FORMAT, "pal");
            break;

            case 2: // 24
            case 5: // 30
            case 8: // 60
               // these are nonstandard, but at least we know what they are
            break;

            default:
                fprintf(stderr, "WARN: unknown frame rate %d\n", framerate);
            break;
              } /* switch(framerate) */
            sprintf(sizestring, "%dx%d", hsize, vsize);
            vobgroup_set_video_attr(va, VIDEO_RESOLUTION, sizestring);
            if (va->vd.vmpeg == VM_MPEG1)
              {
                switch (aspect)
                  {
                case 3:
                    vobgroup_set_video_attr(va, VIDEO_ASPECT, "16:9");
                    vobgroup_set_video_attr(va, VIDEO_FORMAT, "pal");
                break;
                case 6:
                    vobgroup_set_video_attr(va, VIDEO_ASPECT, "16:9");
                    vobgroup_set_video_attr(va, VIDEO_FORMAT, "ntsc");
                break;
                case 8:
                    vobgroup_set_video_attr(va, VIDEO_ASPECT, "4:3");
                    vobgroup_set_video_attr(va, VIDEO_FORMAT, "pal");
                break;
                case 12:
                    vobgroup_set_video_attr(va, VIDEO_ASPECT, "4:3");
                    vobgroup_set_video_attr(va, VIDEO_FORMAT, "ntsc");
                break;
                default:
                    fprintf(stderr,"WARN: unknown mpeg1 aspect ratio %d\n",aspect);
                break;
                  } /*switch*/
                newaspect = 
                        3
                    +
                        (va->vd.vaspect == VA_4x3) * 5
                    +
                        (va->vd.vformat == VF_NTSC) * 3;
                if (newaspect == 11)
                    newaspect++;
                buf[7] = (buf[7] & 0xf) | (newaspect << 4); // reset the aspect ratio
              }
            else if (va->vd.vmpeg == VM_MPEG2)
              {
                if (aspect == 2)
                    vobgroup_set_video_attr(va, VIDEO_ASPECT, "4:3");
                else if (aspect == 3)
                    vobgroup_set_video_attr(va, VIDEO_ASPECT, "16:9");
                else
                    fprintf(stderr, "WARN: unknown mpeg2 aspect ratio %d\n", aspect);
                buf[7] = (buf[7] & 0xf) | (va->vd.vaspect == VA_4x3 ? 2 : 3) << 4;
                  // reset the aspect ratio
              } /*if*/
            break;
          } /* case 0xb3 */

        case 0xb5: /* extension header */
          {
            vobgroup_set_video_attr(va, VIDEO_MPEG, "mpeg2");
            switch (buf[4] & 0xF0)
              {
            case 0x10: // sequence extension
                closelastref(thisvi, vsi, prevbytesect);
            break;

            case 0x20: // sequence display extension
                closelastref(thisvi, vsi, prevbytesect);
                switch (buf[4] & 0xE) /* video format */
                  {
                case 2:
                    vobgroup_set_video_attr(va, VIDEO_FORMAT, "pal");
                break;
                case 4:
                    vobgroup_set_video_attr(va, VIDEO_FORMAT, "ntsc");
                break;
                // case 6: // secam
                // case 10: // unspecified
                  }
            break;

            case 0x80: // picture coding extension
              {
                int padj = 1; // default field pic
                if ((buf[6] & 3) /* picture structure */ == 3)
                    padj++; // adj for frame pic
                if (buf[7] & 2) /* repeat first field */
                    padj++; // adj for repeat flag
                thisvi->numfields += padj;
                if (vsi->lastadjust && vsi->firstgop == 2)
                    thisvi->firstIfield += padj;
                // fprintf(stderr,"INFO: repeat flag=%d, cursect=%d\n",buf[7]&2,cursect);
              }
            break;
              } /*switch*/
        break;
          } /* case 0xb5 */
            
        case 0xb7: /* sequence end */
          {
            thisvi->hasseqend = 1;
        break;
          } /* case 0xb7 */

        case 0xb8: // gop header
            closelastref(thisvi, vsi, prevbytesect);
            if (vsi->firstgop == 1)
              {
                vsi->firstgop = 2; /* found first GOP */
                vsi->firsttemporal = -1;
                vsi->lastadjust = 0;
                vsi->adjustfields = 0;
              }
            else if (vsi->firstgop == 2)
              {
                vsi->firstgop = 0; /* no need to find any more GOPs */
              } /*if*/
            if (0)
              {
                int hr, mi, se, fr;
                hr = (buf[4] >> 2) & 31;
                mi = ((buf[4] & 3) << 4) | (buf[5] >> 4);
                se = ((buf[5] & 7) << 3) | (buf[6] >> 5);
                fr = ((buf[6] & 31) << 1) | (buf[7] >> 7);
                fprintf
                  (
                    stderr,
                    "INFO: GOP header, %d:%02d:%02d:%02d, drop=%d\n",
                    hr, mi, se, fr, buf[4] >> 7
                  );
              } /*if*/
        break;
          } /*switch*/
    } /*if*/
  } /*scanvideoptr*/

static void scanvideoframe(struct vobgroup *va, unsigned char *buf, struct vobuinfo *thisvi, int cursect, int prevsect, struct vscani *vsi)
 {
    int i, f = 0x17 + buf[0x16], l = 0x14 + buf[0x12] * 256 + buf[0x13];
    int mpf;
    struct vobuinfo oldtvi;
    struct vscani oldvsi;
    if (l - f < 8)
      {
        memcpy(videoslidebuf + 7, buf + f, l - f);
        for (i = 0; i < l - f; i++)
            scanvideoptr(va, videoslidebuf + i, thisvi, prevsect, vsi);
        memcpy(buf + f, videoslidebuf + 7, l - f);
        memset(videoslidebuf, 255, 7);
        return;
      } /*if*/
 rescan:
    mpf = va->vd.vmpeg;
    oldtvi = *thisvi;
    oldvsi = *vsi;
    // copy the first 7 bytes to use with the prev 7 bytes in hdr detection
    memcpy(videoslidebuf + 7, buf + f, 8); // we scan the first header using the slide buffer
    for (i = 0; i <= 7; i++)
        scanvideoptr(va, videoslidebuf + i, thisvi, prevsect, vsi);
    memcpy(buf + f, videoslidebuf + 7, 8);
    // quickly scan all but the last 7 bytes for a hdr
    // buf[f]... was already scanned in the videoslidebuffer to give the correct sector
    for (i = f + 1; i < l - 7; i++)
      {
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)
            scanvideoptr(va, buf + i, thisvi, cursect, vsi);
      } /*for*/
    if (!va->vd.vmpeg)
        vobgroup_set_video_attr(va, VIDEO_MPEG, "mpeg1");
    // if the mpeg version changed, then rerun scanvideoframe, because
    // scanvideoptr updates the aspect ratio in the sequence header
    if (mpf != va->vd.vmpeg)
      {
        *thisvi = oldtvi; // we must undo all the frame pointer changes
        *vsi = oldvsi;
        goto rescan;
      } /*if*/
    // use the last 7 bytes in the next iteration
    memcpy(videoslidebuf, buf + l - 7, 7);
  } /*scanvideoframe*/

static void finishvideoscan(struct vobgroup *va, int vob, int prevsect, struct vscani *vsi)
  {
    struct vobuinfo * const lastvi = &va->vobs[vob]->vobu[va->vobs[vob]->numvobus - 1];
    int i;
    memset(videoslidebuf + 7, 0, 7);
    for (i = 0; i < 7; i++)
        scanvideoptr(va, videoslidebuf + i, lastvi, prevsect, vsi);
    memset(videoslidebuf, 255, 7);
    closelastref(lastvi, vsi, prevsect);
  } /*finishvideoscan*/

static void printpts(pts_t pts)
  /* displays a PTS value in seconds. */
  {
    fprintf(stderr,"%d.%03d",(int)(pts/90000),(int)((pts/90)%1000));
  }

enum { CR_BEGIN0,   CR_BEGIN1,    CR_BEGIN2,    CR_BEGIN3, CR_SKIP0,
       CR_SKIP1,    CR_NEXTOFFS0, CR_NEXTOFFS1, CR_WAIT,   CR_CMD,
       CR_SKIPWAIT, CR_COL0,      CR_COL1,      CR_CHGARG, CR_CHGLN0,
       CR_CHGLN1,   CR_CHGLN2,    CR_CHGLN3,    CR_CHGPX0, CR_CHGPX1,
       CR_CHGPX2 };

static char *readpstr(const unsigned char *b, int *i)
/* extracts a null-terminated string beginning at b[*i], advances *i past it and returns
a copy of the string. */
  {
    char *s = strdup((const char *)b + i[0]);
    i[0] += strlen(s) + 1;
    return s;
  } /*readpstr*/

static void initremap(struct colorremap *cr)
  /* initializes the remap table to all identity mappings, and the state machine
    ready to start processing a new SPU. */
  {
    int i;
    for (i = 0; i < 16; i++) /* initially don't remap any colours */
        cr->newcolors[i] = i;
    cr->state = CR_BEGIN0;
    cr->origmap = 0;
  } /*initremap*/

static int remapcolor(struct colorremap *cr, int idx)
  /* returns the appropriate remapping of the colour with the specified index
    to the corresponding index in cr->origmap. */
  {
    int i, nc;
    if (cr->newcolors[idx] < 16)
        return
            cr->newcolors[idx]; /* remapping already worked out */
    nc = cr->newcolors[idx] & 0xffffff; /* get colour to be remapped */
    for (i = 0; i < 16; i++) /* find existing entry, if any */
        if (cr->origmap->color[i] == nc) /* got one */
          {
            cr->newcolors[idx] = i;
            return
                i;
          } /*if; for*/
    for (i = 0; i < 16; i++)
      /* allocate a new entry in origmap for it, if there is room */
        if (cr->origmap->color[i] == COLOR_UNUSED)
          {
            cr->origmap->color[i] = nc;
            cr->newcolors[idx] = i;
            return
                i;
          } /*if; for */
    fprintf(stderr, "ERR: color map full, unable to allocate new colors.\n");
    exit(1);
  } /*remapcolor*/

static void remapbyte(struct colorremap *cr, unsigned char *b)
  /* remaps the two colours in the two nibbles of *b. */
  {
    b[0] = remapcolor(cr, b[0] & 15) | (remapcolor(cr, b[0] >> 4) << 4);
  } /*remapbyte*/

static void procremap
  (
    struct colorremap *cr,
    unsigned char *b,
    int blen, /* length of SPU chunk beginning at b */
    pts_t *timespan /* has duration of SPU display added to it */
  )
  /* interprets the subpicture stream in order to pick up the colour information so it
    can be remapped. */
  {
    while(blen)
      {
        // fprintf(stderr,"INFO: state=%d, byte=%02x (%d)\n",cr->state,*b,*b);
        switch(cr->state)
          {
        case CR_BEGIN0: /* pick up high byte of Sub-Picture Unit length */
            cr->curoffs = 0;
            cr->skip = 0;
            cr->maxlen = b[0] * 256;
            cr->state = CR_BEGIN1;
        break;
        case CR_BEGIN1: /* pick up low byte of Sub-Picture Unit length */
            cr->maxlen += b[0];
            cr->state = CR_BEGIN2;
        break;
        case CR_BEGIN2: /* pick up high byte of offset to SP_DCSQT */
            cr->nextoffs = b[0] * 256;
            cr->state = CR_BEGIN3;
        break;
        case CR_BEGIN3: /* pick up low byte of offset to SP_DCSQT */
            cr->nextoffs += b[0];
            cr->state = CR_WAIT;
        break;
        case CR_WAIT: /* skipping bytes until start of SP_DCSQT */
            if (cr->curoffs == cr->maxlen)
              {
                cr->state = CR_BEGIN0; /* finished this SPU */
                continue; // execute BEGIN0 right away
              } /*if*/
            if (cr->curoffs != cr->nextoffs)
                break;
            cr->state = CR_SKIP0; /* found start of SP_DCSQT */
        // fall through to CR_SKIP0
        case CR_SKIP0: /* pick up high byte of SP_DCSQ_TM */
            *timespan += 1024 * b[0] * 256;
            cr->state = CR_SKIP1;
        break;
        case CR_SKIP1: /* pick up low byte of SP_DCSQ_TM */
            *timespan += 1024 * b[0];
            cr->state = CR_NEXTOFFS0;
        break;
        case CR_NEXTOFFS0: /* pick up high byte of offset to next SP_DCSQ */
            cr->nextoffs = b[0] * 256;
            cr->state = CR_NEXTOFFS1;
        break;
        case CR_NEXTOFFS1: /* pick up low byte of offset to next SP_DCSQ */
            cr->nextoffs += b[0];
            cr->state = CR_CMD; /* expecting first command */
        break;
        case CR_SKIPWAIT: /* skipping rest of current command */
            if (cr->skip)
              {
                cr->skip--;
                break;
              } /*if*/
            cr->state = CR_CMD;
        // fall through to CR_CMD
        case CR_CMD: /* pick up start of next command */
            switch (*b)
              {
            case 0: /* FSTA_DSP */
            case 1: /* STA_DSP */
            case 2: /* STP_DSP */
              /* nothing to do */
            break;
            case 3: /* SET_COLOR */
                cr->state = CR_COL0;
            break;
            case 4: /* SET_CONTR */
                cr->skip = 2; /* no need to look at this */
                cr->state = CR_SKIPWAIT;
            break;
            case 5: /* SET_DAREA */
                cr->skip = 6; /* no need to look at this */
                cr->state = CR_SKIPWAIT;
            break;
            case 6: /* SET_DSPXA */
                cr->skip = 4; /* no need to look at this */
                cr->state = CR_SKIPWAIT;
            break;
            case 7: /* CHG_COLCON */
                cr->skip = 2; /* skip size of parameter area */
                cr->state = CR_CHGARG;
            break;
            case 255: /* CMD_END */
                cr->state = CR_WAIT; /* end of SP_DCSQ */
            break;
            default:
                fprintf(stderr, "ERR: procremap encountered unknown subtitle command: %d\n",*b);
                exit(1);
              } /*switch*/
        break;
        case CR_COL0: /* first and second of four colours for SET_COLOR */
            remapbyte(cr, b);
            cr->state = CR_COL1;
        break;
        case CR_COL1: /* third and fourth of four colours for SET_COLOR */
            remapbyte(cr, b);
            cr->state = CR_CMD;
        break;
        case CR_CHGARG: /* skipping size word for CHG_COLCON */
            if (!--cr->skip)
                cr->state = CR_CHGLN0;
        break;
        case CR_CHGLN0: /* expecting first byte of LN_CTLI subcommand for CHG_COLCON */
            cr->ln_ctli = b[0] << 24;
            cr->state = CR_CHGLN1;
        break;
        case CR_CHGLN1:
            cr->ln_ctli |= b[0] << 16;
            cr->state = CR_CHGLN2;
        break;
        case CR_CHGLN2:
            cr->ln_ctli |= b[0] << 8;
            cr->state = CR_CHGLN3;
        break;
        case CR_CHGLN3:
            cr->ln_ctli |= b[0]; /* got complete four bytes at start of LN_CTLI */
            if (cr->ln_ctli == 0x0fffffff) /* end of CHG_COLCON parameter area */
                cr->state = CR_CMD;
            else
              {
                cr->ln_ctli >>= 12;
                cr->ln_ctli &= 0xf; /* number of PX_CTLI to follow */
                cr->skip = 2;
                cr->state = CR_CHGPX0;
              } /*if*/
        break;
        case CR_CHGPX0: /* expecting starting column nr for PX_CTLI subcommand for CHG_COLCON */
            if (!--cr->skip)
              {
                cr->skip = 2;
                cr->state = CR_CHGPX1;
              } /*if*/
        break;
        case CR_CHGPX1: /* expecting new colour values for PX_CTLI subcommand */
            remapbyte(cr, b);
            if (!--cr->skip)
              {
                cr->skip = 2;
                cr->state = CR_CHGPX2;
              } /*if*/
        break;
        case CR_CHGPX2: /* expecting new contrast values for PX_CTLI subcommand */
            if (!--cr->skip)
              {
              /* done this PX_CTLI */
                if (!--cr->ln_ctli)
                    cr->state = CR_CHGLN0; /* done this LN_CTLI */
                else
                  {
                    cr->skip = 2;
                    cr->state = CR_CHGPX0; /* next PX_CTLI in this LN_CTLI */
                  } /*if*/
              } /*if*/
        break;
        default: /* shouldn't occur */
            assert(0);
          } /*switch*/
        cr->curoffs++;
        b++;
        blen--;
      } /*while*/
  } /*procremap*/

static void printvobustatus(struct vobgroup *va, int cursect)
  /* report total number of VOBUs and PGCs seen so far, and how much of the
    input file has been processed. */
  {
    int j, nv = 0;
    for (j = 0; j < va->numvobs; j++)
        nv += va->vobs[j]->numvobus;
    // fprintf(stderr, "STAT: VOBU %d at %dMB, %d PGCs, %d:%02d:%02d\r", nv, cursect / 512, va->numallpgcs, total / 324000000, (total % 324000000) / 5400000, (total % 5400000) / 90000);
    fprintf(stderr, "STAT: VOBU %d at %dMB, %d PGCs\r", nv, cursect / 512, va->numallpgcs);
  } /*printvobustatus*/

static void audio_scan_ac3(struct audchannel *ach, const unsigned char *buf, int sof, int len)
  /* gets information about AC3 audio. */
  {
    uint32_t parse;
    int acmod,lfeon,nch=0;
    char attr[4];

    if( sof+8>len ) // check if there's room to parse all the interesting info
        return;
    if( buf[sof]!=0x0b || buf[sof+1]!=0x77 ) // verify ac3 sync
        return;
    parse=read4(buf+sof+4);
    if( (parse>>30)!=0 ) { // must be 48kHz
        fprintf(stderr,"WARN: Unknown AC3 sample rate: %d\n",parse>>30);
    }
    audiodesc_set_audio_attr(&ach->ad,&ach->adwarn,AUDIO_SAMPLERATE,"48khz");
  /* now figure out how many channels are present ... (yes, all this code) */
    parse<<=8;
    // check bsid
    if( (parse>>27)!=8 && (parse>>27)!=6 ) // must be v6 or v8
        return;
    parse<<=5;
    // ignore bsmod
    parse<<=3;
    // acmod gives # of channels
    acmod=(parse>>29);
    parse<<=3;
    if( (acmod&1) && (acmod!=1) ) /* centre channel present */
        parse<<=2; /* skip cmixlev */
    if( acmod&4 ) /* surround channel(s) present */
        parse<<=2; /* skip surmixlev */
    if( acmod==2 ) { /* simple stereo */
      /* process dsurmod */
        if( (parse>>30)==2 ) /* 2 => Dolby Surround encoded, 1 => not encoded, 0 => not indicated, 3 => reserved */
            audiodesc_set_audio_attr(&ach->ad,&ach->adwarn,AUDIO_DOLBY,"surround");
        // else if( (parse>>30)==1 )
        // audiodesc_set_audio_attr(&ach->ad,&ach->adwarn,AUDIO_DOLBY,"nosurround");
        parse<<=2;
    }
    lfeon=(parse>>31); /* low-frequency effects on */
    // calc # channels
    switch(acmod) {
    case 0: nch=2; break;
    case 1: nch=1; break;
    case 2: nch=2; break;
    case 3: nch=3; break;
    case 4: nch=3; break;
    case 5: nch=4; break;
    case 6: nch=4; break;
    case 7: nch=5; break;
    }
    if( lfeon ) nch++; /* include LFE channel */
    sprintf(attr,"%dch",nch);
    audiodesc_set_audio_attr(&ach->ad,&ach->adwarn,AUDIO_CHANNELS,attr);
  } /*audio_scan_ac3*/

static void audio_scan_dts(struct audchannel *ach,const unsigned char *buf,int sof,int len)
  /* gets information about DTS audio. */
{
/* fixme: could determine number of channels and sampling rate, but I'm not bothering for now */
}

static void audio_scan_pcm(struct audchannel *ach,const unsigned char *buf,int len)
  /* gets information about LPCM audio. */
{
    char attr[6];

    switch(buf[1]>>6) {
    case 0: audiodesc_set_audio_attr(&ach->ad,&ach->adwarn,AUDIO_QUANT,"16bps"); break;
    case 1: audiodesc_set_audio_attr(&ach->ad,&ach->adwarn,AUDIO_QUANT,"20bps"); break;
    case 2: audiodesc_set_audio_attr(&ach->ad,&ach->adwarn,AUDIO_QUANT,"24bps"); break;
  /* case 3: illegal */
    }
    sprintf(attr,"%dkhz",48*(1+((buf[1]>>4)&1))); /* 48 or 96kHz */
    audiodesc_set_audio_attr(&ach->ad,&ach->adwarn,AUDIO_SAMPLERATE,attr);
    sprintf(attr,"%dch",(buf[1]&7)+1); /* nr channels */
    audiodesc_set_audio_attr(&ach->ad,&ach->adwarn,AUDIO_CHANNELS,attr);
}

int FindVobus(const char *fbase, struct vobgroup *va, vtypes ismenu)
  /* collects audio/video/subpicture information, remaps subpicture colours and generates
    output VOB files for a menu or titleset, complete except for the NAV packs. */
  {
    unsigned char *buf;
    int cursect = 0; /* sector nr in input file */
    int fsect = -1; /* sector nr in current output VOB file, -ve => not opened yet */
    int vnum;
    int outnum = -(int)ismenu + 1; /* +ve for a titleset, in which case used to generate output VOB file names */
    int vobid =0;
    struct mp2info
      {
        int hdrptr;
        unsigned char buf[6];
      } mp2hdr[8]; /* enough for the allowed 8 audio streams */
    struct colorremap *crs;
    
    crs = malloc(sizeof(struct colorremap) * 32); /* enough for 32 subpicture streams */
    for (vnum = 0; vnum < va->numvobs; vnum++)
      {
        int i, j;
        int hadfirstvobu = 0;
        pts_t backoffs = 0, lastscr = 0;
        struct vob * const thisvob = va->vobs[vnum];
        int prevvidsect = -1;
        struct vscani vsi;
        struct vfile vf;
        vsi.lastrefsect = 0;
        for (i = 0; i < 32; i++)
            initremap(crs + i);

        vobid++;
        thisvob->vobid = vobid;
        vsi.firstgop = 1;

        fprintf(stderr, "\nSTAT: Processing %s...\n", thisvob->fname);
        vf = varied_open(thisvob->fname, O_RDONLY, "input video file");
        memset(mp2hdr, 0, 8 * sizeof(struct mp2info));
        while(1)
          {
            if (fsect == 524272)
              {
              /* VOB file reached maximum allowed size */
                writeclose();
                if (outnum <= 0)
                  { /* menu VOB cannot be split */
                    fprintf(stderr, "\nERR:  Menu VOB reached 1gb\n");
                    exit(1);
                  } /*if*/
                outnum++; /* for naming next VOB file */
                fsect = -1;
              } /*if*/
            buf = writegrabbuf();
            i = fread(buf, 1, 2048, vf.h);
            if (i != 2048)
              {
                if (i == -1)
                  {
                    fprintf(stderr, "\nERR:  Error %d while reading: %s\n", errno, strerror(errno));
                    exit(1);
                  }
                else if (i > 0) /* shouldn't occur */
                    fprintf(stderr ,"\nWARN: Partial sector read (%d bytes); discarding data.\n", i);
                writeundo();
                break;
              } /*if*/
            if
              (
                    buf[14] == 0
                &&
                    buf[15] == 0
                &&
                    buf[16] == 1
                &&
                    buf[17] == 0xbe /* padding stream */
                &&
                    !strcmp((const char *)buf + 20, "dvdauthor-data") /* message from spumux */
              )
              {
                // private dvdauthor data, interpret and remove from final stream
                int i = 35;
                if (buf[i] != 2)
                  {
                    fprintf(stderr, "ERR: dvd info packet is version %d\n", buf[i]);
                    exit(1);
                  } /*if*/
                switch (buf[i + 1]) // packet type
                  {
                case 1: // subtitle/menu color and button information
                  {
                    int substreamid = buf[i + 2] & 31;
                    i += 3;
                    i += 8; // skip start pts and end pts
                    while (buf[i] != 0xff)
                      {
                        switch (buf[i])
                          {
                        case 1: // new colormap
                          {
                            int j;
                            crs[substreamid].origmap = thisvob->progchain->colors;
                              /* where to merge colours into */
                            for (j = 0; j < buf[i + 1]; j++)
                              {
                              /* collect colours needing remapping, which won't happen
                                until they're actually referenced */
                                crs[substreamid].newcolors[j] =
                                        COLOR_UNUSED /* indicate colour needs remapping */
                                    |
                                        buf[i + 2 + 3 * j] << 16
                                    |
                                        buf[i + 3 + 3 * j] << 8
                                    |
                                        buf[i + 4 + 3 * j];
                              } /*for*/
                            for (; j < 16; j++) /* fill in unused entries with identity mapping */
                                crs[substreamid].newcolors[j] = j;
                            i += 2 + 3 * buf[i + 1];
                          }
                        break;
                        case 2: // new buttoncoli
                          {
                            int j;
                            memcpy(thisvob->buttoncoli, buf + i + 2, buf[i + 1] * 8);
                            for (j = 0; j < buf[i + 1]; j++)
                              {
                              /* remap the colours, not the contrast values */
                                remapbyte(&crs[substreamid], thisvob->buttoncoli + j * 8 + 0);
                                remapbyte(&crs[substreamid], thisvob->buttoncoli + j * 8 + 1);
                                remapbyte(&crs[substreamid], thisvob->buttoncoli + j * 8 + 4);
                                remapbyte(&crs[substreamid], thisvob->buttoncoli + j * 8 + 5);
                              } /*for*/
                            i += 2 + 8 * buf[i + 1];
                          }
                        break;
                        case 3: // button position information
                          {
                            int j;
                            const int nrbuttons = buf[i + 1];
                            i += 2;
                            for (j = 0; j < nrbuttons; j++)
                              {
                                struct button * b;
                                struct buttoninfo * bi, bitmp;
                                char * const bn = readpstr(buf, &i);
                                if (!findbutton(thisvob->progchain, bn, 0))
                                  {
                                    fprintf
                                      (
                                        stderr,
                                        "ERR:  Cannot find button '%s' as referenced by"
                                            " the subtitle\n",
                                        bn
                                      );
                                    exit(1);
                                  } /*if*/
                                b = &thisvob->progchain->buttons[findbutton(thisvob->progchain, bn, 0) - 1];
                                free(bn);

                                if (b->numstream >= MAXBUTTONSTREAM)
                                  {
                                    fprintf
                                      (
                                        stderr,
                                        "WARN: Too many button streams; ignoring buttons\n"
                                      );
                                    bi = &bitmp; /* place to put discarded data */
                                  }
                                else
                                  {
                                    bi = &b->stream[b->numstream++];
                                  } /*if*/
                                bi->substreamid = substreamid;
                                i += 2; // skip modifier
                                bi->autoaction = buf[i++];
                                bi->grp = buf[i];
                                bi->x1 = read2(buf + i + 1);
                                bi->y1 = read2(buf + i + 3);
                                bi->x2 = read2(buf + i + 5);
                                bi->y2 = read2(buf + i + 7);
                                i += 9;
                              /* neighbouring button names */
                                bi->up = readpstr(buf, &i);
                                bi->down = readpstr(buf, &i);
                                bi->left = readpstr(buf, &i);
                                bi->right = readpstr(buf, &i);
                              } /*for*/
                          } /*case 3*/
                        break;
                        default:
                            fprintf
                              (
                                stderr,
                                "ERR: dvd info packet command within subtitle: %d\n",
                                buf[i]
                              );
                            exit(1);
                          } /*switch*/
                      } /*while*/

                  } /*case 1*/
                break;
                        
                default:
                    fprintf(stderr, "ERR: unknown dvdauthor-data packet type: %d\n", buf[i + 1]);
                    exit(1);
                } /*switch*/

                writeundo(); /* drop private data from output */
                continue;
              } /*if*/
            if (buf[0] == 0 && buf[1] == 0 && buf[2] == 1 && buf[3] == 0xba) /* PACK header */
              {
                const pts_t newscr = readscr(buf + 4);
                if (newscr == 0 && lastscr > 0)
                  /* suggestion from Philippe Sarazin -- alternatively, Shaun Jackman suggests
                    simply treating newscr < lastscr as a warning and continuing */
                  {
                    backoffs -= lastscr;
                    fprintf(stderr, "\nWARN: SCR reset. New back offset = %" PRId64"\n", backoffs);
                  }
                else if (newscr < lastscr)
                  {
                    fprintf
                      (
                        stderr,
                        "ERR: SCR moves backwards, remultiplex input: %" PRId64" < %" PRId64"\n",
                        newscr,
                        lastscr
                      );
                    exit(1);
                  } /*if*/
                lastscr = newscr;
                if (!hadfirstvobu)
                    backoffs = newscr;
              } /*if*/
            transpose_ts(buf, -backoffs);
            if (fsect == -1)
              {
              /* start a new VOB file */
                char newname[200];
                fsect = 0;
                if (fbase)
                  {
                    if (outnum >= 0)
                        sprintf(newname, "%s_%d.VOB", fbase, outnum);
                    else
                        strcpy(newname, fbase);
                    writeopen(newname);
                  } /*if*/
              } /*if*/
            if
              (
                    buf[14] == 0
                &&
                    buf[15] == 0
                &&
                    buf[16] == 1
                &&
                    buf[17] == 0xbb /* system header */
              )
              {
                if
                  (
                        buf[38] == 0
                    &&
                        buf[39] == 0
                    &&
                        buf[40] == 1
                    &&
                        buf[41] == 0xbf // 1st private2
                    &&
                        buf[1024] == 0
                    &&
                        buf[1025] == 0
                    &&
                        buf[1026] == 1
                    &&
                        buf[1027] == 0xbf // 2nd private2
                  ) /* looks like a NAV PACK, which means the start of a new VOBU */
                  {
                    struct vobuinfo *vi;
                    if (thisvob->numvobus)
                        finishvideoscan(va, vnum, prevvidsect, &vsi);
                    // fprintf(stderr,"INFO: vobu\n");
                    hadfirstvobu = 1; /* NAV PACK starts a VOBU */
                    if (thisvob->numvobus == thisvob->maxvobus) /* need more space */
                      {
                        if (!thisvob->maxvobus)
                            thisvob->maxvobus = 1; /* first allocation */
                        else
                            thisvob->maxvobus <<= 1;
                              /* resize in powers of 2 to reduce reallocation calls */
                        thisvob->vobu = (struct vobuinfo *)realloc
                          (
                            /*ptr =*/ thisvob->vobu,
                            /*size =*/ thisvob->maxvobus * sizeof(struct vobuinfo)
                          );
                      } /*if*/
                    vi = &thisvob->vobu[thisvob->numvobus]; /* for the new VOBU */
                    memset(vi, 0, sizeof(struct vobuinfo));
                    vi->sector = cursect;
                    vi->fsect = fsect;
                    vi->fnum = outnum;
                    vi->firstvideopts = -1;
                    vi->firstIfield = 0;
                    vi->numfields = 0;
                    vi->numref = 0;
                    vi->hasseqend = 0;
                    vi->hasvideo = 0;
                    memcpy(thisvob->vobu[thisvob->numvobus].sectdata, buf, 0x26); // save pack and system header; the rest will be reconstructed later
                    thisvob->numvobus++;
                    if (!(thisvob->numvobus & 15)) /* time to let user know progress */
                        printvobustatus(va, cursect);
                    vsi.lastrefsect = 0;
                    vsi.firstgop = 1; /* restart scan for first GOP */
                  }
                else
                  {
                    fprintf
                      (
                        stderr,
                        "WARN: System header found, but PCI/DSI information is not where"
                            " expected\n\t(make sure your system header is 18 bytes!)\n"
                      );
                  } /*if*/
              } /*if*/
            if (!hadfirstvobu)
              {
                fprintf(stderr, "WARN: Skipping sector, waiting for first VOBU...\n");
                writeundo(); /* ignore it */
                continue;
              } /*if*/
            thisvob->vobu[thisvob->numvobus - 1].lastsector = cursect;

            i = 14;
            j = -1;
            while (i <= 2044)
              {
                if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)
                  {
                    if (buf[i + 3] >= 0xBD && buf[i + 3] <= 0xEF)
                      /* private, padding, audio or video stream */
                      {
                        j = i;
                        i += 6 + read2(buf + i + 4); /* start of next packet */
                        continue;
                      }
                    else if
                      (
                            buf[i + 3] == 0xB9 /* program end */
                        &&
                            j >= 14
                        &&
                            buf[j + 3] == 0xBE /* previous was padding stream */
                      )
                      {
                        write2(buf + j + 4, read2(buf + j + 4) + 4);
                        memset(buf + i, 0, 4); // mplex uses 0 for padding, so will I
                      } /*if*/
                  } /*if*/
                break;
              } /*while*/

            if
              (
                    buf[0] == 0
                &&
                    buf[1] == 0
                &&
                    buf[2] == 1
                &&
                    buf[3] == 0xba /* PACK header */
                &&
                    buf[14] == 0
                &&
                    buf[15] == 0
                &&
                    buf[16] == 1
                &&
                    buf[17] == 0xe0 /* video stream */
              )
              {
                struct vobuinfo * const vi = &thisvob->vobu[thisvob->numvobus - 1];
                vi->hasvideo = 1;
                scanvideoframe(va, buf, vi, cursect, prevvidsect, &vsi);
                if
                  (
                        (buf[21] & 128) /* PTS present */
                    &&
                        vi->firstvideopts == -1 /* not seen one yet */
                  )
                  {
                    vi->firstvideopts = readpts(buf + 23);
                  } /*if*/
                prevvidsect = cursect;
              } /*if*/
            if
              (
                    buf[0] == 0
                &&
                    buf[1] == 0
                &&
                    buf[2] == 1
                &&
                    buf[3] == 0xba /* PACK header */
                &&
                    buf[14] == 0
                &&
                    buf[15] == 0
                &&
                    buf[16] == 1
                &&
                    (
                        (buf[17] & 0xf8) == 0xc0 /* MPEG audio stream */
                    ||
                        buf[17] == 0xbd /* private stream 1 -- DVD audio or subpicture */
                    )
              )
              {
                pts_t pts0 = 0, pts1 = 0, backpts1 = 0;
                const int dptr = buf[22] /* PES header data length */ + 23; /* offset to packet data */
                const int endop = read2(buf + 18) /* PES packet length */ + 20 /* fixed PES header length */; /* end of packet */
                int audch;
                const int haspts = (buf[21] & 128) != 0;
                if (buf[17] == 0xbd) /* private stream 1 -- DVD audio or subpicture */
                  {
                    const int sid = buf[dptr]; /* sub-stream ID */
                    const int offs = read2(buf + dptr + 2);
                      /* offset to audio sample frame which corresponds to PTS value */
                    const int nrframes = buf[dptr + 1];
                      /* nr audio sample frames beginning in this packet */
                    switch (sid & 0xf8)
                      {
                    case 0x20:                          // subpicture
                    case 0x28:                          // subpicture
                    case 0x30:                          // subpicture
                    case 0x38:                          // subpicture
                         audch = sid;
                    break;
                    case 0x80:                          // ac3 audio
                        pts1 += 2880 * nrframes;
                        audch = sid & 7;
                        audio_scan_ac3(&thisvob->audch[audch], buf + dptr + 4, offs - 1, endop - (dptr + 4));
                    break;
                    case 0x88:                          // dts audio
                      /* pts1 += 960 * nrframes; */ /* why not? */
                        audch = 24 | (sid & 7);
                        audio_scan_dts(&thisvob->audch[audch], buf + dptr + 4, offs - 1, endop - (dptr + 4));
                    break;
                    case 0xa0:                          // pcm audio
                        pts1 += 150 * nrframes;
                        audch = 16 | (sid & 7);
                        audio_scan_pcm(&thisvob->audch[audch], buf + dptr + 4, endop - (dptr + 4));
                    break;
                    default:         // unknown
                        audch = -1;
                    break;
                      } /*switch*/
                  }
                else /* regular MPEG audio */
                  {
                    const int len = endop - dptr; /* length of packet data */
                    const int index = buf[17] & 7; /* audio stream ID */
                    audch = 8 | index;                      // mp2
                    memcpy(mp2hdr[index].buf + 3, buf + dptr, 3);
                    while (mp2hdr[index].hdrptr + 4 <= len)
                      {
                        const unsigned char * h;
                        if (mp2hdr[index].hdrptr < 0)
                            h = mp2hdr[index].buf + 3 + mp2hdr[index].hdrptr;
                              /* overlap from previous */
                        else
                            h = buf + dptr + mp2hdr[index].hdrptr;
                        if (!mpa_valid(h))
                          {
                            mp2hdr[index].hdrptr++; /* try the next likely offset */
                            continue;
                          } /*if*/
                        if (mp2hdr[index].hdrptr < 0)
                            backpts1 += 2160; /* how much time to add to end of previous packet */
                        else
                            pts1 += 2160;
                        mp2hdr[index].hdrptr += mpa_len(h); /* to next header */
                      } /*while*/
                    mp2hdr[index].hdrptr -= len;
                    memcpy(mp2hdr[index].buf, buf + dptr + len - 3, 3);
                    audiodesc_set_audio_attr(&thisvob->audch[audch].ad, &thisvob->audch[audch]. adwarn, AUDIO_SAMPLERATE, "48khz");
                  } /*if*/
              /* at this point, pts1 is the duration of the audio in the packet */
                if (haspts)
                  {
                    pts0 = readpts(buf + 23);
                    pts1 += pts0;
                  }
                else if (pts1 > 0)
                  {
                    fprintf
                      (
                        stderr,
                        "WARN: Audio channel %d contains sync headers but has no PTS.\n",
                        audch
                      );
                  } /*if*/
                // fprintf(stderr,"aud ch=%d pts %d - %d (%d)\n",audch,pts0,pts1,pts1-pts0);
                // fprintf(stderr,"pts[%d] %d (%02x %02x %02x %02x %02x)\n",va->numaudpts,pts,buf[23],buf[24],buf[25],buf[26],buf[27]);
                if (audch < 0 || audch >= 64)
                  {
                    fprintf(stderr,"WARN: Invalid audio channel %d\n",audch);
                  /* and ignore */
                  }
                else if (haspts)
                  {
                    struct audchannel * const ach = &thisvob->audch[audch];
                    if (ach->numaudpts == ach->maxaudpts) { /* need more space */
                        if (ach->maxaudpts)
                            ach->maxaudpts <<= 1;
                              /* resize in powers of 2 to reduce reallocation calls */
                        else
                            ach->maxaudpts = 1; /* first allocation */
                        ach->audpts = (struct audpts *)realloc
                          (
                            /*ptr =*/ ach->audpts,
                            /*size =*/ ach->maxaudpts * sizeof(struct audpts)
                          );
                    } /*if*/
                    if (ach->numaudpts)
                      {
                        // we cannot compute the length of a DTS audio packet
                        // so just backfill if it is one
                        // otherwise, for mp2 add any pts to the previous
                        // sector for a header that spanned two sectors
                        if ((audch & 0x38) == 0x18) // is this DTS?
                            ach->audpts[ach->numaudpts - 1].pts[1] = pts0;
                        else
                            ach->audpts[ach->numaudpts - 1].pts[1] += backpts1;

                        if (ach->audpts[ach->numaudpts - 1].pts[1] < pts0)
                          {
                            if (audch >= 32)
                                goto noshow; /* not audio */
                            fprintf(stderr, "WARN: Discontinuity of %" PRId64" in audio channel %d; please remultiplex input.\n", pts0 - ach->audpts[ach->numaudpts - 1].pts[1], audch);
                            // fprintf(stderr,"last=%d, this=%d\n",ach->audpts[ach->numaudpts-1].pts[1],pts0);
                          }
                        else if (ach->audpts[ach->numaudpts - 1].pts[1] > pts0)
                            fprintf
                              (
                                stderr,
                                "WARN: Audio pts for channel %d moves backwards by %"
                                    PRId64 "; please remultiplex input.\n",
                                audch,
                                ach->audpts[ach->numaudpts - 1].pts[1] - pts0
                              );
                        else
                            goto noshow;
                        fprintf(stderr, "WARN: Previous sector: ");
                        printpts(ach->audpts[ach->numaudpts - 1].pts[0]);
                        fprintf(stderr, " - ");
                        printpts(ach->audpts[ach->numaudpts - 1].pts[1]);
                        fprintf(stderr, "\nWARN: Current sector: ");
                        printpts(pts0);
                        fprintf(stderr, " - ");
                        printpts(pts1);
                        fprintf(stderr, "\n");
                        ach->audpts[ach->numaudpts - 1].pts[1] = pts0;
                      } /*if*/
                noshow:
                  /* fill in new entry */
                    ach->audpts[ach->numaudpts].pts[0] = pts0;
                    ach->audpts[ach->numaudpts].pts[1] = pts1;
                    ach->audpts[ach->numaudpts].asect = cursect;
                    ach->numaudpts++;
                  } /*if*/
              } /*if*/
            // the following code scans subtitle code in order to
            // remap the colors and update the end pts
            if
              (
                    buf[0] == 0
                &&
                    buf[1] == 0
                &&
                    buf[2] == 1
                &&
                    buf[3] == 0xba /* PACK header */
                &&
                    buf[14] == 0
                &&
                    buf[15] == 0
                &&
                    buf[16] == 1
                &&
                    buf[17] == 0xbd /* private stream 1 */
              )
              {
                int dptr = buf[22] /* PES header data length */ + 23; /* offset to packet data */
                const int ml = read2(buf + 18) /* PES packet length */ + 20 /* fixed PES header length */; /* total length of packet */
                const int st = buf[dptr]; /* sub-stream ID */
                dptr++; /* skip sub-stream ID */
                if ((st & 0xe0) == 0x20)
                  { /* subpicture stream */
                    procremap
                      (
                        /*cr =*/ &crs[st & 31],
                        /*b =*/ buf + dptr,
                        /*blen =*/ ml - dptr,
                        /*timespan =*/
                            &thisvob->audch[st].audpts[thisvob->audch[st].numaudpts - 1].pts[1]
                      );
                  } /*if*/
              } /*if*/
            cursect++;
            fsect++;
          } /*while*/
        varied_close(vf);
        if (thisvob->numvobus)
          {
            int i;
            pts_t finalaudiopts;
            finishvideoscan(va, vnum, prevvidsect, &vsi);
            // find end of audio
            finalaudiopts = -1;
            for (i = 0; i < 32; i++)
              {
                struct audchannel * const ach = thisvob->audch + i;
                if
                  (
                        ach->numaudpts
                    &&
                        ach->audpts[ach->numaudpts - 1].pts[1] > finalaudiopts
                  )
                    finalaudiopts = ach->audpts[ach->numaudpts - 1].pts[1];
              } /*for*/
            // pin down all video vobus
            // note: we make two passes; one assumes that the PTS for the
            // first frame is exact; the other assumes that the PTS for
            // the first frame is off by 1/2.  If both fail, then the third pass
            // assumes things are exact and throws a warning
            for (i = 0; i < 3; i++)
              {
                pts_t pts_align = -1; /* initially undefined */
                int complained = 0, j;
                for (j = 0; j < thisvob->numvobus; j++)
                  {
                    struct vobuinfo * const vi = thisvob->vobu + j;
                    if (vi->hasvideo)
                      {
                        if (pts_align == -1)
                          {
                            pts_align = vi->firstvideopts * 2;
                            if (i == 1)
                              {
                                // I assume pts should round down?  That seems to be how mplex deals with it
                                // also see earlier comment

                                // since pts round down, then the alternative base we should try is
                                // firstvideopts+0.5, thus increment
                                pts_align++;
                              } /*if*/
                            // MarkChapters will complain if firstIfield!=0
                          } /*if*/

                        vi->videopts[0] = calcpts(va, i == 2, &complained, &pts_align, vi->firstvideopts, -vi->firstIfield);
                        vi->videopts[1] = calcpts(va, i == 2, &complained, &pts_align, vi->firstvideopts, -vi->firstIfield + vi->numfields);
                        // if this looks like a dud, abort and try the next pass
                        if (complained && i < 2)
                            break;
                        vi->sectpts[0] = vi->videopts[0];
                        if (j + 1 == thisvob->numvobus && finalaudiopts > vi->videopts[1])
                            vi->sectpts[1] = finalaudiopts;
                        else
                            vi->sectpts[1] = vi->videopts[1];
                      } /*if*/
                  } /*for*/
                if (!complained)
                    break;
              } /*for*/
            // guess at non-video vobus
            for (i = 0; i < thisvob->numvobus; i++)
              {
                struct vobuinfo * const vi = thisvob->vobu + i;
                if (!vi->hasvideo)
                  {
                    int j, k;
                    pts_t firstaudiopts = -1, p;

                    for (j = 0; j < 32; j++)
                      {
                        const struct audchannel * const ach = thisvob->audch + j;
                        for (k = 0; k < ach->numaudpts; k++)
                            if (ach->audpts[k].asect >= vi->sector)
                              {
                                if (firstaudiopts == -1 || ach->audpts[k].pts[0] < firstaudiopts)
                                    firstaudiopts = ach->audpts[k].pts[0];
                                break;
                              } /*if; for*/
                      } /*for*/
                    if (firstaudiopts == -1)
                      {
                        fprintf
                          (
                            stderr,
                            "WARN: Cannot detect pts for VOBU if there is no audio or video\n"
                                "WARN: Using SCR instead.\n"
                          );
                        firstaudiopts = readscr(vi->sectdata + 4) + 4 * 147;
                          // 147 is roughly the minumum pts that must transpire between packets;
                          // we give a couple packets of buffer to allow the dvd player to
                          // process the data
                      } /*if*/
                    if (i)
                      {
                        pts_t frpts = getframepts(va);
                        p = firstaudiopts - thisvob->vobu[i - 1].sectpts[0];
                        // ensure this is a multiple of a framerate, just to be nice
                        p += frpts - 1;
                        p -= p % frpts;
                        p += thisvob->vobu[i - 1].sectpts[0];
                        assert(p >= thisvob->vobu[i - 1].sectpts[1]);
                        thisvob->vobu[i - 1].sectpts[1] = p;
                      }
                    else
                      {
                        fprintf
                          (
                            stderr,
                            "ERR:  Cannot infer pts for VOBU if there is no audio or video"
                                " and it is the\nERR:  first VOBU.\n"
                          );
                        exit(1);
                      } /*if*/
                    vi->sectpts[0] = p;
                    // if we can easily predict the end pts of this sector,
                    // then fill it in.  otherwise, let the next iteration do it
                    if (i + 1 == thisvob->numvobus)
                      { // if this is the end of the vob, use the final audio pts as the last pts
                        if( finalaudiopts>vi->sectpts[0] )
                            p = finalaudiopts;
                        else
                            p = vi->sectpts[0] + getframepts(va);
                              // add one frame of a buffer, so we don't have a zero (or less) length vobu
                      }
                    else if (thisvob->vobu[i+1].hasvideo)
                      // if the next vobu has video, use the start of the video as the end of this vobu
                        p = thisvob->vobu[i + 1].sectpts[0];
                    else
                      // the next vobu is an audio only vobu, and will backfill the pts as necessary
                        continue;
                    if (p <= vi->sectpts[0])
                      {
                        fprintf
                          (
                            stderr,
                            "ERR:  Audio and video are too poorly synchronised; you must"
                                " remultiplex.\n"
                          );
                        exit(1);
                      } /*if*/
                    vi->sectpts[1] = p;
                  } /*if*/
              } /*for*/

            fprintf(stderr, "\nINFO: Video pts = ");
            printpts(thisvob->vobu[0].videopts[0]);
            fprintf(stderr, " .. ");
            for (i = thisvob->numvobus - 1; i >= 0; i--)
                if (thisvob->vobu[i].hasvideo)
                  {
                    printpts(thisvob->vobu[i].videopts[1]);
                    break;
                  } /*if; for*/
            if (i < 0)
                fprintf(stderr, "??");
            for (i = 0; i < 64; i++)
              {
                const struct audchannel * const ach = &thisvob->audch[i];
                if (ach->numaudpts)
                  {
                    fprintf(stderr, "\nINFO: Audio[%d] pts = ", i);
                    printpts(ach->audpts[0].pts[0]);
                    fprintf(stderr, " .. ");
                    printpts(ach->audpts[ach->numaudpts - 1].pts[1]);
                  } /*if*/
              } /*for*/
            fprintf(stderr, "\n");
          } /*if*/
      } /*for*/
    writeclose();
    printvobustatus(va, cursect);
    fprintf(stderr, "\n");
    free(crs);
    return 1;
  } /*FindVobus*/

static pts_t pabs(pts_t pts)
  /* returns the absolute value of pts. */
  {
    if (pts < 0)
        return -pts;
    return pts;
  } /*pabs*/

static int findnearestvobu(struct vobgroup *pg, struct vob *va, pts_t pts)
  /* returns the index of the VOBU closest in time to pts. */
  {
    int l = 0, h = va->numvobus - 1, i;
    if (h < 0)
        return -1;
    pts += va->vobu[0].sectpts[0];
    i = findvobu(va, pts, l, h);
    if
      (
            i + 1 < va->numvobus
        &&
            i >= 0
        &&
            pabs(pts - va->vobu[i + 1].sectpts[0]) < pabs(pts - va->vobu[i].sectpts[0])
              /* next one is closer */
      )
        i++;
    return i;
  } /*findnearestvobu*/

void MarkChapters(struct vobgroup *va)
  /* fills in scellid, ecellid, vobcellid, firstvobuincell, lastvobuincell, numcells fields
    to mark all the cells and programs. */
  {
    int i, j, k, lastcellid;
    // mark start and stop points
    lastcellid = -1;
    for (i = 0; i < va->numallpgcs; i++)
        for (j = 0; j < va->allpgcs[i]->numsources; j++)
          {
          /* use vobcellid fields to mark start of cells, and scellid and ecellid fields
            to hold vobu indexes, all to be replaced later with right values */
            struct source * const thissource = va->allpgcs[i]->sources[j];
            for (k = 0; k < thissource->numcells; k++)
              {
                int v;
                v = findnearestvobu(va, thissource->vob, thissource->cells[k].startpts);
                if (v >= 0 && v < thissource->vob->numvobus)
                  {
                    if (thissource->cells[k].ischapter != CELL_NEITHER) /* from Wolfgang Wershofen */
                      { /* info for user corresponding to the points they marked */
                        fprintf(stderr, "CHAPTERS: VTS[%d/%d] ", i + 1, j + 1);
                        printpts(thissource->vob->vobu[v].sectpts[0] - thissource->vob->vobu[0].sectpts[0]);
                        fprintf(stderr, "\n");
                      } /*if*/
                    thissource->vob->vobu[v].vobcellid = 1; /* cell starts here */
                  } /*if*/
                thissource->cells[k].scellid = v; /* cell starts here */
                if
                  (
                        lastcellid != v
                    &&
                        thissource->vob->vobu[v].firstIfield != 0
                  )
                  {
                    fprintf
                      (
                        stderr,
                        "WARN: GOP may not be closed on cell %d of source %s of pgc %d\n",
                        k + 1,
                        thissource->fname,
                        i + 1
                      );
                  } /*if*/
                if (thissource->cells[k].endpts >= 0)
                  {
                    v = findnearestvobu(va, thissource->vob, thissource->cells[k].endpts);
                    if (v >= 0 && v < thissource->vob->numvobus)
                        thissource->vob->vobu[v].vobcellid = 1; /* next cell starts here */
                  }
                else
                    v = thissource->vob->numvobus;
                thissource->cells[k].ecellid = v; /* next cell starts here */
                lastcellid = v;
              } /*for*/
          } /*for; for*/
   /* At this point, the vobcellid fields have been set to 1 for all VOBUs which
    are supposed to start new cells. */
    for (i = 0; i < va->numvobs; i++)
      {
      /* fill in the vobcellid fields with the right values, and also
        firstvobuincell and lastvobuincell */
        int cellvobu = 0;
        int cellid = 0;
        va->vobs[i]->vobu[0].vobcellid = 1;
        for (j = 0; j < va->vobs[i]->numvobus; j++)
          {
            struct vobuinfo * const thisvobu = &va->vobs[i]->vobu[j];
            if (thisvobu->vobcellid)
              {
                cellid++; /* start new cell */
                cellvobu = j; /* this VOBU is first in cell */
              } /*if*/
            thisvobu->vobcellid = cellid + va->vobs[i]->vobid * 256;
            thisvobu->firstvobuincell = cellvobu;
          } /*for*/
        cellvobu = va->vobs[i]->numvobus - 1;
        for (j = cellvobu; j >= 0; j--)
          { /* fill in the lastvobuincell fields */
            struct vobuinfo * const thisvobu = &va->vobs[i]->vobu[j];
            thisvobu->lastvobuincell = cellvobu;
            if (thisvobu->firstvobuincell == j) /* reached start of this cell */
                cellvobu = j - 1;
          } /*for*/
        va->vobs[i]->numcells = cellid;
        if (cellid >= 256)
          {
            fprintf
              (
                stderr,
                "ERR:  VOB %s has too many cells (%d, 256 allowed)\n",
                va->vobs[i]->fname,
                cellid
              );
            exit(1);
          } /*if*/
      } /*for*/
  /* Now fill in right values for scellid and ecellid fields, replacing the
    vobu indexes I previously put in with the corresponding vobcellid values I
    have just computed. */
    for (i = 0; i < va->numallpgcs; i++)
        for (j = 0; j < va->allpgcs[i]->numsources; j++)
          {
            struct source * const thissource = va->allpgcs[i]->sources[j];
            for (k = 0; k < thissource->numcells; k++)
              {
                struct cell * const thiscell = &thissource->cells[k];
                if (thiscell->scellid < 0)
                    thiscell->scellid = 1;
                else if (thiscell->scellid < thissource->vob->numvobus)
                    thiscell->scellid = thissource->vob->vobu[thiscell->scellid].vobcellid & 255;
                else
                    thiscell->scellid = thissource->vob->numcells + 1;
                if (thiscell->ecellid < 0)
                    thiscell->ecellid = 1;
                else if (thiscell->ecellid < thissource->vob->numvobus)
                    thiscell->ecellid = thissource->vob->vobu[thiscell->ecellid].vobcellid & 255;
                else
                    thiscell->ecellid = thissource->vob->numcells + 1;
                va->allpgcs[i]->numcells += thiscell->ecellid - thiscell->scellid;
                if (thiscell->scellid != thiscell->ecellid && thiscell->ischapter != CELL_NEITHER)
                  {
                    va->allpgcs[i]->numprograms++;
                    if (thiscell->ischapter == CELL_CHAPTER_PROGRAM)
                        va->allpgcs[i]->numchapters++;
                    if (va->allpgcs[i]->numprograms >= 256)
                      {
                        fprintf
                          (
                            stderr,
                            "ERR:  PGC %d has too many programs (%d, 256 allowed)\n",
                            i + 1,
                            va->allpgcs[i]->numprograms
                          );
                        exit(1);
                      } /*if*/
                    // if numprograms<256, then numchapters<256, so
                    // no need to doublecheck
                  } /*if*/
              } /*for*/
          } /*for; for*/
  } /*MarkChapters*/

static pts_t getcellaudiopts(const struct vobgroup *va,int vcid,int ach,int w)
{
    const struct vob *v=va->vobs[(vcid>>8)-1];
    const struct audchannel *a=&v->audch[ach];
    int ai=0;

    assert((vcid&255)==(w?v->numcells:1));
    if( w )
        ai=a->numaudpts-1;
    return a->audpts[ai].pts[w];
}

static int hasaudio(const struct vobgroup *va,int vcid,int ach,int w)
{
    const struct vob *v=va->vobs[(vcid>>8)-1];
    const struct audchannel *a=&v->audch[ach];

    assert((vcid&255)==(w?v->numcells:1));

    return a->numaudpts!=0;
}

static pts_t getcellvideopts(const struct vobgroup *va,int vcid,int w)
{
    const struct vob *v=va->vobs[(vcid>>8)-1];
    int vi=0;

    assert((vcid&255)==(w?v->numcells:1));
    if( w )
        vi=v->numvobus-1;
    // we use sectpts instead of videopts because sometimes you will
    // present the last video frame for a long time; we want to know
    // the last presented time stamp: sectpts
    return v->vobu[vi].sectpts[w];
}

static pts_t calcaudiodiff(const struct vobgroup *va,int vcid,int ach,int w)
{
    return getcellvideopts(va,vcid,w)-getcellaudiopts(va,vcid,ach,w);
}

int calcaudiogap(const struct vobgroup *va,int vcid0,int vcid1,int ach)
{
    if( vcid0==-1 || vcid1==-1 )
        return 0;
    if( vcid1==vcid0+1 )
        return 0;
    if( (vcid1&255)==1 && va->vobs[(vcid0>>8)-1]->numcells==(vcid0&255) ) {
        int g1,g2;

        // there is no discontinuity if there is no audio in the second half
        if( !hasaudio(va,vcid1,ach,0) )
            return 0;

        // we have problems if the second half has audio but the first doesn't
        if( !hasaudio(va,vcid0,ach,1) && hasaudio(va,vcid1,ach,0) ) {
            fprintf(stderr,"WARN: Transition from non-audio to audio VOB; assuming discontinuity.\n");
            return 1;
        }

        g1=calcaudiodiff(va,vcid0,ach,1);
        g2=calcaudiodiff(va,vcid1,ach,0);
        return g1!=g2;
    }
    fprintf(stderr,"WARN: Do not know how to compute the audio gap between '%s' and '%s', assuming discontinuity.\n",va->vobs[(vcid0>>8)-1]->fname,va->vobs[(vcid1>>8)-1]->fname);
    return 1;
}

void FixVobus(const char *fbase,const struct vobgroup *va,const struct workset *ws,vtypes ismenu)
  /* fills in the NAV packs (i.e. PCI and DSI packets) for each VOBU in the
    already-written output VOB files. */
  {
    int outvob = -1;
    int vobuindex, j, pn, fnum = -2;
    pts_t scr;
    int vff, vrew;
    int totvob, curvob; /* for displaying statistics */

    totvob = 0;
    for (pn = 0; pn < va->numvobs; pn++)
        totvob += va->vobs[pn]->numvobus;
    curvob = 0;

    for (pn = 0; pn < va->numvobs; pn++)
      {
        const struct vob * const thisvob = va->vobs[pn];
        for (vobuindex = 0; vobuindex < thisvob->numvobus; vobuindex++)
          {
            const struct vobuinfo * const thisvobu = &thisvob->vobu[vobuindex];
            static unsigned char buf[2048];

            if (thisvobu->fnum != fnum)
              {
              /* time to start a new output file */
                char fname[200];
                if (outvob >= 0)
                  /* fixme: should do an fsync or fdatasync and check for write errors */
                    close(outvob);
                fnum = thisvobu->fnum;
                if (fbase)
                  {
                    if (fnum == -1)
                        strcpy(fname, fbase);
                    else
                        sprintf(fname, "%s_%d.VOB", fbase, fnum);
                    outvob = open(fname, O_WRONLY | O_BINARY);
                    if (outvob < 0)
                      {
                        fprintf
                          (
                            stderr,
                            "\nERR:  Error %d opening %s: %s\n",
                            errno,
                            fname,
                            strerror(errno)
                          );
                        exit(1);
                      } /*if*/
                  } /*if*/
              } /*if*/

            memcpy(buf, thisvobu->sectdata, 0x26);
            write4(buf + 0x26, 0x1bf); // private stream 2
            write2(buf + 0x2a, 0x3d4); // length
            buf[0x2c] = 0; /* substream ID, 0 = PCI */
            memset(buf + 0x2d,0, 0x400 - 0x2d);
            write4(buf + 0x400, 0x1bf); // private stream 2
            write2(buf + 0x404, 0x3fa); // length
            buf[0x406] = 1; /* substream ID, 1 = DSI */
            memset(buf + 0x407, 0, 0x7ff - 0x407);

            scr = readscr(buf + 4);

            write4(buf + 0x2d, thisvobu->sector); /* sector number of this block */
          /* buf[0x35 .. 0x38] -- prohibited user ops -- none for now */
            write4(buf + 0x39, thisvobu->sectpts[0]); /* start presentation time (vobu_s_ptm) */
            write4(buf + 0x3d, thisvobu->sectpts[1]); /* end presentation time (vobu_e_ptm) */
            if (thisvobu->hasseqend) // if sequence_end_code
                write4(buf + 0x41, thisvobu->videopts[1]); // vobu_se_e_ptm
            write4
              (
                buf + 0x45,
                buildtimeeven
                  (
                    va,
                    thisvobu->sectpts[0] - thisvob->vobu[thisvobu->firstvobuincell].sectpts[0]
                  ) // total guess
              );
              /* c_eltm -- BCD cell elapsed time + frame rate */
                
            if (thisvob->progchain->numbuttons)
              {
              /* fill in PCI packet with button info */
                const struct pgc * const pg = thisvob->progchain;
                int mask = getsubpmask(&va->vd), nrgrps, grp;
                char idmap[3];

                write2(buf + 0x8d, 1); /* highlight status = all new highlight information for this VOBU */
                write4(buf + 0x8f, thisvob->vobu[0].sectpts[0]); /* highlight start time */
                write4(buf + 0x93, -1); /* highlight end time */
                write4(buf + 0x97, -1); /* button selection end time (ignore user after this) */

                nrgrps = 0;
                write2(buf + 0x9b, 0); /* button groupings, none to begin with */
                for (j = 0; j < 4; j++)
                    if (mask & (1 << j))
                      {
                        assert(nrgrps < 3);
                        idmap[nrgrps] = j;
                        write2
                          (
                            buf + 0x9b,
                                read2(buf + 0x9b)
                            +
                                0x1000 /* add another button group */
                            +
                                (
                                    ((1 << j) >> 1)
                                      /* panscan/letterbox/normal bit for widescreen,
                                        0 for narrowscreen */
                                <<
                                    (2 - nrgrps) * 4
                                      /* bit-shifted into appropriate button group nibble */
                                )
                          );
                        nrgrps++;
                      } /*if; for*/
                assert(nrgrps > 0);

                buf[0x9e] = pg->numbuttons; /* number of buttons */
                buf[0x9f] = pg->numbuttons; /* number of numerically-selected buttons */
                memcpy(buf + 0xa3, thisvob->buttoncoli, 24);
                for (grp = 0; grp < nrgrps; grp++)
                  {
                    unsigned char *boffs = buf + 0xbb + 18 * (grp * 36 / nrgrps);
                      /* divide BTN_IT entries equally among all groups -- does this matter? */
                    const int sid = pg->subpmap[0][(int)idmap[grp]] & 127;

                    for (j = 0; j < pg->numbuttons; j++)
                      /* fixme: no check against overrunning allocated portion of BTN_IT array? */
                      {
                        static unsigned char compilebuf[128 * 8], *rbuf;
                        const struct button * const b = pg->buttons + j;
                        const struct buttoninfo *bi;
                        int k;

                        for (k = 0; k < b->numstream; k++)
                            if (b->stream[k].substreamid == sid)
                                break;
                        if (k == b->numstream)
                            continue; /* no matching button def for this substream */
                        bi = &b->stream[k];

                      /* construct BTN_IT -- Button Information Table entry */
                        boffs[0] = (bi->grp * 64) | (bi->x1 >> 4);
                        boffs[1] = (bi->x1 << 4) | (bi->x2 >> 8);
                        boffs[2] = bi->x2;
                        boffs[3] = (bi->autoaction ? 64 : 0) | (bi->y1 >> 4);
                        boffs[4] = (bi->y1 << 4) | (bi->y2 >> 8);
                        boffs[5] = bi->y2;
                        boffs[6] = findbutton(pg, bi->up, (j == 0) ? pg->numbuttons : j);
                        boffs[7] = findbutton(pg, bi->down, (j + 1 == pg->numbuttons) ? 1 : j + 2);
                        boffs[8] = findbutton(pg, bi->left, (j == 0) ? pg->numbuttons : j);
                        boffs[9] = findbutton(pg, bi->right, (j + 1 == pg->numbuttons) ? 1 : j + 2);
                        rbuf = vm_compile(compilebuf, compilebuf, ws, pg->pgcgroup, pg, b->commands, ismenu);
                        if (rbuf - compilebuf == 8)
                          {
                            memcpy(boffs + 10, compilebuf, 8);
                          }
                        else if (allowallreg)
                          {
                            fprintf
                              (
                                stderr,
                                "ERR:  Button command is too complex to fit in one instruction,"
                                    " and allgprm==true.\n"
                              );
                            exit(1);
                          }
                        else
                            write8(boffs + 10, 0x71, 0x01, 0x00, 0x0F, 0x00, j + 1, 0x00, 0x0d);
                              // g[15] = j && linktailpgc
                              /* transfer to full instruction sequence which will be
                                generated by dvdpgc.c:genpgc */
                        boffs += 18;
                      } /*for j*/
                  } /*for grp*/
              } /* if thisvob->progchain->numbuttons */

          /* fill in DSI packet */
            write4(buf + 0x407, scr);
            write4(buf + 0x40b, thisvobu->sector); // current lbn
            if (thisvobu->numref > 0)
              {
              /* up to three reference frame relative end blocks */
                for (j = 0; j < thisvobu->numref; j++)
                    write4(buf + 0x413 + j * 4, thisvobu->lastrefsect[j] - thisvobu->sector);
                for (; j < 3; j++) /* duplicate last one if less than 3 */
                    write4(buf + 0x413 + j * 4, thisvobu->lastrefsect[thisvobu->numref - 1] - thisvobu->sector);
              } /*if*/
            write2(buf + 0x41f, thisvobu->vobcellid >> 8); /* VOB number */
            buf[0x422] = thisvobu->vobcellid; /* cell number within VOB */
            write4(buf + 0x423, read4(buf + 0x45)); /* cell elapsed time, BCD + frame rate */
          /* interleaved unit stuff not supported for now */
            write4(buf + 0x433, thisvob->vobu[0].sectpts[0]);
              /* time of first video frame in first GOP of VOB */
            write4(buf + 0x437, thisvob->vobu[thisvob->numvobus - 1].sectpts[1]);
              /* time of last video frame in last GOP of VOB */
          /* audio gap stuff not supported for now */
          /* seamless angle stuff not supported for now */
            write4
              (
                buf + 0x4f1,
                getsect(thisvob, vobuindex, findnextvideo(thisvob, vobuindex, 1), 0, 0xbfffffff)
              );
              /* offset to next VOBU with video */
          /* offset to next VOBU at various times forward filled in below */
            write4(buf + 0x541, getsect(thisvob, vobuindex, vobuindex + 1, 0, 0x3fffffff));
              /* offset to next VOBU */
            write4(buf + 0x545, getsect(thisvob, vobuindex, vobuindex - 1, 0, 0x3fffffff));
              /* offset to previous VOBU */
          /* offset to previous VOBU at various times backward filled in below */
            write4
              (
                buf + 0x595,
                getsect(thisvob, vobuindex, findnextvideo(thisvob, vobuindex, -1), 0, 0xbfffffff)
              );
              /* offset to previous VOBU with video */
            for (j = 0; j < va->numaudiotracks; j++)
              {
                int s = getaudch(va, j);
                if (s >= 0)
                    s = findaudsect(thisvob, s, thisvobu->sectpts[0], thisvobu->sectpts[1]);
                if (s >= 0)
                  {
                    s = s - thisvobu->sector;
                    if (s > 0x1fff || s < -(0x1fff))
                      {
                        fprintf
                          (
                            stderr,
                            "\nWARN: audio sector out of range: %d (vobu #%d, pts ",
                            s,
                            vobuindex
                          );
                        printpts(thisvobu->sectpts[0]);
                        fprintf(stderr, ")\n");
                        s = 0;
                      } /*if*/
                    if (s < 0)
                        s = (-s) | 0x8000; /* absolute value + backward-direction flag */
                  }
                else
                    s = 0x3fff; /* no more audio for this stream */
                write2(buf + 0x599 + j * 2, s);
                  /* relative offset to first audio packet in this stream for this VOBU */
              } /*for*/
            for (j = 0; j < va->numsubpicturetracks; j++)
              {
                const struct audchannel * const ach = &thisvob->audch[j | 32];
                int s;
                if (ach->numaudpts)
                  {
                    int id = findspuidx(thisvob, j | 32, thisvobu->sectpts[0]);
                    // if overlaps A, point to A
                    // else if (A before here or doesn't exist) and (B after here or doesn't exist),
                    //     point to here
                    // else point to B
                    if
                      (
                            id >= 0
                        && 
                            ach->audpts[id].pts[0] < thisvobu->sectpts[1]
                        &&
                            ach->audpts[id].pts[1] >= thisvobu->sectpts[0]
                      )
                        s = findvobubysect(thisvob, ach->audpts[id].asect);
                    else if
                      (
                            (id < 0 || ach->audpts[id].pts[1] < thisvobu->sectpts[0])
                         &&
                            (
                                id + 1 == ach->numaudpts
                            ||
                                ach->audpts[id + 1].pts[0] >= thisvobu->sectpts[1]
                            )
                      )
                        s = vobuindex;
                    else
                        s = findvobubysect(thisvob, ach->audpts[id + 1].asect);
                    id = (s < vobuindex);
                    s = getsect(thisvob, vobuindex, s, 0, 0x7fffffff) & 0x7fffffff;
                    if (!s) /* same VOBU */
                        s = 0x7fffffff;
                          /* indicates current or later VOBU, no explicit forward offsets */
                    if (s != 0x7fffffff && id)
                        s |= 0x80000000; /* indicates offset to prior VOBU */
                  }
                else
                    s = 0; /* doesn't exist */
                write4(buf + 0x5a9 + j * 4, s);
                  /* relative offset to VOBU (NAV pack) containing subpicture data
                    for this stream for this VOBU */
              } /*for*/
            write4(buf + 0x40f, thisvobu->lastsector - thisvobu->sector);
              /* relative offset to last sector of VOBU */
            vff = vobuindex;
            vrew = vobuindex;
            for (j = 0; j < 19; j++)
              {
              /* fill in offsets to next/previous VOBUs at various time steps */
                int nff, nrew;
                nff = findvobu
                  (
                    thisvob,
                    thisvobu->sectpts[0] + timeline[j] * DVD_FFREW_HALFSEC,
                    thisvobu->firstvobuincell,
                    thisvobu->lastvobuincell
                  );
                // a hack -- the last vobu in the cell shouldn't have any forward ptrs
                // EXCEPT this hack violates both Grosse Pointe Blank and Bullitt -- what was I thinking?
                // if (i == thisvobu->lastvobuincell)
                //      nff = i + 1;
                nrew = findvobu
                  (
                    thisvob,
                    thisvobu->sectpts[0] - timeline[j] * DVD_FFREW_HALFSEC,
                    thisvobu->firstvobuincell,
                    thisvobu->lastvobuincell
                  );
              /* note table entries are in order of decreasing time step */
                write4
                  (
                    buf + 0x53d - j * 4, /* forward jump */
                    getsect(thisvob, vobuindex, nff, j >= 15 && nff > vff + 1, 0x3fffffff)
                  );
                write4
                  (
                    buf + 0x549 + j * 4, /* backward jump */
                    getsect(thisvob, vobuindex, nrew, j >= 15 && nrew < vrew - 1, 0x3fffffff)
                  );
                vff = nff;
                vrew = nrew;
              } /*for*/

          /* NAV pack all done, write it out */
            if (outvob != -1)
              {
              /* fixme: check for write errors */
                lseek(outvob, thisvobu->fsect * 2048, SEEK_SET);
                  /* where the NAV pack should go */
                write(outvob, buf, 2048);
                  /* update it */
              } /*if*/
            curvob++;
            if (!(curvob & 15)) /* time for another progress update */
                fprintf
                  (
                    stderr,
                    "STAT: fixing VOBU at %dMB (%d/%d, %d%%)\r",
                    thisvobu->sector / 512,
                    curvob + 1,
                    totvob,
                    curvob * 100 / totvob
                  );
          } /*for vobuindex*/
      } /*for pn*/
    if (outvob != -1)
      /* fixme: should do an fsync or fdatasync and check for write errors */
        close(outvob);
    if (totvob > 0)
        fprintf(stderr, "STAT: fixed %d VOBUS                         ", totvob);
    fprintf(stderr, "\n");
  } /*FixVobus*/

