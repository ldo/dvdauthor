/*
    dvdauthor -- generation of PGCs within IFO files
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
#include <errno.h>
#include <assert.h>

#include "dvdauthor.h"
#include "da-internal.h"



#define MAXCELLS 4096
#define MAXPGCSIZE (236+128*8+256+MAXCELLS*(24+4))
#define BUFFERPAD (MAXPGCSIZE+1024)

static int bigwritebuflen=0;
static unsigned char *bigwritebuf=0;


/*
titleset X: 2-255
vmgm: 1
menu Y: 1-119
menu entry Y: 120-127
title Y: 129-255
chapter Z: 1-255

need 3 bytes plus jump command

for VMGM menu:
[vmgm | titleset X] -> g[15] low
[menu [entry] Y | title Y] -> g[15] high
[chapter Z] -> g[14] low

for VTSM root menu:
[menu [entry] Y | title Y] -> g[15] high
[chapter Z] -> g[15] low

*/

static int genjumppad(unsigned char *buf, vtypes ismenu, int entry, const struct workset *ws, const struct pgcgroup *curgroup)
  /* generates the jumppad if the user wants it. The code is put into buf, and the function
    result is the number of bytes generated. */
  {
    unsigned char *cbuf = buf;
    int i, j, k;
    if (jumppad && ismenu == VTYPE_VTSM && entry == 7 /* root menu? */)
      {
        // *** VTSM jumppad
        write8(cbuf,0x61,0x00,0x00,0x0E,0x00,0x0F,0x00,0x00); cbuf+=8; // g[14]=g[15];
        write8(cbuf,0x71,0x00,0x00,0x0F,0x00,0x00,0x00,0x00); cbuf+=8; // g[15]=0;
        // menu entry jumptable
        for (i = 2; i < 8; i++)
          {
            for (j = 0; j < curgroup->numpgcs; j++)
                if (curgroup->pgcs[j]->entries & (1 << i))
                  {
                    write8(cbuf,0x20,0xA4,0x00,0x0E,i+120,0x00,0x00,j+1); cbuf+=8; // if g[14]==0xXX00 then LinkPGCN XX
                  } /*if; for*/
          } /*for*/
        // menu jumptable
        for (i = 0; i < curgroup->numpgcs; i++)
          {
            write8(cbuf,0x20,0xA4,0x00,0x0E,i+1,0x00,0x00,i+1); cbuf+=8; // if g[14]==0xXX00 then LinkPGCN XX
          } /*for*/
        // title/chapter jumptable
        for (i = 0; i < ws->titles->numpgcs; i++)
          {
            write8(cbuf,0x71,0x00,0x00,0x0D,i+129,0,0x00,0x00); cbuf+=8; // g[13]=(i+1)*256;
            write8(cbuf,0x30,0x23,0x00,0x00,0x00,i+1,0x0E,0x0D); cbuf+=8; // if g[15]==g[13] then JumpSS VTSM i+1, ROOT
            for (j = 0; j < ws->titles->pgcs[i]->numchapters; j++)
              {
                write8(cbuf,0x71,0x00,0x00,0x0D,i+129,j+1,0x00,0x00); cbuf+=8; // g[13]=(i+1)*256;
                write8(cbuf,0x30,0x25,0x00,j+1,0x00,i+1,0x0E,0x0D); cbuf+=8; // if g[15]==g[13] then JumpSS VTSM i+1, ROOT
              } /*for*/
          } /*for*/
      }
    else if (jumppad && ismenu == VTYPE_VMGM && entry == 2 /* title menu */)
      {
        // *** VMGM jumppad
        // remap all VMGM TITLE X -> TITLESET X TITLE Y
        k = 129;
        for (i = 0; i < ws->titlesets->numvts; i++)
            for (j = 0; j < ws->titlesets->vts[i].numtitles; j++)
              {
                write8(cbuf, 0x71, 0xA0, 0x0F, 0x0F, j + 129, i + 2, k, 1);
                  /* if g15 == k << 8 | 1 then g15 = j + 129 << 8 | i + 2 */
                cbuf += 8;
                k++;
              } /*for; for*/
        // move TITLE out of g[15] into g[14] (to mate up with CHAPTER)
        // then put title/chapter into g[15], and leave titleset in g[14]
        write8(cbuf,0x63,0x00,0x00,0x0E,0x00,0x0F,0x00,0x00); cbuf+=8; // g[14]+=g[15]
        write8(cbuf,0x79,0x00,0x00,0x0F,0x00,0xFF,0x00,0x00); cbuf+=8; // g[15]&=255
        write8(cbuf,0x64,0x00,0x00,0x0E,0x00,0x0F,0x00,0x00); cbuf+=8; // g[14]-=g[15]
        write8(cbuf,0x62,0x00,0x00,0x0E,0x00,0x0F,0x00,0x00); cbuf+=8; // g[14]<->g[15]
        // For each titleset, delegate to the appropriate submenu
        for (i = 0; i < ws->titlesets->numvts; i++)
          {
            write8(cbuf,0x71,0x00,0x00,0x0D,0x00,i+2,0x00,0x00); cbuf+=8; // g[13]=(i+2)*256;
            write8(cbuf,0x30,0x26,0x00,0x01,i+1,0x87,0x0E,0x0D); cbuf+=8; // if g[14]==g[13] then JumpSS VTSM i+1, ROOT
          } /*for*/
        // set g[15]=0 so we don't leak dirty registers to other PGC's
        write8(cbuf,0x71,0x00,0x00,0x0F,0x00,0x00,0x00,0x00); cbuf+=8; // g[15]=0;
      } /*if*/
    return cbuf-buf;
  } /*genjumppad*/

static int jumppgc(unsigned char *buf,int pgc,vtypes ismenu,int entry,const struct workset *ws,const struct pgcgroup *curgroup)
  /* generates a command table for a PGC containing a jumppad. Returns the number of bytes output. */
  {
    int base = 0xEC; /* put command table immediately after PGC header */
    int ncmd, offs;
    offs = base + 8; /* room for command table header */
    offs += genjumppad(buf + offs, ismenu, entry, ws, curgroup);
    if (pgc > 0)
        write8(buf + offs, 0x20, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, pgc); // LinkPGCN pgc
    else
        write8(buf + offs, 0x30, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00); // JumpSS FP
    offs += 8;
    ncmd = (offs - base) / 8 - 1;
    if (ncmd > 128)
      {
        fprintf
          (
            stderr,
            "ERR:  Too many titlesets/titles/menus/etc for jumppad to handle."
                "  Reduce complexity and/or disable\njumppad.\n"
          );
        exit(1);
      } /*if*/
    buf[0xE5] = base; /* offset within PGC to command table, low byte */
    buf[base + 1] = ncmd; /* number of pre commands, low byte */
    write2(buf + base + 6, 7 + 8 * ncmd); /* end address relative to command table */
    return offs;
  } /*jumppgc*/

static int genpgc(unsigned char *buf,const struct workset *ws,const struct pgcgroup *group,int pgc,vtypes ismenu,int entry)
/* generates a PGC entry for an IFO file in buf. */
  {
    const struct vobgroup *va = (ismenu != VTYPE_VTS ? ws->menus->mg_vg : ws->titles->pg_vg);
    const struct pgc * const thispgc = group->pgcs[pgc];
    int i, j, d;

  /* buf[0], buf[1] don't seem to be used */

    // PGC header starts at buf[16]
    buf[2] = thispgc->numprograms;
    buf[3] = thispgc->numcells;
    write4(buf + 4, buildtimeeven(va, getptsspan(thispgc))); /* BCD playback time + frame rate */
  /* buf[8 .. 11] -- prohibited user ops -- none for now */
    for (i = 0; i < va->numaudiotracks; i++)
      {
        if (va->ad[i].aid)
            buf[12 + i * 2] = 0x80 | (va->ad[i].aid - 1);
          /* PGC_AST_CTL, audio stream control: actual stream IDs corresponding to
            each stream described in IFO */
      } /*for*/
    for (i = 0; i < va->numsubpicturetracks; i++)
      {
        int m;
        bool e;
        e = false;
        for (m = 0; m < 4; m++)
            if (thispgc->subpmap[i][m] & 128)
              {
                buf[28 + i * 4 + m] = thispgc->subpmap[i][m] & 127;
                  /* PGC_SPST_CTL, subpicture stream control: actual stream IDs corresponding
                    to each stream described in IFO */
                e = true;
              } /*if; for*/
        if (e)
            buf[28 + i * 4] |= 0x80; /* set stream-available flag */
      } /*for*/
    buf[163] = thispgc->pauselen; // PGC stilltime
    for (i = 0; i < 16; i++) /* colour lookup table (0, Y, Cr, Cb) */
        write4
          (
            buf + 164 + i * 4,
            thispgc->colors->color[i] < COLOR_UNUSED ? thispgc->colors->color[i] : 0x108080
          );

    d = 0xEC; /* start assembling commands here */
    // command table
      {
        unsigned char *cd = buf + d + 8, *preptr, *postptr, *cellptr;
        preptr = cd; /* start of pre commands */
        cd += genjumppad(cd, ismenu, entry, ws, group);
        if (thispgc->prei)
          {
            cd = vm_compile(preptr, cd, ws, thispgc->pgcgroup, thispgc, thispgc->prei, ismenu);
            if (!cd)
              {
                fprintf(stderr, "ERR:  in %s pgc %d, <pre>\n", pstypes[ismenu], pgc);
                exit(1);
              } /*if*/
          }
        else if (thispgc->numbuttons)
          {
            write8(cd, 0x56, 0x00, 0x00, 0x00, 4 * 1, 0x00, 0x00, 0x00); cd += 8;
              // set active button to be #1
          } /*if*/

        postptr = cd; /* start of post commands */
        if (thispgc->numbuttons && !allowallreg)
          {
          /* transfer sequence for button instructions too long to fit in PCI packet */
          /* enter with g15 = button number */
            write8(cd, 0x61, 0x00, 0x00, 0x0E, 0x00, 0x0F, 0x00, 0x00);  // g[14]=g[15]
            write8(cd + 8, 0x71, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x00); // g[15]=0
            cd += 8 * (thispgc->numbuttons + 2);
              /* transfer to individual button sequences--reserve space to be filled in below */
          } /*if*/
        if (thispgc->posti)
          {
            cd = vm_compile(postptr, cd, ws, thispgc->pgcgroup, thispgc, thispgc->posti, ismenu);
            if (!cd)
              {
                fprintf(stderr, "ERR:  in %s pgc %d, <post>\n", pstypes[ismenu], pgc);
                exit(1);
              } /*if*/
          }
        else
          {
            write8(cd, 0x30, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00); // exit
            cd += 8;
          } /*if*/
        if (thispgc->numbuttons && !allowallreg)
          {
          /* compile and insert all the button instructions that wouldn't fit
            in the PCI packets in the VOB */
            unsigned char *buttonptr = cd;
            for (i = 0; i < thispgc->numbuttons; i++)
              {
                const struct button * const thisbutton = &thispgc->buttons[i];
                unsigned char * cdd = vm_compile(postptr, cd, ws, thispgc->pgcgroup, thispgc, thisbutton->commands, ismenu);
                if (!cdd)
                  {
                    fprintf(stderr, "ERR:  in %s pgc %d, button %s\n", pstypes[ismenu], pgc, thisbutton->name);
                    exit(1);
                  } /*if*/
                if (cdd == cd + 8)
                  {
                    // the button fits in one command; assume it went in the vob itself
                    memset(postptr + i * 8 + 16, 0, 8); // nop
                    memset(cd, 0, 8);
                      // reset the just compiled command, since it was test written
                      // to part of the pgc structure
                  }
                else
                  {
                    write8(postptr + i * 8 + 16, 0x00, 0xa1, 0x00, 0x0E, 0x00, i + 1, 0x00, (cd - postptr) / 8 + 1);
                      /* if g14 == i + 1 then goto (cd - postptr) / 8 + 1 */
                      /* insert entry in transfer table */
                    cd = cdd;
                  } /*if*/
              } /*for*/
            if (cd == buttonptr)
                memset(postptr, 0, 16);
                  /* no button transfer sequences generated, nop the register transfer */
          } /*if*/

        vm_optimize(postptr, postptr, &cd);
          /* tidy up all the code I just output */

        cellptr = cd;
        for (i = 0; i < thispgc->numsources; i++)
            for (j = 0; j < thispgc->sources[i]->numcells; j++)
              {
                const struct cell * const thiscell = &thispgc->sources[i]->cells[j];
                if (thiscell->commands)
                  {
                    unsigned char *cdd = vm_compile(cellptr, cd, ws, thispgc->pgcgroup, thispgc, thiscell->commands, ismenu);
                    if (!cdd)
                      {
                        fprintf(stderr, "ERR:  in %s pgc %d, <cell>\n", pstypes[ismenu], pgc);
                        exit(1);
                      } /*if*/
                    if (cdd != cd + 8)
                      {
                        fprintf(stderr, "ERR:  Cell command can only compile to one VM instruction.\n");
                        exit(1);
                      } /*if*/
                    cd = cdd;
                  } /*if*/
              } /*for; for*/

        write2(buf + 228, d); /* offset to commands */
        if (cd - (buf + d) - 8 > 128 * 8) // can only have 128 commands
          {
            fprintf(stderr, "ERR:  Can only have 128 commands for pre, post, and cell commands.\n");
            exit(1);
          } /*if*/
      /* fill in command table header */
        write2(buf + d, (postptr - preptr) / 8); /* nr pre commands */
        write2(buf + d + 2, (cellptr - postptr) / 8); /* nr post commands */
        write2(buf + d + 4, (cd - cellptr) / 8); /* nr cell commands */
        write2(buf + d + 6, cd - (buf + d) - 1); /* end address relative to command table */
        d = cd - buf;
      }

    if (thispgc->numsources)
      {
      /* fill in program map, cell playback info table and cell position info table */
        int cellline;
        bool notseamless;
        write2(buf + 230, d); /* offset to program map */
        j = 1;
        for (i = 0; i < thispgc->numsources; i++)
          { /* generate the program map */
            int k;
            const struct source * const thissource = thispgc->sources[i];
            for (k = 0; k < thissource->numcells; k++)
              {
                if (thissource->cells[k].scellid == thissource->cells[k].ecellid)
                    continue; /* empty cell */
                if (thissource->cells[k].ischapter != CELL_NEITHER)
                    buf[d++] = j; /* this cell starts a program (possibly a chapter as well) */
                j += thissource->cells[k].ecellid - thissource->cells[k].scellid;
              } /*for*/
          } /*for*/
        d += d & 1; /* round up to word boundary */

        write2(buf + 232, d); /* offset to cell playback information table */
        j = -1;
        cellline = 1;
        notseamless = true; /* first cell never seamless, otherwise a/v sync problems will occur */
        for (i = 0; i < thispgc->numsources; i++)
          { /* generate the cell playback information table */
            int k, l, m, firsttime;
            const struct source * const thissource = thispgc->sources[i];
            for (k = 0; k < thissource->numcells; k++)
                for (l = thissource->cells[k].scellid; l < thissource->cells[k].ecellid; l++)
                  {
                    const int id = thissource->vob->vobid * 256 + l;
                    int vi;
                    for (m = 0; m < va->numaudiotracks; m++)
                        if
                          (
                                j >= 0
                            &&
                                getaudch(va, m) >= 0
                            &&
                                calcaudiogap(va, j, id, getaudch(va, m))
                          )
                            notseamless = true;
                    buf[d] =
                            (notseamless ? 0 : 8 /* seamless multiplex */)
                        |
                            (j + 1 != id ? 2 /* SCR discontinuity */ : 0);
                              // if this is the first cell of the source, then set 'STC_discontinuity', otherwise set 'seamless presentation'
                    notseamless = false; /* initial assumption for next cell */
                    j = id;

                    vi = findcellvobu(thissource->vob, l);
                    firsttime = thissource->vob->vobu[vi].sectpts[0];
                    write4(buf + 8 + d, thissource->vob->vobu[vi].sector);
                      /* first VOBU start sector */
                  /* first ILVU end sector not supported for now */
                    vi = findcellvobu(thissource->vob, l + 1) - 1;
                    if (l == thissource->cells[k].ecellid - 1)
                      {
                        if (thissource->cells[k].pauselen > 0)
                          {
                            buf[2 + d] = thissource->cells[k].pauselen; /* cell still time */
                            notseamless = true; // cells with stilltime are not seamless
                          } /*if*/
                        if (thissource->cells[k].commands)
                          {
                            buf[3 + d] = cellline++; /* cell command nr */
                            notseamless = true; // cells with commands are not seamless
                          } /*if*/
                      } /*if*/
                    write4(buf + 4 + d, buildtimeeven(va, thissource->vob->vobu[vi]. sectpts[1] - firsttime));
                      /* cell playback time + frame rate */
                    write4(buf + 16 + d, thissource->vob->vobu[vi].sector);
                      /* last VOBU start sector */
                    write4(buf + 20 + d, thissource->vob->vobu[vi].lastsector);
                      /* last VOBU end sector */
                    d += 24;
                  } /*for; for*/
          } /*for*/

        write2(buf + 234, d); /* offset to cell position information table */
        for (i = 0; i < thispgc->numsources; i++)
          { /* build the cell position information table */
            int j,k;
            const struct source * const thissource = thispgc->sources[i];
            for (j = 0; j < thissource->numcells; j++)
                for (k = thissource->cells[j].scellid; k < thissource->cells[j].ecellid; k++)
                  { /* put in IDs of all nonempty cells */
                    buf[1 + d] = thissource->vob->vobid; /* VOB ID low byte */
                    buf[3 + d] = k; /* cell ID */
                    d += 4;
                  } /*for*/
          } /*for*/
      } /*if thispgc->numsources*/

    return d;
  } /*genpgc*/

static int createpgcgroup(const struct workset *ws,vtypes ismenu,const struct pgcgroup *va,unsigned char *buf /* where to put generated table */)
  /* generates a VMGM_LU, VTSM_LU or VTS_PGCI table and all associated PGCs. Returns -1 if there wasn't enough space. */
  {
    int len, i, pgcidx, nrtitles;
    len = va->numpgcs + va->numentries;
      /* duplicate each PGC for each additional menu entry type it may have ... */
    for (i = 0; i < va->numpgcs; i++)
        if (va->pgcs[i]->entries)
            len--; /* ...  beyond the first one */
    write2(buf, len); /* nr language units (VMGM_LU, VTSM_LU)/program chains (VTS_PGCI) */
    len = len * 8 + 8; /* total length of VMGM_LU/VTSM_LU/VTS_PGCI */
    pgcidx = 8;
    nrtitles = 0;
    for (i = 0; i < va->numpgcs; i++)
      { /* generate the PGCs and put in their offsets */
        int j = 0;
        if (buf + len + BUFFERPAD - bigwritebuf > bigwritebuflen)
            return -1; /* caller needs to give me more space */
        if (ismenu == VTYPE_VTS)
          {
            if (va->pgcs[i]->entries == 0)
              {
                ++nrtitles;
                buf[pgcidx] = 0x80 | nrtitles; /* title type & nr for VTS_PGC */
              } /*if*/
          }
        else
          {
            buf[pgcidx] = 0;
            for (j = 2; j < 8; j++)
                if (va->pgcs[i]->entries & (1 << j))
                  {
                    buf[pgcidx] = 0x80 | j; /* menu type for VMGM_LU/VTSM_LU */
                    break;
                  } /*if; for*/
          } /*if*/
        write4(buf + pgcidx + 4, len); /* offset to VMGM_PGC/VTSM_PGC/VTS_PGC */
        len += genpgc(buf + len, ws, va, i, ismenu, j); /* put it there */
        pgcidx += 8;
      } /*for*/
    for (i = 2; i < 8; i++)
      {
        if (va->allentries & (1 << i))
          {
            int j;
            if (buf + len + BUFFERPAD - bigwritebuf > bigwritebuflen)
                return -1; /* caller needs to give me more space */
            for (j = 0; j < va->numpgcs; j++)
                if (va->pgcs[j]->entries & (1 << i))
                    break;
            if (j < va->numpgcs) // this can be false if jumppad is set
              {
                // is this the first entry for this pgc? if so, it was already
                // triggered via the PGC itself, so skip writing the extra
                // entry here
                if ((va->pgcs[j]->entries & ((1 << i) - 1)) == 0)
                    continue;
              } /*if*/
            j++;
            buf[pgcidx] = 0x80 | i; /* menu type */
            write4(buf + pgcidx + 4, len); /* offset to VMGM_PGC/VTSM_PGC */
            len += jumppgc(buf + len, j, ismenu, i, ws, va);
            pgcidx += 8;
          } /*if*/
      } /*for*/
    write4(buf + 4, len - 1);
      /* end address (last byte of last PGC in this LU) relative to VMGM_LU/VTSM_LU */
    return len;
  } /*createpgcgroup*/

int CreatePGC(FILE *h, const struct workset *ws, vtypes ismenu)
  {
    unsigned char *buf;
    int len,ph,i;
    bool in_it;

    in_it = bigwritebuflen == 0; /* don't do first allocation if already got a buffer from last time */
 retry: /* come back here if buffer wasn't big enough to generate a PGC group */
    if (in_it)
      {
        if (bigwritebuflen == 0)
            bigwritebuflen = 128 * 1024; /* first call */
        else
            bigwritebuflen <<= 1; /* previous write failed, try twice the size */
        if (bigwritebuf)
            free(bigwritebuf);
        bigwritebuf = malloc(bigwritebuflen);
      } /*if*/
    in_it = true;
    buf = bigwritebuf;
    memset(buf, 0, bigwritebuflen);
    if (ismenu != VTYPE_VTS) /* create VMGM_PGCI_UT/VTSM_PGCI_UT structure */
      {
        buf[1] = ws->menus->numgroups; // # of language units
        ph = 8 + 8 * ws->menus->numgroups; /* VMGM_LU/VTSM_LU structures start here */

        for (i = 0; i < ws->menus->numgroups; i++)
          {
            unsigned char * const plu = buf + 8 + 8 * i;
            const struct langgroup * const lg = &ws->menus->groups[i];
            memcpy(plu, lg->lang, 2); /* ISO639 language code */
            if (ismenu == VTYPE_VTSM)
                plu[3] = lg->pg->allentries;
            else /* ismenu = VTYPE_VMGM */
                plu[3] = 0x80; // menu system contains entry for title
            write4(plu + 4, ph);
            len = createpgcgroup(ws, ismenu, lg->pg, buf + ph);
            if (len < 0)
                goto retry;
            ph += len;
          } /*for*/
        write4(buf + 4, ph - 1);
          /* end address (last byte of last PGC in last LU) relative to VMGM_PGCI_UT/VTSM_PGCI_UT */
      }
    else
      {
      /* generate VTS_PGCI structure */
        len = createpgcgroup(ws, VTYPE_VTS, ws->titles, buf);
        if (len < 0)
            goto retry;
        ph = len;
      } /*if*/

    assert(ph <= bigwritebuflen);
    ph = (ph + 2047) & (-2048);
    if (h)
      {
        (void)fwrite(buf, 1, ph, h);
        if (errno != 0)
          {
            fprintf
              (
                stderr,
                "\nERR:  Error %d -- %s -- writing output PGC\n",
                errno,
                strerror(errno)
              );
            exit(1);
          } /*if*/
      } /*if*/
    return ph / 2048;
  } /*CreatePGC*/
