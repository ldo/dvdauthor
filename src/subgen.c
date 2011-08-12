/*
    spumux mainline
*/
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

// prog for encoding dvd subtitles and multiplexing them into an mpeg
// available under GPL v2
/* Thanks to TED for sponsoring implementation of --nomux and --nodvdauthor-data options */

#include "config.h"

#include "compat.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>

#include <netinet/in.h>

#include "common.h"
#include "conffile.h"
#include "rgb.h"
#include "subglobals.h"
#include "subrender.h"
#include "subgen.h"

// (90000*300)/(1260000/2048)
// (9*300)/(126/2048)
// (9*300*2048)/126
#define DVDRATE 43886

/* all the subpicture formats follow the DVD-Video convention of using
MPEG Private Stream 1, where the first byte of the packet content indicates
the subpicture stream ID, based off the following values */
#define CVD_SUB_CHANNEL     0x0
#define SVCD_SUB_CHANNEL    0x70 /* fixed value, actual stream ID is next byte */
#define DVD_SUB_CHANNEL     0x20

/* formats supported: */
#define DVD_SUB     0 /* DVD-Video */
#define CVD_SUB     1 /* China Video Disc */
#define SVCD_SUB    2 /* Super Video CD */

#define psbufs 10

#define until_next_sub 1 //if 0 subs without length are made when subs are overlapping
#define tbs  90

static unsigned char *cbuf;

static unsigned int spuindex;
static bool
    show_progress = false,
    dodvdauthor_data = true,
    domux = true;
static int tofs; /* timestamp of first video packet, for synchronizing subpicture timestamps */
static int svcd_adjust;

static uint64_t lps; /* output bytes written */

int default_video_format = VF_NONE;
bool widescreen = false;

stinfo **spus=0;
int numspus=0;
bool have_textsub = false;

int nr_subtitles_skipped;

static char header[32];

unsigned char *sub;
int debug;
static int max_sub_size;

static bool substream_present[256];


// these 4 lines of variables are used by muxnext() and main() to communicate
static int subno,secsize,mode,fdo,header_size,muxrate;
static unsigned char substr, *sector;
static stinfo *newsti;
static uint64_t lastgts, nextgts;


/*


<spu image="foo" highlight="foo" select="foo" transparent="color"
     autooutline="infer/specified" autonavigate="infer/specified" >
<selectmap old="xxxxxx" new="yyyyyy" /> ...
<button label="foo" x0="0" y0="0" x1="1" y1="1" up="foo" down="bar" left="blah" right="werew" />
<action label="foo" x0="0" y0="0" x1="1" y1="1" up="foo" down="bar" left="blah" right="werew" />
</spu>

dvdauthor-data
012345678901234

db 2 (version)

db 1 (subtitle info)
db subnum

TIMESPAN:
dd startpts
dd endpts

COLORMAP:

db 1
db numcolors
per color: db Y, U, V

ST_COLI:

db 2
db numCOLI
per coli: db map[8]

BUTTONS:

db 3
db numbuttons
per button:
   db name (null terminated)
   dw modifier
   db auto
   db colormap
   dw x1, y1, x2, y2
   db up, down, left, right
*/

static void mkpackh
  (
    uint64_t time /* timestamp in 27MHz clock */,
    unsigned int muxrate /* data rate in units of 50 bytes/second */,
    unsigned char stuffing /* nr stuffing bytes to follow, [0 .. 7] */
  )
/* constructs the contents for a PACK header in header. */
{
    unsigned long const th = time / 300, tl = time % 300;
    header[0] = 0x44 | ((th >> 27) & 0x38) | ((th >> 28) & 3);
    header[1] = (th >> 20);
    header[2] = 4 | ((th >> 12) & 0xf8) | ((th >> 13) & 3);
    header[3] = (th >> 5);
    header[4] = 4 | ((th & 31) << 3) | ((tl >> 7) & 3);
    header[5] = 1 | (tl << 1);
    header[6] = muxrate >> 14;
    header[7] = muxrate >> 6;
    header[8] = 3 | (muxrate << 2);
    header[9] = 0xF8 | stuffing;
}

static void mkpesh0(unsigned long int pts)
  /* constructs an MPEG-2 PES header extension with PTS data but no PES extension
    in header. */
{
    header[0] = 0x81; /* original flag set */
    header[1] = 0x80;   //pts flag
    header[2] = 5; /* PES header data length */
    header[3] = 0x21 | ((pts >> 29) & 6); /* PTS[31 .. 30] */
    header[4] = pts >> 22; /* PTS[29 .. 22] */
    header[5] = 1 | (pts >> 14); /* PTS[21 .. 15] */
    header[6] = (pts >> 7); /* PTS[14 .. 7] */
    header[7] = 1 | (pts << 1); /* PTS[6 .. 0] */
}

static void mkpesh1(unsigned long int pts)
/* constructs an MPEG-2 PES header extension with PTS data and a PES extension. */
{
    header[0] = 0x81; /* original flag set */
    header[1] = 0x81;   //pts flag + pes extension flag
    header[2] = 8; /* PES header data length */
    header[3] = 0x21 | ((pts >> 29) & 6); /* PTS[31 .. 30] */
    header[4] = pts >> 22; /* PTS[29 .. 22] */
    header[5] = 1 | (pts >> 14); /* PTS[21 .. 15] */
    header[6] = (pts >> 7); /* PTS[14 .. 7] */
    header[7] = 1 | (pts << 1); /* PTS[6 .. 0] */
    header[8] = 0x1e; /* PES extension byte -- only P-STD buffer flag set */
    header[9] = 0x60; /* P-STD buffer data -- buffer scale = 1024 bytes, top bit of buffer size = 0 */
    header[10] = 0x3a; //buffer size (wtf nu det är..) (svcdverifier tycker  att 64 är ett bra tal här..)
}

static void mkpesh2 ()
/* constructs an empty MPEG-2 PES header extension in header. */
{
    header[0] = 0x81; /* original flag set */
    header[1] = 0; /* nothing else */
    header[2] = 0; /* PES header data length */
}

static unsigned int getmuxr(const unsigned char *buf)
  /* obtains the muxrate value (data rate in units of 50 bytes/second) from the
    contents of a PACK header. */
  {
    return (buf[8] >> 2) | (buf[7] * 64) | (buf[6] * 16384);
  } /*getmuxr*/

static uint64_t getgts(const unsigned char *buf)
  /* returns the timestamp from the contents of a PACK header. This will be
    in units of a 27MHz clock. */
  {
    uint64_t th, tl;
    if
      (
            (buf[8] & 3) != 3
        ||
            (buf[5] & 1) != 1
        ||
            (buf[4] & 4) != 4
        ||
            (buf[2] & 4) != 4
        ||
            (buf[0] & 0xc4) != 0x44
      )
        return -1;
    th =
            (buf[4] >> 3)
        +
            buf[3] * 32
        +
            (buf[2] & 3) * 32 * 256
        +
            (buf[2] & 0xf8) * 32 * 128
        +
            buf[1] * 1024 * 1024
        +
            (buf[0] & 3) * 1024 * 1024 * 256
        +
            (buf[0] & 0x38) * 1024 * 1024 * 128;
    tl =
            (buf[4] & 3) << 7
        |
            buf[5] >> 1;
    return th * 300 + tl;
  } /*getgts*/

static void fixgts(uint64_t *gts, uint64_t *nextgts)
  /* ensures that *gts is increasing in steps of at least DVDRATE
    on successive calls. *nextgts is the expected minimum value
    computed on the previous call, will be updated to the expected
    minimum for the next call. */
  {
    if (gts[0] < nextgts[0])
        gts[0] = nextgts[0];
    nextgts[0] = gts[0] + DVDRATE;
  } /*fixgts*/

static unsigned int getpts(const unsigned char *buf)
  /* returns the PTS value (in 90kHz clock units) from a PES packet header
    if present, else -1. */
  {
    if
      (
            !(buf[1] & 0xc0) /* no PTS */
        ||
            buf[2] < 4 /* PES header length too short */
        ||
            (buf[3] & 0xe1) != 0x21 /* PTS first byte */
        ||
            (buf[5] & 1) != 1 /* PTS second byte */
        ||
            (buf[7] & 1) != 1 /* PTS third byte */
      )
        return -1;
    return
            (buf[7] >> 1)
        +
            buf[6] * 128
        +
            (buf[5] & 254) * 16384
        +
            buf[4] * 16384 * 256
        +
            (buf[3] & 14) * 16384 * 256 * 128;
  } /*getpts*/

int findmasterpal(stinfo *s, const colorspec *p)
  /* returns the index in s->masterpal corresponding to colour p, allocating a
    new palette entry if not there already. */
  {
    int i;
    if (!p->a)
        return 0;
    for (i = 0; i < s->numpal; i++)
        if
          (
                p->r == s->masterpal[i].r
            &&
                p->g == s->masterpal[i].g
            &&
                p->b == s->masterpal[i].b
          )
            return i;
    assert(s->numpal < 16);
    s->masterpal[s->numpal++] = *p;
    return i;
  } /*findmasterpal*/

static void freestinfo(stinfo *s)
  /* frees up memory allocated for s. */
{
    int i;
    if (!s)
        return;
    free(s->img.img);
    free(s->hlt.img);
    free(s->sel.img);
    if (s->fimg)
        free(s->fimg);
    for (i = 0; i < s->numbuttons; i++)
      {
        free(s->buttons[i].name);
        free(s->buttons[i].up);
        free(s->buttons[i].down);
        free(s->buttons[i].left);
        free(s->buttons[i].right);
      } /*for*/
    free(s->buttons);
    free(s);
}

int calcY(const colorspec *p)
{
    return RGB2Y(p->r,p->g,p->b);
}

int calcCr(const colorspec *p)
{
    return RGB2Cr(p->r,p->g,p->b);
}

int calcCb(const colorspec *p)
{
    return RGB2Cb(p->r,p->g,p->b);
}

static unsigned char *wdest;

static void wdbyte(unsigned char c)
{
    wdest[0]=c;
    wdest++;
}

static void wdshort(unsigned short s)
{
    wdest[0]=s>>8;
    wdest[1]=s;
    wdest+=2;
}

static void wdlong(unsigned long l)
{
    wdest[0]=l>>24;
    wdest[1]=l>>16;
    wdest[2]=l>>8;
    wdest[3]=l;
    wdest+=4;
}

static void wdstr(char *s)
{
    while(*s)
        wdbyte(*s++);
    wdbyte(0);
}

static int sread(int h, void *b, int l)
  /* reads l bytes into b from fd h. Returns actual nr bytes read, or -1 on error. */
  {
    int tr = 0; /* count of bytes read */
    while (l > 0)
      {
        int r = read(h, b, l);
        if (r == -1)
          {
            fprintf(stderr, "WARN:  Read error %d -- %s\n", errno, strerror(errno));
            return -1;
          } /*if*/
        if (!r)
          {
            if (tr)
                fprintf(stderr, "WARN:  Read %d, expected %d\n", tr + r, tr + l);
            return tr;
          } /*if*/
        l -= r;
        b = ((unsigned char *)b) + r;
        tr += r;
      } /*while*/
    return tr;
  } /*sread*/

static void swrite(int h, const void *b, int l)
  /* writes l bytes from b to fd h. */
  {
    lps += l;
    while (l > 0) /* keep trying until it's all written */
      {
        int r = write(h, b, l);
        if (r == -1)
          {
            fprintf(stderr,"ERR:  Write error %d -- %s\n", errno, strerror(errno));
            exit(1);
          } /*if*/
        l -= r;
        b = ((const unsigned char *)b) + r;
      } /*while*/
  } /*swrite*/

static stinfo *getnextsub(void)
  /* processes and returns the next subtitle definition, if there is one. */
  {
    while (true)
      {
        stinfo *s;
        if (spuindex >= numspus) /* no more to return */
            return 0;
        s = spus[spuindex++];
        if (tofs > 0)
            s->spts += tofs;
/*      fprintf(stderr,"spts: %d\n",s->spts); */
        fprintf(stderr, "STAT: ");
        fprintf
          (
            stderr,
            "%d:%02d:%02d.%03d\r",
            (int)(s->spts / 90 / 1000 / 60 / 60),
            (int)(s->spts / 90 / 1000 / 60) % 60,
            (int)(s->spts / 90 / 1000) % 60,
            (int)(s->spts / 90) % 1000
          );
        if (process_subtitle(s))
            return s;
        freestinfo(s);
        nr_subtitles_skipped++;
      } /*while*/
  } /*getnextsub*/

static void usage()
{
    fprintf(stderr, "syntax: spumux [options] script.sub < in.mpg > out.mpg\n");
    fprintf(stderr, "\t-m <mode>   dvd, cvd, or svcd (only the first letter is checked).\n\t\tDefault is DVD.\n");
    fprintf(stderr, "\t-s <stream> number of the substream to insert (default 0)\n");
    fprintf(stderr, "\t-v <level>  verbosity level (default 0) \n");
    fprintf(stderr, "\t-P          enable progress indicator\n");
    fprintf(stderr,"\n\tSee manpage for config file format.\n");
    exit(-1);
}

static void muxnext(bool eoinput)
  {
    if (domux && (lastgts == 0 || tofs == -1 || (lps % secsize && !eoinput)))
        return;
    while (newsti)
      {
        stinfo *cursti;
        int bytes_sent, sub_size;
        unsigned char seq;
        unsigned int q;
        int64_t duegts;
      /* wait for correct time to insert sub, leave time for vpts to occur */
        duegts = (newsti->spts - .15 * 90000) * 300;
        if (duegts < 0)
            duegts = 0;
        if (domux && duegts > lastgts && !eoinput)
            break; /* not yet time */
        cursti = newsti;
        if (debug > 1)
          {
            fprintf
              (
                stderr,
                "INFO: After read_bmp(): xd=%d yd=%d x0=%d y0=%d\n",
                cursti->xd, cursti->yd, cursti->x0, cursti->y0
              );
          } /*if*/
        newsti = getnextsub();
        if (!newsti)
          {
            fprintf(stderr, "INFO: Found EOF in .sub file.\n");
          }
        else
          {
            if (cursti->spts + cursti->sd + tbs > newsti->spts)
              {
                if (debug > 4)
                  {
                    fprintf(stderr, "WARN: Overlapping sub\n");
                    fprintf
                      (
                        stderr,
                        "spts: %d sd: %d  nspts: %d\n",
                        cursti->spts / 90000,
                        cursti->sd / 90000,
                        newsti->spts / 90000
                      );
                  } /*if*/
                cursti->sd = -1;
              } /*if*/
          } /*if*/
        if (debug > 4)
          {
            if (newsti)
              {
                fprintf(stderr, "spts: %d  sd: %d  nspts: %d\n",
                        cursti->spts / 90000, cursti->sd / 90000, newsti->spts / 90000);
              }
            else
              {
                fprintf(stderr, "spts: %d  sd: %d  nspts: NULL\n",
                        cursti->spts / 90000, cursti->sd / 90000);
              } /*if*/
          } /*if*/
        if (cursti->sd == -1 && newsti && (!svcd_adjust || until_next_sub))
          {
            if (newsti->spts > cursti->spts + tbs)
                cursti->sd = newsti->spts - cursti->spts - tbs;
            else
              {
                if (debug > -1)
                  {
                    fprintf(stderr,\
                            "WARN:  Sub with too short or negative duration on line %d, skipping\n",\
                            spuindex - 1);
                  } /*if*/
                nr_subtitles_skipped++;
                continue;
              } /*if*/
          } /*if*/
        switch (mode)
          {
        case DVD_SUB:
          /* rle here */
            sub_size = dvd_encode(cursti);
        break;
        case CVD_SUB:
            sub_size = cvd_encode(cursti);
        break;
        case SVCD_SUB:
            sub_size = svcd_encode(cursti);
        break;
        default:
            sub_size = 0;
        break;
          } /*switch*/
        if (sub_size == -1)
          {
            if (debug > -1)
              {
                fprintf(stderr, "WARN: Image too large (encoded size>64k), skipping line %d\n", spuindex - 1);
              } /*if*/
            nr_subtitles_skipped++;
            continue;
          } /*if*/
        if (sub_size > max_sub_size)
          {
            max_sub_size = sub_size;
            if (!have_textsub)
                fprintf(stderr, "INFO: Max_sub_size=%d\n", max_sub_size);
          } /*if*/
        seq = 0;
        subno++;
        lastgts = duegts;
        if (mode == DVD_SUB)
          {
            if (dodvdauthor_data)
              {
              /* write out custom dvdauthor information */
                int pdl = secsize - 6 - 10 - 4, i;
                unsigned int c;
              /* write packet start code */
                c = htonl(0x100 + MPID_PACK);
                swrite(fdo, &c, 4);
                mkpackh(lastgts, muxrate, 0);
                fixgts(&lastgts, &nextgts);
                swrite(fdo, header, 10);
                // start padding streamcode
                header[0] = 0;
                header[1] = 0;
                header[2] = 1;
                header[3] = MPID_PAD; /* for my private button/palette data */
                header[4] = pdl >> 8;
                header[5] = pdl;
                swrite(fdo, header, 6);

                memset(sector, 0xff, pdl);

                wdest = sector;
                wdstr("dvdauthor-data");
                wdbyte(2); // version
                wdbyte(1); // subtitle info
                wdbyte(substr); // sub number
                wdlong(cursti->spts); // start pts
                wdlong(cursti->sd == -1 ? -1 : cursti->sd + cursti->spts); // end pts

                wdbyte(1); // colormap
                wdbyte(cursti->numpal); // number of colors
                for (i = 0; i < cursti->numpal; i++)
                  {
                    wdbyte(calcY(cursti->masterpal + i));
                    wdbyte(calcCr(cursti->masterpal + i));
                    wdbyte(calcCb(cursti->masterpal + i));
                  /* I don't need to pass alpha, because that has already been encoded
                    into the subpicture stream with SPU_SET_CONTR commands */
                  } /*for*/

                if (cursti->numgroups)
                  {
                    wdbyte(2); // st_coli
                    wdbyte(cursti->numgroups);
                    for (i = 0; i < cursti->numgroups; i++)
                      {
                        unsigned short sh[4];
                        int j;
                        for (j = 3; j >= 0; j--)
                          {
                            int k = cursti->groupmap[i][j];
                            if (k == -1)
                              {
                                for (k = 0; k < 4; k++)
                                    sh[k] <<= 4;
                              }
                            else
                              {
                                sh[0] =
                                        sh[0] << 4
                                    |
                                        findmasterpal(cursti, cursti->hlt.pal + (k >> 8 & 255));
                                sh[1] =
                                        sh[1] << 4
                                    |
                                        cursti->hlt.pal[k >> 8 & 255].a >> 4;
                                sh[2] =
                                        sh[2] << 4
                                    |
                                        findmasterpal(cursti, cursti->sel.pal + (k & 255));
                                sh[3] =
                                        sh[3] << 4
                                    |
                                        cursti->sel.pal[k & 255].a >> 4;
                              } /*if*/
                          } /*for*/
                        for (j = 0; j < 4; j++)
                            wdshort(sh[j]);
                      } /*for*/
                  } /*if*/
                if (cursti->numbuttons)
                  {
                    wdbyte(3);
                    wdbyte(cursti->numbuttons);
                    for (i = 0; i < cursti->numbuttons; i++)
                      {
                        const button * const b = &cursti->buttons[i];
                        char nm1[10], nm2[10];
                        wdstr(b->name);
                        wdshort(0);
                        wdbyte(b->autoaction ? 1 : 0);
                        wdbyte(b->grp);
                        wdshort(b->r.x0);
                        wdshort(b->r.y0);
                        wdshort(b->r.x1);
                        wdshort(b->r.y1);
                        if ((b->r.y0 & 1) || (b->r.y1 & 1))
                            fprintf
                              (
                                stderr,
                                "WARN: Button y coordinates are odd for button %s: %dx%d-%dx%d;"
                                    " they may not display properly.\n",
                                b->name,
                                b->r.x0,
                                b->r.y0,
                                b->r.x1,
                                b->r.y1
                              );
                        sprintf(nm1, "%d", i ? i : (cursti->numbuttons));
                        sprintf(nm2, "%d", (i + 1 != cursti->numbuttons) ? (i + 2) : 1);
                        // fprintf(stderr,"BUTTON NAVIGATION FOR %s: up=%s down=%s left=%s right=%s (%s %s)\n",b->name,b->up,b->down,b->left,b->right,nm1,nm2);
                      /* fixme: no constraints on length of button names */
                        wdstr(b->up ? b->up : nm1);
                        wdstr(b->down ? b->down : nm2);
                        wdstr(b->left ? b->left : nm1);
                        wdstr(b->right ? b->right : nm2);
                      } /*for*/
                  } /*if*/
              /* fprintf(stderr,"INFO: Private sector size %d\n",wdest-sector); */
                swrite(fdo, sector, pdl);
              }
            else
              {
                if (cursti->numbuttons)
                  {
                    fprintf(stderr, "WARN: Button info not being passed to dvdauthor.\n");
                  } /*if*/
              } /*if*/
          } /*if mode == DVD_SUB*/
        // header_size is 12 before while starts
      /* header_size includes first byte of PES data, i.e. substream ID */
      /* search packet start code */
        bytes_sent = 0;
        while (bytes_sent != sub_size)
          {
            int bytes_this_packet, stuffing;
            uint32_t c;
            uint16_t b;
          /* if not first time here */
            if (bytes_sent)
                header_size = 4; /* empty MPEG-2 PES header extension on continuation packet */
            else if (header_size != 12) // not first time
                header_size = 9; /* drop PES extension from subsequent packets */
          /* calculate how many bytes to send */
            bytes_this_packet = secsize - 20 - header_size - svcd_adjust;
            stuffing = bytes_this_packet - (sub_size - bytes_sent);
            if ( stuffing < 0)
                stuffing = 0;
            else
              {
                bytes_this_packet -= stuffing;
                if (stuffing > 7)
                    stuffing = 0;
              } /*if*/
          /* write header */
            c = htonl(0x100 + MPID_PACK);
            swrite(fdo, &c, 4);
            mkpackh(lastgts, muxrate, 0);
            fixgts(&lastgts, &nextgts);
/*
  fprintf(stderr, "system time: %d 0x%lx %d\n", lastgts, ftell(fds), frame);
  fprintf(stderr, "spts=%d\n", spts);
*/
            swrite(fdo, header, 10);
          /* write private stream code */
            c = htonl(0x100 + MPID_PRIVATE1);
            swrite(fdo, &c, 4);
          /* write packet length */
            b = ntohs(bytes_this_packet + header_size + svcd_adjust + stuffing);
            swrite(fdo, &b, 2);
            if (header_size == 9)
                mkpesh0(cursti->spts);
            else if (header_size == 12)
                mkpesh1(cursti->spts);
            else /* header_size = 4 */
                mkpesh2();
            header[2] += stuffing; /* include in PES header data size */
            memset(header + header_size - 1, 0xff, stuffing);
            header[header_size + stuffing - 1] = /* substream ID */
                svcd_adjust ?
                    SVCD_SUB_CHANNEL /* real subpicture stream number inserted below */
                :
                    substr; /* subpicture stream number */
            swrite(fdo, header, header_size + stuffing);
            if (svcd_adjust)
              {
              /* additional 4 byte svcd header */
                const uint16_t cc = htons(subno);
                swrite(fdo, &substr, 1); /* real subpicture stream number */
                if (bytes_sent + bytes_this_packet == sub_size)
                    seq |= 128; /* end of current sub */
                swrite(fdo, &seq, 1); // packet number in current sub
                // 0 - up, last packet has bit 7 set
                swrite(fdo, &cc, 2);
              } /*if*/
            seq++; /* won't count past 127? */
          /* write bytes_this_packet data bytes, increment bytes_sent by bytes written */
            swrite(fdo, sub + bytes_sent, bytes_this_packet);
            bytes_sent += bytes_this_packet;
          /* test if full sector */
            bytes_this_packet += 20 + header_size + stuffing + svcd_adjust;
            if (bytes_this_packet != secsize)
              {
                unsigned short bs;
              /* if sector not full, write padding? */
              /* write padding code */
                c = htonl(0x100 + MPID_PAD); /* really just padding this time */
                swrite(fdo, &c, 4);
              /* calculate number of padding bytes */
                b = secsize - bytes_this_packet - 6;
                if (debug > 4)
                  {
                    fprintf(stderr, "INFO: Padding, b: %d\n", b);
                  } /*if*/
              /* write padding stream size */
                bs = htons(b);          //fixa
                swrite(fdo, &bs, 2);
              /* write padding end marker ? */
                c = 0xff;
                for (q = 0; q < b; q++)
                    swrite(fdo, &c, 1);
              } /*if*/
          } /* end while bytes_sent ! sub_size */

        if (debug > 0)
          {
            fprintf
              (
                stderr,
                "INFO: Subtitle inserted at: %f sd=%d\n",
                (double)cursti->spts / 90000,
                cursti->sd / 90000
              );
          } /*if*/
        freestinfo(cursti);
      } /*while*/
  } /*muxnext*/

static void textsub_statistics()
  {
    fprintf(stderr, "\nINFO: Text Subtitle Statistics:\n");
    fprintf(stderr, "INFO: - Processed %d subtitles.\n", numspus);
    fprintf(stderr, "INFO: - The longest display line had %d characters.\n", sub_max_chars - 1);
    fprintf(stderr, "INFO: - The maximum number of displayed lines was %d.\n", sub_max_lines);
    fprintf(stderr, "INFO: - The normal display height of the font %s was %d.\n", sub_font, sub_max_font_height);
    fprintf(stderr, "INFO: - The bottom display height of the font %s was %d.\n", sub_font, sub_max_bottom_font_height);
    fprintf(stderr, "INFO: - The biggest subtitle box had %d bytes.\n", max_sub_size);
  } /*textsub_statistics*/

int main(int argc,char **argv)
{
    int fdi;
    unsigned int c, ch, a;
    unsigned short int b;
    unsigned char psbuf[psbufs];
    int optch;
#ifdef HAVE_GETOPT_LONG
    const static struct option longopts[]={
        {"nodvdauthor-data", 0, 0, 1},
        {"nomux", 0, 0, 2},
        {0, 0, 0, 0}
    };
#define GETOPTFUNC(x,y,z) getopt_long(x,y,z,longopts,NULL)
#else
#define GETOPTFUNC(x,y,z) getopt(x,y,z)
#endif

    default_video_format = get_video_format();
    init_locale();
    newsti = 0;
    mode = DVD_SUB; /* default */
    sub = malloc(SUB_BUFFER_MAX + SUB_BUFFER_HEADROOM);
    if (!sub)
      {
        fprintf(stderr, "ERR:  Could not allocate space for sub, aborting.\n");
        exit(1);
      } /*if*/
//fprintf(stderr, "malloc sub=%p\n", sub);
    if (!(cbuf = malloc(65536)))
      {
        fprintf(stderr, "ERR:  Could not allocate space for sub buffer, aborting.\n");
        exit(1);
      } /*if*/
    image_init();
    fputs(PACKAGE_HEADER("spumux"), stderr);
    if (default_video_format != VF_NONE)
      {
        fprintf
          (
            stderr,
            "INFO: default video format is %s\n",
            default_video_format == VF_PAL ? "PAL" : "NTSC"
          );
      }
    else
      {
#if defined(DEFAULT_VIDEO_FORMAT)
#    if DEFAULT_VIDEO_FORMAT == 1
        fprintf(stderr, "INFO: default video format is NTSC\n");
        default_video_format = VF_NTSC;
#    elif DEFAULT_VIDEO_FORMAT == 2
        fprintf(stderr, "INFO: default video format is PAL\n");
        default_video_format = VF_PAL;
#    endif
#else
        fprintf(stderr, "INFO: no default video format, must explicitly specify NTSC or PAL\n");
#endif
      } /*if*/
    tofs = -1;
    debug = 0;
    substr = 0; /* default */
    while (-1 != (optch = GETOPTFUNC(argc, argv, "hm:s:v:P")))
      {
        switch (optch)
          {
        case 'm':
            switch (optarg[0])
              {
            case 'd':
            case 'D':
                mode = DVD_SUB;
            break;

            case 's':
            case 'S':
                mode = SVCD_SUB;
            break;

            case 'c':
            case 'C':
                mode = CVD_SUB;
            break;

            default:
                fprintf(stderr,"ERR:  Mode must be one of dvd, svcd, or cvd\n");
                usage();
              } /*switch*/
        break;
        case 's':
            substr = strtounsigned(optarg, "substream id");
            if (substr > 31)
              {
                fprintf(stderr, "ERR:  Invalid stream ID, must be in 0 .. 31\n");
                exit(1);
              } /*if*/
        break;
        case 'v':
            debug = strtounsigned(optarg, "verbosity");
        break;
        case 'P':
            show_progress = true;
        break;
        case 'h':
            usage();
        break;
        case 1: /* --nodvdauthor-data */
            dodvdauthor_data = false;
        break;
        case 2: /* --nomux */
            domux = false;
        break;
        default:
            fprintf(stderr, "WARN: Getopt returned %d\n",optch);
            usage();
        break;
          } /*switch*/
      } /*while*/
    if (argc - optind != 1)
      {
        fprintf(stderr, "WARN: Only one argument expected\n");
        usage();
      } /*if*/

    switch(mode)
      {
    case DVD_SUB:
    default:
        svcd_adjust = 0;
        substr += DVD_SUB_CHANNEL;
        muxrate = 10080 * 10 / 4; // 0x1131; // 10080 kbps
        secsize = 2048;
    break;
    case CVD_SUB:
        svcd_adjust = 0;
        substr += CVD_SUB_CHANNEL;
        muxrate = 1040 * 10/4; //0x0a28; // 1040 kbps
        secsize = 2324;
    break;
    case SVCD_SUB:
        svcd_adjust = 4;
        // svcd substream identification works differently...
        // substr += SVCD_SUB_CHANNEL;
        muxrate = 1760 * 10 / 4; //0x1131; // 1760 kbps
        secsize = 2324;
    break;
      } /*switch*/
    fdi = 0; /* stdin */
    fdo = 1; /* stdout */
    if (domux)
      {
        win32_setmode(fdi,O_BINARY);
      } /*if*/
    win32_setmode(fdo,O_BINARY);
    if (spumux_parse(argv[optind]))
        return -1;
    if (tofs >= 0 && debug > 0)
        fprintf(stderr, "INFO: Subtitles offset by %fs\n", (double)tofs / 90000);
    if (have_textsub)
      {
        vo_init_osd();
      } /*if*/
    spuindex = 0;
    nr_subtitles_skipped = 0;
    if (!(sector = malloc(secsize)))
      {
        fprintf(stderr, "ERR:  Could not allocate space for sector buffer, aborting.\n");
        exit(1);
      } /*if*/
    memset(substream_present, false, sizeof substream_present);

    newsti = getnextsub();
    max_sub_size = 0;
    header_size = 12; /* first PES header extension will have PTS data and a PES extension */
    lps = 0;
    lastgts = 0;
    nextgts = 0;
    subno = -1;
    while (domux)
      {
        muxnext(false);
        if (sread(fdi, &c, 4) != 4)
            goto eoi;
        ch = ntohl(c); /* header ID */
        if (ch == 0x100 + MPID_PACK)
          {
l_01ba:
            if (show_progress) /* show progress */
              {
                if (lps % 1024 * 1024 * 10 < secsize)
                    fprintf(stderr, "INFO: %" PRIu64 " bytes of data written\r", lps);
              } /*if*/
            if (debug > 5)
                fprintf(stderr, "INFO: pack_start_code\n");
            if (sread(fdi, psbuf, psbufs) != psbufs)
                break;
            lastgts = getgts(psbuf);
            if (lastgts != -1)
              {
                muxnext(false);
                fixgts(&lastgts, &nextgts);
                muxrate = getmuxr(psbuf);
              }
            else
              {
                if (debug >- 1)
                    fprintf(stderr, "WARN: Incorrect pack header\n");
                lastgts = nextgts;
              } /*if*/
            mkpackh(lastgts, muxrate, 0);
            swrite(fdo, &c, 4);
            swrite(fdo, header, psbufs);
          }
        else if (ch >= 0x100 + MPID_SYSTEM && ch <= 0x100 + MPID_VIDEO_LAST)
          {
            swrite(fdo, &c, 4); /* packet header excl length */
            if (sread(fdi, &b, 2) != 2)
                break;
            swrite(fdo, &b, 2); /* packet length */
            b = ntohs(b);
            if (sread(fdi, cbuf, b) != b) /* packet contents */
                break;
            if (ch == 0x100 + MPID_PRIVATE1)
              {
                const int dptr = cbuf[2] /* PES header data length */ + 3 /* offset to packet data */;
                const int substreamid =
                    mode == SVCD_SUB ?
                        cbuf[dptr] == SVCD_SUB_CHANNEL ?
                            cbuf[dptr + 1]
                        :
                            -1 /*?*/
                    : (cbuf[dptr] & 0xE0) != 0x80 ? /* not DVD-Video-specific audio */
                        cbuf[dptr]
                    :
                        -1;
                if (substreamid >= 0)
                  {
                    if (substreamid == substr)
                      {
                        fprintf(stderr, "ERR:  duplicate substream ID 0x%02x\n", substr);
                        exit(1);
                      } /*if*/
                    if (!substream_present[substreamid])
                      {
                        const char * modestr;
                        int streamid;
                        if (mode == DVD_SUB && substreamid >= DVD_SUB_CHANNEL)
                          {
                            modestr = "DVD-Video";
                            streamid = substreamid - DVD_SUB_CHANNEL;
                          }
                        else if (mode == SVCD_SUB)
                          {
                            modestr = "SVCD";
                            streamid = substreamid;
                          }
                        else if (mode == CVD_SUB)
                          {
                            modestr = "CVD";
                            streamid = substreamid;
                          }
                        else
                          {
                            modestr= "?";
                            streamid = substreamid;
                          } /*if*/
                        fprintf
                          (
                            stderr,
                            "INFO: found existing substream ID 0x%02x (%s %d)\n",
                            substreamid,
                            modestr,
                            streamid
                          );
                        substream_present[substreamid] = true;
                      } /*if*/
                  } /*if*/
              } /*if*/
            swrite(fdo, cbuf, b);
            if (ch == 0x100 + MPID_VIDEO_FIRST && tofs == -1)
              { /* video stream (DVD only allows one) */
                if (debug > 5)
                    fprintf(stderr, "INFO: Video stream\n");
                a = getpts(cbuf);
                if (a != -1)
                  {
                    if (newsti)
                        newsti->spts += a;
                    tofs = a;
                  } /*if*/
              } /*if*/
          }
        else if (ch == 0x100 + MPID_PROGRAM_END)
          {
            swrite(fdo, &c, 4);
            // do nothing
          }
        else
          { /* unrecognized */
            swrite(fdo, &c, 4);
            if (debug > 0)
              {
                fprintf(stderr, "WARN: Unknown header %.2x %.2x %.2x %.2x\n",\
                        c & 255, (c >> 8) & 255, (c >> 16) & 255, (c >> 24) & 255);
              } /*if*/
            a = b = 0;
            while (a != 0x100 + MPID_PACK) /* until next PACK header */
              {
                unsigned char nc;
                if (sread(fdi, &nc, 1) < 1)
                    goto eoi;
                swrite(fdo, &nc, 1);
                a = (a << 8) | nc;
                if (debug > 6)
                    fprintf(stderr, "INFO: 0x%x\n", a);
                b++;
              } /*while*/
            fprintf(stderr, "INFO: Skipped %d bytes of garbage\n", b);
            goto l_01ba;
          } /*if*/
      } /*while*/
 eoi:
    muxnext(true); // end of input
/*    fprintf(stderr, "max_sub_size=%d\n", max_sub_size); */
    if (subno != 0xffff)
      {
        fprintf(stderr,
            "INFO: %d subtitles added, %d subtitles skipped, stream: %d, offset: %.2f\n",
            subno + 1, nr_subtitles_skipped, substr, (double)tofs / 90000);
      }
    else
      {
        fprintf(stderr, "WARN: no subtitles added\n");
      } /*if*/
    if (have_textsub)
      {
        textsub_statistics();
        vo_finish_osd();
      } /*if*/
    image_shutdown();
    return 0;
} /* end function main */
