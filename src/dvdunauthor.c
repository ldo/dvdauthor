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

#include "dvduncompile.h"


#define BIGBLOCKSECT 512
#define BIGBLOCKLEN (DVD_VIDEO_LB_LEN*BIGBLOCKSECT)
static unsigned char bigblock[BIGBLOCKLEN];

static int numtitlesets=0; // set in dvdump
static char filenamebase[128];

static struct cellstarttime {
    int vob,cell,pts;
} *cellstarttimes=0;

static struct vobbutton {
    int vob,cell;
    hli_t h;
} *vobbuttons=0;

static int numcst=0,numvb=0;

static hli_t curhli;
static pci_t hli_pci;

static void setfilename(int vob)
{
    int l=3+1+2+1+1;
    snprintf(filenamebase+l,sizeof(filenamebase)-l,"%03d.vob",vob);
}

static void addcst(int v,int c,int p)
{
    int i;

    for( i=0; i<numcst; i++ )
        if( cellstarttimes[i].vob==v && cellstarttimes[i].cell==c )
            return;
    cellstarttimes=realloc(cellstarttimes,(numcst+1)*sizeof(struct cellstarttime));
    cellstarttimes[numcst].vob=v;
    cellstarttimes[numcst].cell=c;
    cellstarttimes[numcst].pts=p;
    numcst++;
}

static void addbutton(int v,int c,hli_t *h)
{
    vobbuttons=realloc(vobbuttons,(numvb+1)*sizeof(struct vobbutton));
    vobbuttons[numvb].vob=v;
    vobbuttons[numvb].cell=c;
    vobbuttons[numvb].h=*h;
    numvb++;
}

static int vobexists(cell_adr_t *cells,int numcells,int vobid)
{
    int i;

    for( i=0; i<numcells; i++ )
        if( cells[i].vob_id==vobid )
            return 1;
    return 0;
}

static int getpts(int v,int c)
{
    int i;

    for( i=0; i<numcst; i++ )
        if( cellstarttimes[i].vob==v && cellstarttimes[i].cell==c )
            return cellstarttimes[i].pts;
    return -1;
}

static int printtime(int t1,int t2,char *buf,int buflen)
{
    if( t1>=0 && t2>=0 ) {
        int t=t1-t2;
        snprintf(buf, buflen,
                 "%d:%02d:%02d.%03d",
                 (t/90/1000/60/60),
                 (t/90/1000/60)%60,
                 (t/90/1000)%60,
                 (t/90)%1000);
        return 1;
    } else {
        snprintf(buf, buflen,"-1");
        return 0;
    }
}

static int printcelltime(int v,int c, char* buf, int bufLen )
{
    return printtime(getpts(v,c),getpts(v,1),buf,bufLen);
}

/* Add a language attribute to the node */
static void addLangAttr( xmlNodePtr node, uint16_t lang_code )
{
    if( lang_code ) {
        char buffer[8];

        if(isalpha((int)(lang_code >> 8)) &&
           isalpha((int)(lang_code & 0xff))) {
            snprintf( buffer, 8, "%c%c", lang_code>>8,lang_code & 0xff );
        } else {
            snprintf( buffer, 8, "%02x%02x", lang_code>>8,lang_code & 0xff );
        }

        xmlNewProp( node, "lang", buffer );
    }
}

static int getprogramtype(vts_ptt_srpt_t *tt,pgc_t *p,int pn,int c)
{
    int ptype=0, i, j, pg=0;

    for( i=0; i<p->nr_of_programs; i++ )
        if( c==p->program_map[i] ) {
            pg=i+1;
            ptype=2;
            break;
        }
    if( ptype && tt ) {
        for( i=0; i<tt->nr_of_srpts; i++ )
            for( j=0; j<tt->title[i].nr_of_ptts; j++ )
                if( tt->title[i].ptt[j].pgcn==pn &&
                    tt->title[i].ptt[j].pgn==pg )
                    return 1;
    }
    return ptype;
}

/* Output Video Title Set Attributes */

static const char *permitted_df[4] = {NULL, "noletterbox", "nopanscan", NULL};

static const char *audio_format[8] = {"ac3", NULL, NULL, "mp2", "pcm ", NULL, "dts", NULL};
static const char *audio_type[5]   = {NULL, "normal", "impaired", "comments1", "comments2"};

static const char *subp_type[16]   = {
    NULL, "normal",    "large",          "children",
    NULL, "normal_cc", "large_cc",       "children_cc",
    NULL, "forced",    NULL,             NULL,
    NULL, "director",  "large_director", "children_director"
};

struct attrblock {
    video_attr_t *video_attr;
    audio_attr_t *audio_attr;
    subp_attr_t  *subp_attr;
    int numaudio, numsubp;
};

static void get_attr(ifo_handle_t *ifo,int titlef,struct attrblock *ab)
{
    switch( titlef ) {
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
    }
}

static void dump_attr( struct attrblock *ab,
                       xmlNodePtr node ) 
{
    int i;
    xmlNodePtr newNode;

    /* add video attributes */
    newNode = xmlNewTextChild( node, NULL, "video", NULL );

    if( ab->video_attr->display_aspect_ratio ) {
        // 16:9
        
        if( permitted_df[ab->video_attr->permitted_df] )
            xmlNewProp( newNode, "widescreen", permitted_df[ab->video_attr->permitted_df]);
            
        if( ab->video_attr->permitted_df == 3 )
            fprintf(stderr,"WARN: permitted_df == 3 on 16:9 material");
    } else {
        // 4:3

        if( ab->video_attr->letterboxed )
            xmlNewProp( newNode, "widescreen", "crop");

        if( ab->video_attr->permitted_df != 3 )
            fprintf(stderr,"WARN: permitted_df != 3 on 4:3 material");
    }

    for(i = 0; i < ab->numaudio; i++) {
        newNode = xmlNewTextChild( node, NULL, "audio", NULL );
        addLangAttr( newNode, ab->audio_attr[i].lang_code);
        if( audio_format[ab->audio_attr[i].audio_format] )
            xmlNewProp( newNode, "format", audio_format[ab->audio_attr[i].audio_format] );
        if( audio_type[ab->audio_attr[i].code_extension] )
            xmlNewProp( newNode, "content", audio_type[ab->audio_attr[i].code_extension] );
    }

    for(i = 0; i < ab->numsubp; i++) {
        xmlNodePtr newNode = xmlNewTextChild( node, NULL, "subpicture", NULL );
        addLangAttr( newNode, ab->subp_attr[i].lang_code);
        if( subp_type[ab->subp_attr[i].code_extension] )
            xmlNewProp( newNode, "content", subp_type[ab->subp_attr[i].code_extension] );
    }

}

static void dump_buttons(xmlNodePtr cellNode,int vob)
{
    int i,j;
    char buffer[128];
    hli_t *last=0;

    for( i=0; i<numvb; i++ )
        if( vobbuttons[i].vob==vob ) {
            struct vobbutton *v=&vobbuttons[i];
            hli_t *h=&v->h;
            xmlNodePtr buttonsNode = xmlNewTextChild( cellNode, NULL, "buttons", NULL );

            // XXX: add proper button overlap detection

            if( last &&
                h->hl_gi.hli_s_ptm < last->hl_gi.hli_e_ptm &&
                last->hl_gi.hli_e_ptm != -1 )
            {
                fprintf(stderr,"WARN: Button information spans cells; errors may occur: %d - %d (cell=%d - %d).\n",
                        h->hl_gi.hli_s_ptm,h->hl_gi.hli_e_ptm,
                        getpts(v->vob,v->cell),getpts(v->vob,v->cell+1));
            }
            
            if( printtime(h->hl_gi.hli_s_ptm,getpts(vob,1),buffer,sizeof(buffer)) )
                xmlNewProp( buttonsNode, "start", buffer );

            if( printtime(h->hl_gi.hli_e_ptm,getpts(vob,1),buffer,sizeof(buffer)) )
                xmlNewProp( buttonsNode, "end", buffer );

            for( j=0; j<h->hl_gi.btn_ns; j++) {
                btni_t  *b=&h->btnit[j];
                xmlNodePtr buttonNode = xmlNewTextChild( buttonsNode, NULL, "button", NULL );
                snprintf(buffer,sizeof(buffer),"%d",j+1);
                xmlNewProp( buttonNode, "name", buffer );
                vm_add_mnemonics( buttonNode,
                                  "\n              ",
                                  1,
                                  &b->cmd );
            }
        }
}

static int setvob(unsigned char **vobs,int *numvobs,int vob)
{
    int r;

    if( vob >= *numvobs ) {
        *vobs = realloc( *vobs, (vob+1)*sizeof(unsigned char));
        memset( (*vobs) + (*numvobs), 0, vob+1 - *numvobs );
        *numvobs = vob+1;
    }
    r=(*vobs)[vob];
    (*vobs)[vob]=1;
    return r;
}

static void FindWith(xmlNodePtr angleNode, pgcit_t *pgcs, int vob, cell_playback_t *cp)
{
    int i,j;
    unsigned char *vobs=0;
    int numvobs=0;

    setvob(&vobs,&numvobs,vob);

    for( i=0; i<pgcs->nr_of_pgci_srp; i++ )
        for( j=0; j<pgcs->pgci_srp[i].pgc->nr_of_cells; j++ ) {
            xmlNodePtr withNode;

            cell_playback_t *ncpl=&pgcs->pgci_srp[i].pgc->cell_playback[j];
            cell_position_t *ncpo=&pgcs->pgci_srp[i].pgc->cell_position[j];

            if( ncpl->first_sector > cp->last_sector ||
                ncpl->last_sector < cp->first_sector )
                continue;
            if( setvob(&vobs,&numvobs,ncpo->vob_id_nr) )
                continue;

            withNode=xmlNewTextChild( angleNode, NULL, "with", NULL );
            setfilename(ncpo->vob_id_nr);
            xmlNewProp(withNode, "file",  filenamebase);
        }
            

}


static const char *entries[16]={
    "UNKNOWN0",  "UNKNOWN1",  "title",     "root",      // XXX: is 1 == fpc?
    "subtitle",  "audio",     "angle",     "ptt",
    "UNKNOWN8",  "UNKNOWN9",  "UNKNOWN10", "UNKNOWN11",
    "UNKNOWN12", "UNKNOWN13", "UNKNOWN14", "UNKNOWN15"
};

static const char *subp_control_modes[4]={
    "panscan","letterbox","widescreen","normal"
};

/* Output attributes for a pgcs (program group chain sequence) */
static void dump_pgcs(ifo_handle_t *ifo,pgcit_t *pgcs,struct attrblock *ab,int titleset,int titlef, xmlNodePtr titleNode)
{
    if( pgcs ) {
        int i, j;

        for( i=0; i<pgcs->nr_of_pgci_srp; i++ ) {
            pgc_t      *pgc    = pgcs->pgci_srp[i].pgc;
            xmlNodePtr pgcNode, vobNode = 0, angleNode = 0;
            char       buffer[100];
            int        curvob = -1;

            if( !titlef )
                snprintf(buffer,sizeof(buffer)," Menu %d/%d ",i+1,pgcs->nr_of_pgci_srp);
            else
                snprintf(buffer,sizeof(buffer)," Title %d/%d ",i+1,pgcs->nr_of_pgci_srp);
            xmlAddChildList(titleNode,xmlNewComment(buffer));

            pgcNode = xmlNewTextChild( titleNode, NULL, "pgc", NULL );

            if( !titlef && (pgcs->pgci_srp[i].entry_id&0x80) ) {
                if( pgcs->pgci_srp[i].entry_id&0x70 ) {
                    fprintf(stderr,"WARN: Invalid entry on menu PGC: 0x%x\n",pgcs->pgci_srp[i].entry_id);
                }
                xmlNewProp( pgcNode, "entry", entries[pgcs->pgci_srp[i].entry_id&0xF] );
            }

            /* add pgc nav info */
            if( pgc->goup_pgc_nr ) {
                snprintf(buffer,sizeof(buffer),"%d",pgc->goup_pgc_nr);
                xmlNewProp( pgcNode, "up", buffer);
            }
            if( pgc->next_pgc_nr ) {
                snprintf(buffer,sizeof(buffer),"%d",pgc->next_pgc_nr);
                xmlNewProp( pgcNode, "next", buffer);
            }
            if( pgc->prev_pgc_nr ) {
                snprintf(buffer,sizeof(buffer),"%d",pgc->prev_pgc_nr);
                xmlNewProp( pgcNode, "prev", buffer);
            }
 
            /* add pgc still time attribute */
            if( pgc->still_time ) {
                if( pgc->still_time==255 )
                    snprintf(buffer,sizeof(buffer),"inf");
                else
                    snprintf(buffer,sizeof(buffer),"%d",pgc->still_time);
                xmlNewProp( pgcNode, "pause", buffer );
            }

            for( j=0; j<ab->numaudio; j++ ) {
                xmlNodePtr audioNode = xmlNewTextChild( pgcNode, NULL, "audio", NULL );
                if( pgc->audio_control[j] & 0x8000 ) {
                    snprintf(buffer,sizeof(buffer),"%d",(pgc->audio_control[j]>>8)&7);
                    xmlNewProp( audioNode, "id", buffer );
                } else {
                    // fprintf(stderr,"WARN: Audio is not present for stream %d\n",j);
                    xmlNewProp( audioNode, "present", "no" );
                }
            }

            for( ; j<8; j++ ) {
                if( pgc->audio_control[j] & 0x8000 )
                    fprintf(stderr,"WARN: Audio is present for stream %d; but there should be only %d\n",j,ab->numaudio);
            }

            for( j=0; j<ab->numsubp; j++ ) {
                xmlNodePtr subpNode = xmlNewTextChild( pgcNode, NULL, "subpicture", NULL );
                if( pgc->subp_control[j] & 0x80000000 ) {
                    int mask=0,k,first;

                    if( ab->video_attr->display_aspect_ratio ) {
                        // 16:9
                        mask|=4;
                        if( !(ab->video_attr->permitted_df & 1) ) // letterbox
                            mask|=2;
                        if( !(ab->video_attr->permitted_df & 2) ) // panscan
                            mask|=1;
                    } else {
                        // 4:3
                        mask|=8;
                        if( ab->video_attr->letterboxed )
                            mask|=4; // XXX: total guess
                    }

                    first=-1;
                    for( k=0; k<4; k++ )
                        if( mask&(1<<k) ) {
                            if( first<0 )
                                first=k;
                            else if( ((pgc->subp_control[j]>>8*k)&0x1f) != ((pgc->subp_control[j]>>8*first)&0x1f) )
                                break;
                        }
                    if( k==4 ) { // all ids are the same
                        snprintf(buffer,sizeof(buffer),"%d",(pgc->subp_control[j]>>8*first)&0x1f);
                        xmlNewProp( subpNode, "id", buffer );
                    } else {
                        for( k=3; k>=0; k-- )
                            if( mask&(1<<k) ) {
                                xmlNodePtr streamNode = xmlNewTextChild( subpNode, NULL, "stream", NULL );
                                snprintf(buffer,sizeof(buffer),"%d",(pgc->subp_control[j]>>8*k)&0x1f);
                                xmlNewProp( streamNode, "mode", subp_control_modes[k] );
                                xmlNewProp( streamNode, "id", buffer );
                            }
                    }
                    for( k=3; k>=0; k-- ) {
                        if( !(mask&(1<<k)) && ((pgc->subp_control[j]>>8*k)&0x1f)!=0 ) {
                            fprintf(stderr,"WARN: Subpicture defined for mode '%s' which is impossible for this video.\n",subp_control_modes[k]);
                        }
                    }
                } else {
                    // fprintf(stderr,"WARN: Subpicture is not present for stream %d\n",j);
                    xmlNewProp( subpNode, "present", "no" );
                }                
            }

            for( ; j<32; j++ ) {
                if( pgc->subp_control[j] & 0x80000000 )
                    fprintf(stderr,"WARN: Subpicture is present for stream %d; but there should be only %d\n",j,ab->numsubp);
            }


            if( pgc->command_tbl &&
                pgc->command_tbl->nr_of_pre > 0 )
            {
                xmlNodePtr preNode = xmlNewTextChild( pgcNode, NULL, "pre", NULL );
                vm_add_mnemonics( preNode,
                                  "\n        ",
                                  pgc->command_tbl->nr_of_pre,
                                  pgc->command_tbl->pre_cmds );
            }
            

            for( j=0; j<pgc->nr_of_cells; j++ ) {
                xmlNodePtr cellNode;
                cell_playback_t *cp=&pgc->cell_playback[j];

                if( cp->interleaved ) {
                    switch( cp->block_mode ) {
                    case 0:
                        // The Matrix has interleaved cells that are part of different
                        // titles, instead of using angles
                        // fprintf(stderr,"WARN: Block is interleaved but block mode is normal\n");
                        // fall through and create an interleave tag, just to give a hint to
                        // dvdauthor that it can optimize it

                        // if the previous cell was from the same vob, and it was interleaved
                        // (but not an angle), then don't bother with new interleave/vob tags
                        if( j>0 &&
                            pgc->cell_playback[j-1].interleaved &&
                            pgc->cell_playback[j-1].block_mode == 0 &&
                            curvob == pgc->cell_position[j].vob_id_nr )
                        {
                            break;
                        }
                        angleNode = xmlNewTextChild( pgcNode, NULL, "interleave", NULL );
                        curvob=-1;
                        FindWith(angleNode, pgcs, pgc->cell_position[j].vob_id_nr, cp);
                        break;

                    case 1:
                        angleNode = xmlNewTextChild( pgcNode, NULL, "interleave", NULL );
                        curvob=-1;
                        break;
                    case 2:
                    case 3:
                        break;
                    }
                    if( !cp->block_type && cp->block_mode!=0 )
                        fprintf(stderr,"WARN: Block is interleaved but block type is normal\n");
                } else {
                    if( cp->block_mode )
                        fprintf(stderr,"WARN: Block is not interleaved but block mode is not normal\n");
                    if( cp->block_type )
                        fprintf(stderr,"WARN: Block is not interleaved but block type is not normal\n");
                    if( angleNode != pgcNode ) // force a new vobNode if the previous one was interleaved
                        curvob=-1;
                    angleNode = pgcNode;
                }

                if( curvob != pgc->cell_position[j].vob_id_nr ) {
                    vobNode = xmlNewTextChild( angleNode, NULL, "vob", NULL );
                    curvob = pgc->cell_position[j].vob_id_nr;

                    setfilename(curvob);
                    xmlNewProp( vobNode, "file", filenamebase );

                    dump_buttons( vobNode, curvob );
                }

                cellNode = xmlNewTextChild( vobNode, NULL, "cell", NULL );

                /* add cell time attribute */
                if( printcelltime(pgc->cell_position[j].vob_id_nr,pgc->cell_position[j].cell_nr, buffer, sizeof(buffer)) )
                    xmlNewProp( cellNode, "start", buffer );

                if( printcelltime(pgc->cell_position[j].vob_id_nr,pgc->cell_position[j].cell_nr+1, buffer, sizeof(buffer)) )
                    xmlNewProp( cellNode, "end", buffer );


                switch( getprogramtype( titlef?ifo->vts_ptt_srpt:0,pgc,i+1,j+1) ) {
                case 1:
                    xmlNewProp( cellNode, "chapter", "1" );
                    break;
                case 2:
                    xmlNewProp( cellNode, "program", "1" );
                    break;
                    /* default: 
                       do nothing */
                }

                /* add cell still time attribute */
                if( cp->still_time ) {
                    if( cp->still_time==255 )
                        snprintf(buffer,sizeof(buffer),"inf");
                    else
                        snprintf(buffer,sizeof(buffer),"%d",cp->still_time);
                    xmlNewProp( cellNode, "pause", buffer );
                }

                /* add cell commands */
                if( cp->cell_cmd_nr ) {
                    vm_add_mnemonics(cellNode,
                                     "\n          ",
                                     1,
                                     &pgc->command_tbl->cell_cmds[cp->cell_cmd_nr-1]);
                }
            } /* end for */

            if( pgc->command_tbl &&
                pgc->command_tbl->nr_of_post > 0 )
            {
                xmlNodePtr postNode = xmlNewTextChild( pgcNode, NULL, "post", NULL );
                vm_add_mnemonics( postNode,
                                  "\n        ",
                                  pgc->command_tbl->nr_of_post,
                                  pgc->command_tbl->post_cmds );
            }
        }
    }
}

/* Output attributes for a fpc (first play chain sequence) */
static void dump_fpc(pgc_t *p,xmlNodePtr titlesetNode)
{
    /* add fpc commands */
    if( p && p->command_tbl && p->command_tbl->nr_of_pre ) {
        xmlAddChildList(titlesetNode,xmlNewComment(" First Play "));
        xmlNodePtr fpcNode = xmlNewTextChild( titlesetNode, NULL, "fpc", NULL );
        vm_add_mnemonics( fpcNode,
                          "\n    ",
                          p->command_tbl->nr_of_pre,
                          p->command_tbl->pre_cmds);
    }
}


static void findpalette(int vob,pgcit_t *pgcs,uint32_t **palette,int *length)
{
    if( pgcs ) {
        int i,j,ptime;

        for( i=0; i<pgcs->nr_of_pgci_srp; i++ ) {
            pgc_t *p=pgcs->pgci_srp[i].pgc;

            for( j=0; j<p->nr_of_cells; j++ )
                if( p->cell_position[j].vob_id_nr==vob )
                    break;
            if( j==p->nr_of_cells )
                continue;
            ptime=(p->playback_time.hour<<24)|
                (p->playback_time.hour<<16)|
                (p->playback_time.hour<<8)|
                (p->playback_time.hour);
            if( !(*palette) || ptime > *length ) {
                if( *palette && memcmp(*palette,p->palette,16*sizeof(uint32_t)) )
                    fprintf(stderr,"WARN:  VOB %d has conflicting palettes\n",vob);
                *palette=p->palette;
                *length=ptime;
            }
        }
    }
}

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

static void wdstr(unsigned char *s)
{
    while(*s)
        wdbyte(*s++);
    wdbyte(0);
}

static void writepalette(int h,uint32_t *palette)
{
    static unsigned char sector[2048];
    int i,j;

    if( !palette )
        return;

    for( j=0; j<32; j++ ) {
        sector[0]=0;
        sector[1]=0;
        sector[2]=1;
        sector[3]=0xba;
        sector[4]=0x44;
        sector[5]=0;
        sector[6]=4;
        sector[7]=0;
        sector[8]=4;
        sector[9]=1;
        sector[10]=1;
        sector[11]=137;
        sector[12]=195;
        sector[13]=0xf8;

        sector[14]=0; // padding
        sector[15]=0;
        sector[16]=1;
        sector[17]=0xbe;
        sector[18]=(2048-20)>>8;
        sector[19]=(2048-20)&255;
        memset(sector+20,0xff,2048-20);

        wdest=sector+20;
        wdstr("dvdauthor-data");
        wdbyte(1); // version
        wdbyte(1); // subtitle info
        wdbyte(j); // sub number

        wdlong(0); // start/end pts
        wdlong(0);

        wdbyte(1); // colormap
        wdbyte(16); // number of colors
        for( i=0; i<16; i++ ) {  
            wdbyte(palette[i]>>16);
            wdbyte(palette[i]>>8);
            wdbyte(palette[i]);
        }

        if( write(h,sector,2048) < 2048 ) {
            fprintf(stderr,"ERR:  Error writing data: %s\n",strerror(errno));
            exit(1);
        }
    }
}

static void writebutton(int h,unsigned char *packhdr,hli_t *hli)
{
    static unsigned char sector[2048];
    int i;

    if( !hli->hl_gi.hli_ss )
        return;
    memcpy(sector,packhdr,14); // copy pack header

    sector[14]=0; // padding
    sector[15]=0;
    sector[16]=1;
    sector[17]=0xbe;
    sector[18]=(2048-20)>>8;
    sector[19]=(2048-20)&255;
    memset(sector+20,0xff,2048-20);

    wdest=sector+20;
    wdstr("dvdauthor-data");
    wdbyte(1); // version
    wdbyte(1); // subtitle info
    wdbyte(0); // sub number

    wdlong(hli->hl_gi.hli_s_ptm); // start pts
    wdlong(hli->hl_gi.hli_e_ptm); // end pts

    wdbyte(2); // sl_coli
    wdbyte(3);
    for( i=0; i<6; i++ )
        wdlong(hli->btn_colit.btn_coli[i>>1][i&1]);
    
    wdbyte(3); // btn_it
    wdbyte(hli->hl_gi.btn_ns);
    for( i=0; i<hli->hl_gi.btn_ns; i++ ) {
        btni_t *b=hli->btnit+i;
        char nm1[10];
        
        sprintf(nm1,"%d",i+1);
        wdstr(nm1);
        wdshort(0);
        wdbyte(b->auto_action_mode);
        if( b->auto_action_mode != 0 ) {
            wdbyte(b->btn_coln);
            wdshort(b->x_start);
            wdshort(b->y_start);
            wdshort(b->x_end);
            wdshort(b->y_end);

            sprintf(nm1,"%d",b->up);    wdstr(nm1);
            sprintf(nm1,"%d",b->down);  wdstr(nm1);
            sprintf(nm1,"%d",b->left);  wdstr(nm1);
            sprintf(nm1,"%d",b->right); wdstr(nm1);
        }
    }
    
    if( write(h,sector,2048) < 2048 ) {
        fprintf(stderr,"ERR:  Error writing data: %s\n",strerror(errno));
        exit(1);
    }
}


static void getVobs( dvd_reader_t *dvd, ifo_handle_t *ifo, int titleset, int titlef )
{
    dvd_file_t *vobs;
    c_adt_t *cptr;
    cell_adr_t *cells;
    int numcells,i,j,totalsect,numsect;
    clock_t start,now,clkpsec;

    cptr=titlef?ifo->vts_c_adt:ifo->menu_c_adt;
    if( cptr ) {
        cells=cptr->cell_adr_table;
        numcells=(cptr->last_byte+1-C_ADT_SIZE)/sizeof(cell_adr_t);
    } else {
        cells=0;
        numcells=0;
    }
    numcst=0;

    vobs=DVDOpenFile(dvd,titleset,titlef?DVD_READ_TITLE_VOBS:DVD_READ_MENU_VOBS);

    numsect=0;
    totalsect=0;
    for( i=0; i<numcells; i++ )
        totalsect += cells[i].last_sector - cells[i].start_sector + 1;
    clkpsec=sysconf(_SC_CLK_TCK);
    start=times(NULL);
    
    for( i=0; i<numcells; i++ ) {
        int h,b,plen;
        uint32_t *palette=0;

        if( titlef ) {
            findpalette(cells[i].vob_id,ifo->vts_pgcit,&palette,&plen);
        } else {
            if( ifo->pgci_ut ) {
                for( j=0; j<ifo->pgci_ut->nr_of_lus; j++ ) {
                    pgci_lu_t *lu=&ifo->pgci_ut->lu[j];
                    findpalette(cells[i].vob_id,lu->pgcit,&palette,&plen);
                }
            }
        }

        setfilename(cells[i].vob_id);
        if( vobexists(cells,i,cells[i].vob_id) )
            h=open(filenamebase,O_CREAT|O_APPEND|O_WRONLY|O_BINARY,0666);
        else {
            h=open(filenamebase,O_CREAT|O_TRUNC|O_WRONLY|O_BINARY,0666);
            writepalette(h,palette);
        }

        if( i==0 ||
            cells[i].vob_id != cells[i-1].vob_id )
        {
            memset(&curhli,0,sizeof(curhli));
        }

        if( h<0 ) {
            fprintf(stderr,"ERR:  Cannot open %s for writing\n",filenamebase);
            exit(1);
        }
        for( b=cells[i].start_sector; b<=cells[i].last_sector; b+=BIGBLOCKSECT ) {
            int rl=cells[i].last_sector+1-b;
            if( rl > BIGBLOCKSECT ) rl = BIGBLOCKSECT;
            now=times(NULL);
            if( now-start>3*clkpsec && numsect>0 ) {
                int rmn=(totalsect-numsect)*(now-start)/(numsect*clkpsec);
                fprintf(stderr,"STAT: [%d] VOB %d, Cell %d (%d%%, %d:%02d remain)\r",i,cells[i].vob_id,cells[i].cell_id,(numsect*100+totalsect/2)/totalsect,rmn/60,rmn%60);
            } else
                fprintf(stderr,"STAT: [%d] VOB %d, Cell %d (%d%%)\r",i,cells[i].vob_id,cells[i].cell_id,(numsect*100+totalsect/2)/totalsect);
            if( DVDReadBlocks(vobs,b,rl,bigblock) < rl ) {
                fprintf(stderr,"\nERR:  Error reading data: %s\n",strerror(errno));
                break;
            }
            numsect+=rl;
            for( j=0; j<rl; j++ ) {
                if( bigblock[j*DVD_VIDEO_LB_LEN+14] == 0 &&
                    bigblock[j*DVD_VIDEO_LB_LEN+15] == 0 &&
                    bigblock[j*DVD_VIDEO_LB_LEN+16] == 1 &&
                    bigblock[j*DVD_VIDEO_LB_LEN+17] == 0xbb && // system header
                    bigblock[j*DVD_VIDEO_LB_LEN+38] == 0 &&
                    bigblock[j*DVD_VIDEO_LB_LEN+39] == 0 &&
                    bigblock[j*DVD_VIDEO_LB_LEN+40] == 1 &&
                    bigblock[j*DVD_VIDEO_LB_LEN+41] == 0xbf && // 1st private2
                    bigblock[j*DVD_VIDEO_LB_LEN+1024] == 0 &&
                    bigblock[j*DVD_VIDEO_LB_LEN+1025] == 0 &&
                    bigblock[j*DVD_VIDEO_LB_LEN+1026] == 1 &&
                    bigblock[j*DVD_VIDEO_LB_LEN+1027] == 0xbf ) // 2nd private2
                {
                    pci_t p;
                    //dsi_t d;
                    
                    navRead_PCI(&p,bigblock+j*DVD_VIDEO_LB_LEN+0x2d);
                    //navRead_DSI(&d,bigblock+j*DVD_VIDEO_LB_LEN+0x407);

                    addcst(cells[i].vob_id,cells[i].cell_id,p.pci_gi.vobu_s_ptm);

                    if( p.hli.hl_gi.hli_ss ) {
                        if( !palette ) {
                            fprintf(stderr,"\nWARN: How can there be buttons but no palette?\n");
                        }
                        if( p.hli.hl_gi.hli_ss >= 2 && !curhli.hl_gi.hli_ss ) {
                            fprintf(stderr,"\nWARN: Button information carries over from previous VOBU, but there is no\nWARN: record of previous button information.\n");
                        }
                        if( p.hli.hl_gi.hli_s_ptm < curhli.hl_gi.hli_e_ptm ) {
                            
                            switch( p.hli.hl_gi.hli_ss ) {
                            case 1:
                                if( memcmp(&p.hli,&curhli,sizeof(curhli)) ) {
                                    // fprintf(stderr,"\nWARN: Button information changes!\n");
                                    // we detect overlapping button ptm in dump_buttons

                                    memcpy(&curhli,&p.hli,sizeof(curhli));
                                    hli_pci = p;
                                    addbutton(cells[i].vob_id,cells[i].cell_id,&curhli);
                                    writebutton(h,bigblock+j*DVD_VIDEO_LB_LEN,&curhli);
                                }
                                break;

                            case 3:
                                if( memcmp(&p.hli.btnit,&curhli.btnit,sizeof(curhli.btnit)) ) {
                                    fprintf(stderr,"\nWARN: Button commands changes!\n");
                                }
                                break;
                            }
                        } else {
                            memcpy(&curhli,&p.hli,sizeof(curhli));
                            hli_pci = p;
                            addbutton(cells[i].vob_id,cells[i].cell_id,&curhli);
                            writebutton(h,bigblock+j*DVD_VIDEO_LB_LEN,&curhli);
                        }
                    }
                }
                if( write(h,bigblock+j*DVD_VIDEO_LB_LEN,DVD_VIDEO_LB_LEN) < DVD_VIDEO_LB_LEN ) {
                    fprintf(stderr,"\nERR:  Error writing data: %s\n",strerror(errno));
                    exit(1);
                }
            }
        }
        close(h);
    }
}

static void dump_dvd(dvd_reader_t *dvd,int titleset,int titlef, xmlNodePtr titlesetNode )
{
    ifo_handle_t *ifo;
 
    if( titleset<0 || titleset>99 ) {
        fprintf(stderr,"ERR:  Can only handle titlesets 0..99\n");
        exit(1);
    }
    if( titlef<0 || titlef>1 ) {
        fprintf(stderr,"ERR:  Title flag must be 0 (menu) or 1 (title)\n");
        exit(1);
    }
    if (titlef==1 && titleset==0) {
        fprintf(stderr,"ERR:  No title for VMGM\n");
        exit(1);
    }
    ifo=ifoOpen(dvd,titleset);
    if( !titleset )
        numtitlesets=ifo->vmgi_mat->vmg_nr_of_title_sets;

    if( numcst ) {
        free(cellstarttimes);
        cellstarttimes=0;
        numcst=0;
    }

    if( numvb ) {
        free(vobbuttons);
        vobbuttons=0;
        numvb=0;
    }

    snprintf( filenamebase, sizeof(filenamebase), "vob_%02d%c_",
              titleset,
              titlef?'t':'m');

    getVobs( dvd, ifo, titleset, titlef );

    if( titlef ) {
        xmlNodePtr titleNode = xmlNewTextChild( titlesetNode, NULL, "titles", NULL );
        struct attrblock ab;

        get_attr(ifo,titlef,&ab);
        dump_attr( &ab, titleNode );
        dump_pgcs(ifo,ifo->vts_pgcit,&ab,titleset,titlef, titleNode );
    } else {
        if( titleset==0 ) {
            dump_fpc(ifo->first_play_pgc,titlesetNode);
            if( ifo->tt_srpt ) {
                int i;

                for( i=0; i<ifo->tt_srpt->nr_of_srpts; i++ ) {
                    char buffer[100];
                    xmlNodePtr titleMapNode = xmlNewTextChild( titlesetNode, NULL, "titlemap", NULL );
                    
                    snprintf(buffer,sizeof(buffer),"%d",ifo->tt_srpt->title[i].title_set_nr);
                    xmlNewProp(titleMapNode, "titleset", buffer);

                    snprintf(buffer,sizeof(buffer),"%d",ifo->tt_srpt->title[i].vts_ttn);
                    xmlNewProp(titleMapNode, "title", buffer);
                }
            }
        }
        if( ifo->pgci_ut ) {
            int i;

            for( i=0; i<ifo->pgci_ut->nr_of_lus; i++ ) {
                pgci_lu_t *lu=&ifo->pgci_ut->lu[i];
                struct attrblock ab;
                xmlNodePtr menusNode = xmlNewTextChild( titlesetNode, NULL, "menus", NULL );
                addLangAttr( menusNode, lu->lang_code );
            
                get_attr(ifo, titleset==0?-1:0, &ab );
                dump_attr( &ab, menusNode );
                dump_pgcs(ifo,lu->pgcit,&ab,titleset,titlef, menusNode);
            }
        }
    }

    ifoClose(ifo);
}

static void usage(void)
{
    fprintf(stderr,"syntax: dvdunauthor pathname\n"
            "\n"
            "\tpathname can either be a DVDROM device name, an ISO image, or a path to\n"
            "\ta directory with the appropriate files in it.\n"
        );
    exit(1);
}

int main(int argc,char **argv)
{
    xmlDocPtr  myXmlDoc;
    xmlNodePtr mainNode;
    xmlNodePtr titlesetNode;

    dvd_reader_t *dvd;
    int i;
    char *devname=0;

    fputs(PACKAGE_HEADER("dvdunauthor"),stderr);

    while( -1 != (i=getopt(argc,argv,"h")) ) {
        switch(i) {
        case 'h':
        default:
            usage();
        }
    }

    if( optind+1 != argc ) {
        usage();
    }
    devname=argv[optind];

    dvd=DVDOpen(devname);
    if(!dvd) {
        fprintf(stderr,"ERR:  Cannot open path '%s'\n",argv[1]);
        return 1;
    }


    myXmlDoc = xmlNewDoc( "1.0" );
    mainNode = xmlNewDocNode( myXmlDoc, NULL, "dvdauthor", NULL );
    xmlDocSetRootElement(myXmlDoc, mainNode);
    xmlNewProp( mainNode, "allgprm", "yes");
      
    for( i=0; i<=numtitlesets; i++ ) {
        if( i ) {
            char buffer[100];

            fprintf(stderr,"\n\nINFO: VTSM %d/%d\n",i,numtitlesets);
            snprintf(buffer,sizeof(buffer)," Titleset %d/%d ", i, numtitlesets);
            xmlAddChildList(mainNode,xmlNewComment(buffer));
            titlesetNode = xmlNewTextChild( mainNode, NULL, "titleset", NULL );
        } else {
            fprintf(stderr,"\n\nINFO: VMGM\n");
            titlesetNode = xmlNewTextChild( mainNode, NULL, "vmgm", NULL );
        }

        dump_dvd(dvd,i,0, titlesetNode );
        if( i ) {
            fprintf(stderr,"\n\nINFO: VTS %d/%d\n",i,numtitlesets);
            dump_dvd(dvd,i,1, titlesetNode );
        }
    }
     
    xmlSaveFormatFile( "dvdauthor.xml", myXmlDoc, 1 );
    xmlFreeDoc( myXmlDoc );

    DVDClose(dvd);
    fprintf(stderr,"\n\n");
    return 0;
}
