/*
    Utility to extract audio/video streams and dump information about
    packetes in an MPEG stream.
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

#include "config.h"

#include "compat.h"

#include <setjmp.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>

// this is needed for FreeBSD and Windows
#include <sys/time.h>

#include "common.h"

// #define SHOWDATA

static unsigned int
    inputpos = 0, /* position in stdin */
    queuedlen = 0; /* total nr bytes awaiting writing to output files */

const char * const frametype = "0IPB4567";

#define SCRTIME 27000000
#define PTSTIME 90000

#define WAITLEN (256*1024)

#define BUFLEN (65536)

struct fdbuf
  {
    int pos,len;
    struct fdbuf *next;
    unsigned char buf[BUFLEN];
  };

#define MAX_FILES 256
  /* how many output files I can write at once -- can't have
    more than this number of MPEG streams anyway */

/* values for ofd.fd: */
#define FD_TOOPEN (-2) /* open file named ofd.fname on first write */
#define FD_CLOSED (-1)

struct ofd
  {
    int fd;
    char *fname;
    struct fdbuf *firstbuf,**lastbufptr; /* queue of buffers awaiting writing */
    int len;
    bool isvalid;
  } outputfds[256]; /* files to write, indexed by stream id */

static int
    ofdlist[MAX_FILES], /* indexes of used entries in outputfds */
    numofd; /* contiguous used size of ofdlist */

static int firstpts[256]; /* indexed by stream id */

static bool
    outputenglish = true,
    nounknown = false,
    closing = false,
    outputmplex = false;
static int
    audiodrop = 0;

static fd_set
  /* should be local to dowork routine */
    rfd, wfd;

static int64_t readpts(const unsigned char *buf)
  {
    return
            (int64_t)((buf[0] & 0xf) >> 1) << 30
        |
            ((int64_t)buf[1] << 8 | buf[2]) >> 1 << 15
        |
            ((int64_t)buf[3] << 8 | buf[4]) >> 1;
  } /*readpts*/

static bool hasbecomevalid(int stream, const struct ofd *o)
  /* checks if there is a valid packet header at the start of the data to be
    written to the specified output stream. Assumes there is at least 4 bytes
    of data waiting to be written. */
  {
    bool valid = false;
    unsigned int realquad;
    const struct fdbuf * const f1 = o->firstbuf;
    if (f1 != 0)
      {
        unsigned char quad[4];
        int i;
        const struct fdbuf * const f2 = f1->next;
        valid = true; /* next assumption */
        for (i = 0; valid && i < 4; i++)
          {
            if (f1->len - f1->pos - i > 0)
                quad[i] = f1->buf[f1->pos + i];
            else if (f2 != 0)
                quad[i] = f2->buf[f2->pos + i - (f1->len - f1->pos)];
            else
                valid = false;
          } /*for*/
        if (valid)
          {
            realquad =
                    quad[0] << 24
                |
                    quad[1] << 16
                |
                    quad[2] << 8
                |
                    quad[3];
          } /*if*/
      } /*if*/
    return
            valid
        &&
            (
                    stream >= MPID_AUDIO_FIRST
                &&
                    stream <= MPID_AUDIO_LAST
                &&
                    (realquad & 0xFFE00000) == 0xFFE00000
            ||
                    stream >= MPID_VIDEO_FIRST
                &&
                    stream <= MPID_VIDEO_LAST
                &&
                    realquad == 0x100 + MPID_SEQUENCE
            );
  } /*hasbecomevalid*/

static bool dowork
  (
    bool checkin /* whether to check if bytes are available to be read from stdin */
  )
  /* writes pending packets to output streams. This is done concurrently to allow
    for use with pipes. Returns true iff stdin has input available. */
  {
    int highestfd = -1;
    struct timeval tv;
    if (!numofd)
        return checkin;
    if (checkin)
      {
        FD_SET(STDIN_FILENO, &rfd);
        highestfd = STDIN_FILENO;
      }
    else
      {
        FD_CLR(STDIN_FILENO, &rfd);
      } /*if*/
    while (true)
      {
        int i, minq = -1;
        for (i = 0; i < numofd; i++)
          {
            struct ofd *o = &outputfds[ofdlist[i]];
            if (o->fd != FD_CLOSED)
              {
                if (o->fd == FD_TOOPEN)
                  {
                    int fd;
                    fd = open(o->fname, O_CREAT | O_WRONLY | O_NONBLOCK, 0666);
                    if (fd == -1 && errno == ENXIO)
                      {
                        continue; /* try again later, in case pipe not created yet */
                      } /*if*/
                    if (fd == -1)
                      {
                        fprintf(stderr,"Cannot open %s: %s\n",o->fname,strerror(errno));
                        exit(1);
                      } /*if*/
                    o->fd = fd;
                  } /*if*/
                // at this point, fd >= 0
                if (minq == -1 || o->len < minq)
                  {
                    minq = o->len;
                  } /*if*/
                if ((o->len > 0 && o->isvalid) || o->len >= 4)
                  {
                    if (o->fd > highestfd)
                        highestfd = o->fd;
                    FD_SET(o->fd, &wfd);
                  }
                else
                  {
                    FD_CLR(o->fd, &wfd);
                    if (closing)
                      {
                        close(o->fd);
                        o->fd = FD_CLOSED;
                      } /*if*/
                  } /*if*/
              } /*if*/
          } /*for*/
        // if all the open files have more then WAITLEN bytes of data
        // queued up, then don't process anymore
        if (minq >= WAITLEN)
          {
            FD_CLR(STDIN_FILENO, &rfd);
            break;
          }
        else if (minq >= 0 || outputmplex) // as long as one file is open, continue
            break;
        sleep(1);
      } /*while*/
    if (highestfd == -1)
        return false; /* nothing to do */
    tv.tv_sec = 1; // set timeout to 1 second just in case any files need to be opened
    tv.tv_usec = 0;
    if (select(highestfd + 1, &rfd, &wfd, NULL, &tv) > 0)
      {
        int i;
        for (i = 0; i < numofd; i++)
          {
            struct ofd * const o = &outputfds[ofdlist[i]];
            if (o->fd >= 0 && FD_ISSET(o->fd, &wfd))
              {
                struct fdbuf * const f = o->firstbuf;
                int written;
                if (!o->isvalid && hasbecomevalid(ofdlist[i], o))
                    o->isvalid = true;
                if (o->isvalid)
                    written = write(o->fd, f->buf + f->pos, f->len - f->pos);
                else if (f->len - f->pos > 0)
                    written = 1; /* discard one byte while waiting for valid packet */
                else
                    written = 0;
                if (written == -1)
                  {
                    fprintf(stderr,"Error writing to fifo: %s\n",strerror(errno));
                    exit(1);
                  } /*if*/
                queuedlen -= written;
                f->pos += written;
                if (f->pos == f->len)
                  {
                  /* finished writing buffer at head of queue */
                    o->firstbuf = f->next;
                    if (o->lastbufptr == &f->next)
                        o->lastbufptr = &o->firstbuf;
                    free(f);
                  } /*if*/
                o->len -= written;
              } /*if*/
          } /*for*/
        if (FD_ISSET(STDIN_FILENO, &rfd))
            return true;
      } /*if*/
    return false;
  } /*dowork*/

static void flushwork(void)
  /* flushes the work queue. */
  {
    closing = true;
    while (queuedlen)
        dowork(false);
  } /*flushwork*/

static void forceread(void *ptr, int len, bool required)
  /* reads the specified number of bytes from standard input, finishing processing
    if EOF is reached. */
  {
    int nrbytes;
    while (!dowork(true))
      /* flush output queues while waiting for more input */;
    nrbytes = fread(ptr, 1, len, stdin);
    if (nrbytes != len)
      {
        bool success = true;
        if (nrbytes < 0)
          {
            fprintf(stderr, "Error %d reading: %s\n", errno, strerror(errno));
            success = false;
          }
        else if (nrbytes < len && required)
          {
            fprintf(stderr, "Unexpected read EOF\n");
            success = false;
          } /*if*/
        flushwork();
        exit(success ? 0 : 1);
      } /*if*/
    inputpos += len;
  } /*forceread*/

static void writetostream(int stream, unsigned char *buf, int len)
  /* queues more data to be written to the output file for the specified stream id,
    if I am writing it. */
  {
    struct ofd * const o = &outputfds[stream];
    if (o->fd == FD_CLOSED) /* not extracting this stream */
        return;
    while (len > 0)
      {
        int thislen;
        struct fdbuf *fb;
        if (!o->lastbufptr[0])
          {
            o->lastbufptr[0] = malloc(sizeof(struct fdbuf));
            o->lastbufptr[0]->pos = 0;
            o->lastbufptr[0]->len = 0;
            o->lastbufptr[0]->next = 0;
          } /*if*/
        fb = o->lastbufptr[0];
        thislen = BUFLEN - fb->len;
        if (!thislen)
          {
            o->lastbufptr = &fb->next;
            continue;
          } /*if*/
        if (thislen > len)
            thislen = len;
        o->len += thislen;
        memcpy(fb->buf + fb->len, buf, thislen);
        fb->len += thislen;
        len -= thislen;
        buf += thislen;
        queuedlen += thislen;
      } /*while*/
  } /*writetostream*/

static void process_packets
  (
    void (*readinput)(void *ptr, int len, bool required),
    bool recursed
  )
  {
    unsigned char hdr[4];
    unsigned char buf[200];
    bool fetchhdr = true;
    while (true)
      {
        const int disppos = fetchhdr ? inputpos : inputpos - 4; /* where packet actually started */
        bool handled = true;
        int hdrid;
        if (fetchhdr)
          {
            readinput(hdr, 4, false);
          } /*if*/
        fetchhdr = true; /* initial assumption */
        hdrid =
                hdr[0] << 24
            |
                hdr[1] << 16
            |
                hdr[2] << 8
            |
                hdr[3];
        switch (hdrid)
          {
      // start codes:
        case 0x100 + MPID_PICTURE: // picture header
            readinput(buf, 4, true);
            if (outputenglish)
                printf
                  (
                    "%08x: picture hdr, frametype=%c, temporal=%d\n",
                    disppos,
                    frametype[buf[1] >> 3 & 7],
                    buf[0] << 2 | buf[1] >> 6
                  );
        break;

        case 0x100 + MPID_SEQUENCE: // sequence header
            readinput(buf, 8, true);
            if (outputenglish)
                printf
                  (
                    "%08x: sequence hdr: %dx%d, a/f:%02x, bitrate=%d\n",
                    disppos,
                    buf[0] << 4 | buf[1] >> 4,
                    buf[1] << 8 & 0xf00 | buf[2],
                    buf[3],
                    buf[4] << 10 | buf[5] << 2 | buf[6] >> 6
                  );
            if (buf[7] & 2)
                readinput(buf + 8, 64, true);
            if (buf[7] & 1)
                readinput(buf + 8, 64, true);
        break;

        case 0x100 + MPID_EXTENSION: // extension header
            readinput(buf, 1, true);
            switch (buf[0] >> 4)
              {
            case 1:
                if (outputenglish)
                    printf("%08x: sequence extension hdr\n", disppos);
                readinput(buf + 1, 5, true);
            break;
            case 2:
                if (outputenglish)
                    printf("%08x: sequence display extension hdr\n", disppos);
                readinput(buf + 1, (buf[0] & 1) ? 7 : 3, true);
            break;
            case 7:
                if (outputenglish)
                    printf("%08x: picture display extension hdr\n", disppos);
            break;
            case 8:
                readinput(buf + 1, 4, true);
                if (buf[4] & 64)
                    readinput(buf + 5, 2, true);
                if (outputenglish)
                  {
                    printf
                      (
                        "%08x: picture coding extension hdr%s%s\n",
                        disppos,
                        (buf[3] & 0x80) ? ", top" : ", bottom",
                        (buf[3] & 2) ? ", repeat" : ""
                     );
                  } /*if*/
            break;
            default:
                if (outputenglish)
                    printf("%08x: extension hdr %x\n", disppos, buf[0] >> 4);
            break;
              } /*switch*/
        break;

        case 0x100 + MPID_SEQUENCE_END: // end of sequence
            if (outputenglish)
                printf("%08x: end of sequence\n", disppos);
        break;

        case 0x100 + MPID_GOP: // group of pictures
            readinput(buf, 4, true);
            if (outputenglish)
              {
                printf
                  (
                    "%08x: GOP: %s%d:%02d:%02d.%02d, %s%s\n",
                    disppos,
                    buf[0] & 128 ? "drop, " : "",
                    buf[0] >> 2 & 31,
                    (buf[0] << 4 | buf[1] >> 4) & 63,
                    (buf[1] << 3 | buf[2] >> 5) & 63,
                    (buf[2] << 1 | buf[3] >> 7) & 63,
                    buf[3] & 64 ? "closed" : "open",
                    buf[3] & 32 ? ", broken" : ""
                    );
              } /*if*/
        break;

        case 0x100 + MPID_PROGRAM_END: // end of program stream
            if (outputenglish)
                printf("%08x: end of program stream\n", disppos);
        break;

        case 0x100 + MPID_PACK: // mpeg_pack_header
          {
            uint32_t scr,scrhi,scrext;
            int64_t fulltime;
            bool mpeg2 = true;
            readinput(buf, 8, true);
            if ((buf[0] & 0xC0) == 0x40)
              {
                readinput(buf + 8, 2, true);
                scrhi = (buf[0] & 0x20) >> 5;
                scr =
                        (buf[0] & 0x18) << 27
                    |
                        (buf[0] & 3) << 28
                    |
                        buf[1] << 20
                    |
                        (buf[2] & 0xf8) << 12
                    |
                        (buf[2] & 3) << 13
                    |
                        buf[3] << 5
                    |
                        (buf[4] & 0xf8) >> 3;
                scrext =
                        (buf[4] & 3) << 7
                    |
                        buf[5] >> 1;
                if (scrext >= 300 && outputenglish)
                  {
                    printf("WARN: scrext in pack hdr > 300: %u\n", scrext);
                  } /*if*/
                fulltime = (int64_t)scrhi << 32 | (int64_t)scr;
                fulltime *= 300;
                fulltime += scrext;
                mpeg2 = true;
              }
            else if ((buf[0] & 0xF0) == 0x20)
              {
                mpeg2 = false;
                fulltime = readpts(buf);
                fulltime *= 300;
              }
            else
              {
                if (outputenglish)
                    printf("WARN: unknown pack header version\n");
                fulltime = 0;
              } /*if*/
            if (outputenglish)
                printf
                  (
                    "%08x: mpeg%c pack hdr, %" PRId64 ".%03" PRId64 " sec\n",
                    disppos,
                    mpeg2 ? '2' : '1',
                    fulltime / SCRTIME,
                    (fulltime % SCRTIME) / (SCRTIME / 1000)
                  );
          }
        break;
        default:
            handled = false;
        break;
      } /*switch*/
        if
          (
                !handled
            &&
                !recursed
            &&
                (
                    hdrid == 0x100 + MPID_SYSTEM
                ||
                    hdrid == 0x100 + MPID_PRIVATE1
                ||
                    hdrid == 0x100 + MPID_PAD
                ||
                    hdrid == 0x100 + MPID_PRIVATE2
                ||
                    hdrid >= 0x100 + MPID_AUDIO_FIRST && hdrid <= 0x100 + MPID_AUDIO_LAST
                ||
                    hdrid >= 0x100 + MPID_VIDEO_FIRST && hdrid <= 0x100 + MPID_VIDEO_LAST
                )
          )
          {
            bool has_extension = false;
            unsigned int headerlen, packetlen, contentoffs;
            int readlen;
            bool dowrite = !recursed;
            const int packetid = hdrid & 255;
            if (outputenglish)
                printf("%08x: ", disppos);
            if (packetid == MPID_SYSTEM)
              {
                if (outputenglish)
                    printf("system header");
              }
            else if (packetid == MPID_PRIVATE1)
              {
                if (outputenglish)
                    printf("pes private1");
                has_extension = true;
              }
            else if (packetid == MPID_PAD)
              {
                if (outputenglish)
                    printf("pes padding");
              }
            else if (packetid == MPID_PRIVATE2)
              {
                if (outputenglish)
                    printf("pes private2");
              }
            else if (packetid >= MPID_AUDIO_FIRST && packetid <= MPID_AUDIO_LAST)
              {
                if (outputenglish)
                    printf("pes audio %d", packetid - MPID_AUDIO_FIRST);
                if (audiodrop)
                  {
                    dowrite = false;
                    audiodrop--;
                  } /*if*/
                has_extension = true;
              }
            else if (packetid >= MPID_VIDEO_FIRST && packetid <= MPID_VIDEO_LAST)
              {
                if (outputenglish)
                    printf("pes video %d", packetid - MPID_VIDEO_FIRST);
                has_extension = true;
              } /*if*/
            readinput(buf, 2, true); // pes packet length
            packetlen = buf[0] << 8 | buf[1];
            readlen = packetlen > sizeof buf ? sizeof buf : packetlen;
            readinput(buf, readlen, true);
            packetlen -= readlen;
            headerlen = buf[2]; /* length of packet header */
            contentoffs = 3 + headerlen; /* beginning of packet content */
            if (outputenglish)
              {
                if (packetid == MPID_PRIVATE1) // private stream 1
                  {
                    const int sid = buf[contentoffs]; /* substream ID is first byte after header */
                    switch (sid & 0xf8)
                      {
                    case 0x20:
                    case 0x28:
                    case 0x30:
                    case 0x38:
                        printf(", subpicture %d", sid & 0x1f);
                    break;
                    case 0x80:
                        printf(", AC3 audio %d", sid & 7);
                    break;
                    case 0x88:
                        printf(", DTS audio %d", sid & 7);
                    case 0xa0:
                        printf(", LPCM audio %d", sid & 7);
                    break;
                    default:
                        printf(", substream id 0x%02x", sid);
                    break;
                      } /*switch*/
                  }
                else if (packetid == MPID_PRIVATE2) // private stream 2
                  {
                    const int sid = buf[0];
                    switch (sid)
                      {
                    case 0:
                        printf(", PCI");
                    break;
                    case 1:
                        printf(", DSI");
                    break;
                    default:
                        printf(", substream id 0x%02x", sid);
                    break;
                      } /*switch*/
                  } /*if*/
                printf("; length=%d", packetlen + readlen);
                if (has_extension)
                  {
                    int eptr;
                    bool has_std = false, has_pts, has_dts;
                    int hdroffs, std=0, std_scale=0;
                    const bool mpeg2 = (buf[0] & 0xC0) == 0x80;
                    if (mpeg2)
                      {
                        hdroffs = contentoffs;
                        eptr = 3;
                        has_pts = (buf[1] & 128) != 0;
                        has_dts = (buf[1] & 64) != 0;
                      }
                    else
                      {
                        hdroffs = 0;
                        while (hdroffs < sizeof(buf) && buf[hdroffs] == 0xff)
                            hdroffs++;
                        if ((buf[hdroffs] & 0xC0) == 0x40)
                          {
                            has_std = true;
                            std_scale = (buf[hdroffs] & 32) ? 1024 : 128;
                            std = ((buf[hdroffs] & 31) * 256 + buf[hdroffs + 1]) * std_scale;
                            hdroffs += 2;
                          } /*if*/
                        eptr = hdroffs;
                        has_pts = (buf[hdroffs] & 0xE0) == 0x20;
                        has_dts = (buf[hdroffs] & 0xF0) == 0x30;
                      } /*if*/
                    printf("; hdr=%d", hdroffs);
                    if (has_pts)
                      {
                        int64_t pts;
                        pts = readpts(buf + eptr);
                        eptr += 5;
                        printf
                          (
                            "; pts %" PRId64 ".%03" PRId64 " sec",
                            pts / PTSTIME,
                            (pts % PTSTIME) / (PTSTIME / 1000)
                          );
                      } /*if*/
                    if (has_dts)
                      {
                        int64_t dts;
                        dts = readpts(buf + eptr);
                        eptr += 5;
                        printf
                          (
                            "; dts %" PRId64 ".%03" PRId64 " sec",
                            dts / PTSTIME,
                            (dts % PTSTIME) / (PTSTIME / 1000)
                          );
                      } /*if*/
                    if (mpeg2)
                      {
                        if (buf[1] & 32)
                          {
                            printf("; escr");
                            eptr += 6;
                          } /*if*/
                        if (buf[1] & 16)
                          {
                            printf("; es");
                            eptr += 2;
                          } /*if*/
                        if (buf[1] & 4)
                          {
                            printf("; ci");
                            eptr++;
                          } /*if*/
                        if (buf[1] & 2)
                          {
                            printf("; crc");
                            eptr += 2;
                          } /*if*/
                        if (buf[1] & 1)
                          {
                            int pef = buf[eptr];
                            eptr++;
                            printf("; (pext)");
                            if (pef & 128)
                              {
                                printf("; user");
                                eptr += 16;
                              } /*if*/
                            if (pef & 64)
                              {
                                printf("; pack");
                                eptr++;
                              } /*if*/
                            if (pef & 32)
                              {
                                printf("; ppscf");
                                eptr += 2;
                              } /*if*/
                            if (pef & 16)
                              {
                                std_scale = (buf[eptr] & 32) ? 1024 : 128;
                                printf
                                  (
                                    "; pstd=%d (scale=%d)",
                                    ((buf[eptr] & 31) * 256 + buf[eptr + 1]) * std_scale,
                                    std_scale
                                  );
                                eptr += 2;
                              } /*if*/
                            if (pef & 1)
                              {
                                printf("; (pext2)");
                              /* eptr += 2; */ /* not further used */
                              } /*if*/
                          } /*if*/
                      }
                    else
                      {
                        if (has_std)
                            printf("; pstd=%d (scale=%d)", std, std_scale);
                      } /*if*/
                  } /*if*/
                printf("\n");
              } /*if*/
            if (outputmplex && has_extension)
              {
                if ((buf[1] & 128) != 0 && firstpts[packetid] == -1)
                    firstpts[packetid] = readpts(buf + 3);
                if (firstpts[MPID_AUDIO_FIRST] != -1 && firstpts[MPID_VIDEO_FIRST] != -1)
                  {
                    printf("%d\n", firstpts[MPID_VIDEO_FIRST] - firstpts[MPID_AUDIO_FIRST]);
                    fflush(stdout);
                    close(1);
                    outputmplex = false;
                    if (!numofd)
                        exit(0);
                  } /*if*/
              } /*if*/
#ifdef SHOWDATA
            if (has_extension && outputenglish)
              {
                int j;
                printf("  ");
                for (j=0; j<16; j++)
                    printf(" %02x", buf[j + contentoffs]);
                printf("\n");
              } /*if*/
#endif
            if (!recursed)
              {
                if (has_extension && dowrite)
                  {
                    writetostream(packetid, buf + contentoffs, readlen - contentoffs);
                  } /*if*/
#if defined(HAVE_NESTED_ROUTINES)
                if (outputenglish && packetid >= MPID_VIDEO_FIRST && packetid <= MPID_VIDEO_LAST)
                  {
                  /* look inside PES packet to report on details of video packets */
                    unsigned int remaining = readlen;
                    jmp_buf resume;
                  /* GCC extension! nested routine */
                    void bufread(void *ptr, int len, bool required)
                      {
                        const unsigned int tocopy = remaining > len ? len : remaining;
                        if (tocopy != 0)
                          {
                            memcpy(ptr, buf + contentoffs, tocopy);
                            ptr = (unsigned char *)ptr + tocopy;
                            len -= tocopy;
                            contentoffs += tocopy;
                            remaining -= tocopy;
                            inputpos += tocopy;
                          } /*if*/
                        if (len != 0)
                          {
                          /* read more of packet */
                            const unsigned int toread = packetlen < len ? packetlen : len;
                            readinput(ptr, toread, required);
                            if (dowrite)
                              {
                                writetostream(packetid, ptr, toread);
                              } /*if*/
                            packetlen -= toread;
                            len -= toread;
                            if (len != 0)
                              {
                                if (false /*required*/)
                                  {
                                    fprintf(stderr, "Unexpected nested read EOF\n");
                                  } /*if*/
                                longjmp(resume, 1);
                              } /*if*/
                          } /*if*/
                      } /*bufread*/
                    inputpos -= remaining; /* rewind to start of packet content */
                    if (!setjmp(resume))
                      {
                        process_packets(bufread, true);
                      } /*if*/
                  }
                else
#endif
                  {
                    while (packetlen != 0)
                      {
                        readlen = packetlen > sizeof buf ? sizeof(buf) : packetlen;
                        readinput(buf, readlen, true);
                        if (dowrite)
                          {
                            writetostream(packetid, buf, readlen);
                          } /*if*/
                        packetlen -= readlen;
                      } /*while*/
                  } /*if*/
              } /*if*/
            handled = true;
          } /*if*/
        if (!handled)
          {
            do
              {
                if (!recursed && outputenglish && !nounknown)
                    printf("%08x: unknown hdr: %08x\n", disppos, hdrid);
                hdr[0] = hdr[1];
                hdr[1] = hdr[2];
                hdr[2] = hdr[3];
                readinput(hdr + 3, 1, false);
                hdrid =
                        hdr[0] << 24
                    |
                        hdr[1] << 16
                    |
                        hdr[2] << 8
                    |
                        hdr[3];
              }
            while ((hdrid & 0xffffff00) != 0x100);
            fetchhdr = false; /* already got it */
          } /*if*/
      } /*while*/
  } /*process_packets*/

int main(int argc,char **argv)
  {
    bool skiptohdr = false;
    fputs(PACKAGE_HEADER("mpeg2desc"), stderr);
      {
        int outputstream = 0, oc, i;
        for (oc = 0; oc < 256; oc++)
            outputfds[oc].fd = FD_CLOSED;
        while (-1 != (oc = getopt(argc,argv,"ha:v:o:msd:u")))
          {
            switch (oc)
              {
            case 'd':
                audiodrop = strtounsigned(optarg, "audio drop count");
            break;
            case 'a':
            case 'v':
                if (outputstream)
                  {
                    fprintf(stderr,"can only output one stream to stdout at a time\n; use -o to output more than\none stream\n");
                    exit(1);
                  } /*if*/
                outputstream = (oc == 'a' ? MPID_AUDIO_FIRST : MPID_VIDEO_FIRST) + strtounsigned(optarg, "stream id");
            break;
            case 'm':
                outputmplex = true;
            break;
            case 's':
                skiptohdr = true;
            break;
            case 'o':
                if (!outputstream)
                  {
                    fprintf(stderr,"no stream selected for '%s'\n",optarg);
                    exit(1);
                  } /*if*/
                outputfds[outputstream].fd = FD_TOOPEN;
                outputfds[outputstream].fname = optarg;
                outputstream = 0;
            break;
            case 'u':
                nounknown = true;
            break;
          // case 'h':
            default:
                fprintf(stderr,
                        "usage: mpeg2desc [options] < movie.mpg\n"
                        "\t-a #: output audio stream # to stdout\n"
                        "\t-v #: output video stream # to stdout\n"
                        "\t-o FILE: output previous stream to FILE instead of stdout\n"
                        "\t-s: skip to first valid header -- ensures mplex can handle output\n"
                        "\t-m: output mplex offset to stdout\n"
                        "\t-u: ignore unknown hdrs\n"
                        "\t-h: help\n"
                    );
                exit(1);
            break;
              } /*switch*/
          } /*while*/
        if (outputstream)
          {
            outputenglish = false;
            outputfds[outputstream].fd = STDOUT_FILENO;
          } /*if*/
        if (outputmplex)
          {
            if (!outputenglish)
              {
                fprintf(stderr,"Cannot output a stream and the mplex offset at the same time\n");
                exit(1);
              } /*if*/
            outputenglish = false;
          } /*if*/
        numofd = 0;
        for (oc = 0; oc < MAX_FILES; oc++)
            if (outputfds[oc].fd != -1)
              {
                ofdlist[numofd++] = oc;
                outputfds[oc].firstbuf = 0;
                outputfds[oc].lastbufptr = &outputfds[oc].firstbuf;
                outputfds[oc].len = 0;
                outputfds[oc].isvalid = !skiptohdr;
              } /*if; for*/
        FD_ZERO(&rfd);
        FD_ZERO(&wfd);
        for (i = 0; i < 256; i++)
          {
            firstpts[i] = -1;
          } /*for*/
      }
    process_packets(forceread, false);
    return
        0;
  } /*main*/
