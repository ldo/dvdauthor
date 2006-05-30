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

#include "dvdauthor.h"
#include "da-internal.h"

static const char RCSID[]="$Id: //depot/dvdauthor/src/dvdpgc.c#20 $";


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

static int jumppgc(unsigned char *buf,int pgc,struct workset *ws)
{
    int base=0xEC,ncmd,offs,i,j,k;
    offs=base+8;
    
    if( ws && ws->ts && ws->menus ) {
        // *** VMGM jumppad
        // remap all VMGM TITLE X -> TITLESET X TITLE Y
        k=129;
        for( i=0; i<ws->ts->numvts; i++ )
            for( j=0; j<ws->ts->vts[i].numtitles; j++ ) {
                write8(buf+offs,0x71,0xA0,0x0F,0x0F,j+129,i+2,k,1); 
                offs+=8;
                k++;
            }
        // move TITLE out of g[15] into g[14] (to mate up with CHAPTER)
        // then put title/chapter into g[15], and leave titleset in g[14]
        write8(buf+offs,0x63,0x00,0x00,0x0E,0x00,0x0F,0x00,0x00); offs+=8; // g[14]+=g[15]
        write8(buf+offs,0x79,0x00,0x00,0x0F,0x00,0xFF,0x00,0x00); offs+=8; // g[15]&=255
        write8(buf+offs,0x64,0x00,0x00,0x0E,0x00,0x0F,0x00,0x00); offs+=8; // g[14]-=g[15]
        write8(buf+offs,0x62,0x00,0x00,0x0E,0x00,0x0F,0x00,0x00); offs+=8; // g[14]<->g[15]
        // For each titleset, delegate to the appropriate submenu
        for( i=0; i<ws->ts->numvts; i++ ) {
            write8(buf+offs,0x71,0x00,0x00,0x0D,0x00,i+2,0x00,0x00); offs+=8; // g[13]=(i+1)*256;
            write8(buf+offs,0x30,0x26,0x00,0x01,i+1,0x87,0x0E,0x0D); offs+=8; // if g[14]==g[13] then JumpSS VTSM i+1, ROOT
        }
        // set g[15]=0 so we don't leak dirty registers to other PGC's
        write8(buf+offs,0x71,0x00,0x00,0x0F,0x00,0x00,0x00,0x00); offs+=8; // g[15]=0;
    } else if( ws && ws->menus && ws->titles ) {
        struct pgcgroup *mpg=ws->menus->groups[ws->curmenu].pg;

        // *** VTSM jumppad
        write8(buf+offs,0x61,0x00,0x00,0x0E,0x00,0x0F,0x00,0x00); offs+=8; // g[14]=g[15];
        write8(buf+offs,0x71,0x00,0x00,0x0F,0x00,0x00,0x00,0x00); offs+=8; // g[15]=0;
        // menu entry jumptable
        for( i=2; i<8; i++ ) {
            for( j=0; j<mpg->numpgcs; j++ )
                if( mpg->pgcs[j]->entries&(1<<i) ) {
                    write8(buf+offs,0x20,0xA4,0x00,0x0E,i+120,0x00,0x00,j+1); offs+=8; // if g[14]==0xXX00 then LinkPGCN XX
                }
        }
        // menu jumptable
        for( i=0; i<mpg->numpgcs; i++ ) {
            write8(buf+offs,0x20,0xA4,0x00,0x0E,i+1,0x00,0x00,i+1); offs+=8; // if g[14]==0xXX00 then LinkPGCN XX
        }
        // title/chapter jumptable
        for( i=0; i<ws->titles->numpgcs; i++ ) {
            write8(buf+offs,0x71,0x00,0x00,0x0D,i+129,0,0x00,0x00); offs+=8; // g[13]=(i+1)*256;
            write8(buf+offs,0x30,0x23,0x00,0x00,0x00,i+1,0x0E,0x0D); offs+=8; // if g[15]==g[13] then JumpSS VTSM i+1, ROOT
            for( j=0; j<ws->titles->pgcs[i]->numchapters; j++ ) {
                write8(buf+offs,0x71,0x00,0x00,0x0D,i+129,j+1,0x00,0x00); offs+=8; // g[13]=(i+1)*256;
                write8(buf+offs,0x30,0x25,0x00,j+1,0x00,i+1,0x0E,0x0D); offs+=8; // if g[15]==g[13] then JumpSS VTSM i+1, ROOT
            }
        }
    }
    
    if( pgc>0 )
        write8(buf+offs,0x20,0x04,0x00,0x00,0x00,0x00,0x00,pgc); // LinkPGCN pgc
    else
        write8(buf+offs,0x30,0x06,0x00,0x00,0x00,0x00,0x00,0x00); // JumpSS FP
    offs+=8;

    ncmd=(offs-base)/8-1;
    if( ncmd>128 ) {
        fprintf(stderr,"ERR:  Too many pre/post/cell commands.  Reduce complexity and/or disable\njumppad.\n");
        exit(1);
    }
    buf[0xE5]=base;
    buf[base+1]=ncmd;
    buf[base+7]=7+8*ncmd;
    return offs;
}

static int genpgc(unsigned char *buf,struct workset *ws,int pgc,int ismenu)
{
    struct vobgroup *va=(ismenu?ws->menus->vg:ws->titles->vg);
    struct pgc *p=(ismenu?ws->menus->groups[ws->curmenu].pg:ws->titles)->pgcs[pgc];
    int i,j,d;

    // PGC header starts at buf[16]
    buf[2]=p->numprograms;
    buf[3]=p->numcells;
    write4(buf+4,buildtimeeven(va,getptsspan(p)));
    for( i=0; i<va->numaudiotracks; i++ ) {
        if( va->ad[i].aid )
            buf[12+i*2]=0x80|(va->ad[i].aid-1);
    }
    for( i=0; i<va->numsubpicturetracks; i++ ) {
        int id=va->sp[i].sid-1;
        buf[28+i*4]=0x80|id;
        buf[29+i*4]=id;
        buf[30+i*4]=id;
        buf[31+i*4]=id;
    }
    buf[163]=p->pauselen; // PGC stilltime
    for( i=0; i<16; i++ )
        write4(buf+164+i*4,p->ci->colors[i]<0x1000000?p->ci->colors[i]:0x108080);

    d=0xEC;

    // command table
    {
        unsigned char *cd=buf+d+8,*preptr,*postptr,*cellptr;

        preptr=cd;
        if( p->prei ) {
            cd=vm_compile(preptr,cd,ws,pgc,p->prei,ismenu,COMPILE_PRE);
            if(!cd) {
                fprintf(stderr,"ERR:  in %s pgc %d, <pre>\n",pstypes[ismenu],pgc);
                exit(1);
            }
        } else if( p->numbuttons ) {
            write8(cd,0x56,0x00,0x00,0x00,4*1,0x00,0x00,0x00); cd+=8; // set active button to be #1
        }

        postptr=cd;
        if( p->numbuttons ) {
            write8(cd,0x61,0x00,0x00,0x0E,0x00,0x0F,0x00,0x00);  // g[14]=g[15]
            write8(cd+8,0x71,0x00,0x00,0x0F,0x00,0x00,0x00,0x00); // g[15]=0
            cd+=8*(p->numbuttons+2);
        }
        if( p->posti ) {
            cd=vm_compile(postptr,cd,ws,pgc,p->posti,ismenu,COMPILE_POST);
            if(!cd) {
                fprintf(stderr,"ERR:  in %s pgc %d, <post>\n",pstypes[ismenu],pgc);
                exit(1);
            }
        } else {
            write8(cd,0x30,0x01,0x00,0x00,0x00,0x00,0x00,0x00); // exit
            cd+=8;
        }
        for( i=0; i<p->numbuttons; i++ ) {
            struct button *b=&p->buttons[i];
            
            write8(postptr+i*8+16,0x00,0xa1,0x00,0x0E,0x00,i+1,0x00,(cd-postptr)/8+1);
            cd=vm_compile(postptr,cd,ws,pgc,b->cs,ismenu,COMPILE_POST);
            if(!cd) {
                fprintf(stderr,"ERR:  in %s pgc %d, button %s\n",pstypes[ismenu],pgc,b->name);
                exit(1);
            }
        }

        cellptr=cd;
        for( i=0; i<p->numsources; i++ )
            for( j=0; j<p->sources[i]->numcells; j++ ) {
                struct cell *c=&p->sources[i]->cells[j];
                if( c->cs ) {
                    unsigned char *cdd=vm_compile(postptr,cd,ws,pgc,c->cs,ismenu,COMPILE_CELL);
                    if( cdd!=cd+8 ) {
                        fprintf(stderr,"ERR:  Cell command can only compile to one VM instruction.\n");
                        exit(1);
                    }
                    cd=cdd;
                }
            }
        
        write2(buf+228,d);
        assert(cd-(buf+d)-8<=128*8); // can only have 128 commands
        write2(buf+d,(postptr-preptr)/8); // # pre commands
        write2(buf+d+2,(cellptr-postptr)/8); // # post command
        write2(buf+d+4,(cd-cellptr)/8); // # cell command
        write2(buf+d+6,cd-(buf+d)-1); // last byte
        d=cd-buf;
    }

    if( p->numsources ) {
        int cellline,notseamless;

        // program map entry
        write2(buf+230,d);
        j=1;
        for( i=0; i<p->numsources; i++ ) {
            int k;
            struct source *si=p->sources[i];

            for( k=0; k<si->numcells; k++ ) {
                if( si->cells[k].scellid==si->cells[k].ecellid )
                    continue;
                if( si->cells[k].ischapter )
                    buf[d++]=j;
                j+=si->cells[k].ecellid-si->cells[k].scellid;
            }
        }
        d+=d&1;

        // cell playback information table
        write2(buf+232,d);
        j=-1;
        cellline=1;
        notseamless=1;
        for( i=0; i<p->numsources; i++ ) {
            int k,l,m,firsttime;
            struct source *s=p->sources[i];

            for( k=0; k<s->numcells; k++ )
                for( l=s->cells[k].scellid; l<s->cells[k].ecellid; l++ ) {
                    int id=s->vob->vobid*256+l,vi;
                    for( m=0; m<va->numaudiotracks; m++ )
                        if( j>=0 && getaudch(va,m)>=0 && calcaudiogap(va,j,id,getaudch(va,m)) )
                            notseamless=1;
                    buf[d]=(notseamless?0:8)|(j+1!=id?2:0); // if this is the first cell of the source, then set 'STC_discontinuity', otherwise set 'seamless presentation'
                    notseamless=0;
                    // you can't set the seamless presentation on the first cell or else a/v sync problems will occur
                    j=id;

                    vi=findcellvobu(s->vob,l);
                    firsttime=s->vob->vi[vi].sectpts[0];
                    write4(buf+8+d,s->vob->vi[vi].sector);
                    vi=findcellvobu(s->vob,l+1)-1;
                    if( l==s->cells[k].ecellid-1 ) {
                        if( s->cells[k].pauselen>0 ) {
                            buf[2+d]=s->cells[k].pauselen; // still time
                            notseamless=1; // cells with stilltime are not seamless
                        }
                        if( s->cells[k].cs ) {
                            buf[3+d]=cellline++;
                            notseamless=1; // cells with commands are not seamless
                        }
                    }
                    write4(buf+4+d,buildtimeeven(va,s->vob->vi[vi].sectpts[1]-firsttime));
                    write4(buf+16+d,s->vob->vi[vi].sector);
                    write4(buf+20+d,s->vob->vi[vi].lastsector);
                    d+=24;
                }
        }

        // cell position information table
        write2(buf+234,d);
        for( i=0; i<p->numsources; i++ ) {
            int j,k;
            struct source *s=p->sources[i];

            for( j=0; j<s->numcells; j++ )
                for( k=s->cells[j].scellid; k<s->cells[j].ecellid; k++ ) {
                    buf[1+d]=s->vob->vobid;
                    buf[3+d]=k;
                    d+=4;
                }
        }
    }

    return d;
}

static int createpgcgroup(struct workset *ws,int ismenu,struct pgcgroup *va,unsigned char *buf)
{
    int len,i,pgcidx;

    len=va->numpgcs+va->numentries;
    write2(buf,len);
    len=len*8+8;
    pgcidx=8;

    for( i=0; i<va->numpgcs; i++ ) {
        if( buf+len+BUFFERPAD-bigwritebuf > bigwritebuflen )
            return -1;
        if( !ismenu )
            buf[pgcidx]=0x81+i;
        write4(buf+pgcidx+4,len);
        len+=genpgc(buf+len,ws,i,ismenu);
        pgcidx+=8;
    }

    for( i=2; i<8; i++ ) {
        if( va->allentries&(1<<i) ) {
            int j;

            if( buf+len+BUFFERPAD-bigwritebuf > bigwritebuflen )
                return -1;

            for( j=0; j<va->numpgcs; j++ )
                if( va->pgcs[j]->entries&(1<<i) )
                    break;
            if( j<va->numpgcs )
                j++;
            else
                j=-1;
            buf[pgcidx]=0x80|i;
            write4(buf+pgcidx+4,len);
            if( jumppad && ((ismenu==1 && i==7) || 
                            (ismenu==2 && i==2) ))
                len+=jumppgc(buf+len,j,ws);
            else
                len+=jumppgc(buf+len,j,0);
            pgcidx+=8;
        }
    }

    write4(buf+4,len-1); // last byte rel to buf
    return len;
}

int CreatePGC(FILE *h,struct workset *ws,int ismenu)
{
    unsigned char *buf;
    int len,ph,i,in_it;

    in_it=!bigwritebuflen;
 retry:
    if( in_it ) {
        if( bigwritebuflen==0 )
            bigwritebuflen=128*1024;
        else
            bigwritebuflen<<=1;
        if( bigwritebuf )
            free(bigwritebuf);
        bigwritebuf=malloc(bigwritebuflen);
    }
    in_it=1;
    buf=bigwritebuf;
    memset(buf,0,bigwritebuflen);
    ph=0;
    if( ismenu ) {
        buf[1]=ws->menus->numgroups; // # of language units
        ph=8+8*ws->menus->numgroups;

        for( i=0; i<ws->menus->numgroups; i++ ) {
            unsigned char *plu=buf+8+8*i;
            struct langgroup *lg=&ws->menus->groups[i];

            memcpy(plu,lg->lang,2);
            if( ismenu==1 )
                plu[3]=lg->pg->allentries;
            else
                plu[3]=0x80; // menu system contains entry for title
            write4(plu+4,ph);

            ws->curmenu=i;
            len=createpgcgroup(ws,ismenu,lg->pg,buf+ph);
            if( len<0 )
                goto retry;
            ph+=len;            
        }
        write4(buf+4,ph-1);
    } else {
        len=createpgcgroup(ws,0,ws->titles,buf);
        if( len<0 )
            goto retry;
        ph=len;
    }

    assert(ph<=bigwritebuflen);
    ph=(ph+2047)&(-2048);
    if( h )
        fwrite(buf,1,ph,h);
    return ph/2048;
}
