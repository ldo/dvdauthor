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

static const char RCSID[]="$Id: //depot/dvdauthor/src/dvdifo.c#19 $";

#define BIGWRITEBUFLEN (16*2048)
static unsigned char bigwritebuf[BIGWRITEBUFLEN];

static struct vobuinfo *globalfindvobu(struct pgc *ch,int pts)
{
    int s,c,ci;

    for( s=0; s<ch->numsources; s++ ) {
        struct source *sc=ch->sources[s];
        for( c=0; c<sc->numcells; c++ ) {
            struct cell *cl=&sc->cells[c];
            int span=0;
            int fv=findcellvobu(sc->vob,cl->scellid);

            if( pts < 0 )
                return &sc->vob->vi[fv];

            for( ci=cl->scellid; ci<cl->ecellid; ci++ )
                span+=getcellpts(sc->vob,ci);
            if( pts<span ) {
                int r=findvobu(sc->vob,pts+sc->vob->vi[fv].sectpts[0],
                               fv,sc->vob->numvobus-1);
                return &sc->vob->vi[r];
            }
            pts-=span;
        }
    }
    // return last vob
    // if( ch->numsources ) {
    // struct vob *s=ch->sources[ch->numsources-1];
    // return &s->vi[s->numvobus-1];
    // }
    return 0;
}

static int getvoblen(struct vobgroup *va)
{
    int i;

    for( i=va->numvobs-1; i>=0; i-- )
        if( va->vobs[i]->numvobus )
            return va->vobs[i]->vi[va->vobs[i]->numvobus-1].lastsector+1;
    return 0;
}

static unsigned int getptssec(struct vobgroup *va,int nsec)
{
    return nsec*getratedenom(va);
}

static unsigned int findptssec(struct vobgroup *va,int pts)
{
    return pts/getratedenom(va);
}

static int numsec(struct pgcgroup *va,int c)
{
    return findptssec(va->vg,getptsspan(va->pgcs[c]));
}

static int secunit(int ns)
{
    const int maxunits=2040;

    if(!ns) return 1;
    return (ns+maxunits-1)/maxunits;
}

static int tmapt_block_size(struct pgcgroup *va,int pgc)
{
    int v=numsec(va,pgc);
    v=v/secunit(v);
    return v*4+4;
}

static int sizeTMAPT(struct pgcgroup *va)
{
    int s=0,i;
    for( i=0; i<va->numpgcs; i++ )
        s+=tmapt_block_size(va,i);
    return s+va->numpgcs*4+8;
}

static int numsectTMAPT(struct pgcgroup *va)
{
    return (sizeTMAPT(va)+2047)/2048;
}

static void CreateTMAPT(FILE *h,struct pgcgroup *va)
{
    int i,mapblock;
    unsigned char buf[8];

    write2(buf,va->numpgcs);
    write2(buf+2,0);
    write4(buf+4,sizeTMAPT(va)-1);
    fwrite(buf,1,8,h);

    mapblock=8+4*va->numpgcs;
    for( i=0; i<va->numpgcs; i++ ) {
        write4(buf,mapblock);
        fwrite(buf,1,4,h);
        mapblock+=tmapt_block_size(va,i);
    }

    for( i=0; i<va->numpgcs; i++ ) {
        int numtmapt=numsec(va,i), ptsbase, j, lastsrc=-1;
        int units=secunit(numtmapt);
        struct pgc *p=va->pgcs[i];

        numtmapt/=units;
        buf[0]=units;
        buf[1]=0;
        if( numtmapt>0 ) {
            write2(buf+2,numtmapt);
            ptsbase=-getframepts(va->vg);
            for( j=0; j<numtmapt; j++ ) {
                struct vobuinfo *vobu=globalfindvobu(p,ptsbase+getptssec(va->vg,(j+1)*units));
                if( vobu->vobcellid!=lastsrc ) {
                    if( lastsrc!=-1 )
                        buf[0]|=0x80;
                    lastsrc=vobu->vobcellid;
                }
                fwrite(buf,1,4,h);
                write4(buf,vobu->sector);
            }
        } else
            write2(buf+2,0);
        fwrite(buf,1,4,h);
    }

    i=(-sizeTMAPT(va))&2047;
    if( i ) {
        memset(buf,0,8);
        while(i>=8) {
            fwrite(buf,1,8,h);
            i-=8;
        }
        if( i )
            fwrite(buf,1,i,h);
    }
}

static int numsectVOBUAD(struct vobgroup *va)
{
    int nv=0, i;

    for( i=0; i<va->numvobs; i++ )
        nv+=va->vobs[i]->numvobus;

    return (4+nv*4+2047)/2048;
}

static int CreateCallAdr(FILE *h,struct vobgroup *va)
{
    unsigned char *buf=bigwritebuf;
    int i,p,k;

    memset(buf,0,BIGWRITEBUFLEN);
    p=8;
    for( k=0; k<va->numvobs; k++ ) {
        struct vob *c=va->vobs[k];
        for( i=0; i<c->numvobus; i++ ) {
            if( !i || c->vi[i].vobcellid!=c->vi[i-1].vobcellid ) {
                if( i ) {
                    write4(buf+p+8,c->vi[i-1].lastsector);
                    p+=12;
                }
                write2(buf+p,c->vi[i].vobcellid>>8);
                buf[p+2]=c->vi[i].vobcellid;
                write4(buf+p+4,c->vi[i].sector);
            }
        }
        write4(buf+p+8,c->vi[i-1].lastsector);
        p+=12;
    }
    write4(buf+4,p-1);
    write2(buf,(p-8)/12);
    assert(p<=BIGWRITEBUFLEN);
    p=(p+2047)&(-2048);
    if(h)
        fwrite(buf,1,p,h);
    return p/2048;
}

static void CreateVOBUAD(FILE *h,struct vobgroup *va)
{
    int i,j,nv;
    unsigned char buf[16];

    nv=0;
    for( i=0; i<va->numvobs; i++ )
        nv+=va->vobs[i]->numvobus;

    write4(buf,nv*4+3);
    fwrite(buf,1,4,h);
    for( j=0; j<va->numvobs; j++ ) {
        struct vob *p=va->vobs[j];
        for( i=0; i<p->numvobus; i++ ) {
            write4(buf,p->vi[i].sector);
            fwrite(buf,1,4,h);
        }
    }
    i=(-(4+nv*4))&2047;
    if( i ) {
        memset(buf,0,16);
        while(i>=16) {
            fwrite(buf,1,16,h);
            i-=16;
        }
        if( i )
            fwrite(buf,1,i,h);
    }
}

static int Create_PTT_SRPT(FILE *h,struct pgcgroup *t)
{
    unsigned char *buf=bigwritebuf;
    int i,j,p;

    memset(buf,0,BIGWRITEBUFLEN);
    write2(buf,t->numpgcs); // # of titles
    p=8+t->numpgcs*4;
    assert(p<=2048); // need to make sure all the pgc pointers fit in the first sector because of dvdauthor.c:ScanIfo
    for( j=0; j<t->numpgcs; j++ ) {
        struct pgc *pgc=t->pgcs[j];
        int pgm=1,k;

        write4(buf+8+j*4,p);
        for( i=0; i<pgc->numsources; i++ )
            for( k=0; k<pgc->sources[i]->numcells; k++ ) {
                struct cell *c=&pgc->sources[i]->cells[k];
                if( c->scellid!=c->ecellid )
                    switch(c->ischapter ) {
                    case 1:
                        buf[1+p]=j+1;
                        buf[3+p]=pgm;
                        p+=4;
                    case 2:
                        pgm++;
                    }
            }
        
    }
    write4(buf+4,p-1);
    assert(p<=BIGWRITEBUFLEN);
    p=(p+2047)&(-2048);
    if(h)
        fwrite(buf,1,p,h);
    return p/2048;    
}

static int Create_TT_SRPT(FILE *h,struct toc_summary *ts,int vtsstart)
{
    unsigned char *buf=bigwritebuf;
    int i,j,k,p,tn;

    memset(buf,0,BIGWRITEBUFLEN);

    j=vtsstart;
    tn=0;
    p=8;
    for( i=0; i<ts->numvts; i++ ) {
        for( k=0; k<ts->vts[i].numtitles; k++ ) {
            buf[0 + p]=0x3c; // title type
            buf[1 + p]=0x1; // number of angles
            write2(buf+2+p,ts->vts[i].numchapters[k]); // number of chapters
            buf[6 + p]=i+1; // VTS #
            buf[7 + p]=k+1; // title # within VTS
            write4(buf+8+p,j); // start sector for VTS
            tn++;
            p+=12;
        }
        j+=ts->vts[i].numsectors;
    }
    write2(buf,tn); // # of titles
    write4(buf+4,p-1); // last byte of entry

    assert(p<=BIGWRITEBUFLEN);
    p=(p+2047)&(-2048);
    if(h)
        fwrite(buf,1,p,h);
    return p/2048;        
}

static void BuildAVInfo(unsigned char *buf,struct vobgroup *va)
{
    int i;

    write2(buf,
           (va->vd.vmpeg==2?0x4000:0)
           |(va->vd.vdisallow<<8)
           |(va->vd.vformat==VF_PAL?0x1000:0)
           |(va->vd.vaspect==VA_16x9?0xc00:0)
           |((va->vd.vcaption&1)?0x80:0)
           |((va->vd.vcaption&2)?0x40:0)
           |((va->vd.vres-1)<<2));
    buf[3]=va->numaudiotracks;
    for( i=0; i<va->numaudiotracks; i++ ) {
        buf[4+i*8]=(va->ad[i].aformat-1)<<6;
        if( va->ad[i].alangp==AL_LANG ) {
            buf[4+i*8]|=4;
            memcpy(buf+6+i*8,va->ad[i].lang,2);
        }
        if( va->ad[i].adolby==AD_SURROUND ) {
            buf[4+i*8]|=2;
            buf[11+i*8]=8;
        }

        buf[5+i*8]=
            ((va->ad[i].aquant-1)<<6)  |
            ((va->ad[i].asample-1)<<4) |
            (va->ad[i].achannels-1);
    }
    buf[0x55]=va->numsubpicturetracks;
    for( i=0; i<va->numsubpicturetracks; i++ ) {
        if( va->sp[i].slangp==AL_LANG ) {
            buf[0x56+i*6]=1;
            memcpy(buf+0x58+i*6,va->sp[i].lang,2);
        }
    }
}

static int needmenus(struct menugroup *mg)
{
    if (!mg ) return 0;
    if( !mg->numgroups ) return 0;
    if( !mg->groups[0].pg->numpgcs ) return 0;
    return 1;
}

static void WriteIFO(FILE *h,struct workset *ws)
{
    static unsigned char buf[2048];
    int i,forcemenus=needmenus(ws->menus);

    // sect 0: VTS toplevel
    memset(buf,0,2048);
    memcpy(buf,"DVDVIDEO-VTS",12);
    buf[33]=0x11;
    write4(buf+128,0x7ff);
    i=1;

    buf[0xCB]=i; // VTS_PTT_SRPT
    i+=Create_PTT_SRPT(0,ws->titles);

    buf[0xCF]=i; // VTS_PGCI
    i+=CreatePGC(0,ws,0);

    if( jumppad || forcemenus ) {
        buf[0xD3]=i; // VTSM_PGCI
        i+=CreatePGC(0,ws,1);
    }

    buf[0xD7]=i; // VTS_TMAPT
    i+=numsectTMAPT(ws->titles);

    if( jumppad || forcemenus ) {
        buf[0xDB]=i; // VTSM_C_ADT
        i+=CreateCallAdr(0,ws->menus->vg);
        
        buf[0xDF]=i; // VTSM_VOBU_ADMAP
        i+=numsectVOBUAD(ws->menus->vg);
    }

    buf[0xE3]=i; // VTS_C_ADT
    i+=CreateCallAdr(0,ws->titles->vg);

    buf[0xE7]=i; // VTS_VOBU_ADMAP
    i+=numsectVOBUAD(ws->titles->vg);

    buf[31]=i-1;
    if( jumppad || forcemenus ) {
        buf[0xC3]=i;
        i+=getvoblen(ws->menus->vg);
    }
    write4(buf+0xC4,i);
    if( ws->titles->numpgcs )
        i+=getvoblen(ws->titles->vg);
    i+=buf[31];
    write4(buf+12,i);

    if( jumppad || forcemenus )
        BuildAVInfo(buf+256,ws->menus->vg);
    BuildAVInfo(buf+512,ws->titles->vg);
    fwrite(buf,1,2048,h);

    // sect 1: VTS_PTT_SRPT
    Create_PTT_SRPT(h,ws->titles);
   
    // sect 2: VTS_PGCIT
    CreatePGC(h,ws,0);

    if( jumppad || forcemenus )
        CreatePGC(h,ws,1);

    // sect 3: ??? VTS_TMAPT
    CreateTMAPT(h,ws->titles);

    if( jumppad || forcemenus ) {
        CreateCallAdr(h,ws->menus->vg);
        CreateVOBUAD(h,ws->menus->vg);
    }
    CreateCallAdr(h,ws->titles->vg);
    CreateVOBUAD(h,ws->titles->vg);
}

void WriteIFOs(char *fbase,struct workset *ws)
{
    FILE *h;
    static char buf[1000];

    sprintf(buf,"%s_0.IFO",fbase);
    h=fopen(buf,"wb");
    WriteIFO(h,ws);
    fclose(h);

    sprintf(buf,"%s_0.BUP",fbase);
    h=fopen(buf,"wb");
    WriteIFO(h,ws);
    fclose(h);
}

void TocGen(struct workset *ws,struct pgc *fpc,char *fname)
{
    static unsigned char buf[2048];
    int i,j,vtsstart,forcemenus=needmenus(ws->menus);
    FILE *h;

    h=fopen(fname,"wb");

    memset(buf,0,2048);
    memcpy(buf,"DVDVIDEO-VMG",12);
    buf[0x21]=0x11;
    buf[0x27]=1;
    buf[0x29]=1;
    buf[0x2a]=1;
    write2(buf+0x3e,ws->ts->numvts);
    strncpy((char *)(buf+0x40),PACKAGE_STRING,31);
    buf[0x86]=4;
    i=1;

    write4(buf+0xc4,i); // TT_SRPT
    i+=Create_TT_SRPT(0,ws->ts,0);

    if( jumppad || forcemenus ) { // PGC
        write4(buf+0xc8,i);
        i+=CreatePGC(0,ws,2);
    }

    write4(buf+0xd0,i); // VTS_ATRT
    i+=(8+ws->ts->numvts*0x30c+2047)/2048;

    if( jumppad || forcemenus ) {
        write4(buf+0xd8,i); // C_ADT
        i+=CreateCallAdr(0,ws->menus->vg);

        write4(buf+0xdc,i); // VOBU_ADMAP
        i+=numsectVOBUAD(ws->menus->vg);
    }

    write4(buf+0x1c,i-1);
    vtsstart=i*2;
    if( jumppad || forcemenus ) {
        write4(buf+0xc0,i);
        vtsstart+=getvoblen(ws->menus->vg);
    }
    write4(buf+0xc,vtsstart-1);

    if( forcemenus )
        BuildAVInfo(buf+256,ws->menus->vg);

    buf[0x407]=0xc0;
    buf[0x4e5]=0xec; // pointer to command table
    // command table
    buf[0x4ed]=1; // # pre commands
    // commands start at 0x4f4
    i=0x4f4;
    if( fpc ) {
        unsigned char *pi;
        if( fpc->posti || fpc->numsources || fpc->numbuttons || fpc->entries ) {
            fprintf(stderr,"ERR:  FPC can ONLY contain prei commands, nothing else\n");
            exit(1);
        }
        pi=vm_compile(buf+i,buf+i,ws,-1,fpc->prei,2,COMPILE_PRE);
        if( !pi ) {
            fprintf(stderr,"ERR:  in FPC\n");
            exit(1);
        }
        i=(pi-buf-i)/8;
        assert(i<=128);
        buf[0x4ed]=i;
    } else if( forcemenus ) {
        buf[i+0]=0x30; // jump to VMGM 1
        buf[i+1]=0x06;
        buf[i+2]=0x00;
        buf[i+3]=0x00;
        buf[i+4]=0x00;
        buf[i+5]=0x42;
        buf[i+6]=0x00;
        buf[i+7]=0x00;
    } else if( ws->ts->numvts && ws->ts->vts[0].hasmenu ) {
        buf[i+0]=0x30; // jump to VTSM vts=1, ttn=1, menu=1
        buf[i+1]=0x06;
        buf[i+2]=0x00;
        buf[i+3]=0x01;
        buf[i+4]=0x01;
        buf[i+5]=0x83;
        buf[i+6]=0x00;
        buf[i+7]=0x00;
    } else {
        buf[i+0]=0x30; // jump to title 1
        buf[i+1]=0x02;
        buf[i+2]=0x00;
        buf[i+3]=0x00;
        buf[i+4]=0x00;
        buf[i+5]=0x01;
        buf[i+6]=0x00;
        buf[i+7]=0x00;
    }
    write2(buf+0x4f2,7+buf[0x4ed]*8);
    write2(buf+0x82,0x4ec+read2(buf+0x4f2));
    fwrite(buf,1,2048,h);

    Create_TT_SRPT(h,ws->ts,vtsstart);

    // PGC
    if( jumppad || forcemenus )
        CreatePGC(h,ws,2);

    // VMG_VTS_ATRT
    memset(buf,0,2048);
    j=8+ws->ts->numvts*4;
    write2(buf,ws->ts->numvts);
    write4(buf+4,ws->ts->numvts*0x30c+8-1);
    for( i=0; i<ws->ts->numvts; i++ )
        write4(buf+8+i*4,j+i*0x308);
    fwrite(buf,1,j,h);
    for( i=0; i<ws->ts->numvts; i++ ) {
        write4(buf,0x307);
        memcpy(buf+4,ws->ts->vts[i].vtscat,4);
        memcpy(buf+8,ws->ts->vts[i].vtssummary,0x300);
        fwrite(buf,1,0x308,h);
        j+=0x308;
    }
    j=2048-(j&2047);
    if( j < 2048 ) {
        memset(buf,0,j);
        fwrite(buf,1,j,h);
    }

    if( jumppad || forcemenus ) {
        CreateCallAdr(h,ws->menus->vg);
        CreateVOBUAD(h,ws->menus->vg);
    }
    fclose(h);
}

