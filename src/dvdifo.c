/*
    dvdauthor -- generation of IFO and BUP files
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
#include <errno.h>
#include <assert.h>

#include "dvdauthor.h"
#include "da-internal.h"

static unsigned char *
    bigbuf = 0;
static size_t
    bigbufsize = 0;

#define MIN_IFO_SECTORS 16
  /* need to ensure that .IFO and .BUP files end up in different ECC blocks */

static void buf_init()
  /* ensures there's no leftover junk in bigbuf. */
  {
    free(bigbuf);
    bigbuf = 0;
    bigbufsize = 0;
  } /*buf_init*/

static void buf_need(size_t sizeneeded)
  /* ensures that bigbuf is at least sizeneeded bytes in size. */
  {
    if (sizeneeded > bigbufsize)
      {
        const size_t newbufsize = (sizeneeded + 2047) / 2048 * 2048; /* allocate next whole sector */
        // fprintf(stderr, "INFO: need_buf: bigbufsize now %ld sectors.\n", newbufsize / 2048);
        bigbuf = realloc(bigbuf, newbufsize);
        if (bigbuf == 0)
          {
            fprintf(stderr, "ERR:  buf_need: out of memory\n");
            exit(1);
          } /*if*/
        memset(bigbuf + bigbufsize, 0, newbufsize - bigbufsize); /* zero added memory */
        bigbufsize = newbufsize;
      } /*if*/
  } /*buf_need*/

static void buf_write1(size_t o, unsigned char b)
  /* puts a byte into buf at offset o. */
  {
    buf_need(o + 1);
    bigbuf[o] = b;
  }/*buf_write1*/

static void buf_write2(size_t o, unsigned short w)
  /* puts a big-endian word into buf at offset o. */
  {
    buf_need(o + 2);
    bigbuf[o] = w >> 8 & 255;
    bigbuf[o + 1] = w & 255;
  } /*buf_write2*/

static void buf_write4(size_t o, unsigned int l)
  /* puts a big-endian longword into buf at offset o. */
  {
    buf_need(o + 4);
    bigbuf[o] = l >> 24 & 255;
    bigbuf[o + 1] = l >> 16 & 255;
    bigbuf[o + 2] = l >> 8 & 255;
    bigbuf[o + 3] = l & 255;
  } /*buf_write4*/

static void buf_write8b
  (
    size_t o,
    unsigned char b0,
    unsigned char b1,
    unsigned char b2,
    unsigned char b3,
    unsigned char b4,
    unsigned char b5,
    unsigned char b6,
    unsigned char b7
  )
  /* writes 8 bytes into buf at offset o. */
  {
    buf_need(o + 8);
    bigbuf[o] = b0;
    bigbuf[o + 1] = b1;
    bigbuf[o + 2] = b2;
    bigbuf[o + 3] = b3;
    bigbuf[o + 4] = b4;
    bigbuf[o + 5] = b5;
    bigbuf[o + 6] = b6;
    bigbuf[o + 7] = b7;
  } /*buf_write8b*/

static void nfwrite(const void *ptr, size_t len, FILE *h)
  /* writes to h, or turns into a noop if h is null. */
  {
    if (h)
      {
        errno = 0; /* where is this being set? */
        (void)fwrite(ptr, len, 1, h);
        if (errno != 0)
          {
            fprintf
              (
                stderr,
                "\nERR:  Error %d -- %s -- writing output IFO\n",
                errno,
                strerror(errno)
              );
            exit(1);
          } /*if*/
      } /*if*/
  } /*nfwrite*/

static void nfpad(size_t len, FILE *h)
  /* writes len bytes of padding to h, or turns into a noop if h is null. */
  {
    static unsigned char buf[2048];
    static size_t himark = 0;
    if (h != NULL)
      {
        while (len != 0)
          {
            size_t uselen = sizeof buf;
            if (uselen > len)
              {
                uselen = len;
              } /*if*/
            if (uselen > himark)
              {
                memset(buf + himark, 0, uselen - himark);
                himark = uselen;
              } /*if*/
            nfwrite(buf, uselen, h);
            len -= uselen;
          } /*while*/
      } /*if*/
  } /*nfpad*/

static const struct vobuinfo *globalfindvobu(const struct pgc *ch, int pts)
  /* finds the VOBU spanning the specified time. */
  {
    int s, c, ci;
    for (s = 0; s < ch->numsources; s++)
      {
        const struct source * const thissource = ch->sources[s];
        for (c = 0; c < thissource->numcells; c++)
          {
            const struct cell * const thiscell = &thissource->cells[c];
            int span = 0;
            const int fv = findcellvobu(thissource->vob, thiscell->scellid);
            if (pts < 0)
                return
                    &thissource->vob->vobu[fv];
            for (ci = thiscell->scellid; ci < thiscell->ecellid; ci++)
                span += getcellpts(thissource->vob, ci);
            if (pts < span)
              {
              /* desired time lies within timespan of this cell */
                const int r = findvobu
                  (
                    /*va =*/ thissource->vob,
                    /*pts =*/ pts + thissource->vob->vobu[fv].sectpts[0],
                      /* offset from start time */
                    /*l =*/ fv,
                    /*h =*/ thissource->vob->numvobus-1
                  );
                return
                    &thissource->vob->vobu[r];
              } /*if*/
            pts -= span; /* desired time lies beyond this cell */
          } /*for*/
      } /*for*/
    // return last vob
    // if (ch->numsources) {
    // struct vob *s = ch->sources[ch->numsources - 1];
    // return &s->vobu[s->numvobus - 1];
    // }
    return
        0;
  } /*globalfindvobu*/

static int getvoblen(const struct vobgroup *va)
  /* returns the length in sectors of the VOB group. */
  {
    int i;
    for (i = va->numvobs - 1; i >= 0; i--)
        if (va->vobs[i]->numvobus)
            return
                va->vobs[i]->vobu[va->vobs[i]->numvobus - 1].lastsector + 1;
    return
        0;
  } /*getvoblen*/

static unsigned int pts_seconds_to_ticks(const struct vobgroup *va, int nsec)
  /* converts nsec seconds to clock ticks adjusted for the video frame rate. */
  {
    return nsec * getratedenom(va);
  } /*pts_seconds_to_ticks*/

static unsigned int pts_ticks_to_seconds(const struct vobgroup *va, int pts)
  /* converts pts frame-rate-adjusted clock ticks to truncated seconds. */
  {
    return pts / getratedenom(va);
  } /*pts_ticks_to_seconds*/

static int get_pgc_duration_seconds(const struct pgcgroup *va, int c)
  /* returns the duration in truncated seconds of the PGC with the specified index. */
  {
    // we subtract 1 because there is a bug if getptsspan() returns
    // an exact multiple of 90090*units; if so, then the last entry of the
    // TMAPT table cannot be properly computed, because that entry will have
    // fallen off the end of the VOBU table
    return pts_ticks_to_seconds(va->pg_vg, getptsspan(va->pgcs[c]) - 1);
  } /*get_pgc_duration_seconds*/

static int secunit(int ns)
  /* returns ns / 2040 rounded up. This is the duration in seconds of each VTS_TMAP entry
    for a PGC with total duration ns to ensure the number of entries never exceeds 2048. */
  {
    const int maxunits = 2040; /* ensure nr entries don't exceed 2040, just to be safe I guess */
    if (!ns)
        return 1; /* minimum unit of 1 second */
    return
        (ns + maxunits - 1) / maxunits;
  } /*secunit*/

static int tmapt_block_size(const struct pgcgroup *va, int pgc)
  /* computes the size of the VTS_TMAP entries for one PGC. */
  {
    int v = get_pgc_duration_seconds(va, pgc);
      /* start by assuming one VOBU per second (VOBUs shouldn't be longer than one second) */
    v = v / secunit(v); /* if that would be too many, then adjust to one per n seconds */
    return
        v * 4 + 4; /* 4-byte header plus 4 bytes per VOBU */
  } /*tmapt_block_size*/

static int sizeTMAPT(const struct pgcgroup *va)
  /* computes the total size of all the VTS_TMAP arrays for this PGC group. */
  {
    int s = 0, i;
    for (i = 0; i < va->numpgcs; i++)
        s += tmapt_block_size(va, i);
    return
        s + va->numpgcs * 4 + 8;
  } /*sizeTMAPT*/

static int numsectTMAPT(const struct pgcgroup *va)
  /* computes the total number of sectors to hold all the VTS_TMAP arrays for this PGC group. */
  {
    return
        (sizeTMAPT(va) + 2047) / 2048;
  } /*numsectTMAPT*/

static void CreateTMAPT(FILE *h, const struct pgcgroup *va)
/* creates the VTS_TMAPTI structure which contains the time maps for each PGC. */
{
    int pgcindex, mapblock;
    unsigned char buf[8];

    write2(buf, va->numpgcs); /* nr program chains, low word */
    write2(buf + 2, 0); /* nr program chains, high word */
    write4(buf + 4, sizeTMAPT(va) - 1); /* end address (last byte of last VTS_TMAP) */
    nfwrite(buf, 8, h);

    mapblock = 8 + 4 * va->numpgcs;
    for (pgcindex = 0; pgcindex < va->numpgcs; pgcindex++)
      {
        write4(buf, mapblock); /* offset to VTS_TMAP[pgcindex + 1] */
        nfwrite(buf, 4, h);
        mapblock += tmapt_block_size(va, pgcindex);
      } /*for*/

    for (pgcindex = 0; pgcindex < va->numpgcs; pgcindex++)
      {
      /* fill in each VTS_TMAP */
        int numtmapt = get_pgc_duration_seconds(va, pgcindex), ptsbase, j;
        const int units = secunit(numtmapt);
        const struct pgc * const thispgc = va->pgcs[pgcindex];

        numtmapt /= units;
        buf[0] = units; /* time units, seconds */
        buf[1] = 0; /* unused */
        write2(buf + 2, numtmapt); /* nr entries in map, [0 .. 2048] */
        nfwrite(buf, 4, h); /* write VTS_TMAP header */
        if (numtmapt > 0)
          {
          /* write VTS_TMAP entries */
            const struct vobuinfo *vobu1;
            // I don't know why I ever did this
            // ptsbase = -getframepts(va->pg_vg);
            ptsbase = 0; // this matches Bullitt
            vobu1 = globalfindvobu(thispgc, ptsbase + pts_seconds_to_ticks(va->pg_vg, units));
            for (j = 0; j < numtmapt; j++)
              {
                const struct vobuinfo * const vobu2 = globalfindvobu
                  (
                    thispgc,
                    ptsbase + pts_seconds_to_ticks(va->pg_vg, (j + 2) * units)
                  );
                write4(buf, vobu1->sector);
                if (!vobu2 || vobu1->vobcellid != vobu2->vobcellid)
                    buf[0] |= 0x80; /* next time entry will be for a different cell */
                nfwrite(buf, 4, h);
                vobu1 = vobu2;
              } /*for*/
          } /*if*/
      } /*for*/

    pgcindex = (-sizeTMAPT(va)) & 2047;
    if (pgcindex)
      {
      /* clear out unused part of last sector */
        memset(buf, 0, 8);
        while(pgcindex >= 8)
          {
            nfwrite(buf, 8, h);
            pgcindex -= 8;
          } /*while*/
        if (pgcindex) /* shouldn't occur? */
            nfwrite(buf, pgcindex, h);
      } /*if*/
}

static int numsectVOBUAD(const struct vobgroup *va)
/* returns the number of sectors a VOBU_ADMAP will take up. */
{
    int nv = 0, i;
    for (i = 0; i < va->numvobs; i++)
        nv += va->vobs[i]->numvobus;
    return (4+nv*4+2047)/2048;
}

static int CreateCellAddressTable(FILE *h, const struct vobgroup *va)
  /* outputs a VMGM_C_ADT, VTSM_C_ADT or VTS_C_ADT structure containing pointers to all cells. */
  {
    int i, p, k;
    buf_init();
    p = 8;
    for (k = 0; k < va->numvobs; k++)
      {
        const struct vob * const thisvob = va->vobs[k];
        for (i = 0; i < thisvob->numvobus; i++)
          {
            if (!i || thisvob->vobu[i].vobcellid != thisvob->vobu[i - 1].vobcellid)
              { /* starting a new cell */
                if (i)
                  {
                    buf_write4(p + 8, thisvob->vobu[i - 1].lastsector);
                      /* ending sector within VOB in previous entry */
                    p += 12;
                  } /*if*/
                buf_write2(p, thisvob->vobu[i].vobcellid >> 8); /* VOBidn */
                buf_write1(p + 2, thisvob->vobu[i].vobcellid); /* CELLidn */
                buf_write4(p + 4, thisvob->vobu[i].sector); /* starting sector within VOB */
              } /*if*/
          } /*for*/
        buf_write4(p + 8, thisvob->vobu[i - 1].lastsector);
          /* ending sector within VOB in last entry */
        p += 12;
      } /*for*/
    buf_write4(4, p - 1); /* end address (last byte of last entry) */
    // first 2 bytes of C_ADT contains number of vobs
    buf_write2(0, va->numvobs);
    p = (p + 2047) & (-2048); /* round up to whole sectors */
    nfwrite(bigbuf, p, h);
    return p / 2048; /* nr sectors written */
  } /*CreateCellAddressTable*/

static void CreateVOBUAD(FILE *h, const struct vobgroup *va)
/* outputs a VOBU_ADMAP structure containing pointers to all VOBUs. */
  {
    int i, j, nv;
    unsigned char buf[16];
    nv = 0;
    for (i = 0; i < va->numvobs; i++)
        nv += va->vobs[i]->numvobus;
    write4(buf, nv * 4 + 3); /* end address (last byte of last entry) */
    nfwrite(buf, 4, h);
    for (j = 0; j < va->numvobs; j++)
      {
        const struct vob * const thisvob = va->vobs[j];
        for (i = 0; i < thisvob->numvobus; i++)
          {
            write4(buf, thisvob->vobu[i].sector); /* starting sector of VOBU within VOB */
            nfwrite(buf, 4, h);
          } /*for*/
      } /*for*/
    i = (-(4 + nv * 4)) & 2047;
    if (i)
      {
      /* zero out unused part of last whole sector */
        memset(buf, 0, 16);
        while (i >= 16)
          {
            nfwrite(buf, 16, h);
            i -= 16;
          } /*while*/
        if (i)
            nfwrite(buf, i, h);
      } /*if*/
  } /*CreateVOBUAD*/

static int Create_PTT_SRPT(FILE *h, const struct pgcgroup *t)
  /* creates the VTS_PTT_SRPT and VTS_PTT tables for each title. */
  {
    int i, j, p;
    buf_init();
    buf_write2(0, t->numpgcs); // # of titles
    p = 8 + t->numpgcs * 4; /* start generating VTS_PTT entries here */
    assert(p <= 2048);
      // need to make sure all the pgc pointers fit in the first sector because of
      // dvdauthor.c:ScanIfo
    for (j = 0; j < t->numpgcs; j++)
      {
        const struct pgc * const pgc = t->pgcs[j];
        int pgm = 1, k;
        buf_write4(8 + j * 4, p); /* offset to VTS_PTT for title */
        for (i = 0; i < pgc->numsources; i++) /* generate the associated VTS_PTT entries */
            for (k = 0;  k < pgc->sources[i]->numcells; k++)
              {
                const struct cell * const thiscell = &pgc->sources[i]->cells[k];
                if (thiscell->scellid != thiscell->ecellid)
                    switch (thiscell->ischapter)
                      {
                    case CELL_CHAPTER_PROGRAM:
                        buf_write1(1 + p, j + 1); /* PGCN low byte */
                        buf_write1(3 + p, pgm); /* PGN low byte */
                        p += 4;
                  /* fallthru */
                    case CELL_PROGRAM:
                        pgm++; /* keep right count for next chapter */
                    break;
                    case CELL_NEITHER:
                      /* fine */
                    break;
                      } /*switch*/
              } /*for; for*/
      } /*for*/
    buf_write4(4, p - 1); /* end address (last byte of last VTS_PTT) */
    p = (p + 2047) & (-2048); /* round up to next whole sector */
    nfwrite(bigbuf, p, h); /* write it all out */
    return p / 2048; /* nr sectors generated */
  } /*Create_PTT_SRPT*/

static int Create_TT_SRPT
  (
    FILE *h,
    const struct toc_summary *ts,
    int vtsstart /* starting sector for VTS */
  )
  /* creates a TT_SRPT structure containing pointers to all the titles on the disc. */
  {
    int i, j, k, p, tn;
    buf_init();
    j = vtsstart;
    tn = 0;
    p = 8; /* offset to first entry */
    for (i = 0; i < ts->numvts; i++)
      {
        for (k = 0; k < ts->vts[i].numtitles; k++)
          {
            buf_write1(0 + p, 0x3c);
              /* title type = one sequential PGC, jump/link/call may be found in all places,
                PTT & time play/search uops not inhibited */
            buf_write1(1 + p, 0x1); /* number of angles always 1 for now */
            buf_write2(2 + p, ts->vts[i].numchapters[k]); /* number of chapters (PTTs) */
            buf_write1(6 + p, i + 1); /* video titleset number, VTSN */
            buf_write1(7 + p, k + 1); /* title nr within VTS, VTS_TTN */
            buf_write4(8 + p, j); // start sector for VTS
            tn++;
            p += 12; /* offset to next entry */
          } /*for*/
        j += ts->vts[i].numsectors;
      } /*for*/
    buf_write2(0, tn); // # of titles
    buf_write4(4, p - 1); /* end address (last byte of last entry) */
    p = (p + 2047) & (-2048); /* round up to next whole sector */
    nfwrite(bigbuf, p, h);
    return p / 2048; /* nr sectors generated */
  } /*Create_TT_SRPT*/

  static void BuildAVInfo(unsigned char *buf, const struct vobgroup *va)
  /* builds the part of the IFO structure from offset 0x100 (VMGM, VTSM) and 0x200 (VTS) onwards,
    containing the video, audio and subpicture stream attributes. Note these attributes
    don't include any stream IDs; those are specified per-PGC. */
  {
    int i;
    static const int widescreen_bits[4] =
        {
            0, /* VW_NONE */
            0x100, /* VW_NOLETTERBOX */
            0x200, /* VW_NOPANSCAN */
            2 /* VW_CROP */
        };
    write2
      (
        buf,
            (va->vd.vmpeg == 2 ? 0x4000 : 0) /* coding mode: MPEG-1 or MPEG-2 */
        |
            widescreen_bits[va->vd.vwidescreen]
              /* whether to allow automatic pan-scan, automatic letterbox, do cropping */
        |
            (va->vd.vformat == VF_PAL ? 0x1000 : 0) /* NTSC or PAL */
        |
            (va->vd.vaspect == VA_16x9 ? 0xc00 : 0x300)
              // if 16:9, set aspect flag; if 4:3 set noletterbox/nopanscan
        |
            ((va->vd.vcaption & 1) ? 0x80 : 0)
              /* caption=field1 (line-21 closed-captioning, NTSC only) */
        |
            ((va->vd.vcaption & 2) ? 0x40 : 0)
              /* caption=field2 (line-21 closed-captioning, NTSC only) */
        |
            ((va->vd.vres - 1) << 3) /* resolution code */
      );
        /* bit rate always VBR, letterbox-cropped unset, PAL film flag unset for now */
    buf[3] = va->numaudiotracks; /* nr audio streams, low byte */
    for (i = 0; i < va->numaudiotracks; i++)
      { /* fill in menu/title audio attributes */
        buf[4 + i * 8] = (va->ad[i].aformat - 1) << 6; /* audio coding mode */
        if (va->ad[i].alangpresent == AL_LANG) /* for title audio, not menu audio */
          {
            buf[4 + i * 8] |= 4; /* language type = as per language code */
            memcpy(buf + 6 + i * 8, va->ad[i].lang, 2); /* language code */
          } /*if*/
      /* multichannel extension not supported for now */
        if (va->ad[i].adolby == AD_SURROUND) /* for title audio, not menu audio */
          {
            buf[4 + i * 8] |= 2; /* application mode = surround */
            buf[11 + i * 8] = 8; /* suitable for Dolby surround decoding */
          } /*if*/
      /* karaoke options not supported for now */
        buf[5 + i * 8] =
                ((va->ad[i].aquant - 1) << 6) /* quantization/DRC */
            |
                ((va->ad[i].asample - 1) << 4) /* sample rate */
            |
                (va->ad[i].achannels - 1); /* nr channels - 1 */

        buf[9 + i * 8] = va->ad[i].acontent; /* audio code extension for title audio, not menu audio */
      } /*for*/
    buf[0x55] = va->numsubpicturetracks; /* nr subpicture streams, low byte */
    for (i = 0; i < va->numsubpicturetracks; i++)
      {
      /* coding mode always RLE */
        if (va->sp[i].slangpresent == AL_LANG) /* for title subpicture, not menu subpicture */
          {
            buf[0x56 + i * 6] = 1; /* language type = as per language code */
            memcpy(buf + 0x58 + i * 6, va->sp[i].lang, 2); /* language code */
          } /*if*/
        buf[0x56 + i * 6 + 5] = va->sp[i].scontent;
          /* title code extension (title subpicture only) */
      } /*for*/
  } /*BuildAVInfo*/

static bool needmenus(const struct menugroup *mg)
/* do I actually have any menu definitions in mg. */
{
    if (!mg ) return false;
    if( !mg->numgroups ) return false;
    if( !mg->groups[0].pg->numpgcs ) return false; /* fixme: what about checking rest of groups? */
    return true;
}

static void WriteIFO(FILE *h, const struct workset *ws)
  /* writes the IFO for a VTSM. */
  {
    static unsigned char buf[2048];
    int nextsector;
    const bool forcemenus = needmenus(ws->menus);
    size_t ifo_pad = 0;

    // sect 0: VTS toplevel
    memset(buf, 0, 2048);
    memcpy(buf, "DVDVIDEO-VTS", 12);
    buf[33] = 0x11; /* version number */
    write4(buf + 128, 0x7ff);
    nextsector = 1;

    write4(buf + 0xC8, nextsector); // VTS_PTT_SRPT
    nextsector += Create_PTT_SRPT(0, ws->titles);

    write4(buf + 0xCC, nextsector); // VTS_PGCI
    nextsector += CreatePGC(0, ws, VTYPE_VTS);

    if (jumppad || forcemenus)
      {
        write4(buf + 0xD0,nextsector); // VTSM_PGCI
        nextsector += CreatePGC(0, ws, VTYPE_VTSM);
      } /*if*/

    write4(buf + 0xD4, nextsector); // VTS_TMAPT
    nextsector += numsectTMAPT(ws->titles);

    if (jumppad || forcemenus)
      {
        write4(buf + 0xD8, nextsector); // VTSM_C_ADT
        nextsector += CreateCellAddressTable(0, ws->menus->mg_vg);

        write4(buf + 0xDC, nextsector); // VTSM_VOBU_ADMAP
        nextsector += numsectVOBUAD(ws->menus->mg_vg);
      } /*if*/

    write4(buf + 0xE0, nextsector); // VTS_C_ADT
    nextsector += CreateCellAddressTable(0, ws->titles->pg_vg);

    write4(buf + 0xE4, nextsector); // VTS_VOBU_ADMAP
    nextsector += numsectVOBUAD(ws->titles->pg_vg);

    write4(buf + 28, nextsector - 1); /* last sector of IFO */
    if (jumppad || forcemenus)
      {
        write4(buf + 0xC0, nextsector); /* start sector of menu VOB */
        nextsector += getvoblen(ws->menus->mg_vg);
      } /*if*/
    write4(buf + 0xC4, nextsector); /* start sector of title VOB */
    if (ws->titles->numpgcs)
        nextsector += getvoblen(ws->titles->pg_vg);
    nextsector += read4(buf + 28); /* offset by last sector of IFO */
    if (nextsector < MIN_IFO_SECTORS - 1)
      {
        ifo_pad = (MIN_IFO_SECTORS - 1 - nextsector) * 2048;
        nextsector = MIN_IFO_SECTORS - 1;
      } /*if*/
    write4(buf + 12, nextsector); /* gives last sector of title set (last sector of BUP) */

    if (jumppad || forcemenus)
        BuildAVInfo(buf + 256, ws->menus->mg_vg);
    BuildAVInfo(buf + 512, ws->titles->pg_vg);
    nfwrite(buf, 2048, h);

    // sect 1: VTS_PTT_SRPT
    Create_PTT_SRPT(h, ws->titles);

    // sect 2: VTS_PGCI
    CreatePGC(h, ws, VTYPE_VTS);

    if( jumppad || forcemenus )
        CreatePGC(h, ws, VTYPE_VTSM);

    // sect 3: ??? VTS_TMAPT
    CreateTMAPT(h, ws->titles);

    if (jumppad || forcemenus)
      {
        CreateCellAddressTable(h, ws->menus->mg_vg);
        CreateVOBUAD(h, ws->menus->mg_vg);
      } /*if*/
    CreateCellAddressTable(h, ws->titles->pg_vg);
    CreateVOBUAD(h, ws->titles->pg_vg);
  /* nfpad(ifo_pad, h); */ /* unneeded, genisoimage will fix it for me */
  } /*WriteIFO*/

void WriteIFOs(const char *fbase, const struct workset *ws)
/* writes out a .IFO and corresponding .BUP file for a VTSM. */
  {
    FILE *h;
    static char buf[1000];
    bool backup;

    errno = 0;
    if (fbase)
      {
        backup = false;
        for (;;)
          {
            snprintf(buf, sizeof buf, "%s_0.%s", fbase, backup ? "BUP" : "IFO");
            h = fopen(buf, "wb");
            if (!h)
              {
                fprintf
                  (
                    stderr,
                    "\nERR:  Error %d -- %s -- creating %s\n",
                    errno,
                    strerror(errno),
                    buf
                  );
                exit(1);
              } /*if*/
            WriteIFO(h, ws);
            fflush(h);
            if (errno != 0)
              {
                fprintf
                  (
                    stderr,
                    "\nERR:  Error %d -- %s -- flushing %s\n",
                    errno,
                    strerror(errno),
                    buf
                  );
                exit(1);
              } /*if*/
            fclose(h);
            if (backup)
                break;
            backup = true;
          } /*for*/
      }
    else
      /* dummy write */
        WriteIFO(0, ws);
  } /*WriteIFOs*/

void TocGen(const struct workset *ws, const struct pgc *fpc, const char *fname)
  /* writes the IFO for a VMGM. */
  {
    static unsigned char buf[2048];
    int nextsector, offset, i, j, vtsstart;
    const bool forcemenus = needmenus(ws->menus);
    FILE *h;
    size_t ifo_pad = 0;

    h = fopen(fname, "wb");

    memset(buf, 0, 2048);
    memcpy(buf, "DVDVIDEO-VMG", 12);
    buf[0x21] = 0x11; /* version number */
    buf[0x27] = 1; /* number of volumes */
    buf[0x29] = 1; /* volume number */
    buf[0x2a] = 1; /* side ID */
    write2(buf + 0x3e, ws->titlesets->numvts); /* number of title sets */
    strncpy((char *)(buf + 0x40), provider_str, PROVIDER_SIZE - 1); /* provider ID */
    buf[0x86] = 4; /* start address of FP_PGC = 0x400 */
    nextsector = 1;

    write4(buf + 0xc4, nextsector); /* sector pointer to TT_SRPT (table of titles) */
    nextsector += Create_TT_SRPT(0, ws->titlesets, 0);
      /* just to figure out how many sectors will be needed */

    if (jumppad || forcemenus)
      {
        write4(buf + 0xc8, nextsector); /* sector pointer to VMGM_PGCI_UT (menu PGC table) */
        nextsector += CreatePGC(0, ws, VTYPE_VMGM);
      } /*if*/

    write4(buf + 0xd0, nextsector);
      /* sector pointer to VMG_VTS_ATRT (copies of VTS audio/subpicture attrs) */
      /* I will output it immediately following IFO header */
    nextsector += (8 + ws->titlesets->numvts * 0x30c + 2047) / 2048;
      /* round up size of VMG_VTS_ATRT to whole sectors */

    if (jumppad || forcemenus)
      {
        write4(buf + 0xd8, nextsector);
          /* sector pointer to VMGM_C_ADT (menu cell address table) */
          /* I make it follow VMG_VTS_ATRT */
        nextsector += CreateCellAddressTable(0, ws->menus->mg_vg); /* how much room it will need */

        write4(buf + 0xdc, nextsector);
          /* sector pointer to VMGM_VOBU_ADMAP (menu VOBU address map) */
        nextsector += numsectVOBUAD(ws->menus->mg_vg);
      } /*if*/

    write4(buf + 0x1c, nextsector - 1); /* last sector of IFO */
    vtsstart = nextsector * 2; /* size of two copies of everything above including BUP */
    if (jumppad || forcemenus)
      {
        write4(buf + 0xc0, nextsector); /* start sector of menu VOB */
        vtsstart += getvoblen(ws->menus->mg_vg);
      } /*if*/
    if (vtsstart < 2 * MIN_IFO_SECTORS)
      {
        ifo_pad = (2 * MIN_IFO_SECTORS - vtsstart) * 2048 / 2;
        vtsstart = 2 * MIN_IFO_SECTORS;
      } /*if*/
    write4(buf + 0xc, vtsstart - 1); /* last sector of VMG set (last sector of BUP) */

    if (forcemenus)
        BuildAVInfo(buf + 256, ws->menus->mg_vg);

  /* create FPC at 0x400 as promised */
    buf[0x407] = (getratedenom(ws->menus->mg_vg) == 90090 ? 3 : 1) << 6;
      // only set frame rate XXX: should check titlesets if there is no VMGM menu
    buf[0x4e5] = 0xec; /* offset to command table, low byte */
    offset = 0x4f4; /* commands start here, after 8-byte header of command table */
    if (fpc)
      {
        unsigned char *pi;
        if (fpc->posti || fpc->numsources || fpc->numbuttons || fpc->entries)
          {
            fprintf(stderr,"ERR:  FPC can ONLY contain prei commands, nothing else\n");
            exit(1);
          } /*if*/
        if (ws->menus && ws->menus->numgroups)
            pi = vm_compile(buf + offset, buf + offset, ws, ws->menus->groups[0].pg, 0, fpc->prei, 2);
              // XXX: just use the first pgcgroup as a reference
        else
            pi = vm_compile(buf + offset, buf + offset, ws, 0, 0, fpc->prei, 2);
        if (!pi)
          {
            fprintf(stderr,"ERR:  in FPC\n");
            exit(1);
          } /*if*/
        offset = (pi - buf - offset) / 8; /* number of instructions */
        assert(offset <= 128);
        buf[0x4ed] = offset; /* number of pre commands, low byte */
      }
    else
      {
      /* generate default FPC */
        if (forcemenus)
          {
            buf[offset + 0] = 0x30; // jump to VMGM 1
            buf[offset + 1] = 0x06;
            buf[offset + 2] = 0x00;
            buf[offset + 3] = 0x00;
            buf[offset + 4] = 0x00;
            buf[offset + 5] = 0x42;
            buf[offset + 6] = 0x00;
            buf[offset + 7] = 0x00;
          }
        else if (ws->titlesets->numvts && ws->titlesets->vts[0].hasmenu)
          {
            buf[offset + 0] = 0x30; // jump to VTSM vts=1, ttn=1, menu=1
            buf[offset + 1] = 0x06;
            buf[offset + 2] = 0x00;
            buf[offset + 3] = 0x01;
            buf[offset + 4] = 0x01;
            buf[offset + 5] = 0x83;
            buf[offset + 6] = 0x00;
            buf[offset + 7] = 0x00;
          }
        else
          {
            buf[offset + 0] = 0x30; // jump to title 1
            buf[offset + 1] = 0x02;
            buf[offset + 2] = 0x00;
            buf[offset + 3] = 0x00;
            buf[offset + 4] = 0x00;
            buf[offset + 5] = 0x01;
            buf[offset + 6] = 0x00;
            buf[offset + 7] = 0x00;
          } /*if*/
        buf[0x4ed] = 1; /* number of pre commands, low byte */
      } /*if*/
    write2(buf + 0x4f2, 7 + buf[0x4ed] * 8); /* end address relative to command table */
    write2(buf + 0x82 /* end byte address, low word, of VMGI_MAT */, 0x4ec + read2(buf + 0x4f2));
    nfwrite(buf, 2048, h);

    Create_TT_SRPT(h, ws->titlesets, vtsstart); /* generate it for real */

    // PGC
    if (jumppad || forcemenus)
        CreatePGC(h, ws, VTYPE_VMGM);

  /* VMG_VTS_ATRT contains copies of menu and title attributes from all titlesets */
  /* output immediately following IFO header, as promised above */
    memset(buf, 0, 2048);
    j = 8 + ws->titlesets->numvts * 4;
    write2(buf, ws->titlesets->numvts); /* number of titlesets */
    write4(buf + 4, ws->titlesets->numvts * 0x30c + 8 - 1); /* end address (last byte of last VTS_ATRT) */
    for (i = 0; i < ws->titlesets->numvts; i++)
        write4(buf + 8 + i * 4, j + i * 0x308); /* offset to VTS_ATRT i */
    nfwrite(buf, j, h);
    for (i = 0; i < ws->titlesets->numvts; i++) /* output each VTS_ATRT */
      {
        write4(buf, 0x307); /* end address */
        memcpy(buf + 4, ws->titlesets->vts[i].vtscat, 4);
          /* VTS_CAT (copy of bytes 0x22 .. 0x25 of VTS IFO) */
        memcpy(buf + 8, ws->titlesets->vts[i].vtssummary, 0x300);
          /* copy of VTS attributes (bytes 0x100 onwards of VTS IFO) */
        nfwrite(buf, 0x308, h);
        j += 0x308;
      } /*for*/
    j = 2048 - (j & 2047);
    if (j < 2048)
      { /* pad to next whole sector */
        nfpad(j, h);
      } /*if*/

    if (jumppad || forcemenus)
      {
        CreateCellAddressTable(h, ws->menus->mg_vg); /* actually generate VMGM_C_ADT */
        CreateVOBUAD(h, ws->menus->mg_vg); /* generate VMGM_VOBU_ADMAP */
      } /*if*/
  /* nfpad(ifo_pad, h); */ /* unneeded, genisoimage will fix it for me */
    fflush(h);
    if (errno != 0)
      {
        fprintf(stderr, "\nERR:  Error %d -- %s -- flushing VMGM\n", errno, strerror(errno));
        exit(1);
      } /*if*/
    fclose(h);
  } /*TocGen*/
