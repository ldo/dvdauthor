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

static const char RCSID[]="$Id: //depot/dvdauthor/src/dvdvob.c#55 $";


struct colorremap {
    int newcolors[16];
    int state,curoffs,maxlen,nextoffs,skip;
    struct colorinfo *origmap;
};

struct vscani {
    int lastrefsect;
    int firstgop,firsttemporal,lastadjust,adjustfields;
};

static pts_t timeline[19]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
                           20,60,120,240};

#define BIGWRITEBUFLEN (16*2048)
static unsigned char bigwritebuf[BIGWRITEBUFLEN];
static int writebufpos=0;
static int writefile=-1;

static unsigned char videoslidebuf[15]={255,255,255,255, 255,255,255, 0,0,0,0, 0,0,0,0};

static pts_t calcpts(struct vobgroup *va,int cancomplain,int *didcomplain,pts_t *align,pts_t basepts,int nfields)
{
    // I assume pts should round down?  That seems to be how mplex deals with it
    // also see later comment

    int fpts=getframepts(va);
    int bpframe=(basepts*2-*align+fpts/2)/fpts;
    if( (*align+bpframe*fpts)/2 != basepts ) {
        if( !*didcomplain ) {
            if( cancomplain )
                fprintf(stderr,"WARN: Video PTS does not line up on a multiple of a field.\n");
            *didcomplain=1;
        }
        *align=basepts;
    } else
        nfields += bpframe;
    return (*align+nfields*fpts)/2;
}

static int findnextvideo(struct vob *va, int cur, int dir)
{
    // find next (dir=1) or previous(dir=-1) vobu with video
    int i, numvobus;
    
    numvobus = va->numvobus;
    switch(dir){
    case 1:  // forward
        for(i = cur+1; i < numvobus; i++) if(va->vi[i].hasvideo) return i;
        return -1;
    case -1: // backward
        for(i = cur-1; i > -1; i--) if(va->vi[i].hasvideo) return i;
        return -1;
    default:
        // ??
        return -1;
    }
}

static int findaudsect(struct vob *va,int aind,pts_t pts0,pts_t pts1)
{
    struct audchannel *ach=&va->audch[aind];
    int l=0,h=ach->numaudpts-1;

    if( h<l )
        return -1;
    while(h>l) {
        int m=(l+h+1)/2;
        if( pts0<ach->audpts[m].pts[0] )
            h=m-1;
        else
            l=m;
    }
    if( ach->audpts[l].pts[0] > pts1 )
        return -1;
    return ach->audpts[l].sect;
}

static int findvobubysect(struct vob *va,int sect)
{
    int l=0,h=va->numvobus-1;

    if( h<0 )
        return -1;
    if( sect<va->vi[0].sector )
        return -1;
    while(l<h) {
        int m=(l+h+1)/2;
        if( sect < va->vi[m].sector )
            h=m-1;
        else
            l=m;
    }
    return l;
}

static int findspuidx(struct vob *va,int ach,pts_t pts0)
{
    int l=0,h=va->audch[ach].numaudpts-1;

    if( h<l )
        return -1;
    while(h>l) {
        int m=(l+h+1)/2;
        if( pts0<va->audch[ach].audpts[m].pts[0] )
            h=m-1;
        else
            l=m;
    }
    return l;
}

static unsigned int getsect(struct vob *va,int curvobnum,int jumpvobnum,int skip,unsigned notfound)
{
    if( skip ) {
        int l,h,i;

        // only set skip bit if one of hte VOBU's from here to there contain video
        if( curvobnum<jumpvobnum ) {
            l=curvobnum+1;
            h=jumpvobnum-1;
        } else {
            l=jumpvobnum+1;
            h=curvobnum-1;
        }
        for( i=l; i<=h; i++ )
            if( va->vi[i].hasvideo )
                break;
        if( i<=h )
            skip=0x40000000;
        else
            skip=0;
    }
    if( jumpvobnum < 0 || jumpvobnum >= va->numvobus || 
        va->vi[jumpvobnum].vobcellid != va->vi[curvobnum].vobcellid )
        return notfound|skip;
    return abs(va->vi[jumpvobnum].sector-va->vi[curvobnum].sector)
        |(va->vi[jumpvobnum].hasvideo?0x80000000:0)
        |skip;
}

static pts_t readscr(const unsigned char *buf)
{
    return (((pts_t)(buf[0]&0x38))<<27)|
        ((buf[0]&3)<<28)|
        (buf[1]<<20)|
        ((buf[2]&0xf8)<<12)|
        ((buf[2]&3)<<13)|
        (buf[3]<<5)|
        ((buf[4]&0xf8)>>3);
}

static void writescr(unsigned char *buf,pts_t scr)
{
    buf[0]=((scr>>27)&0x38)|((scr>>28)&3)|68;
    buf[1]=scr>>20;
    buf[2]=((scr>>12)&0xf8)|((scr>>13)&3)|4;
    buf[3]=scr>>5;
    buf[4]=((scr<<3)&0xf8)|(buf[4]&7);
}

static pts_t readpts(const unsigned char *buf)
{
    int a1,a2,a3;
    a1=(buf[0]&0xe)>>1;
    a2=((buf[1]<<8)|buf[2])>>1;
    a3=((buf[3]<<8)|buf[4])>>1;

    return (((pts_t)a1)<<30)|
        (a2<<15)|
        a3;
}


static void writepts(unsigned char *buf,pts_t pts)
{
    buf[0]=((pts>>29)&0xe)|(buf[0]&0xf1); // this preserves the PTS / DTS / PTSwDTS top bits
    write2(buf+1,(pts>>14)|1);
    write2(buf+3,(pts<<1)|1);
}

static int findbutton(struct pgc *pg,char *dest,int dflt)
{
    int i;

    if( !dest )
        return dflt;
    for( i=0; i<pg->numbuttons; i++ )
        if( !strcmp(pg->buttons[i].name,dest) )
            return i+1;
    return dflt;
}

static void transpose_ts(unsigned char *buf,pts_t tsoffs)
{
    // pack scr
    if( buf[0] == 0 &&
        buf[1] == 0 &&
        buf[2] == 1 &&
        buf[3] == 0xba )
    {
        writescr(buf+4,readscr(buf+4)+tsoffs);

        // video/audio?
        // pts?
        if( buf[14] == 0 &&
            buf[15] == 0 &&
            buf[16] == 1 &&
            (buf[17]==0xbd || (buf[17]>=0xc0 && buf[17]<=0xef)) &&
            (buf[21] & 128))
        {
            writepts(buf+23,readpts(buf+23)+tsoffs);
            // dts?
            if( buf[21] & 64 ) {
                writepts(buf+28,readpts(buf+28)+tsoffs);
            }
        }
    }
}

static int mpa_valid(unsigned char *b)
{
    unsigned int v=(b[0]<<24)|(b[1]<<16)|(b[2]<<8)|b[3];
    int t;

    // sync, mpeg1, layer2, 48khz
    if( (v&0xFFFE0C00) != 0xFFFC0400 )
        return 0;
    // bitrate 1..14
    t=(v>>12)&15;
    if( t==0 || t==15 )
        return 0;
    // emphasis reserved
    if( (v&3)==2 )
        return 0;
    return 1;
}


static int mpa_len(unsigned char *b)
{
    static int bitratetable[16]={0,32,48,56,64,80,96,112,128,160,192,224,256,320,384,0};
    int padding=(b[2]>>1)&1;
    int bitrate=bitratetable[(b[2]>>4)&15];
    
    return 3*bitrate+padding; // 144*bitrate/sampling; 144/48=3
}

static void writeflush()
{
    if( !writebufpos ) return;
    if( write(writefile,bigwritebuf,writebufpos) != writebufpos ) {
        fprintf(stderr,"ERR:  Error writing data\n");
        exit(1);
    }
    writebufpos=0;
}

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
{
    writebufpos-=2048;
}

static void writeclose()
{
    writeflush();
    if( writefile!=-1 ) {
        close(writefile);
        writefile=-1;
    }
}

static void writeopen(const char *newname)
{
    writefile=open(newname,O_CREAT|O_WRONLY|O_BINARY,0666);
    if( writefile < 0 ) {
        fprintf(stderr,"ERR:  Error opening %s: %s\n",newname,strerror(errno));
        exit(1);
    }
}

static void closelastref(struct vobuinfo *thisvi,struct vscani *vsi,int cursect)
{
    if( vsi->lastrefsect && thisvi->numref < 3 ) {
        thisvi->lastrefsect[thisvi->numref++]=cursect;
        vsi->lastrefsect=0;
    }
}

// this function is allowed to update buf[7] and gaurantee it will end up
// in the output stream
// prevbytesect is the sector for the byte immediately preceding buf[0]
static void scanvideoptr(struct vobgroup *va,unsigned char *buf,struct vobuinfo *thisvi,int prevbytesect,struct vscani *vsi)
{
    if( buf[0]==0 &&
        buf[1]==0 &&
        buf[2]==1 ) {
        switch(buf[3]) {
        case 0: {
            int ptype=(buf[5]>>3)&7;
            int temporal=(buf[4]<<2)|(buf[5]>>6);

            closelastref(thisvi,vsi,prevbytesect);
            if( vsi->firsttemporal==-1 )
                vsi->firsttemporal=temporal;
            vsi->lastadjust=( temporal < vsi->firsttemporal );
            if( ptype == 1 || ptype == 2 ) // I or P frame
                vsi->lastrefsect=1;

            if( va->vd.vmpeg==VM_MPEG1) {
                thisvi->numfields+=2;
                if(vsi->lastadjust && vsi->firstgop==2)
                    thisvi->firstIfield+=2;
            }

            // fprintf(stderr,"INFO: frame type %d, tempref=%d, prevsect=%d\n",ptype,temporal,prevbytesect);
            break;
        } 

        case 0xb3: { // sequence header
            int hsize,vsize,aspect,frame,newaspect;
            char sizestring[30];
            
            closelastref(thisvi,vsi,prevbytesect);

            hsize=(buf[4]<<4)|(buf[5]>>4);
            vsize=((buf[5]<<8)&0xf00)|buf[6];
            aspect=buf[7]>>4;
            frame=buf[7]&0xf;

            vobgroup_set_video_framerate(va,frame);
            switch(frame) {
            case 1:
            case 4:
            case 7:
                vobgroup_set_video_attr(va,VIDEO_FORMAT,"ntsc");
                break;

            case 3:
            case 6:
                vobgroup_set_video_attr(va,VIDEO_FORMAT,"pal");
                break;

            default:
                fprintf(stderr,"WARN: unknown frame rate %d\n",frame);
                break;
            }
           
            sprintf(sizestring,"%dx%d",hsize,vsize);
            vobgroup_set_video_attr(va,VIDEO_RESOLUTION,sizestring);

            if( va->vd.vmpeg==VM_MPEG1) {
                switch( aspect ) {
                case 3:
                    vobgroup_set_video_attr(va,VIDEO_ASPECT,"16:9");
                    vobgroup_set_video_attr(va,VIDEO_FORMAT,"pal");
                    break;
                case 6:
                    vobgroup_set_video_attr(va,VIDEO_ASPECT,"16:9");
                    vobgroup_set_video_attr(va,VIDEO_FORMAT,"ntsc");
                    break;
                case 8:
                    vobgroup_set_video_attr(va,VIDEO_ASPECT,"4:3");
                    vobgroup_set_video_attr(va,VIDEO_FORMAT,"pal");
                    break;
                case 12:
                    vobgroup_set_video_attr(va,VIDEO_ASPECT,"4:3");
                    vobgroup_set_video_attr(va,VIDEO_FORMAT,"ntsc");
                    break;
                default:
                    fprintf(stderr,"WARN: unknown mpeg1 aspect ratio %d\n",aspect);
                    break;
                }
                newaspect=3+
                    (va->vd.vaspect==VA_4x3)*5+
                    (va->vd.vformat==VF_NTSC)*3;
                if( newaspect==11 ) newaspect++;
                buf[7]=(buf[7]&0xf)|(newaspect<<4); // reset the aspect ratio
            } else if( va->vd.vmpeg==VM_MPEG2 ) {
                if( aspect==2 )
                    vobgroup_set_video_attr(va,VIDEO_ASPECT,"4:3");
                else if( aspect==3 )
                    vobgroup_set_video_attr(va,VIDEO_ASPECT,"16:9");
                else
                    fprintf(stderr,"WARN: unknown mpeg2 aspect ratio %d\n",aspect);
                buf[7]=(buf[7]&0xf)|(va->vd.vaspect==VA_4x3?2:3)<<4; // reset the aspect ratio
            }
            break;
        }

        case 0xb5: { // extension header
            vobgroup_set_video_attr(va,VIDEO_MPEG,"mpeg2");
            switch(buf[4]&0xF0) {
            case 0x10: // sequence extension
                closelastref(thisvi,vsi,prevbytesect);
                break;

            case 0x20: // sequence display extension
                closelastref(thisvi,vsi,prevbytesect);
                switch(buf[4]&0xE) {
                case 2: vobgroup_set_video_attr(va,VIDEO_FORMAT,"pal"); break;
                case 4: vobgroup_set_video_attr(va,VIDEO_FORMAT,"ntsc"); break;
                    // case 6: // secam
                    // case 10: // unspecified
                }
                break;

            case 0x80: { // picture coding extension
                int padj=1; // default field pic
                
                if( (buf[6]&3)==3 ) padj++; // adj for frame pic
                if( buf[7]&2 )      padj++; // adj for repeat flag
                
                thisvi->numfields+=padj;
                if(vsi->lastadjust && vsi->firstgop==2)
                    thisvi->firstIfield+=padj;
                // fprintf(stderr,"INFO: repeat flag=%d, cursect=%d\n",buf[7]&2,cursect);
                break;
            }

            }
            break;
        }
            
        case 0xb7: { // sequence end code
            thisvi->hasseqend=1;
            break;
        }

        case 0xb8: // gop header
            closelastref(thisvi,vsi,prevbytesect);
            if( vsi->firstgop==1 ) {
                vsi->firstgop=2;
                vsi->firsttemporal=-1;
                vsi->lastadjust=0;
                vsi->adjustfields=0;
            } else if( vsi->firstgop==2 ) {
                vsi->firstgop=0;
            }
            break;

        /*
        case 0xb8: { // gop header
            int hr,mi,se,fr;

            hr=(buf[4]>>2)&31;
            mi=((buf[4]&3)<<4)|(buf[5]>>4);
            se=((buf[5]&7)<<3)|(buf[6]>>5);
            fr=((buf[6]&31)<<1)|(buf[7]>>7);
            fprintf(stderr,"INFO: GOP header, %d:%02d:%02d:%02d, drop=%d\n",hr,mi,se,fr,(buf[4]>>7));
            break;
        }
        */

        }
    }
}

static void scanvideoframe(struct vobgroup *va,unsigned char *buf,struct vobuinfo *thisvi,int cursect,int prevsect,struct vscani *vsi)
{
    int i,f=0x17+buf[0x16],l=0x14+buf[0x12]*256+buf[0x13];
    int mpf;
    struct vobuinfo oldtvi;
    struct vscani oldvsi;

    if( l-f<8 ) {
        memcpy(videoslidebuf+7,buf+f,l-f);
        for( i=0; i<l-f; i++ )
            scanvideoptr(va,videoslidebuf+i,thisvi,prevsect,vsi);
        memcpy(buf+f,videoslidebuf+7,l-f);
        memset(videoslidebuf,255,7);
        return;
    }

 rescan:
    mpf=va->vd.vmpeg;
    oldtvi=*thisvi;
    oldvsi=*vsi;

    // copy the first 7 bytes to use with the prev 7 bytes in hdr detection
    memcpy(videoslidebuf+7,buf+f,8); // we scan the first header using the slide buffer
    for( i=0; i<=7; i++ )
        scanvideoptr(va,videoslidebuf+i,thisvi,prevsect,vsi);
    memcpy(buf+f,videoslidebuf+7,8);

    // quickly scan all but the last 7 bytes for a hdr
    // buf[f]... was already scanned in the videoslidebuffer to give the correct sector
    for( i=f+1; i<l-7; i++ ) {
        if( buf[i]==0 && buf[i+1]==0 && buf[i+2]==1 )
            scanvideoptr(va,buf+i,thisvi,cursect,vsi);
    }
    if( !va->vd.vmpeg )
        vobgroup_set_video_attr(va,VIDEO_MPEG,"mpeg1");
    // if the mpeg version changed, then rerun scanvideoframe, because
    // scanvideoptr updates the aspect ratio in the sequence header
    if( mpf != va->vd.vmpeg ) {
        *thisvi=oldtvi; // we must undo all the frame pointer changes
        *vsi=oldvsi;
        goto rescan;
    }

    // use the last 7 bytes in the next iteration
    memcpy(videoslidebuf,buf+l-7,7);
}

static void finishvideoscan(struct vobgroup *va,int vob,int prevsect,struct vscani *vsi)
{
    struct vobuinfo *lastvi=&va->vobs[vob]->vi[va->vobs[vob]->numvobus-1];
    int i;

    memset(videoslidebuf+7,0,7);
    for( i=0; i<7; i++ )
        scanvideoptr(va,videoslidebuf+i,lastvi,prevsect,vsi);
    memset(videoslidebuf,255,7);
    closelastref(lastvi,vsi,prevsect);
}

static void printpts(pts_t pts)
{
    fprintf(stderr,"%d.%03d",(int)(pts/90000),(int)((pts/90)%1000));
}

static FILE *openvob(char *f,int *ispipe)
{
    FILE *h;
    int l=strlen(f);

    if( l>0 && f[l-1]=='|' ) {
        char *str;
        int i;

        f[l-1]=0;
        str=(char *)malloc(l*2+1+10);
        strcpy(str,"sh -c \"");
        l=strlen(str);
        for( i=0; f[i]; i++ ) {
            if( f[i]=='\"' || f[i]=='\'' )
                str[l++]='\\';
            str[l++]=f[i];
        }
        str[l]=0;
        strcpy(str+l,"\"");
        h=popen(str,"r");
        free(str);
        ispipe[0]=1;
    } else if( !strcmp(f,"-") ) {
        h=stdin;
        ispipe[0]=2;
    } else if( f[0]=='&' && isdigit(f[1]) ) {
        h=fdopen(atoi(&f[1]),"rb");
        ispipe[0]=0;
    } else {
        h=fopen(f,"rb");
        ispipe[0]=0;
    }
    if( !h ) {
        fprintf(stderr,"ERR:  Error opening %s: %s\n",f,strerror(errno));
        exit(1);
    }
    return h;
}

enum { CR_BEGIN0,   CR_BEGIN1,    CR_BEGIN2,    CR_BEGIN3, CR_SKIP0,
       CR_SKIP1,    CR_NEXTOFFS0, CR_NEXTOFFS1, CR_WAIT,   CR_CMD,
       CR_SKIPWAIT, CR_COL0,      CR_COL1};

static char *readpstr(char *b,int *i)
{
    char *s=strdup(b+i[0]);
    i[0]+=strlen(s)+1;
    return s;
}

static void initremap(struct colorremap *cr)
{
    int i;

    for( i=0; i<16; i++ )
        cr->newcolors[i]=i;
    cr->state=CR_BEGIN0;
    cr->origmap=0;
}

static int remapcolor(struct colorremap *cr,int idx)
{
    int i,nc;
    
    if( cr->newcolors[idx] < 16 )
        return cr->newcolors[idx];
    
    nc=cr->newcolors[idx]&0xffffff;
    for( i=0; i<16; i++ )
        if( cr->origmap->colors[i]==nc ) {
            cr->newcolors[idx]=i;
            return i;
        }
    for( i=0; i<16; i++ )
        if( cr->origmap->colors[i]==0x1000000 ) {
            cr->origmap->colors[i]=nc;
            cr->newcolors[idx]=i;
            return i;
        }
    fprintf(stderr,"ERR: color map full, unable to allocate new colors.\n");
    exit(1);
}

static void remapbyte(struct colorremap *cr,unsigned char *b)
{
    b[0]=remapcolor(cr,b[0]&15)|(remapcolor(cr,b[0]>>4)<<4);
}

static void procremap(struct colorremap *cr,unsigned char *b,int l,pts_t *timespan)
{
    while(l) {
        // fprintf(stderr,"INFO: state=%d, byte=%02x (%d)\n",cr->state,*b,*b);
        switch(cr->state) {
        case CR_BEGIN0: cr->curoffs=0; cr->skip=0; cr->maxlen=b[0]*256; cr->state=CR_BEGIN1; break;
        case CR_BEGIN1: cr->maxlen+=b[0]; cr->state=CR_BEGIN2; break;
        case CR_BEGIN2: cr->nextoffs=b[0]*256; cr->state=CR_BEGIN3; break;
        case CR_BEGIN3: cr->nextoffs+=b[0]; cr->state=CR_WAIT; break;

        case CR_WAIT:
            if( cr->curoffs==cr->maxlen ) {
                cr->state=CR_BEGIN0;
                continue; // execute BEGIN0 right away
            }
            if( cr->curoffs!=cr->nextoffs )
                break;
            cr->state=CR_SKIP0;
            // fall through to CR_SKIP0
        case CR_SKIP0: *timespan+=1024*b[0]*256; cr->state=CR_SKIP1; break;
        case CR_SKIP1: *timespan+=1024*b[0]; cr->state=CR_NEXTOFFS0; break;
        case CR_NEXTOFFS0: cr->nextoffs=b[0]*256; cr->state=CR_NEXTOFFS1; break;
        case CR_NEXTOFFS1: cr->nextoffs+=b[0]; cr->state=CR_CMD; break;
        case CR_SKIPWAIT:
            if( cr->skip ) {
                cr->skip--;
                break;
            }
            cr->state=CR_CMD;
            // fall through to CR_CMD
        case CR_CMD:
            switch(*b) {
            case 0: break;
            case 1: break;
            case 2: break;
            case 3: cr->state=CR_COL0; break;
            case 4: cr->skip=2; cr->state=CR_SKIPWAIT; break;
            case 5: cr->skip=6; cr->state=CR_SKIPWAIT; break;
            case 6: cr->skip=4; cr->state=CR_SKIPWAIT; break;
            case 255: cr->state=CR_WAIT; break;
            default:
                fprintf(stderr,"ERR: procremap encountered unknown subtitle command: %d\n",*b);
                exit(1);
            }
            break;

        case CR_COL0:
            remapbyte(cr,b);
            cr->state=CR_COL1;
            break;

        case CR_COL1:
            remapbyte(cr,b);
            cr->state=CR_CMD;
            break;
           
        default:
            assert(0);
        }
        cr->curoffs++;
        b++;
        l--;
    }
}

static void printvobustatus(struct vobgroup *va,int cursect)
{
    int j,nv=0;

    for( j=0; j<va->numvobs; j++ )
        nv+=va->vobs[j]->numvobus;

    // fprintf(stderr,"STAT: VOBU %d at %dMB, %d PGCS, %d:%02d:%02d\r",nv,cursect/512,va->numallpgcs,total/324000000,(total%324000000)/5400000,(total%5400000)/90000);
    fprintf(stderr,"STAT: VOBU %d at %dMB, %d PGCS\r",nv,cursect/512,va->numallpgcs);
}

static void audio_scan_ac3(struct audchannel *ach,unsigned char *buf,int sof,int len)
{
    u_int32_t parse;
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
    // skip cmixlev?
    if( (acmod&1) && (acmod!=1) )
        parse<<=2;
    // skip surmixlev?
    if( acmod&4 )
        parse<<=2;
    // dsurmod
    if( acmod==2 ) {
        if( (parse>>30)==2 ) 
            audiodesc_set_audio_attr(&ach->ad,&ach->adwarn,AUDIO_DOLBY,"surround");
        // else if( (parse>>30)==1 )
        // audiodesc_set_audio_attr(&ach->ad,&ach->adwarn,AUDIO_DOLBY,"nosurround");
        parse<<=2;
    }
    // lfeon
    lfeon=(parse>>31);

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
    if( lfeon ) nch++;
    sprintf(attr,"%dch",nch);
    audiodesc_set_audio_attr(&ach->ad,&ach->adwarn,AUDIO_CHANNELS,attr);
}

static void audio_scan_dts(struct audchannel *ach,unsigned char *buf,int sof,int len)
{
}

static void audio_scan_pcm(struct audchannel *ach,unsigned char *buf,int len)
{
    char attr[6];

    switch(buf[1]>>6) {
    case 0: audiodesc_set_audio_attr(&ach->ad,&ach->adwarn,AUDIO_QUANT,"16bps"); break;
    case 1: audiodesc_set_audio_attr(&ach->ad,&ach->adwarn,AUDIO_QUANT,"20bps"); break;
    case 2: audiodesc_set_audio_attr(&ach->ad,&ach->adwarn,AUDIO_QUANT,"24bps"); break;
    }
    sprintf(attr,"%dkhz",48*(1+((buf[1]>>4)&1)));
    audiodesc_set_audio_attr(&ach->ad,&ach->adwarn,AUDIO_SAMPLERATE,attr);
    sprintf(attr,"%dch",(buf[1]&7)+1);
    audiodesc_set_audio_attr(&ach->ad,&ach->adwarn,AUDIO_CHANNELS,attr);
}

int FindVobus(char *fbase,struct vobgroup *va,int ismenu)
{
    unsigned char *buf;
    FILE *h;
    int cursect=0,fsect=-1,vnum,outnum=-ismenu+1;
    int ispipe,vobid=0;
    struct mp2info {
        int hdrptr;
        unsigned char buf[6];
    } mp2hdr[8];
    struct colorremap *crs;
    
    crs=malloc(sizeof(struct colorremap)*32);
    for( vnum=0; vnum<va->numvobs; vnum++ ) {
        int i,j;
        int hadfirstvobu=0;
        pts_t backoffs=0, lastscr=0;
        struct vob *s=va->vobs[vnum];
        int prevvidsect=-1;
        struct vscani vsi;

        vsi.lastrefsect=0;
        for( i=0; i<32; i++ )
            initremap(crs+i);

        vobid++;
        s->vobid=vobid;
        vsi.firstgop=1;

        fprintf(stderr,"\nSTAT: Processing %s...\n",s->fname);
        h=openvob(s->fname,&ispipe);
        memset(mp2hdr,0,8*sizeof(struct mp2info));
        while(1) {
            if( fsect == 524272 ) {
                writeclose();
                if( outnum<=0 ) {
                    fprintf(stderr,"\nERR:  Menu VOB reached 1gb\n");
                    exit(1);
                }
                outnum++;
                fsect=-1;
            }
            buf=writegrabbuf();

            i=fread(buf,1,2048,h);
            if( i!=2048 ) {
                if( i==-1 ) {
                    fprintf(stderr,"\nERR:  Error while reading: %s\n",strerror(errno));
                    exit(1);
                } else if( i>0 )
                    fprintf(stderr,"\nWARN: Partial sector read (%d bytes); discarding data.\n",i);
                writeundo();
                break;
            }
            if( buf[14]==0 && buf[15]==0 && buf[16]==1 && buf[17]==0xbe && !strcmp(buf+20,"dvdauthor-data")) {
                // private dvdauthor data, should be removed from the final stream
                int i=35;
                if( buf[i]!=1 ) {
                    fprintf(stderr,"ERR: dvd info packet is version %d\n",buf[i]);
                    exit(1);
                }
                switch(buf[i+1]) { // packet type

                case 1: // subtitle/menu color and button information
                {
                    int st=buf[i+2]&31;
                    i+=3;
                    i+=8; // skip start pts and end pts
                    while(buf[i]!=0xff) {
                        switch(buf[i]) {
                        case 1: // new colormap
                        {
                            int j;
                            crs[st].origmap=s->p->ci;
                            for( j=0; j<buf[i+1]; j++ ) {
                                crs[st].newcolors[j]=0x1000000|
                                    (buf[i+2+3*j]<<16)|
                                    (buf[i+3+3*j]<<8)|
                                    (buf[i+4+3*j]);
                            }
                            for( ; j<16; j++ )
                                crs[st].newcolors[j]=j;
                            i+=2+3*buf[i+1];
                            break;
                        }
                        case 2: // new buttoncoli
                        {
                            int j;

                            memcpy(s->buttoncoli,buf+i+2,buf[i+1]*8);
                            for( j=0; j<buf[i+1]; j++ ) {
                                remapbyte(&crs[st],s->buttoncoli+j*8+0);
                                remapbyte(&crs[st],s->buttoncoli+j*8+1);
                                remapbyte(&crs[st],s->buttoncoli+j*8+4);
                                remapbyte(&crs[st],s->buttoncoli+j*8+5);
                            }
                            i+=2+8*buf[i+1];
                            break;
                        }
                        case 3: // button position information
                        {
                            int j,k;

                            k=buf[i+1];
                            i+=2;
                            for( j=0; j<k; j++ ) {
                                struct button *b;
                                char *st=readpstr(buf,&i);
                                    
                                if( !findbutton(s->p,st,0) ) {
                                    fprintf(stderr,"ERR:  Cannot find button '%s' as referenced by the subtitle\n",st);
                                    exit(1);
                                }
                                b=&s->p->buttons[findbutton(s->p,st,0)-1];

                                i+=2; // skip modifier
                                b->autoaction=buf[i++];
                                if( !b->autoaction ) {
                                    b->grp=buf[i];
                                    b->x1=read2(buf+i+1);
                                    b->y1=read2(buf+i+3);
                                    b->x2=read2(buf+i+5);
                                    b->y2=read2(buf+i+7);
                                    i+=9;
                                    // up down left right
                                    b->up=readpstr(buf,&i);
                                    b->down=readpstr(buf,&i);
                                    b->left=readpstr(buf,&i);
                                    b->right=readpstr(buf,&i);
                                }
                            }
                            break;
                        }
                        default:
                            fprintf(stderr,"ERR: dvd info packet command within subtitle: %d\n",buf[i]);
                            exit(1);
                        }
                    }

                    break;
                }
                        
                default:
                    fprintf(stderr,"ERR: unknown dvd info packet type: %d\n",buf[i+1]);
                    exit(1);
                }

                writeundo();
                continue;
            }
            if( buf[0]==0 && buf[1]==0 && buf[2]==1 && buf[3]==0xba ) {
                pts_t newscr=readscr(buf+4);
                if( newscr < lastscr ) {
                    fprintf(stderr,"ERR:  SCR moves backwards, remultiplex input.\n");
                    exit(1);
                }
                lastscr=newscr;
                if( !hadfirstvobu )
                    backoffs=newscr;
            }
            transpose_ts(buf,-backoffs);
            if( fsect == -1 ) {
                char newname[200];
                fsect=0;
                if( outnum>=0 )
                    sprintf(newname,"%s_%d.VOB",fbase,outnum);
                else
                    strcpy(newname,fbase);
                writeopen(newname);
            }
            if( buf[14] == 0 &&
                buf[15] == 0 &&
                buf[16] == 1 &&
                buf[17] == 0xbb ) // system header
            {
                if( buf[38] == 0 &&
                    buf[39] == 0 &&
                    buf[40] == 1 &&
                    buf[41] == 0xbf && // 1st private2
                    buf[1024] == 0 &&
                    buf[1025] == 0 &&
                    buf[1026] == 1 &&
                    buf[1027] == 0xbf ) // 2nd private2
                {
                    struct vobuinfo *vi;
                    if( s->numvobus )
                        finishvideoscan(va,vnum,prevvidsect,&vsi);
                    // fprintf(stderr,"INFO: vobu\n");
                    hadfirstvobu=1;
                    s->numvobus++;
                    if( s->numvobus > s->maxvobus ) {
                        if( !s->maxvobus )
                            s->maxvobus=1;
                        else
                            s->maxvobus<<=1;
                        s->vi=(struct vobuinfo *)realloc(s->vi,s->maxvobus*sizeof(struct vobuinfo));
                    }
                    vi=&s->vi[s->numvobus-1];
                    memset(vi,0,sizeof(struct vobuinfo));
                    vi->sector=cursect;
                    vi->fsect=fsect;
                    vi->fnum=outnum;
                    vi->firstvideopts=-1;
                    vi->firstIfield=0;
                    vi->numfields=0;
                    vi->numref=0;
                    vi->hasseqend=0;
                    vi->hasvideo=0;
                    vi->sectdata=(unsigned char *)malloc(2048);
                    memcpy(s->vi[s->numvobus-1].sectdata,buf,2048);
                    if( !(s->numvobus&15) )
                        printvobustatus(va,cursect);
                    vsi.lastrefsect=0;
                    vsi.firstgop=1;
                } else {
                    fprintf(stderr,"WARN: System header found, but PCI/DSI information is not where expected\n\t(make sure your system header is 18 bytes!)\n");
                }
            }
            if( !hadfirstvobu ) {
                fprintf(stderr,"WARN: Skipping sector, waiting for first VOBU...\n");
                writeundo();
                continue;
            }
            s->vi[s->numvobus-1].lastsector=cursect;

            i=14;
            j=-1;
            while(i<=2044) {
                if( buf[i]==0 && buf[i+1]==0 && buf[i+2]==1 ) {
                    if (buf[i+3]>=0xBD && buf[i+3]<=0xEF ) {
                        j=i;
                        i+=6+read2(buf+i+4);
                        continue;
                    } else if( buf[i+3]==0xB9 && j>=14 && buf[j+3]==0xBE ) {
                        write2(buf+j+4,read2(buf+j+4)+4);
                        memset(buf+i,0,4); // mplex uses 0 for padding, so will I                        
                    }
                }
                break;
            }

            if( buf[0] == 0 &&
                buf[1] == 0 &&
                buf[2] == 1 &&
                buf[3] == 0xba &&
                buf[14] == 0 &&
                buf[15] == 0 &&
                buf[16] == 1 &&
                buf[17] == 0xe0 ) { // video
                struct vobuinfo *vi=&s->vi[s->numvobus-1];
                vi->hasvideo=1;
                scanvideoframe(va,buf,vi,cursect,prevvidsect,&vsi);
                if( (buf[21] & 128) && vi->firstvideopts==-1 ) { // check whether there's a pts
                    vi->firstvideopts=readpts(buf+23);
                }
                prevvidsect=cursect;
            }
            if( buf[0] == 0 &&
                buf[1] == 0 &&
                buf[2] == 1 &&
                buf[3] == 0xba &&
                buf[14] == 0 &&
                buf[15] == 0 &&
                buf[16] == 1 &&
                ((buf[17] & 0xf8) == 0xc0 || buf[17]==0xbd) &&
                buf[21] & 128 ) {
                pts_t pts0=readpts(buf+23),pts1,backpts1;
                int dptr=buf[22]+23,endop=read2(buf+18)+20;
                int audch;
                pts1=pts0;
                backpts1=0;
                if( buf[17]==0xbd ) {
                    int sid=buf[dptr],offs=read2(buf+dptr+2);

                    switch(sid&0xf8) {
                    case 0x20:                          // subpicture
                    case 0x28:                          // subpicture
                    case 0x30:                          // subpicture
                    case 0x38: audch=sid; break;        // subpicture
                    case 0x80:                          // ac3
                        pts1+=2880*buf[dptr+1];
                        audch=sid&7;
                        audio_scan_ac3(&s->audch[audch],buf+dptr+4,offs-1,endop-(dptr+4));
                        break;
                    case 0x88:                          // dts
                        audch=24|(sid&7);
                        audio_scan_dts(&s->audch[audch],buf+dptr+4,offs-1,endop-(dptr+4));
                        break;
                    case 0xa0:                          // pcm
                        pts1+=150*buf[dptr+1];
                        audch=16|(sid&7);
                        audio_scan_pcm(&s->audch[audch],buf+dptr+4,endop-(dptr+4));
                        break;
                    default:   audch=-1; break;         // unknown
                    }
                } else {
                    int len=endop-dptr;
                    int index=buf[17]&7;
                    audch=8|index;                      // mp2
                    memcpy(mp2hdr[index].buf+3,buf+dptr,3);
                    while(mp2hdr[index].hdrptr+4<=len) {
                        unsigned char *h;

                        if( mp2hdr[index].hdrptr < 0 )
                            h=mp2hdr[index].buf+3+mp2hdr[index].hdrptr;
                        else
                            h=buf+dptr+mp2hdr[index].hdrptr;

                        if( !mpa_valid(h) ) {
                            mp2hdr[index].hdrptr++;
                            continue;
                        }
                        if( mp2hdr[index].hdrptr<0 )
                            backpts1+=2160;
                        else
                            pts1+=2160;
                        mp2hdr[index].hdrptr+=mpa_len(h);
                    }
                    mp2hdr[index].hdrptr-=len;
                    memcpy(mp2hdr[index].buf,buf+dptr+len-3,3);
                    audiodesc_set_audio_attr(&s->audch[audch].ad,&s->audch[audch].adwarn,AUDIO_SAMPLERATE,"48khz");
                }
                // fprintf(stderr,"aud ch=%d pts %d - %d\n",audch,pts0,pts1);
                // fprintf(stderr,"pts[%d] %d (%02x %02x %02x %02x %02x)\n",va->numaudpts,pts,buf[23],buf[24],buf[25],buf[26],buf[27]);
                if( audch<0 || audch>=64 ) {
                    fprintf(stderr,"WARN: Invalid audio channel %d\n",audch);
                } else {
                    struct audchannel *ach=&s->audch[audch];

                    if( ach->numaudpts>=ach->maxaudpts ) {
                        if( ach->maxaudpts )
                            ach->maxaudpts<<=1;
                        else
                            ach->maxaudpts=1;
                        ach->audpts=(struct audpts *)realloc(ach->audpts,ach->maxaudpts*sizeof(struct audpts));
                    }
                    if( ach->numaudpts ) {
                        // we cannot compute the length of a DTS audio packet
                        // so just backfill if it is one
                        // otherwise, for mp2 add any pts to the previous
                        // sector for a header that spanned two sectors
                        if( (audch&0x38) == 0x18 ) // is this DTS?
                            ach->audpts[ach->numaudpts-1].pts[1]=pts0;
                        else
                            ach->audpts[ach->numaudpts-1].pts[1]+=backpts1;

                        if( ach->audpts[ach->numaudpts-1].pts[1]<pts0 ) {
                            if( audch>=32 )
                                goto noshow;
                            fprintf(stderr,"WARN: Discontinuity in audio channel %d; please remultiplex input.\n",audch);
                        } else if( ach->audpts[ach->numaudpts-1].pts[1]>pts0 )
                            fprintf(stderr,"WARN: Audio pts for channel %d moves backwards; please remultiplex input.\n",audch);
                        else
                            goto noshow;
                        fprintf(stderr,"WARN: Previous sector: ");
                        printpts(ach->audpts[ach->numaudpts-1].pts[0]);
                        fprintf(stderr," - ");
                        printpts(ach->audpts[ach->numaudpts-1].pts[1]);
                        fprintf(stderr,"\nWARN: Current sector: ");
                        printpts(pts0);
                        fprintf(stderr," - ");
                        printpts(pts1);
                        fprintf(stderr,"\n");
                        ach->audpts[ach->numaudpts-1].pts[1]=pts0;
                    }
                noshow:
                    ach->audpts[ach->numaudpts].pts[0]=pts0;
                    ach->audpts[ach->numaudpts].pts[1]=pts1;
                    ach->audpts[ach->numaudpts].sect=cursect;
                    ach->numaudpts++;
                }
            }
            // the following code scans subtitle code in order to
            // remap the colors and update the end pts
            if( buf[0] == 0 &&
                buf[1] == 0 &&
                buf[2] == 1 &&
                buf[3] == 0xba &&
                buf[14] == 0 &&
                buf[15] == 0 &&
                buf[16] == 1 &&
                buf[17] == 0xbd) {
                int dptr=buf[22]+23,ml=read2(buf+18)+20;
                int st=buf[dptr];
                dptr++;
                if( (st&0xf8)==0x20 ) { // subtitle
                    procremap(&crs[st&31],buf+dptr,ml-dptr,&s->audch[st].audpts[s->audch[st].numaudpts-1].pts[1]);
                }
            }
            cursect++;
            fsect++;
        }
        switch(ispipe) {
        case 0:
            if (fclose(h)) {
                fprintf(stderr,"\nERR:  Error reading from file: %s\n",strerror(errno));
                exit(1);
            }
            break;
        case 1:
            if (pclose(h)) {
                fprintf(stderr,"\nERR:  Error reading from pipe: %s\n",strerror(errno));
                exit(1);
            }
            break;
        case 2: break;
        }
        if( s->numvobus ) {
            int i;
            pts_t finalaudiopts;
                
            finishvideoscan(va,vnum,prevvidsect,&vsi);
            // find end of audio
            finalaudiopts=-1;
            for( i=0; i<32; i++ ) {
                struct audchannel *ach=s->audch+i;
                if( ach->numaudpts &&
                    ach->audpts[ach->numaudpts-1].pts[1] > finalaudiopts )
                    finalaudiopts=ach->audpts[ach->numaudpts-1].pts[1];
            }
                    
            // pin down all video vobus
            // note: we make two passes; one assumes that the PTS for the
            // first frame is exact; the other assumes that the PTS for
            // the first frame is off by 1/2.  If both fail, then the third pass
            // assumes things are exact and throws a warning
            for( i=0; i<3; i++ ) {
                pts_t pts_align=-1;
                int complain=0, j;
                
                for( j=0; j<s->numvobus; j++ ) {
                    struct vobuinfo *vi=s->vi+j;
                    
                    if( vi->hasvideo ) {
                        if( pts_align==-1 ) {
                            pts_align=vi->firstvideopts*2;
                            if( i==1 ) {
                                // I assume pts should round down?  That seems to be how mplex deals with it
                                // also see earlier comment

                                // since pts round down, then the alternative base we should try is
                                // firstvideopts+0.5, thus increment
                                pts_align++;
                            }
                            // MarkChapters will complain if firstIfield!=0
                        }

                        vi->videopts[0]=calcpts(va,i==2,&complain,&pts_align,vi->firstvideopts,-vi->firstIfield);
                        vi->videopts[1]=calcpts(va,i==2,&complain,&pts_align,vi->firstvideopts,-vi->firstIfield+vi->numfields);
                        // if this looks like a dud, abort and try the next pass
                        if( complain && i<2 )
                            break;

                        vi->sectpts[0]=vi->videopts[0];
                        vi->sectpts[1]=vi->videopts[1];
                    }
                }
                if( !complain )
                    break;
            }
            
            // guess at non-video vobus
            for( i=0; i<s->numvobus; i++ ) {
                struct vobuinfo *vi=s->vi+i;
                if( !vi->hasvideo ) {
                    int j,k;
                    pts_t firstaudiopts=-1,p;

                    for( j=0; j<32; j++ ) {
                        struct audchannel *ach=s->audch+j;
                        for( k=0; k<ach->numaudpts; k++ )
                            if( ach->audpts[k].sect>=vi->sector ) {
                                if( firstaudiopts==-1 || ach->audpts[k].pts[0]<firstaudiopts )
                                    firstaudiopts=ach->audpts[k].pts[0];
                                break;
                            }
                    }
                    if( firstaudiopts==-1 ) {
                        fprintf(stderr,"WARN: Cannot detect pts for VOBU if there is no audio or video\nWARN: Using SCR instead.\n");
                        firstaudiopts=readscr(vi->sectdata+4)+4*147; // 147 is roughly the minumum pts that must transpire between packets; we give a couple packets of buffer to allow the dvd player to process the data
                    }
                    if( i ) {
                        pts_t frpts=getframepts(va);
                        p=firstaudiopts-s->vi[i-1].sectpts[0];
                        // ensure this is a multiple of a framerate, just to be nice
                        p+=frpts-1;
                        p-=p%frpts;
                        p+=s->vi[i-1].sectpts[0];
                        assert(p>=s->vi[i-1].sectpts[1]);
                        s->vi[i-1].sectpts[1]=p;
                    } else {
                        fprintf(stderr,"ERR:  Cannot infer pts for VOBU if there is no audio or video and it is the\nERR:  first VOBU.\n");
                        exit(1);
                    }
                    vi->sectpts[0]=p;

                    // if we can easily predict the end pts of this sector,
                    // then fill it in.  otherwise, let the next iteration do it
                    if( i+1==s->numvobus ) { // if this is the end of the vob, use the final audio pts as the last pts
                        if( finalaudiopts>vi->sectpts[0] )
                            p=finalaudiopts;
                        else
                            p=vi->sectpts[0]+getframepts(va); // add one frame of a buffer, so we don't have a zero (or less) length vobu
                    } else if( s->vi[i+1].hasvideo ) // if the next vobu has video, use the start of the video as the end of this vobu
                        p=s->vi[i+1].sectpts[0];
                    else // the next vobu is an audio only vobu, and will backfill the pts as necessary
                        continue;
                    assert(p>vi->sectpts[0]);
                    vi->sectpts[1]=p;
                }
            }

            fprintf(stderr,"\nINFO: Video pts = ");
            printpts(s->vi[0].videopts[0]);
            fprintf(stderr," .. ");
            for( i=s->numvobus-1; i>=0; i-- )
                if( s->vi[i].hasvideo ) {
                    printpts(s->vi[i].videopts[1]);
                    break;
                }
            if( i<0 )
                fprintf(stderr,"??");
            for( i=0; i<64; i++ ) {
                struct audchannel *ach=&s->audch[i];

                if( ach->numaudpts ) {
                    fprintf(stderr,"\nINFO: Audio[%d] pts = ",i);
                    printpts(ach->audpts[0].pts[0]);
                    fprintf(stderr," .. ");
                    printpts(ach->audpts[ach->numaudpts-1].pts[1]);
                }
            }
            fprintf(stderr,"\n");
        }
    }
    writeclose();
    printvobustatus(va,cursect);
    fprintf(stderr,"\n");
    free(crs);
    return 1;
}

static pts_t pabs(pts_t pts)
{
    if( pts<0 )
        return -pts;
    return pts;
}

static int findnearestvobu(struct vobgroup *pg,struct vob *va,pts_t pts)
{
    int l=0,h=va->numvobus-1,i;

    if( h<0 )
        return -1;
    pts+=va->vi[0].sectpts[0];
    i=findvobu(va,pts,l,h);
    if( i+1<va->numvobus && i>=0 &&
        pabs(pts-va->vi[i+1].sectpts[0])<pabs(pts-va->vi[i].sectpts[0]) )
        i++;
    return i;
}

void MarkChapters(struct vobgroup *va)
{
    int i,j,k,lastcellid;

    // mark start and stop points
    lastcellid=-1;
    for( i=0; i<va->numallpgcs; i++ )
        for( j=0; j<va->allpgcs[i]->numsources; j++ ) {
            struct source *s=va->allpgcs[i]->sources[j];

            for( k=0; k<s->numcells; k++ ) {
                int v;
                
                v=findnearestvobu(va,s->vob,s->cells[k].startpts);
                if( v>=0 && v<s->vob->numvobus )
                    s->vob->vi[v].vobcellid=1;
                s->cells[k].scellid=v;

                if( lastcellid!=v &&
                    s->vob->vi[v].firstIfield!=0) {
                    fprintf(stderr,"WARN: GOP is not closed on cell %d of source %s of pgc %d\n",k+1,s->fname,i+1);
                }

                if( s->cells[k].endpts>=0 ) {
                    v=findnearestvobu(va,s->vob,s->cells[k].endpts);
                    if( v>=0 && v<s->vob->numvobus )
                        s->vob->vi[v].vobcellid=1;
                } else
                    v=s->vob->numvobus;
                s->cells[k].ecellid=v;

                lastcellid=v;
            }
        }
    // tally up actual cells
    for( i=0; i<va->numvobs; i++ ) {
        int cellvobu=0;
        int cellid=0;
        va->vobs[i]->vi[0].vobcellid=1;
        for( j=0; j<va->vobs[i]->numvobus; j++ ) {
            struct vobuinfo *v=&va->vobs[i]->vi[j];
            if( v->vobcellid ) {
                cellid++;
                cellvobu=j;
            }
            v->vobcellid=cellid+va->vobs[i]->vobid*256;
            v->firstvobuincell=cellvobu;
        }
        cellvobu=va->vobs[i]->numvobus-1;
        for( j=cellvobu; j>=0; j-- ) {
            struct vobuinfo *v=&va->vobs[i]->vi[j];
            v->lastvobuincell=cellvobu;
            if(v->firstvobuincell==j )
                cellvobu=j-1;
        }
        va->vobs[i]->numcells=cellid;
        if( cellid>=256 ) {
            fprintf(stderr,"ERR:  VOB %s has too many cells (%d, 256 allowed)\n",va->vobs[i]->fname,cellid);
            exit(1);
        }
    }

    // now compute scellid and ecellid
    for( i=0; i<va->numallpgcs; i++ )
        for( j=0; j<va->allpgcs[i]->numsources; j++ ) {
            struct source *s=va->allpgcs[i]->sources[j];

            for( k=0; k<s->numcells; k++ ) {
                struct cell *c=&s->cells[k];

                if( c->scellid<0 )
                    c->scellid=1;
                else if( c->scellid<s->vob->numvobus )
                    c->scellid=s->vob->vi[c->scellid].vobcellid&255;
                else
                    c->scellid=s->vob->numcells+1;

                if( c->ecellid<0 )
                    c->ecellid=1;
                else if( c->ecellid<s->vob->numvobus )
                    c->ecellid=s->vob->vi[c->ecellid].vobcellid&255;
                else
                    c->ecellid=s->vob->numcells+1;

                va->allpgcs[i]->numcells+=c->ecellid-c->scellid;
                if( c->scellid!=c->ecellid && c->ischapter ) {
                    va->allpgcs[i]->numprograms++;
                    if( c->ischapter==1 )
                        va->allpgcs[i]->numchapters++;
                    if( va->allpgcs[i]->numprograms>=256 ) {
                        fprintf(stderr,"ERR:  PGC %d has too many programs (%d, 256 allowed)\n",i+1,va->allpgcs[i]->numprograms);
                        exit(1);
                    }
                    // if numprograms<256, then numchapters<256, so
                    // no need to doublecheck
                }
            }
        }
}

static pts_t getcellaudiopts(struct vobgroup *va,int vcid,int ach,int w)
{
    struct vob *v=va->vobs[(vcid>>8)-1];
    struct audchannel *a=&v->audch[ach];
    int ai=0;

    assert((vcid&255)==(w?v->numcells:1));
    if( w )
        ai=a->numaudpts-1;
    return a->audpts[ai].pts[w];
}

static int hasaudio(struct vobgroup *va,int vcid,int ach,int w)
{
    struct vob *v=va->vobs[(vcid>>8)-1];
    struct audchannel *a=&v->audch[ach];

    assert((vcid&255)==(w?v->numcells:1));

    return a->numaudpts!=0;
}

static pts_t getcellvideopts(struct vobgroup *va,int vcid,int w)
{
    struct vob *v=va->vobs[(vcid>>8)-1];
    int vi=0;

    assert((vcid&255)==(w?v->numcells:1));
    if( w )
        vi=v->numvobus-1;
    // we use sectpts instead of videopts because sometimes you will
    // present the last video frame for a long time; we want to know
    // the last presented time stamp: sectpts
    return v->vi[vi].sectpts[w];
}

static pts_t calcaudiodiff(struct vobgroup *va,int vcid,int ach,int w)
{
    return getcellvideopts(va,vcid,w)-getcellaudiopts(va,vcid,ach,w);
}

int calcaudiogap(struct vobgroup *va,int vcid0,int vcid1,int ach)
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
    fprintf(stderr,"WARN: Do not know how to compute the audio gap, assuming discontinuity.\n");
    return 1;
}

void FixVobus(char *fbase,struct vobgroup *va,int ismenu)
{
    int h=-1;
    int i,j,pn,fnum=-2;
    pts_t scr;
    int vff,vrew,totvob,curvob;
    unsigned char *buf;

    totvob=0;
    for( pn=0; pn<va->numvobs; pn++ )
        totvob+=va->vobs[pn]->numvobus;
    curvob=0;

    for( pn=0; pn<va->numvobs; pn++ ) {
        struct vob *p=va->vobs[pn];

        for( i=0; i<p->numvobus; i++ ) {
            struct vobuinfo *vi=&p->vi[i];

            if( vi->fnum!=fnum ) {
                char fname[200];
                if( h >= 0 )
                    close(h);
                fnum=vi->fnum;
                if( fnum==-1 )
                    strcpy(fname,fbase);
                else
                    sprintf(fname,"%s_%d.VOB",fbase,fnum);
                h=open(fname,O_WRONLY|O_BINARY);
                if( h < 0 ) {
                    fprintf(stderr,"\nERR:  Error opening %s: %s\n",fname,strerror(errno));
                    exit(1);
                }
            }
            buf=vi->sectdata;
            memset(buf+0x2d,0,0x400-0x2d);
            memset(buf+0x407,0,0x7ff-0x407);

            scr=readscr(buf+4);

            buf[0x2c]=0;
            write4(buf+0x2d,vi->sector);
            write4(buf+0x39,vi->sectpts[0]); // vobu_s_ptm
            write4(buf+0x3d,vi->sectpts[1]); // vobu_e_ptm
            if( vi->hasseqend ) // if sequence_end_code
                write4(buf+0x41,vi->videopts[1]); // vobu_se_e_ptm
            write4(buf+0x45,buildtimeeven(va,vi->sectpts[0]-p->vi[vi->firstvobuincell].sectpts[0])); // total guess
                
            if( p->p->numbuttons ) {
                struct pgc *pg=p->p;

                write2(buf+0x8d,1);
                write4(buf+0x8f,p->vi[0].sectpts[0]);
                write4(buf+0x93,-1);
                write4(buf+0x97,-1);
                write2(buf+0x9b,0x1000);
                buf[0x9e]=pg->numbuttons;
                buf[0x9f]=pg->numbuttons;
                memcpy(buf+0xa3,p->buttoncoli,24);
                for( j=0; j<pg->numbuttons; j++ ) {
                    struct button *b=pg->buttons+j;
                    if( b->autoaction ) {
                        buf[0xbb+j*18+3]=64;
                    } else {
                        buf[0xbb+j*18+0]=(b->grp*64)|(b->x1>>4);
                        buf[0xbb+j*18+1]=(b->x1<<4)|(b->x2>>8);
                        buf[0xbb+j*18+2]=b->x2;
                        buf[0xbb+j*18+3]=(b->y1>>4);
                        buf[0xbb+j*18+4]=(b->y1<<4)|(b->y2>>8);
                        buf[0xbb+j*18+5]=b->y2;
                        buf[0xbb+j*18+6]=findbutton(pg,b->up,(j==0)?pg->numbuttons:j);
                        buf[0xbb+j*18+7]=findbutton(pg,b->down,(j+1==pg->numbuttons)?1:j+2);
                        buf[0xbb+j*18+8]=findbutton(pg,b->left,(j==0)?pg->numbuttons:j);
                        buf[0xbb+j*18+9]=findbutton(pg,b->right,(j+1==pg->numbuttons)?1:j+2);
                    }
                    write8(buf+0xbb+j*18+10,0x71,0x01,0x00,0x0F,0x00,j+1,0x00,0x0d); // g[0]=j && linktailpgc
                }
            }

            buf[0x406]=1;
            write4(buf+0x407,scr);
            write4(buf+0x40b,vi->sector); // current lbn
            for( j=0; j<vi->numref; j++ )
                write4(buf+0x413+j*4,vi->lastrefsect[j]-vi->sector);
            write2(buf+0x41f,vi->vobcellid>>8);
            buf[0x422]=vi->vobcellid;
            write4(buf+0x423,read4(buf+0x45));
            write4(buf+0x433,p->vi[0].sectpts[0]);
            write4(buf+0x437,p->vi[p->numvobus-1].sectpts[1]);

            write4(buf+0x4f1,getsect(p,i,findnextvideo(p,i,1),0,0xbfffffff));
            write4(buf+0x541,getsect(p,i,i+1,0,0x3fffffff));
            write4(buf+0x545,getsect(p,i,i-1,0,0x3fffffff));
            write4(buf+0x595,getsect(p,i,findnextvideo(p,i,-1),0,0xbfffffff));
            for( j=0; j<va->numaudiotracks; j++ ) {
                int s=getaudch(va,j);

                if( s>=0 )
                    s=findaudsect(p,s,vi->sectpts[0],vi->sectpts[1]);

                if( s>=0 ) {
                    s=s-vi->sector;
                    if( s > 0x1fff || s < -(0x1fff)) {
                        fprintf(stderr,"\nWARN: audio sector out of range: %d (vobu #%d, pts ",s,i);
                        printpts(vi->sectpts[0]);
                        fprintf(stderr,")\n");
                        s=0;
                    }
                    if( s < 0 )
                        s=(-s)|0x8000;
                } else
                    s=0x3fff;
                write2(buf+0x599+j*2,s);
            }
            for( j=0; j<va->numsubpicturetracks; j++ ) {
                struct audchannel *ach=&p->audch[j|32];
                int s;

                if( ach->numaudpts ) {
                    int id=findspuidx(p,j|32,vi->sectpts[0]);
                    
                    // if overlaps A, point to A
                    // else if (A before here or doesn't exist) and (B after here or doesn't exist), point to here
                    // else point to B

                    if( id>=0 && 
                        ach->audpts[id].pts[0]<vi->sectpts[1] &&
                        ach->audpts[id].pts[1]>=vi->sectpts[0] )
                        s=findvobubysect(p,ach->audpts[id].sect);
                    else if( (id<0 || ach->audpts[id].pts[1]<vi->sectpts[0]) &&
                             (id+1==ach->numaudpts || ach->audpts[id+1].pts[0]>=vi->sectpts[1]) )
                        s=i;
                    else
                        s=findvobubysect(p,ach->audpts[id+1].sect);
                    id=(s<i);
                    s=getsect(p,i,s,0,0x7fffffff)&0x7fffffff;
                    if(!s)
                        s=0x7fffffff;
                    if(s!=0x7fffffff && id)
                        s|=0x80000000;
                } else
                    s=0;
                write4(buf+0x5a9+j*4,s);
            }
            write4(buf+0x40f,vi->lastsector-vi->sector);
            vff=i;
            vrew=i;
            for( j=0; j<19; j++ ) {
                int nff,nrew;

                nff=findvobu(p,vi->sectpts[0]+timeline[j]*45000,
                             vi->firstvobuincell,
                             vi->lastvobuincell);
                // a hack -- the last vobu in the cell shouldn't have any forward ptrs
                if( i==vi->lastvobuincell )
                    nff=i+1;
                nrew=findvobu(p,vi->sectpts[0]-timeline[j]*45000,
                              vi->firstvobuincell,
                              vi->lastvobuincell);
                write4(buf+0x53d-j*4,getsect(p,i,nff,j>=15 && nff>vff+1,0x3fffffff));
                write4(buf+0x549+j*4,getsect(p,i,nrew,j>=15 && nrew<vrew-1,0x3fffffff));
                vff=nff;
                vrew=nrew;
            }

            lseek(h,vi->fsect*2048,SEEK_SET);
            write(h,buf,2048);
            curvob++;
            if( !(curvob&15) ) 
                fprintf(stderr,"STAT: fixing VOBU at %dMB (%d/%d, %d%%)\r",vi->sector/512,curvob+1,totvob,curvob*100/totvob);
        }
    }
    if( h>=0 )
        close(h);
    if( totvob>0 )
        fprintf(stderr,"STAT: fixed %d VOBUS                         ",totvob);
    fprintf(stderr,"\n");
}

