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

#include "config.h"

#include "compat.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>

#include <netinet/in.h>

#include "common.h"
#include "rgb.h"
#include "subgen.h"
#include "textsub.h"

// (90000*300)/(1260000/2048)
// (9*300)/(126/2048)
// (9*300*2048)/126
#define DVDRATE 43886

#define CVD_SUB_CHANNEL     0x0
#define SVCD_SUB_CHANNEL    0x70
#define DVD_SUB_CHANNEL     0x20

#define DVD_SUB     0
#define CVD_SUB     1
#define SVCD_SUB    2

#define psbufs 10

#define until_next_sub 1 //if 0 subs without length are made when subs are overlapping
#define tbs  90

static unsigned char *cbuf;

static unsigned int spuindex;
static bool progr;
static int tofs;
static int svcd;

static uint64_t lps;

stinfo **spus=0;
int numspus=0;
bool have_textsub = false;
bool have_transparent = true;
int transparent_color=0x808080;

int skip;

static char header[32];

unsigned char *sub;
int debug;
int max_sub_size;



// these 4 lines of variables are used by mux() and main() to communicate
static int subno,secsize,mode,fdo,header_size,muxrate;
static unsigned char substr, *sector;
static stinfo *newsti;
static uint64_t gts, nextgts;


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
/* constructs the contents for a PACK header. */
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
/* constructs an MPEG-2 PES header extension with PTS data but no PES extension. */
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
    header[3] = 0x21 | ((pts >> 29)&6); /* PTS[31 .. 30] */
    header[4] = pts >> 22; /* PTS[29 .. 22] */
    header[5] = 1 | (pts >> 14); /* PTS[21 .. 15] */
    header[6] = (pts >> 7); /* PTS[14 .. 7] */
    header[7] = 1 | (pts << 1); /* PTS[6 .. 0] */
    header[8] = 0x1e; /* PES extension byte -- only P-STD buffer flag set */
    header[9] = 0x60; /* P-STD buffer data -- buffer scale = 1024 bytes, top bit of buffer size = 0 */
    header[10] = 0x3a; //buffer size (wtf nu det är..) (svcdverifier tycker  att 64 är ett bra tal här..)
}

static void mkpesh2 ()
/* constructs an empty MPEG-2 PES header extension. */
{
    header[0] = 0x81; /* original flag set */
    header[1] = 0; /* nothing else */
    header[2] = 0; /* PES header data length */
}

static unsigned int getmuxr(const unsigned char *buf)
/* obtains the muxrate value from the contents of a PACK header. */
{
    return (buf[8] >> 2)|(buf[7]*64)|(buf[6]*16384);
}

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

int findmasterpal(stinfo *s, const palt *p)
  /* returns the index in s->masterpal corresponding to colour p, allocating a
    new palette entry if not there already. */
  {
    int i;
    if (!p->t)
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

int calcY(const palt *p)
{
    return RGB2Y(p->r,p->g,p->b);
}

int calcCr(const palt *p)
{
    return RGB2Cr(p->r,p->g,p->b);
}

int calcCb(const palt *p)
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
    while(1)
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
        skip++;
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

static void mux(int eoinput)
  {
    if (gts == 0 || tofs == -1 || (lps % secsize && !eoinput))
        return;
    while (newsti)
      {
        stinfo *cursti;
        int bytes_send, sub_size;
        unsigned char seq;
        unsigned int q;
        int64_t dgts;
      /* wait for correct time to insert sub, leave time for vpts to occur */
        dgts = (newsti->spts - .15 * 90000) * 300;
        if (dgts < 0)
            dgts = 0;
        if (dgts > gts && !eoinput)
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
        if ((cursti->sd == -1) && newsti && ((!svcd) || until_next_sub))
          {
            if (newsti->spts > cursti->spts + tbs)
                cursti->sd = newsti->spts - cursti->spts - tbs;
            else
              {
                if (debug > -1)
                  {
                    fprintf(stderr,\
                            "ERR: Sub with too short or negative duration on line %d, skipping\n",\
                            spuindex - 1);
                  } /*if*/
                exit(1);
                skip++;
                continue;
              } /*if*/
          } /*if*/
        switch(mode)
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
            skip++;
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
        gts = dgts;
        if (mode == DVD_SUB)
          {
          /* write out custom dvdauthor information */
            int pdl = secsize - 6 - 10 - 4, i;
            unsigned int c;
          /* write packet start code */
            c = htonl(0x100 + MPID_PACK);
            swrite(fdo, &c, 4);
            mkpackh(gts, muxrate, 0);
            fixgts(&gts, &nextgts);
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
                                    cursti->hlt.pal[k >> 8 & 255].t >> 4;
                            sh[2] =
                                    sh[2] << 4
                                |
                                    findmasterpal(cursti, cursti->sel.pal + (k & 255));
                            sh[3] =
                                    sh[3] << 4
                                |
                                    cursti->sel.pal[k & 255].t >> 4;
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
          } /*if mode == DVD_SUB*/
        // header_size is 12 before while starts
      /* search packet start code */
        bytes_send = 0;
        while (bytes_send != sub_size)
          {
            int i, stuffing;
            uint32_t c;
            uint16_t b;
          /* if not first time here */
            if (bytes_send)
                header_size = 4;
            else if (header_size != 12)
                header_size = 9; // not first time
          /* calculate how many bytes to send */
            i = secsize - 20 - header_size - svcd;
            stuffing = i - (sub_size - bytes_send);
            if ( stuffing < 0)
                stuffing = 0;
            else
              {
                i -= stuffing;
                if (stuffing > 7)
                    stuffing = 0;
              } /*if*/
          /* write header */
            c = htonl(0x100 + MPID_PACK);
            swrite(fdo, &c, 4);
            mkpackh(gts, muxrate, 0);
            fixgts(&gts, &nextgts);
/*
  fprintf(stderr, "system time: %d 0x%lx %d\n", gts, ftell(fds), frame);
  fprintf(stderr, "spts=%d\n", spts);
*/
            swrite(fdo, header, 10);
          /* write private stream code */
            c = htonl(0x100 + MPID_PRIVATE1);
            swrite(fdo, &c, 4);
          /* write packet length */
            b = ntohs(i + header_size + svcd + stuffing);
            swrite(fdo, &b, 2);
          /* i has NOT changed here! and is still bytes to send */
            if (header_size == 9)
                mkpesh0(cursti->spts);
            else if (header_size == 12)
                mkpesh1(cursti->spts);
            else
                mkpesh2();
            header[2] += stuffing;
            memset(header + header_size - 1, 0xff, stuffing);
            header[header_size + stuffing - 1] = svcd ? SVCD_SUB_CHANNEL : substr; /* subpicture stream number */
            swrite(fdo, header, header_size + stuffing);
            if (svcd)
              {
              /* 4 byte svcd header */
                const uint16_t cc = htons(subno);
                swrite(fdo, &substr, 1); // current subtitle stream
                if (bytes_send + i == sub_size)
                    seq |= 128;
                swrite(fdo, &seq, 1); // packet number in current sub
                // 0 - up, last packet has bit 7 set
                swrite(fdo, &cc, 2);
              } /*if*/
            seq++;
          /* write i data bytes, increment bytes_send by bytes written */
            swrite(fdo, sub + bytes_send, i);
            bytes_send += i;
          /* test if full sector */
            i += 20 + header_size + stuffing + svcd;
            if (i != secsize)
              {
                unsigned short bs;
              /* if sector not full, write padding? */
              /* write padding code */
                c = htonl(0x100 + MPID_PAD); /* really just padding this time */
                swrite(fdo, &c, 4);
              /* calculate number of padding bytes */
                b = secsize - i - 6;
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
          } /* end while bytes_send ! sub_size */

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
  } /*mux*/

int main(int argc,char **argv)
{
    int fdi;
    unsigned int c, ch, a, frame;
    unsigned short int b, vss;
    unsigned char psbuf[psbufs], ncnt;
    int optch;

    init_locale();
    newsti = 0;
    mode = 0; /* default DVD */
    sub = malloc(SUB_BUFFER_MAX + SUB_BUFFER_HEADROOM);
    if (!sub)
      {
        fprintf(stderr, "ERR: Could not allocate space for sub, aborting.\n");
        exit(1);
      } /*if*/
//fprintf(stderr, "malloc sub=%p\n", sub);
    if (!(cbuf = malloc(65536)))
      {
        fprintf(stderr, "ERR: Could not allocate space for sub buffer, aborting.\n");
        exit(1);
      } /*if*/
    image_init();
    fputs(PACKAGE_HEADER("spumux"), stderr);
    gts = 0;
    nextgts = 0;
    tofs = -1;
    progr = false;
    debug = 0;
    ncnt = 0;
    substr = 0;
    while (-1 != (optch = getopt(argc, argv, "hm:s:v:P")))
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
                fprintf(stderr,"ERR: Mode must be one of dvd, svcd, or cvd\n");
                usage();
              } /*switch*/
        break;
        case 's':
            substr = strtounsigned(optarg, "substream id");
        break;
        case 'v':
            debug = strtounsigned(optarg, "verbosity");
        break;
        case 'P':
            progr = true;
        break;
        case 'h':
            usage();
        break;
        default:
            fprintf(stderr, "WARN: Getopt returned %d\n",optch);
            usage();
        break;
          } /*switch*/
      } /*while*/

    switch(mode)
      {
    case DVD_SUB:
    default:
        svcd = 0;
        substr += DVD_SUB_CHANNEL;
        muxrate = 10080 * 10 / 4; // 0x1131; // 10080 kbps
        secsize = 2048;
    break;
    case CVD_SUB:
        svcd = 0;
        substr += CVD_SUB_CHANNEL;
        muxrate = 1040 * 10/4; //0x0a28; // 1040 kbps
        secsize = 2324;
    break;
    case SVCD_SUB:
        svcd = 4;
        // svcd substream identification works differently...
        // substr += SVCD_SUB_CHANNEL;
        muxrate = 1760 * 10 / 4; //0x1131; // 1760 kbps
        secsize = 2324;
    break;
      } /*switch*/
    if (argc - optind != 1)
      {
        fprintf(stderr, "WARN: Only one argument expected\n");
        usage();
      } /*if*/
    fdi = 0; /* stdin */
    fdo = 1; /* stdout */
    win32_setmode(fdi,O_BINARY);
    win32_setmode(fdo,O_BINARY);
    if (spumux_parse(argv[optind]))
        return -1;
    if (tofs >= 0 && debug > 0)
        fprintf(stderr, "INFO: Subtitles offset by %fs\n", (double)tofs / 90000);
    spuindex = 0;
    skip = 0;
    if (!(sector = malloc(secsize)))
      {
        fprintf(stderr, "ERR: Could not allocate space for sector buffer, aborting.\n");
        exit(1);
      } /*if*/

    newsti = getnextsub();
    max_sub_size = 0;
    header_size = 12;
    vss = 0;
    frame = 0;
    lps = 0;
    gts = 0;
    subno = -1;
    while (1)
      {
        mux(0);
        if (sread(fdi, &c, 4) != 4)
            goto eoi;
        ch = ntohl(c); /* header ID */
        if (ch == 0x100 + MPID_PACK)
          {
l_01ba:
            if (progr) /* show progress */
              {
                if (lps % 1024 * 1024 * 10 < secsize)
                    fprintf(stderr, "INFO: %" PRIu64 " bytes of data written\r", lps);
              } /*if*/
            if (debug > 5)
                fprintf(stderr, "INFO: pack_start_code\n");
            if (sread( fdi, psbuf, psbufs) != psbufs)
                break;
            gts = getgts(psbuf);
            if (gts != -1)
              {
                mux(0);
                fixgts(&gts, &nextgts);
                muxrate = getmuxr(psbuf);
              }
            else
              {
                if (debug >- 1)
                    fprintf(stderr, "WARN: Incorrect pack header\n");
                gts = nextgts;
              } /*if*/
            mkpackh(gts, muxrate, 0);
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
    mux(1); // end of input
/*    fprintf(stderr, "max_sub_size=%d\n", max_sub_size); */
    if (subno != 0xffff)
      {
        fprintf(stderr,
            "INFO: %d subtitles added, %d subtitles skipped, stream: %d, offset: %.2f\n",
            subno + 1, skip, substr, (double)tofs / 90000);
      }
    else
      {
        fprintf(stderr, "WARN: no subtitles added\n");
      } /*if*/
    textsub_statistics();
    textsub_finish();
    image_shutdown();
    return 0;
} /* end function main */
