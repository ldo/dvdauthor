/*
    dvdunauthor mainline
*/
/*
 * Copyright (C) 2002 Scott Smith (trckjunky@users.sourceforge.net)
 * 13.11.2004, Ralf Engels <ralf-engels@gmx.de> added lang options for titles,
 *                                              conversion to write-xml2 lib
 * 28.11.2004, Ralf Engels <ralf-engels@gmx.de> added button and command support
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

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/times.h>

#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_types.h>
#include <dvdread/ifo_read.h>
#include <dvdread/nav_read.h>

#include <libxml/tree.h>

#include "common.h"
#include "dvduncompile.h"


#define BIGBLOCKSECT 512
#define BIGBLOCKLEN (DVD_VIDEO_LB_LEN*BIGBLOCKSECT)
static unsigned char bigblock[BIGBLOCKLEN];

static int
    numtitlesets=0; // set in dvdump
static char
    filenamebase[128];

static char *
    fmtbuf = 0;
static size_t
    fmtbufsize;

static struct cellstarttime {
    int vob,cell,pts;
} *cellstarttimes=0;

static struct vobbutton {
    int vob,cell;
    hli_t h;
} *vobbuttons=0;

static int
    numcst = 0, /* length of cellstarttimes array */
    numvb = 0; /* length of vobbuttons array */

static hli_t curhli;
static pci_t hli_pci;

static void setfilename(int vob)
  /* sets filenamebase to be a suitable name for a numbered video file. */
  {
    const int l=3+1+2+1+1; /* length of "vob_%02d%c_" */
    snprintf(filenamebase + l, sizeof(filenamebase) - l, "%03d.vob", vob);
  } /*setfilename*/

static void addcst(int v, int c, int p)
  /* adds another entry to cellstarttimes, if not already there. */
  {
    int i;

    for (i = 0; i < numcst; i++)
        if (cellstarttimes[i].vob == v && cellstarttimes[i].cell == c)
            return; /* already present */
    cellstarttimes = realloc(cellstarttimes, (numcst + 1) * sizeof(struct cellstarttime));
    cellstarttimes[numcst].vob = v;
    cellstarttimes[numcst].cell = c;
    cellstarttimes[numcst].pts = p;
    numcst++;
  } /*addcst*/

static void addbutton(int v,int c, const hli_t *h)
  /* adds another button definition. */
  {
    vobbuttons = realloc(vobbuttons, (numvb + 1) * sizeof(struct vobbutton));
    vobbuttons[numvb].vob = v;
    vobbuttons[numvb].cell = c;
    vobbuttons[numvb].h = *h;
    numvb++;
  } /*addbutton*/

static bool vobexists(const cell_adr_t *cells, int numcells, int vobid)
  /* do I already know about a vob with this id. */
  {
    int i;
    for (i = 0; i < numcells; i++)
        if (cells[i].vob_id == vobid)
            return true;
    return false;
  } /*vobexists*/

static int getpts(int v, int c)
  /* returns the start time of the specified cell if known, else -1. */
  {
    int i;
    for (i = 0; i < numcst; i++)
        if (cellstarttimes[i].vob == v && cellstarttimes[i].cell == c)
            return cellstarttimes[i].pts;
    return -1;
  } /*getpts*/

static cell_chapter_types getprogramtype(const vts_ptt_srpt_t *tt, const pgc_t *p, int pn, int c)
  /* returns the type of the cell with program number pn and cell id c. */
  {
    cell_chapter_types ptype = CELL_NEITHER;
    int i, j, pg = 0;
    for (i = 0; i < p->nr_of_programs; i++)
        if (c == p->program_map[i])
          {
            pg = i + 1;
            ptype = CELL_PROGRAM;
            break;
          } /*if; for */
    if (ptype != CELL_NEITHER && tt)
      {
        for (i = 0; i < tt->nr_of_srpts; i++)
            for (j = 0; j < tt->title[i].nr_of_ptts; j++)
                if
                  (
                        tt->title[i].ptt[j].pgcn == pn
                    &&
                        tt->title[i].ptt[j].pgn == pg
                  )
                    return CELL_CHAPTER_PROGRAM;
      } /*if*/
    return ptype;
  } /*getprogramtype*/

static const char * va_buf_printf
  (
    const char * format,
    va_list ap
  )
  /* formats its arguments into fmtbuf, dynamically (re)allocating the space as necessary,
    and returns a pointer to the result. */
  {
    size_t bytesneeded;
    if (fmtbuf == 0) /* first call */
      {
        fmtbufsize = 64;
        fmtbuf = malloc(fmtbufsize);
      } /*if*/
    for (;;)
      {
        bytesneeded = vsnprintf(fmtbuf, fmtbufsize, format, ap);
        if (bytesneeded < fmtbufsize)
            break;
        fmtbufsize = (bytesneeded + 63) / 64 * 64; /* reduce nr of reallocation calls */
        fmtbuf = realloc(fmtbuf, fmtbufsize);
      } /*for*/
    return fmtbuf;
  } /*va_buf_printf*/

static const char * buf_printf
  (
    const char * format,
    ...
  )
  {
    const char * result;
    va_list ap;
    va_start(ap, format);
    result = va_buf_printf(format, ap);
    va_end(ap);
    return result;
  } /*buf_printf*/

static const char * fmttime(int t1, int t2)
  /* formats the difference between two 90kHz timestamps as hh:mm:ss.fff. */
  {
    const char * result;
    if (t1 >= 0 && t2 >= 0)
      {
        const int t = t1 - t2;
        result = buf_printf
          (
            "%d:%02d:%02d.%03d",
            t / 90 / 1000 / 60 / 60,
            (t / 90 / 1000 / 60) % 60,
            (t / 90 / 1000) % 60,
            (t / 90) % 1000
          );
      }
    else
      {
        result = 0;
      } /*if*/
    return result;
  } /*fmttime*/

static xmlNodePtr NewChildTag
  (
    xmlNodePtr parent,
    const char * name
  )
  /* returns a new empty child tag of parent. */
  {
    return
        xmlNewTextChild(parent, NULL, (const xmlChar *)name, NULL);
  } /*NewChildTag*/

static void AddAttribute
  (
    xmlNodePtr parent,
    const char * attrname,
    const char * format,
    ...
  )
  /* formats a value for a new attribute and attaches it to parent. */
  {
    va_list ap;
    va_start(ap, format);
    xmlNewProp(parent, (const xmlChar *)attrname, (const xmlChar *)va_buf_printf(format, ap));
    va_end(ap);
  } /*AddAttribute*/

static void AddTimeAttribute
  (
    xmlNodePtr parent,
    const char * attrname,
    int t1,
    int t2
  )
  /* adds an attribute containing the formatted difference between two 90kHz timestamps
    as hh:mm:ss.fff. */
  {
    const char * const str = fmttime(t1, t2);
    if (str != 0)
      {
        xmlNewProp(parent, (const xmlChar *)attrname, (const xmlChar *)str);
      } /*if*/
  } /*AddTimeAttribute*/

static void AddCellTimeAttribute
  (
    xmlNodePtr parent,
    const char * attrname,
    int vobid,
    int cellid
  )
  /* adds an attribute containing the formatted offset from the start time of the
    first cell to cell cellid in vob vobid. */
  {
    AddTimeAttribute(parent, attrname, getpts(vobid, cellid), getpts(vobid, 1));
  } /*AddCellTimeAttribute*/

static void AddComment
  (
    xmlNodePtr parent,
    const char * format,
    ...
  )
  /* formats an XML comment and attaches it to parent. */
  {
    va_list ap;
    va_start(ap, format);
    xmlAddChildList(parent, xmlNewComment((const xmlChar *)va_buf_printf(format, ap)));
    va_end(ap);
  } /*AddComment*/

/* Add a language attribute to the node */
static void addLangAttr(xmlNodePtr node, uint16_t lang_code)
  {
    if (lang_code)
      {
        AddAttribute
          (
            /*parent =*/ node,
            /*attrname =*/ "lang",
            /*format =*/
                        isalpha((int)(lang_code >> 8))
                    &&
                        isalpha((int)(lang_code & 0xff))
                ?
                    "%c%c"
                :
                    "0x%02x%02x",
            lang_code >> 8,
            lang_code & 0xff
          );
      } /*if*/
  } /*addLangAttr*/

/* Output Video Title Set Attributes */

static const char * const permitted_df[4] = {NULL, "noletterbox", "nopanscan", NULL};

static const char * const audio_format[8] = {"ac3", NULL, NULL, "mp2", "pcm ", NULL, "dts", NULL};
static const char * const audio_type[5]   = {NULL, "normal", "impaired", "comments1", "comments2"};

static const char * const subp_type[16]   = {
    NULL, "normal",    "large",          "children",
    NULL, "normal_cc", "large_cc",       "children_cc",
    NULL, "forced",    NULL,             NULL,
    NULL, "director",  "large_director", "children_director"
};

struct attrblock {
    const video_attr_t *video_attr;
    const audio_attr_t *audio_attr;
    const subp_attr_t  *subp_attr;
    int numaudio, numsubp;
};

static void get_attr(const ifo_handle_t *ifo, int titlef, struct attrblock *ab)
  /* collects attribute info from an IFO. titlef indicates what type of IFO and what part. */
  {
    switch (titlef)
      {
    case -1: // vmgm
        ab->video_attr = &ifo->vmgi_mat->vmgm_video_attr;
        ab->numaudio   =  ifo->vmgi_mat->nr_of_vmgm_audio_streams;
        ab->audio_attr = &ifo->vmgi_mat->vmgm_audio_attr;
        ab->numsubp    =  ifo->vmgi_mat->nr_of_vmgm_subp_streams;
        ab->subp_attr  = &ifo->vmgi_mat->vmgm_subp_attr;
    break;

    case 0: // vtsm
        ab->video_attr = &ifo->vtsi_mat->vtsm_video_attr;
        ab->numaudio   =  ifo->vtsi_mat->nr_of_vtsm_audio_streams;
        ab->audio_attr = &ifo->vtsi_mat->vtsm_audio_attr;
        ab->numsubp    =  ifo->vtsi_mat->nr_of_vtsm_subp_streams;
        ab->subp_attr  = &ifo->vtsi_mat->vtsm_subp_attr;
    break;

    case 1: // vts
        ab->video_attr = &ifo->vtsi_mat->vts_video_attr;
        ab->numaudio   =  ifo->vtsi_mat->nr_of_vts_audio_streams;
        ab->audio_attr =  ifo->vtsi_mat->vts_audio_attr;
        ab->numsubp    =  ifo->vtsi_mat->nr_of_vts_subp_streams;
        ab->subp_attr  =  ifo->vtsi_mat->vts_subp_attr;
    break;

    default:
        ab->video_attr = 0;
        ab->numaudio   = 0;
        ab->audio_attr = 0;
        ab->numsubp    = 0;
        ab->subp_attr  = 0;
    break;
      } /*switch*/
  } /*get_attr*/

static void dump_attr
  (
    const struct attrblock *ab,
    xmlNodePtr node
  )
  /* dumps out the information collected by get_attr. */
  {
    int i;
    xmlNodePtr newNode;
    /* add video attributes */
    newNode = NewChildTag(node, "video");
    if (ab->video_attr->display_aspect_ratio)
      {
        // 16:9
        if (permitted_df[ab->video_attr->permitted_df])
            xmlNewProp(newNode, (const xmlChar *)"widescreen", (const xmlChar *)permitted_df[ab->video_attr->permitted_df]);
        if (ab->video_attr->permitted_df == 3)
            fprintf(stderr, "WARN: permitted_df == 3 on 16:9 material");
      }
    else
      {
        // 4:3
        if (ab->video_attr->letterboxed)
            xmlNewProp(newNode, (const xmlChar *)"widescreen", (const xmlChar *)"crop");
        if (ab->video_attr->permitted_df != 3)
            fprintf(stderr,"WARN: permitted_df != 3 on 4:3 material");
      } /*if*/
    for (i = 0; i < ab->numaudio; i++)
      {
        newNode = NewChildTag(node, "audio");
        addLangAttr(newNode, ab->audio_attr[i].lang_code);
        if (audio_format[ab->audio_attr[i].audio_format])
            xmlNewProp
              (
                newNode,
                (const xmlChar *)"format",
                (const xmlChar *)audio_format[ab->audio_attr[i].audio_format]
              );
        if (audio_type[ab->audio_attr[i].code_extension])
            xmlNewProp
              (
                newNode,
                (const xmlChar *)"content",
                (const xmlChar *)audio_type[ab->audio_attr[i].code_extension]
              );
      } /*for*/
    for (i = 0; i < ab->numsubp; i++)
      {
        newNode = NewChildTag(node, "subpicture");
        addLangAttr(newNode, ab->subp_attr[i].lang_code);
        if (subp_type[ab->subp_attr[i].code_extension])
            xmlNewProp
              (
                newNode,
                (const xmlChar *)"content",
                (const xmlChar *)subp_type[ab->subp_attr[i].code_extension]
              );
      } /*for*/
  } /*dump_attr*/

static void dump_buttons(xmlNodePtr cellNode, int vob)
  {
    int i, j;
    const hli_t *last = 0;
    for (i = 0; i < numvb; i++)
        if (vobbuttons[i].vob == vob)
          {
            const struct vobbutton * const v = &vobbuttons[i];
            const hli_t * const h = &v->h;
            const xmlNodePtr buttonsNode = NewChildTag(cellNode, "buttons");
            // fixme: add proper button overlap detection
            if
              (
                    last
                &&
                    h->hl_gi.hli_s_ptm < last->hl_gi.hli_e_ptm
                &&
                    last->hl_gi.hli_e_ptm != -1
              )
              {
                fprintf
                  (
                    stderr,
                    "WARN: Button information spans cells; errors may occur: %d - %d"
                        " (cell=%d - %d).\n",
                    h->hl_gi.hli_s_ptm,
                    h->hl_gi.hli_e_ptm,
                    getpts(v->vob, v->cell),
                    getpts(v->vob, v->cell + 1)
                  );
              } /*if*/
            AddTimeAttribute(buttonsNode, "start", h->hl_gi.hli_s_ptm, getpts(vob, 1));
            AddTimeAttribute(buttonsNode, "end", h->hl_gi.hli_e_ptm, getpts(vob, 1));
            for (j = 0; j < h->hl_gi.btn_ns; j++)
              {
                const btni_t * const b = &h->btnit[j];
                const xmlNodePtr buttonNode = NewChildTag(buttonsNode, "button");
                AddAttribute(buttonNode, "name", "%d", j + 1);
                vm_add_mnemonics
                  (
                    buttonNode,
                    "\n              ",
                    1,
                    &b->cmd
                  );
              } /*for*/
            last = h;
          } /*if; for*/
  } /*dump_buttons*/

static int setvob(unsigned char **vobs, int *numvobs, int vob)
  {
    int r;
    if (vob >= *numvobs)
      {
        *vobs = realloc(*vobs, (vob + 1)*sizeof(unsigned char));
        memset((*vobs) + (*numvobs), 0, vob + 1 - *numvobs); /* clear all newly-allocated flags */
        *numvobs = vob + 1;
      } /*if*/
    r = (*vobs)[vob];
    (*vobs)[vob] = 1;
    return r;
  } /*setvob*/

static void FindWith(xmlNodePtr angleNode, const pgcit_t *pgcs, int vob, const cell_playback_t *cp)
  /* handles interleaving of different titles as an alternative to an angle block. */
  {
    int i, j;
    unsigned char *vobs = 0;
    int numvobs = 0;
    setvob(&vobs, &numvobs, vob);
    for (i = 0; i < pgcs->nr_of_pgci_srp; i++)
        for (j = 0; j < pgcs->pgci_srp[i].pgc->nr_of_cells; j++)
          {
            xmlNodePtr withNode;
            const cell_playback_t * const ncpl = &pgcs->pgci_srp[i].pgc->cell_playback[j];
            const cell_position_t * const ncpo = &pgcs->pgci_srp[i].pgc->cell_position[j];
            if
              (
                    ncpl->first_sector > cp->last_sector
                ||
                    ncpl->last_sector < cp->first_sector
              )
                continue; /* doesn't overlap area of interest */
            if (setvob(&vobs, &numvobs, ncpo->vob_id_nr))
                continue;
            withNode = NewChildTag(angleNode, "with");
            setfilename(ncpo->vob_id_nr);
            xmlNewProp(withNode, (const xmlChar *)"file", (const xmlChar *)filenamebase);
          } /*for; for*/
    free(vobs);
  } /*FindWith*/

static const char * const entries[16] =
  /* types of entry menus in VMGM_PGCI_UT and VTSM_PGCI_UT tables. FPC doesn't
    occur in this list because it is found via its own special pointer in the VMG IFO. */
  {
    "UNKNOWN0",  "UNKNOWN1",  "title",     "root",
    "subtitle",  "audio",     "angle",     "ptt",
    "UNKNOWN8",  "UNKNOWN9",  "UNKNOWN10", "UNKNOWN11",
    "UNKNOWN12", "UNKNOWN13", "UNKNOWN14", "UNKNOWN15",
  };

static const char * const subp_control_modes[4]={
    "panscan","letterbox","widescreen","normal"
};

/* Output attributes for a pgcs (program group chain sequence) */
static void dump_pgcs(const ifo_handle_t *ifo, const pgcit_t *pgcs, const struct attrblock *ab, int titleset, int titlef, xmlNodePtr titleNode)
  {
    if (pgcs)
      {
        int i, j, titlenr, titletotal;
        titlenr = 0;
        if (titlef)
          {
            titletotal = 0;
            for (i = 0; i < pgcs->nr_of_pgci_srp; i++)
              {
                if ((pgcs->pgci_srp[i].entry_id & 0x80) != 0)
                  {
                    ++titletotal;
                  } /*if*/
              } /*for*/
          } /*if*/
        for (i = 0; i < pgcs->nr_of_pgci_srp; i++)
          {
            const pgc_t * const pgc = pgcs->pgci_srp[i].pgc;
            xmlNodePtr pgcNode, vobNode = 0, angleNode = 0;
            int curvob = -1;
            if (!titlef || (pgcs->pgci_srp[i].entry_id & 0x80) != 0)
              {
                if (titlef)
                  {
                    AddComment
                      (
                        /*parent =*/ titleNode,
                        /*format =*/ " Title %d/%d ",
                        pgcs->pgci_srp[i].entry_id & 0x7f,
                        titletotal
                      );
                  }
                else
                  {
                    AddComment
                      (
                        /*parent =*/ titleNode,
                        /*format =*/ " Menu %d/%d ",
                        i + 1,
                        pgcs->nr_of_pgci_srp
                      );
                  } /*if*/
              } /*if*/
            pgcNode = NewChildTag(titleNode, "pgc");
            if (titlef)
              {
                if ((pgcs->pgci_srp[i].entry_id & 0x80) == 0)
                  {
                    xmlNewProp(pgcNode, (const xmlChar *)"entry", "notitle");
                  }
                else
                  {
                    ++titlenr;
                  } /*if*/
              }
            else if ((pgcs->pgci_srp[i].entry_id & 0x80) != 0) /* entry PGC */
              {
                if (pgcs->pgci_srp[i].entry_id & 0x70)
                  {
                    fprintf
                      (
                        stderr,
                        "WARN: Invalid entry on menu PGC: 0x%x\n",
                        pgcs->pgci_srp[i].entry_id
                      );
                  } /*if*/
                xmlNewProp
                  (
                    pgcNode,
                    (const xmlChar *)"entry",
                    (const xmlChar *)entries[pgcs->pgci_srp[i].entry_id & 0xF]
                  );
              } /*if*/
          /* add pgc nav info */
            if (pgc->goup_pgc_nr)
              {
                AddAttribute(pgcNode, "up", "%d", pgc->goup_pgc_nr);
              } /*if*/
            if (pgc->next_pgc_nr)
              {
                AddAttribute(pgcNode, "next", "%d", pgc->next_pgc_nr);
              } /*if*/
            if (pgc->prev_pgc_nr)
              {
                AddAttribute(pgcNode, "prev", "%d", pgc->prev_pgc_nr);
              } /*if*/
          /* add pgc still time attribute */
            if (pgc->still_time)
              {
                if (pgc->still_time == 255)
                    AddAttribute(pgcNode, "pause", "inf");
                else
                    AddAttribute(pgcNode, "pause", "%d", pgc->still_time);
              } /*if*/
            for (j = 0; j < ab->numaudio; j++)
              {
                xmlNodePtr const audioNode = NewChildTag(pgcNode, "audio");
                if (pgc->audio_control[j] & 0x8000)
                  {
                    AddAttribute(audioNode, "id", "%d", (pgc->audio_control[j] >> 8) & 7);
                  }
                else
                  {
                    // fprintf(stderr,"WARN: Audio is not present for stream %d\n",j);
                    xmlNewProp(audioNode, (const xmlChar *)"present", (const xmlChar *)"no");
                  } /*if*/
              } /*for*/
            for (; j < 8; j++)
              {
                if (pgc->audio_control[j] & 0x8000)
                    fprintf
                      (
                        stderr,
                        "WARN: Audio is present for stream %d; but there should be only %d streams\n",
                        j,
                        ab->numaudio
                      );
              } /*for*/
            for (j = 0; j < ab->numsubp; j++)
              {
                xmlNodePtr const subpNode = NewChildTag(pgcNode, "subpicture");
                if (pgc->subp_control[j] & 0x80000000)
                  {
                    int mask = 0, k, first;
                    if (ab->video_attr->display_aspect_ratio)
                      {
                        // 16:9
                        mask |= 4;
                        if (!(ab->video_attr->permitted_df & 1)) // letterbox
                            mask |= 2;
                        if (!(ab->video_attr->permitted_df & 2)) // panscan
                            mask |= 1;
                      }
                    else
                      {
                        // 4:3
                        mask |= 8;
                        if (ab->video_attr->letterboxed)
                            mask |= 4; // XXX: total guess
                      } /*if*/
                    first = -1;
                    for (k = 0; k < 4; k++)
                        if (mask & (1 << k))
                          {
                            if (first < 0)
                                first = k;
                            else if
                              (
                                    ((pgc->subp_control[j] >> 8 * k) & 0x1f)
                                !=
                                    ((pgc->subp_control[j] >> 8 * first) & 0x1f)
                              )
                                break;
                          } /*if; for*/
                    if (k == 4)
                      { // all ids are the same
                        AddAttribute
                          (
                            /*parent =*/ subpNode,
                            /*attrname =*/ "id",
                            /*format =*/ "%d",
                            (pgc->subp_control[j] >> 8 * first) & 0x1f
                          );
                      }
                    else
                      {
                      /* generate individual <stream> subtags listing all the ids */
                        for (k = 3; k >= 0; k--)
                            if (mask & (1 << k))
                              {
                                xmlNodePtr const streamNode = NewChildTag(subpNode, "stream");
                                xmlNewProp(streamNode, (const xmlChar *)"mode", (const xmlChar *)subp_control_modes[k]);
                                AddAttribute(streamNode, "id", "%d", (pgc->subp_control[j] >> 8 * k) & 0x1f);
                              } /*if; for*/
                      } /*if*/
                    for (k = 3; k >= 0; k--)
                      {
                        if (!(mask & (1 << k)) && ((pgc->subp_control[j] >> 8 * k) & 0x1f) != 0)
                          {
                            fprintf
                              (
                                stderr,
                                "WARN: Subpicture defined for mode '%s' which is impossible"
                                    " for this video.\n",
                                subp_control_modes[k]
                              );
                          } /*if*/
                      } /*for*/
                  }
                else
                  {
                    // fprintf(stderr,"WARN: Subpicture is not present for stream %d\n",j);
                    xmlNewProp(subpNode, (const xmlChar *)"present", (const xmlChar *)"no");
                  } /*if*/
              } /*for*/
            for (; j < 32; j++)
              {
                if (pgc->subp_control[j] & 0x80000000)
                    fprintf
                      (
                        stderr,
                        "WARN: Subpicture is present for stream %d; but there should be only"
                            " %d streams\n",
                        j,
                        ab->numsubp
                      );
              } /*for*/
            if
              (
                    pgc->command_tbl
                &&
                    pgc->command_tbl->nr_of_pre > 0
              )
              {
                xmlNodePtr const preNode = NewChildTag(pgcNode, "pre");
                vm_add_mnemonics(preNode,
                                  "\n        ",
                                  pgc->command_tbl->nr_of_pre,
                                  pgc->command_tbl->pre_cmds);
              } /*if*/
            for (j = 0; j < pgc->nr_of_cells; j++)
              {
                xmlNodePtr cellNode;
                const cell_playback_t * const cp = &pgc->cell_playback[j];
                if (cp->interleaved)
                  {
                    switch (cp->block_mode)
                      {
                    case 0: /* normal block */
                        // The Matrix has interleaved cells that are part of different
                        // titles, instead of using angles
                        // fprintf(stderr,"WARN: Block is interleaved but block mode is normal\n");
                        // fall through and create an interleave tag, just to give a hint to
                        // dvdauthor that it can optimize it

                        // if the previous cell was from the same vob, and it was interleaved
                        // (but not an angle), then don't bother with new interleave/vob tags
                        if
                          (
                                j > 0
                            &&
                                pgc->cell_playback[j - 1].interleaved
                            &&
                                pgc->cell_playback[j - 1].block_mode == 0
                            &&
                                curvob == pgc->cell_position[j].vob_id_nr
                          )
                            break;
                        angleNode = NewChildTag(pgcNode, "interleave");
                        curvob = -1;
                        FindWith(angleNode, pgcs, pgc->cell_position[j].vob_id_nr, cp);
                    break;

                    case 1: /* angle block */
                        angleNode = NewChildTag(pgcNode, "interleave");
                        curvob = -1;
                    break;
                    case 2:
                    case 3:
                      /* unrecognized */
                    break;
                      } /*switch*/
                    if (!cp->block_type && cp->block_mode != 0)
                        fprintf(stderr, "WARN: Block is interleaved but block type is normal\n");
                  }
                else
                  {
                    if (cp->block_mode)
                        fprintf(stderr, "WARN: Block is not interleaved but block mode is not normal\n");
                    if (cp->block_type)
                        fprintf(stderr, "WARN: Block is not interleaved but block type is not normal\n");
                    if (angleNode != pgcNode) // force a new vobNode if the previous one was interleaved
                        curvob = -1;
                    angleNode = pgcNode;
                  } /*if*/

                if (curvob != pgc->cell_position[j].vob_id_nr)
                  {
                    vobNode = NewChildTag(angleNode, "vob");
                    curvob = pgc->cell_position[j].vob_id_nr;
                    setfilename(curvob);
                    xmlNewProp(vobNode, (const xmlChar *)"file", (const xmlChar *)filenamebase);
                    dump_buttons(vobNode, curvob);
                  } /*if*/
                cellNode = NewChildTag(vobNode, "cell");
                AddCellTimeAttribute(cellNode, "start", pgc->cell_position[j].vob_id_nr, pgc->cell_position[j].cell_nr);
                AddCellTimeAttribute(cellNode, "end", pgc->cell_position[j].vob_id_nr, pgc->cell_position[j].cell_nr + 1);
                switch (getprogramtype(titlef ? ifo->vts_ptt_srpt : 0, pgc, i + 1, j + 1))
                  {
                case CELL_CHAPTER_PROGRAM:
                    xmlNewProp(cellNode, (const xmlChar *)"chapter", (const xmlChar *)"1");
                break;
                case CELL_PROGRAM:
                    xmlNewProp(cellNode, (const xmlChar *)"program", (const xmlChar *)"1");
                break;
                case CELL_NEITHER:
                  /* do nothing */
                break;
                  } /*switch*/
              /* add cell still time attribute */
                if (cp->still_time)
                  {
                    if (cp->still_time == 255)
                        AddAttribute(cellNode, "pause", "inf");
                    else
                        AddAttribute(cellNode, "pause", "%d", cp->still_time);
                  } /*if*/
              /* add cell commands */
                if (cp->cell_cmd_nr)
                  {
                    vm_add_mnemonics(cellNode,
                                     "\n          ",
                                     1,
                                     &pgc->command_tbl->cell_cmds[cp->cell_cmd_nr-1]);
                  } /*if*/
              } /*for*/
            if
              (
                    pgc->command_tbl
                &&
                    pgc->command_tbl->nr_of_post > 0
              )
              {
                xmlNodePtr const postNode = NewChildTag(pgcNode, "post");
                vm_add_mnemonics(postNode,
                                  "\n        ",
                                  pgc->command_tbl->nr_of_post,
                                  pgc->command_tbl->post_cmds);
              } /*if*/
          } /*for*/
      } /*if*/
  } /*dump_pgcs*/

/* Output attributes for a fpc (first play chain sequence) */
static void dump_fpc(const pgc_t *p, xmlNodePtr titlesetNode)
  {
    /* add fpc commands */
    if (p && p->command_tbl && p->command_tbl->nr_of_pre)
      {
        AddComment(titlesetNode, " First Play ");
        xmlNodePtr const fpcNode = NewChildTag(titlesetNode, "fpc");
        vm_add_mnemonics(fpcNode,
                          "\n    ",
                          p->command_tbl->nr_of_pre,
                          p->command_tbl->pre_cmds);
      } /*if*/
  } /*dump_fpc*/

static void findpalette(int vob, const pgcit_t *pgcs, const uint32_t **palette, int *length)
  {
    if (pgcs)
      {
        int i, j, ptime;
        for (i = 0; i < pgcs->nr_of_pgci_srp; i++)
          {
            const pgc_t * const p = pgcs->pgci_srp[i].pgc;
            for (j = 0; j < p->nr_of_cells; j++)
                if (p->cell_position[j].vob_id_nr == vob)
                    break;
            if (j == p->nr_of_cells)
                continue;
            ptime =
                    p->playback_time.hour << 24
                |
                    p->playback_time.minute << 16
                |
                    p->playback_time.second << 8
                |
                    p->playback_time.frame_u & 0x3f;
            if (!(*palette) || ptime > *length)
              {
                if (*palette && memcmp(*palette, p->palette, 16 * sizeof(uint32_t)))
                    fprintf(stderr, "WARN:  VOB %d has conflicting palettes\n", vob);
                *palette = p->palette;
                *length = ptime;
              } /*if*/
          } /*for*/
      } /*if*/
  } /*findpalette*/

unsigned int read4(unsigned char *p)
{
    return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3];
}

unsigned int read2(unsigned char *p)
{
    return (p[0]<<8)|p[1];
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

static void wdstr(const char *s)
{
    while(*s)
        wdbyte(*s++);
    wdbyte(0);
}

static void writepalette(int h, const uint32_t *palette)
  {
    static unsigned char sector[2048];
    int i,j;
    if (!palette)
        return;
    for (j = 0; j < 32; j++)
      {
        sector[0] = 0;
        sector[1] = 0;
        sector[2] = 1;
        sector[3] = MPID_PACK; /* PACK header */
        sector[4] = 0x44; /* SCR = 0 */
        sector[5] = 0;
        sector[6] = 4;
        sector[7] = 0;
        sector[8] = 4;
        sector[9] = 1;
        sector[10] = 1;
        sector[11] = 137;
        sector[12] = 195;
        sector[13] = 0xf8;

        sector[14] = 0; // padding stream header
        sector[15] = 0;
        sector[16] = 1;
        sector[17] = MPID_PAD;
        sector[18] = (2048 - 20) >> 8; /* packet length, high byte */
        sector[19] = (2048 - 20) & 255; /* packet length, low byte */
        memset(sector + 20, 0xff, 2048 - 20); /* initialize with pad bytes */

        wdest = sector + 20;
        wdstr("dvdauthor-data");
        wdbyte(2); // version
        wdbyte(1); // subtitle info
        wdbyte(j); // sub number

        wdlong(0); // start/end pts
        wdlong(0);

        wdbyte(1); // colormap
        wdbyte(16); // number of colors
        for (i = 0; i < 16; i++)
          {  
            wdbyte(palette[i] >> 16);
            wdbyte(palette[i] >> 8);
            wdbyte(palette[i]);
          } /*for*/
        if (write(h, sector, 2048) < 2048)
          {
            fprintf(stderr, "ERR:  Error %d writing data: %s\n", errno, strerror(errno));
            exit(1);
          } /*if*/
      } /*for*/
 } /*writepalette*/

static void writebutton(int h, const unsigned char *packhdr, const hli_t *hli)
  {
    static unsigned char sector[2048];
    int i;
    if (!hli->hl_gi.hli_ss)
        return;
    memcpy(sector, packhdr, 14); // copy pack header
    sector[14] = 0; // padding stream header
    sector[15] = 0;
    sector[16] = 1;
    sector[17] = MPID_PAD;
    sector[18] = (2048 - 20) >> 8; /* packet length, high byte */
    sector[19] = (2048 - 20) & 255; /* packet length, low byte */
    memset(sector + 20, 0xff, 2048 - 20); /* initialize with pad bytes */

    wdest = sector + 20;
    wdstr("dvdauthor-data");
    wdbyte(2); // version
    wdbyte(1); // subtitle info
    wdbyte(0); // sub number

    wdlong(hli->hl_gi.hli_s_ptm); // start pts
    wdlong(hli->hl_gi.hli_e_ptm); // end pts

    wdbyte(2); // sl_coli
    wdbyte(3);
    for (i = 0; i < 6; i++)
        wdlong(hli->btn_colit.btn_coli[i >> 1][i & 1]);
    
    wdbyte(3); // btn_it
    wdbyte(hli->hl_gi.btn_ns);
    for (i = 0; i < hli->hl_gi.btn_ns; i++)
      {
        const btni_t * const b = hli->btnit + i;
        char nm1[10]; /* should be big enough to avoid overflow! */
        sprintf(nm1, "%d", i + 1);
        wdstr(nm1);
        wdshort(0);
        wdbyte(b->auto_action_mode);
        wdbyte(b->btn_coln);
        wdshort(b->x_start);
        wdshort(b->y_start);
        wdshort(b->x_end);
        wdshort(b->y_end);

        sprintf(nm1, "%d", b->up);    wdstr(nm1);
        sprintf(nm1, "%d", b->down);  wdstr(nm1);
        sprintf(nm1, "%d", b->left);  wdstr(nm1);
        sprintf(nm1, "%d", b->right); wdstr(nm1);
      } /*for*/
    
    if (write(h, sector, 2048) < 2048)
      {
        fprintf(stderr, "ERR:  Error %d writing data: %s\n", errno, strerror(errno));
        exit(1);
      } /*if*/
  } /*writebutton*/


static void getVobs(dvd_reader_t *dvd, const ifo_handle_t *ifo, int titleset, int titlef)
  {
    dvd_file_t *vobs;
    const c_adt_t *cptr;
    const cell_adr_t *cells;
    int numcells,i,j,totalsect,numsect;
    clock_t start,now,clkpsec;
    struct tms unused_tms;

    cptr = titlef ? ifo->vts_c_adt : ifo->menu_c_adt;
    if (cptr)
      {
        cells = cptr->cell_adr_table;
        numcells = (cptr->last_byte + 1 - C_ADT_SIZE) / sizeof(cell_adr_t);
      }
    else
      {
        cells = 0;
        numcells = 0;
      } /*if*/
    numcst = 0;

    vobs = DVDOpenFile(dvd, titleset, titlef ? DVD_READ_TITLE_VOBS : DVD_READ_MENU_VOBS);
    if (vobs == NULL)
      {
      /* error message already output */
        exit(1);
      } /*if*/

    numsect = 0;
    totalsect = 0;
    for (i = 0; i < numcells; i++)
        totalsect += cells[i].last_sector - cells[i].start_sector + 1;
    clkpsec = sysconf(_SC_CLK_TCK);
    start = times(&unused_tms);
    
    for (i = 0; i < numcells; i++)
      {
        int h, b, plen;
        const uint32_t * palette = 0;

        if (titlef)
          {
            findpalette(cells[i].vob_id, ifo->vts_pgcit, &palette, &plen);
          }
        else
          {
            if (ifo->pgci_ut)
              {
                for (j = 0; j < ifo->pgci_ut->nr_of_lus; j++)
                  {
                    const pgci_lu_t * const lu = &ifo->pgci_ut->lu[j];
                    findpalette(cells[i].vob_id, lu->pgcit, &palette, &plen);
                  } /*for*/
              } /*if*/
          } /*if*/

        setfilename(cells[i].vob_id);
        if (vobexists(cells,i,cells[i].vob_id))
            h = open(filenamebase, O_CREAT | O_APPEND | O_WRONLY | O_BINARY, 0666);
        else
          {
            h = open(filenamebase, O_CREAT | O_TRUNC | O_WRONLY | O_BINARY, 0666);
            writepalette(h, palette);
          } /*if*/

        if
          (
                i == 0
            ||
                cells[i].vob_id != cells[i-1].vob_id
          )
          {
            memset(&curhli, 0, sizeof(curhli));
          } /*if*/

        if (h < 0)
          {
            fprintf(stderr, "ERR:  Cannot open %s for writing\n", filenamebase);
            exit(1);
          } /*if*/
        for (b = cells[i].start_sector; b <= cells[i].last_sector; b += BIGBLOCKSECT)
          {
            int rl = cells[i].last_sector + 1 - b;
            if (rl > BIGBLOCKSECT)
                rl = BIGBLOCKSECT;
            now = times(&unused_tms);
            if (now-start > 3 * clkpsec && numsect > 0)
              {
                const int rmn = (totalsect - numsect) * (now - start) / (numsect * clkpsec);
                  /* estimate of time remaining */
                fprintf
                  (
                    stderr,
                    "STAT: [%d] VOB %d, Cell %d (%d%%, %d:%02d remain)\r",
                    i,
                    cells[i].vob_id,
                    cells[i].cell_id,
                    (numsect * 100 + totalsect / 2) / totalsect,
                    rmn / 60,
                    rmn % 60
                  );
              }
            else
                fprintf
                  (
                    stderr,
                    "STAT: [%d] VOB %d, Cell %d (%d%%)\r",
                    i,
                    cells[i].vob_id,
                    cells[i].cell_id,
                    (numsect * 100 + totalsect / 2) / totalsect
                  );
            if (DVDReadBlocks(vobs, b, rl, bigblock) < rl)
              {
                fprintf(stderr, "\nERR:  Error %d reading data: %s\n", errno, strerror(errno));
                break;
              } /*if*/
            numsect += rl;
            for (j = 0; j < rl; j++)
              {
                if
                  (
                        bigblock[j * DVD_VIDEO_LB_LEN + 14] == 0
                    &&
                        bigblock[j * DVD_VIDEO_LB_LEN + 15] == 0
                    &&
                        bigblock[j * DVD_VIDEO_LB_LEN + 16] == 1
                    &&
                        bigblock[j * DVD_VIDEO_LB_LEN + 17] == MPID_SYSTEM // system header
                    &&
                        bigblock[j * DVD_VIDEO_LB_LEN + 38] == 0
                    &&
                        bigblock[j * DVD_VIDEO_LB_LEN + 39] == 0
                    &&
                        bigblock[j * DVD_VIDEO_LB_LEN + 40] == 1
                    &&
                        bigblock[j * DVD_VIDEO_LB_LEN + 41] == MPID_PRIVATE2 // 1st private2
                    &&
                        bigblock[j * DVD_VIDEO_LB_LEN + 1024] == 0
                    &&
                        bigblock[j * DVD_VIDEO_LB_LEN + 1025] == 0
                    &&
                        bigblock[j * DVD_VIDEO_LB_LEN + 1026] == 1
                    &&
                        bigblock[j * DVD_VIDEO_LB_LEN + 1027] == MPID_PRIVATE2 // 2nd private2
                  )
                  {
                  /* looks like a NAV pack */
                    pci_t p;
                    //dsi_t d;
                    
                    navRead_PCI(&p, bigblock + j * DVD_VIDEO_LB_LEN + 0x2d);
                    //navRead_DSI(&d,bigblock+j*DVD_VIDEO_LB_LEN+0x407);

                    addcst(cells[i].vob_id, cells[i].cell_id, p.pci_gi.vobu_s_ptm);

                    if (p.hli.hl_gi.hli_ss)
                      {
                        if (!palette)
                          {
                            fprintf(stderr, "\nWARN: How can there be buttons but no palette?\n");
                          } /*if*/
                        if (p.hli.hl_gi.hli_ss >= 2 && !curhli.hl_gi.hli_ss)
                          {
                            fprintf
                              (
                                stderr,
                                "\nWARN: Button information carries over from previous VOBU,"
                                " but there is no\nWARN: record of previous button information.\n"
                              );
                          } /*if*/
                        if (p.hli.hl_gi.hli_s_ptm < curhli.hl_gi.hli_e_ptm)
                          {
                          /* button(s) being highlighted for nonzero time */
                            switch (p.hli.hl_gi.hli_ss)
                              {
                          /* case 0: no highlight information for this VOBU -- nothing to do */
                            case 1: /* all new highlight information for this VOBU */
                                if (memcmp(&p.hli, &curhli, sizeof(curhli)))
                                  {
                                    // fprintf(stderr,"\nWARN: Button information changes!\n");
                                    // we detect overlapping button ptm in dump_buttons
                                    memcpy(&curhli, &p.hli, sizeof(curhli));
                                    hli_pci = p;
                                    addbutton(cells[i].vob_id, cells[i].cell_id, &curhli);
                                    writebutton(h, bigblock + j * DVD_VIDEO_LB_LEN, &curhli);
                                  } /*if*/
                            break;
                         /* case 2: use highlight information from previous VOBU -- nothing to do */
                            case 3:
                              /* use highlight information from previous VOBU except commands,
                                which come from this VOBU */
                                if (memcmp(&p.hli.btnit, &curhli.btnit, sizeof(curhli.btnit)))
                                  {
                                    fprintf(stderr, "\nWARN: Button commands changes!\n");
                                      /* fixme: deal with this? */
                                  } /*if*/
                            break;
                              } /*switch*/
                          }
                        else
                          {
                            memcpy(&curhli, &p.hli, sizeof(curhli));
                            hli_pci = p;
                            addbutton(cells[i].vob_id, cells[i].cell_id, &curhli);
                            writebutton(h, bigblock + j * DVD_VIDEO_LB_LEN, &curhli);
                          } /*if*/
                      } /*if*/
                  } /*if NAV pack*/
                if (write(h, bigblock + j * DVD_VIDEO_LB_LEN, DVD_VIDEO_LB_LEN) < DVD_VIDEO_LB_LEN)
                  {
                    fprintf(stderr, "\nERR:  Error %d writing data: %s\n", errno, strerror(errno));
                    exit(1);
                  } /*if*/
              } /*for*/
          } /*for*/
        close(h);
      } /*for*/
  } /*getVobs*/

static void dump_dvd
  (
    dvd_reader_t *dvd,
    int titleset, /* titleset nr, 0 for VMG */
    int titlef, /* 0 for menu, 1 for title */
    xmlNodePtr titlesetNode /* parent node to attach dump to */
  )
  {
    ifo_handle_t *ifo;
 
    if (titleset < 0 || titleset > 99)
      {
        fprintf(stderr, "ERR:  Can only handle titlesets 0..99\n");
        exit(1);
      } /*if*/
    if (titlef < 0 || titlef > 1)
      {
        fprintf(stderr, "ERR:  Title flag must be 0 (menu) or 1 (title)\n");
        exit(1);
      } /*if*/
    if (titlef == 1 && titleset == 0)
      {
        fprintf(stderr, "ERR:  No title for VMGM\n");
        exit(1);
      } /*if*/
    ifo = ifoOpen(dvd, titleset);
    if (ifo == NULL)
      {
      /* error message already output */
        exit(1);
      } /*if*/
    if (!titleset)
        numtitlesets = ifo->vmgi_mat->vmg_nr_of_title_sets;

    if (numcst)
      {
      /* dispose of storage allocated for previous dump */
        free(cellstarttimes);
        cellstarttimes = 0;
        numcst = 0;
      } /*if*/

    if (numvb)
      {
      /* dispose of storage allocated for previous dump */
        free(vobbuttons);
        vobbuttons = 0;
        numvb = 0;
      } /*if*/

    snprintf(filenamebase, sizeof(filenamebase), "vob_%02d%c_",
              titleset,
              titlef?'t':'m'); /* ready for appending vob nr by setfilename */

    getVobs(dvd, ifo, titleset, titlef);

    if (titlef)
      {
        xmlNodePtr titleNode = NewChildTag(titlesetNode, "titles");
        struct attrblock ab;

        get_attr(ifo, titlef, &ab);
        dump_attr( &ab, titleNode );
        dump_pgcs(ifo, ifo->vts_pgcit, &ab, titleset, titlef, titleNode);
      }
    else
      {
        if (titleset == 0) /* VMG */
          {
            dump_fpc(ifo->first_play_pgc, titlesetNode);
            if (ifo->tt_srpt)
              {
                int i;
                for (i = 0; i < ifo->tt_srpt->nr_of_srpts; i++)
                  {
                    const xmlNodePtr titleMapNode = NewChildTag(titlesetNode, "titlemap");
                    AddAttribute(titleMapNode, "titleset", "%d", ifo->tt_srpt->title[i].title_set_nr);
                    AddAttribute(titleMapNode, "title", "%d", ifo->tt_srpt->title[i].vts_ttn);
                  } /*for*/
              } /*if*/
          } /*if*/
        if (ifo->pgci_ut)
          {
            int i;
            for (i = 0; i < ifo->pgci_ut->nr_of_lus; i++)
              {
                const pgci_lu_t * const lu = &ifo->pgci_ut->lu[i];
                struct attrblock ab;
                const xmlNodePtr menusNode = NewChildTag(titlesetNode, "menus");
                addLangAttr(menusNode, lu->lang_code);
                get_attr(ifo, titleset == 0 ? -1 : 0, &ab);
                dump_attr(&ab, menusNode);
                dump_pgcs(ifo, lu->pgcit, &ab, titleset, titlef, menusNode);
              } /*for*/
          } /*if*/
      } /*if*/

    ifoClose(ifo);
  } /*dump_dvd*/

static void usage(void)
{
    fprintf(stderr,"syntax: dvdunauthor pathname\n"
            "\n"
            "\tpathname can either be a DVDROM device name, an ISO image, or a path to\n"
            "\ta directory with the appropriate files in it.\n"
        );
    exit(1);
}

int main(int argc, char **argv)
  {
    xmlDocPtr  myXmlDoc;
    xmlNodePtr mainNode;
    xmlNodePtr titlesetNode;

    dvd_reader_t *dvd;
    int i;
    char *devname = 0;

    fputs(PACKAGE_HEADER("dvdunauthor"), stderr);

    while (-1 != (i = getopt(argc, argv, "h")))
      {
        switch(i)
          {
        case 'h':
        default:
            usage();
        break;
          } /*switch*/
      } /*while*/

    if (optind + 1 != argc)
      {
        usage();
      } /*if*/
    devname = argv[optind];

    dvd = DVDOpen(devname);
    if (!dvd)
      {
        fprintf(stderr, "ERR:  Cannot open path '%s'\n", argv[1]);
        return 1;
      } /*if*/

    myXmlDoc = xmlNewDoc( (xmlChar *)"1.0" );
    mainNode = xmlNewDocNode(myXmlDoc, NULL, (xmlChar *)"dvdauthor", NULL);
    xmlDocSetRootElement(myXmlDoc, mainNode);
    xmlNewProp(mainNode, (const xmlChar *)"allgprm", (const xmlChar *)"yes");
      
    for (i = 0; i <= numtitlesets; i++)
      {
        if (i)
          {
          /* dump a titleset menu */
            fprintf(stderr, "\n\nINFO: VTSM %d/%d\n", i, numtitlesets);
            AddComment(mainNode, " Titleset %d/%d ", i, numtitlesets);
            titlesetNode = NewChildTag(mainNode, "titleset");
          }
        else
          {
          /* dump the VMG menu */
            fprintf(stderr, "\n\nINFO: VMGM\n");
            titlesetNode = NewChildTag(mainNode, "vmgm");
          } /*if*/
        dump_dvd(dvd, i, 0, titlesetNode);
        if (i)
          {
          /* dump a titleset title */
            fprintf(stderr, "\n\nINFO: VTS %d/%d\n", i, numtitlesets);
            dump_dvd(dvd, i, 1, titlesetNode);
          } /*if*/
      } /*for*/
     
    xmlSaveFormatFile("dvdauthor.xml", myXmlDoc, 1);
    xmlFreeDoc(myXmlDoc);

    DVDClose(dvd);
    fprintf(stderr, "\n\n");
    return 0;
  } /*main*/
