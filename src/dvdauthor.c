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
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>

#include "dvdauthor.h"
#include "da-internal.h"

static const char RCSID[]="$Id: //depot/dvdauthor/src/dvdauthor.c#73 $";



// with this enabled, extra PGC commands will be generated to allow
// jumping/calling to a wider number of destinations
int jumppad=0;

static char *vmpegdesc[4]={"","mpeg1","mpeg2",0};
static char *vresdesc[6]={"","720xfull","704xfull","352xfull","352xhalf",0};
static char *vformatdesc[4]={"","ntsc","pal",0};
static char *vaspectdesc[4]={"","4:3","16:9",0};
static char *vdisallowdesc[4]={"","noletterbox","nopanscan",0};
// taken from mjpegtools, also GPL
static char *vratedesc[9]={"",
                           "24000.0/1001.0 (NTSC 3:2 pulldown converted FILM)",
                           "24.0 (NATIVE FILM)",
                           "25.0 (PAL/SECAM VIDEO / converted FILM)",
                           "30000.0/1001.0 (NTSC VIDEO)",
                           "30.0",
                           "50.0 (PAL FIELD RATE)",
                           "60000.0/1001.0 (NTSC FIELD RATE)",
                           "60.0"
};
static char *aformatdesc[6]={"","ac3","mp2","pcm","dts",0};
static char *aquantdesc[6]={"","16bps","20bps","24bps","drc",0};
static char *adolbydesc[3]={"","surround",0};
static char *alangdesc[4]={"","nolang","lang",0};
static char *achanneldesc[10]={"","1ch","2ch","3ch","4ch","5ch","6ch","7ch","8ch",0};
static char *asampledesc[4]={"","48khz","96khz",0};

char *entries[9]={"","","title","root","subtitle","audio","angle","ptt",0};

char *pstypes[3]={"VTS","VTSM","VMGM"};

static const int colors[16]={
    0x1000000,
    0x1000000,
    0x1000000,
    0x1000000,

    0x1000000,
    0x1000000,
    0x1000000,
    0x1000000,

    0x1000000,
    0x1000000,
    0x1000000,
    0x1000000,

    0x1000000,
    0x1000000,
    0x1000000,
    0x1000000
};

static int ratedenom[9]={0,90090,90000,90000,90090,90000,90000,90090,90000};
static int evenrate[9]={0,    24,   24,   25,   30,   30,   50,   60,   60};

static int getratecode(struct vobgroup *va)
{
    if( va->vd.vframerate )
        return va->vd.vframerate;
    else
        return 4;
}

int getratedenom(struct vobgroup *va)
{
    return ratedenom[getratecode(va)];
}

pts_t getframepts(struct vobgroup *va)
{
    int rc=getratecode(va);

    return ratedenom[rc]/evenrate[rc];
}

static int tobcd(int v)
{
    return (v/10)*16+v%10;
}

static unsigned int buildtimehelper(struct vobgroup *va,int64_t num,int64_t denom)
{
    int hr,min,sec,fr,rc;
    int64_t frate;

    if( denom==90090 ) {
        frate=30;
        rc=3;
    } else {
        frate=25;
        rc=1;
    }
    num+=denom/(frate*2)+1;
    sec=num/denom;
    min=sec/60;
    hr=tobcd(min/60);
    min=tobcd(min%60);
    sec=tobcd(sec%60);
    num%=denom;
    fr=tobcd(num*frate/denom);
    return (hr<<24)|(min<<16)|(sec<<8)|fr|(rc<<6);
}

unsigned int buildtimeeven(struct vobgroup *va,int64_t num)
{
    int rc=getratecode(va);

    return buildtimehelper(va,num,ratedenom[rc]);
}

unsigned int getaudch(struct vobgroup *va,int a)
{
    if( !va->ad[a].aid )
        return -1;
    return va->ad[a].aid-1+(va->ad[a].aformat-1)*8;
}

void write8(unsigned char *p,unsigned char d0,unsigned char d1,unsigned char d2,unsigned char d3,unsigned char d4,unsigned char d5,unsigned char d6,unsigned char d7)
{
    p[0]=d0;
    p[1]=d1;
    p[2]=d2;
    p[3]=d3;
    p[4]=d4;
    p[5]=d5;
    p[6]=d6;
    p[7]=d7;
}

void write4(unsigned char *p,unsigned int v)
{
    p[0]=(v>>24)&255;
    p[1]=(v>>16)&255;
    p[2]=(v>>8)&255;
    p[3]=v&255;
}

void write2(unsigned char *p,unsigned int v)
{
    p[0]=(v>>8)&255;
    p[1]=v&255;
}

unsigned int read4(unsigned char *p)
{
    return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3];
}

unsigned int read2(unsigned char *p)
{
    return (p[0]<<8)|p[1];
}

static int warnupdate(int *oldval,int newval,int *warnval,char *desc,char **lookup)
{
    if( oldval[0]==0 ) {
        oldval[0]=newval;
        return 0;
    } if (oldval[0]==newval )
        return 0;
    else if( warnval[0]!=newval ) {
        fprintf(stderr,"WARN: attempt to update %s from %s to %s; skipping\n",desc,lookup[oldval[0]],lookup[newval]);
        warnval[0]=newval;
    }
    return 1;
}

static int scanandwarnupdate(int *oldval,char *newval,int *warnval,char *desc,char **lookup)
{
    int i;

    for( i=1; lookup[i]; i++ )
        if( !strcasecmp(newval,lookup[i]) )
            return warnupdate(oldval,i,warnval,desc,lookup)+1;
    return 0;
}

int vobgroup_set_video_framerate(struct vobgroup *va,int rate)
{
    int w;

    if( !va->vd.vframerate && rate!=3 && rate!=4 )
        fprintf(stderr,"WARN: not a valid DVD frame rate: %s\n",vratedesc[rate]);
    w=scanandwarnupdate(&va->vd.vframerate,vratedesc[rate],&va->vdwarn.vframerate,"frame rate",vratedesc);
    if(w) return w-1;
    return 0;
}

#define ATTRMATCH(a) (attr==0 || attr==(a))

int vobgroup_set_video_attr(struct vobgroup *va,int attr,char *s)
{
    int w;

    if( ATTRMATCH(VIDEO_MPEG) ) {
        w=scanandwarnupdate(&va->vd.vmpeg,s,&va->vdwarn.vmpeg,"mpeg format",vmpegdesc);
        if(w) return w-1;
    }

    if( ATTRMATCH(VIDEO_FORMAT) ) {
        w=scanandwarnupdate(&va->vd.vformat,s,&va->vdwarn.vformat,"tv format",vformatdesc);
        if(w) return w-1;
    }

    if( ATTRMATCH(VIDEO_ASPECT) ) {
        w=scanandwarnupdate(&va->vd.vaspect,s,&va->vdwarn.vaspect,"aspect ratio",vaspectdesc);
        if(w) return w-1;
    }

    if( ATTRMATCH(VIDEO_WIDESCREEN) ) {
        w=scanandwarnupdate(&va->vd.vdisallow,s,&va->vdwarn.vdisallow,"16:9 disallow",vdisallowdesc);
        if(w) return w-1;
    }

    if( ATTRMATCH(VIDEO_CAPTION) ) {
        if( !strcasecmp(s,"field1") )
            va->vd.vcaption|=1;
        else if( !strcasecmp(s,"field2") )
            va->vd.vcaption|=2;
    }

    if( ATTRMATCH(VIDEO_RESOLUTION) && strstr(s,"x") ) {
        int h=atoi(s),v,r,w;
        char *s2=strstr(s,"x")+1;

        if(isdigit(s2[0]))
            v=atoi(s2);
        else if(!strcasecmp(s2,"full") || !strcasecmp(s2,"high"))
            v=384;
        else
            v=383;
       
        if( h>704 )
            r=VS_720H;
        else if( h>352 )
            r=VS_704H;
        else if( v>=384 )
            r=VS_352H;
        else
            r=VS_352L;
        w=warnupdate(&va->vd.vres,r,&va->vdwarn.vres,"resolution",vresdesc);

        if( va->vd.vformat==VF_NONE ) {
            if( !(v%5) )
                va->vd.vformat=VF_NTSC;
            else if( !(v%9) )
                va->vd.vformat=VF_PAL;
        }
        return w;
    }
    fprintf(stderr,"ERR:  Cannot parse video option '%s'\n",s);
    exit(1);
}

int audiodesc_set_audio_attr(struct audiodesc *ad,struct audiodesc *adwarn,int attr,char *s)
{
    int w;

    if (ATTRMATCH(AUDIO_FORMAT)) {
        w=scanandwarnupdate(&ad->aformat,s,&adwarn->aformat,"audio format",aformatdesc);
        if(w) return w-1;
    }

    if (ATTRMATCH(AUDIO_QUANT)) {
        w=scanandwarnupdate(&ad->aquant,s,&adwarn->aquant,"audio quantization",aquantdesc);
        if(w) return w-1;
    }

    if (ATTRMATCH(AUDIO_DOLBY)) {
        w=scanandwarnupdate(&ad->adolby,s,&adwarn->adolby,"surround",adolbydesc);
        if(w) return w-1;
    }

    if (ATTRMATCH(AUDIO_ANY)) {
        w=scanandwarnupdate(&ad->alangp,s,&adwarn->alangp,"audio language",alangdesc);
        if(w) return w-1;
    }

    if (ATTRMATCH(AUDIO_CHANNELS)) {
        w=scanandwarnupdate(&ad->achannels,s,&adwarn->achannels,"number of channels",achanneldesc);
        if(w) return w-1;
    }

    if (ATTRMATCH(AUDIO_SAMPLERATE)) {
        w=scanandwarnupdate(&ad->asample,s,&adwarn->asample,"sampling rate",asampledesc);
        if(w) return w-1;
    }

    if (ATTRMATCH(AUDIO_LANG) && 2==strlen(s)) {
        w=warnupdate(&ad->alangp,AL_LANG,&adwarn->alangp,"audio language",alangdesc);
        if(ad->lang[0] || ad->lang[1])
            w=1;
        ad->lang[0]=tolower(s[0]);
        ad->lang[1]=tolower(s[1]);
        return w;
    }
    fprintf(stderr,"ERR:  Cannot parse audio option '%s'\n",s);
    exit(1);
}

static int vobgroup_set_audio_attr(struct vobgroup *va,int attr,char *s,int ch)
{
    if( ch>=va->numaudiotracks )
        va->numaudiotracks=ch+1;

    return audiodesc_set_audio_attr(&va->ad[ch],&va->adwarn[ch],attr,s);
}

static int vobgroup_set_subpic_attr(struct vobgroup *va,int attr,char *s,int ch)
{
    int w;

    if( ch>=va->numsubpicturetracks )
        va->numsubpicturetracks=ch+1;

    if (ATTRMATCH(SPU_ANY)) {
        w=scanandwarnupdate(&va->sp[ch].slangp,s,&va->spwarn[ch].slangp,"subpicture language",alangdesc);
        if(w) return w-1;
    }

    if(ATTRMATCH(SPU_LANG) && 2==strlen(s)) {
        w=warnupdate(&va->sp[ch].slangp,AL_LANG,&va->spwarn[ch].slangp,"subpicture language",alangdesc);
        if(va->sp[ch].lang[0] || va->sp[ch].lang[1])
            w=1;
        va->sp[ch].lang[0]=tolower(s[0]);
        va->sp[ch].lang[1]=tolower(s[1]);
        return w;
    }
    fprintf(stderr,"ERR:  Cannot parse subpicture option '%s' on track %d\n",s,ch);
    exit(1);
}

static void inferattr(int *a,int def)
{
    if( a[0]!=0 ) return;
    a[0]=def;
}

static void setattr(struct vobgroup *va,int pstype)
{
    int i;

    if( va->vd.vmpeg==VM_NONE )
        fprintf(stderr,"WARN: video mpeg version was not autodetected\n");
    if( va->vd.vres==VS_NONE )
        fprintf(stderr,"WARN: video resolution was not autodetected\n");
    if( va->vd.vformat==VF_NONE )
        fprintf(stderr,"WARN: video format was not autodetected\n");
    if( va->vd.vaspect==VA_NONE )
        fprintf(stderr,"WARN: aspect ratio was not autodetected\n");
    inferattr(&va->vd.vmpeg,  VM_MPEG2);
    inferattr(&va->vd.vres,   VS_720H);
    inferattr(&va->vd.vformat,VF_NTSC);
    inferattr(&va->vd.vaspect,VA_4x3);
    if( va->vd.vaspect==VA_4x3 )
        va->vd.vdisallow=VD_LETTERBOX|VD_PANSCAN; // if you are 4:3 then you don't need letterbox or pan&scan

    for( i=0; i<32; i++ ) {
        int id=(i>>2)+1, f=(i&3)+1, j,fnd; // note this does not follow the normal stream order
        struct audchannel *fad=0;
        int matchidx,matchcnt;

        fnd=0;
        for( j=0; j<va->numvobs; j++ ) {
            fad=&va->vobs[j]->audch[id-1+(f-1)*8];
            if( fad->numaudpts ) {
                fnd=1;
                break;
            }
        }
        if( !fnd )
            continue;

        // do we already know about this stream?
        fnd=0;
        for( j=0; j<va->numaudiotracks; j++ )
            if( va->ad[j].aformat==f && va->ad[j].aid==id )
                fnd=1;
        if( fnd )
            continue;

        matchcnt=-1;
        matchidx=-1;
        // maybe we know about this type of stream but haven't matched the id yet?
        for( j=0; j<va->numaudiotracks; j++ )
            if( va->ad[j].aid==0 ) {
                int c=0;

#define ACMPV(setting,val) \
                if( va->ad[j].setting!=0 && val!=0 && va->ad[j].setting!=val ) continue; \
                if( va->ad[j].setting==val ) c++

#define ACMP(setting) ACMPV(setting,fad->ad.setting)

                ACMPV(aformat,f);
                ACMP(aquant);
                ACMP(adolby);
                ACMP(achannels);
                ACMP(asample);

#undef ACMP
#undef ACMPV

                if( c>matchcnt ) {
                    matchcnt=c;
                    matchidx=j;
                }
                fnd=1;
            }
        if( !fnd ) {
            // guess we need to add this stream
            j=va->numaudiotracks++;
        } else
            j=matchidx;

        va->ad[j].aformat=f;
        va->ad[j].aid=id;
        warnupdate(&va->ad[j].aquant,
                   fad->ad.aquant,
                   &va->adwarn[j].aquant,
                   "audio quantization",
                   aquantdesc);
        warnupdate(&va->ad[j].adolby,
                   fad->ad.adolby,
                   &va->adwarn[j].adolby,
                   "surround",
                   adolbydesc);
        warnupdate(&va->ad[j].achannels,
                   fad->ad.achannels,
                   &va->adwarn[j].achannels,
                   "number of channels",
                   achanneldesc);
        warnupdate(&va->ad[j].asample,
                   fad->ad.asample,
                   &va->adwarn[j].asample,
                   "sampling rate",
                   asampledesc);
    }

    for( i=0; i<va->numaudiotracks; i++ ) {
        if( va->ad[i].aformat==AF_NONE ) {
            fprintf(stderr,"WARN: audio stream %d was not autodetected\n",i);
        }
        inferattr(&va->ad[i].aformat,AF_MP2);
        switch(va->ad[i].aformat) {
        case AF_AC3:
        case AF_DTS:
            inferattr(&va->ad[i].aquant,AQ_DRC);
            inferattr(&va->ad[i].achannels,6);
            break;

        case AF_MP2:
            inferattr(&va->ad[i].aquant,AQ_20);
            inferattr(&va->ad[i].achannels,2);
            break;
            
        case AF_PCM:
            inferattr(&va->ad[i].achannels,2);
            inferattr(&va->ad[i].aquant,AQ_16);
            break;
        }
        inferattr(&va->ad[i].asample,AS_48KHZ);
    }

    for( i=32; i<64; i++ ) {
        int id=i-32+1, j, fnd;

        fnd=0;
        for( j=0; j<va->numvobs; j++ )
            if( va->vobs[j]->audch[i].numaudpts )
                fnd=1;
        if( !fnd )
            continue;
      
        // do we already know about this stream?
        fnd=0;
        for( j=0; j<va->numsubpicturetracks; j++ )
            if( va->sp[j].sid==id )
                fnd=1;
        if( fnd )
            continue;

        // maybe we know about this type of stream but haven't matched the id yet?
        for( j=0; j<va->numsubpicturetracks; j++ )
            if( va->sp[j].sid==0 ) {
                va->sp[j].sid=id;
                fnd=1;
                break;
            }
        if( fnd )
            continue;

        // guess we need to add this stream
        va->sp[va->numsubpicturetracks].sid=id;
        va->numsubpicturetracks++;
    }

    fprintf(stderr,"INFO: Generating %s with the following video attributes:\n",pstypes[pstype]);
    fprintf(stderr,"INFO: MPEG version: %s\n",vmpegdesc[va->vd.vmpeg]);
    fprintf(stderr,"INFO: TV standard: %s\n",vformatdesc[va->vd.vformat]);
    fprintf(stderr,"INFO: Aspect ratio: %s\n",vaspectdesc[va->vd.vaspect]);
    fprintf(stderr,"INFO: Resolution: %dx%d\n",
            va->vd.vres!=VS_720H?(va->vd.vres==VS_704H?704:352):720,
            (va->vd.vres==VS_352L?240:480)*(va->vd.vformat==VF_PAL?6:5)/5);
    for( i=0; i<va->numaudiotracks; i++ ) {
        fprintf(stderr,"INFO: Audio ch %d format: %s/%s, %s %s",i,aformatdesc[va->ad[i].aformat],achanneldesc[va->ad[i].achannels],asampledesc[va->ad[i].asample],aquantdesc[va->ad[i].aquant]);
        if( va->ad[i].adolby==AD_SURROUND )
            fprintf(stderr,", surround");
        if( va->ad[i].alangp==AL_LANG )
            fprintf(stderr,", '%c%c'",va->ad[i].lang[0],va->ad[i].lang[1]);
        fprintf(stderr,"\n");
        if( !va->ad[i].aid )
            fprintf(stderr,"WARN: Audio ch %d is not used!\n",i);
    }
}

int findcellvobu(struct vob *va,int cellid)
{
    int l=0,h=va->numvobus-1;
    if( h<l )
        return 0;
    cellid=(cellid&255)|(va->vobid*256);
    if( cellid<va->vi[0].vobcellid )
        return 0;
    if( cellid>va->vi[h].vobcellid )
        return h+1;
    while(l<h) {
        int m=(l+h)/2;
        if( cellid<=va->vi[m].vobcellid )
            h=m;
        else
            l=m+1;
    }
    return l;
}

pts_t getcellpts(struct vob *va,int cellid)
{
    int s=findcellvobu(va,cellid),e=findcellvobu(va,cellid+1);
    if( s==e ) return 0;
    return va->vi[e-1].sectpts[1]-va->vi[s].sectpts[0];
}

int findvobu(struct vob *va,pts_t pts,int l,int h)
{
    // int l=0,h=va->numvobus-1;

    if( h<l )
        return l-1;
    if( pts<va->vi[l].sectpts[0] )
        return l-1;
    if( pts>=va->vi[h].sectpts[1] )
        return h+1;
    while(l<h) {
        int m=(l+h+1)/2;
        if( pts < va->vi[m].sectpts[0] )
            h=m-1;
        else
            l=m;
    }
    return l;
}

pts_t getptsspan(struct pgc *ch)
{
    int s,c,ci;
    pts_t ptsspan=0;

    for( s=0; s<ch->numsources; s++ ) {
        struct source *sc=ch->sources[s];
        for( c=0; c<sc->numcells; c++ ) {
            struct cell *cl=&sc->cells[c];
            for( ci=cl->scellid; ci<cl->ecellid; ci++ )
                ptsspan+=getcellpts(sc->vob,ci);
        }
    }
    return ptsspan;
}

static char *makevtsdir(char *s)
{
    static char fbuf[1000];

    if( !s )
        return 0;
    strcpy(fbuf,s);
    strcat(fbuf,"/VIDEO_TS");
    return strdup(fbuf);
}

// jumppad requires the existance of a menu to operate
// if no languages exist, create an english one
static void jp_force_menu(struct menugroup *mg,int type)
{
    struct pgcgroup *pg;

    if( !jumppad ) return;
    if( mg->numgroups ) return;
    fprintf(stderr,"WARN: The use of jumppad requires a menu; creating a dummy ENGLISH menu\n");
    pg=pgcgroup_new(type);
    menugroup_add_pgcgroup(mg,"en",pg);
}

static void ScanIfo(struct toc_summary *ts,char *ifo)
{
    static unsigned char buf[2048];
    struct vtsdef *vd;
    int i,first;
    FILE *h=fopen(ifo,"rb");
    if( !h ) {
        fprintf(stderr,"ERR:  cannot open %s: %s\n",ifo,strerror(errno));
        exit(1);
    }
    if( ts->numvts+1 >= MAXVTS ) {
        fprintf(stderr,"ERR:  Too many VTSs\n");
        exit(1);
    }
    fread(buf,1,2048,h);
    vd=&ts->vts[ts->numvts];
    if( read4(buf+0xc0)!=0 )
        vd->hasmenu=1;
    else
        vd->hasmenu=0;
    vd->numsectors=read4(buf+0xc)+1;
    memcpy(vd->vtscat,buf+0x22,4);
    memcpy(vd->vtssummary,buf+0x100,0x300);
    fread(buf,1,2048,h); // VTS_PTT_SRPT is 2nd sector
    // we only need to read the 1st sector of it because we only need the
    // pgc pointers
    vd->numtitles=read2(buf);
    vd->numchapters=(int *)malloc(sizeof(int)*vd->numtitles);
    first=8+vd->numtitles*4;
    for( i=0; i<vd->numtitles-1; i++ ) {
        int n=read4(buf+12+i*4);
        vd->numchapters[i]=(n-first)/4;
        first=n;
    }
    vd->numchapters[i]=(read4(buf+4)+1-first)/4;
    fclose(h);
    ts->numvts++;
}

static void forceaddentry(struct pgcgroup *va,int entry)
{
    if( !va->numpgcs && !jumppad )
        return;
    if( !(va->allentries&entry) ) {
        if( va->numpgcs )
            va->pgcs[0]->entries|=entry;
        va->allentries|=entry;
        va->numentries++;
    }
}

static void checkaddentry(struct pgcgroup *va,int entry)
{
    if( va->numpgcs )
        forceaddentry(va,entry);
}

static int getvtsnum(char *fbase)
{
    static char realfbase[1000];
    int i;
    
    for( i=1; i<=99; i++ ) {
        FILE *h;
        sprintf(realfbase,"%s/VIDEO_TS/VTS_%02d_0.IFO",fbase,i);
        h=fopen(realfbase,"rb");
        if( !h )
            break;
        fclose(h);
    }
    fprintf(stderr,"STAT: Picking VTS %02d\n",i);
    return i;
}

static void initdir(char *fbase)
{
    static char realfbase[1000];

    mkdir(fbase,0777);
    sprintf(realfbase,"%s/VIDEO_TS",fbase);
    mkdir(realfbase,0777);
    sprintf(realfbase,"%s/AUDIO_TS",fbase);
    mkdir(realfbase,0777);
}

static struct colorinfo *colorinfo_new()
{
    struct colorinfo *ci=malloc(sizeof(struct colorinfo));
    memcpy(ci->colors,colors,16*sizeof(int));
    return ci;
}

static struct vobgroup *vobgroup_new()
{
    struct vobgroup *vg=malloc(sizeof(struct vobgroup));
    memset(vg,0,sizeof(struct vobgroup));
    return vg;
}

static void vobgroup_addvob(struct vobgroup *pg,struct pgc *p,struct source *s)
{
    int i,forcenew;
    struct vob *v;

    forcenew=(p->numbuttons!=0);
    if( !forcenew ) {
        for( i=0; i<pg->numvobs; i++ )
            if( !strcmp(pg->vobs[i]->fname,s->fname) && pg->vobs[i]->p->numbuttons==0 )
            {
                s->vob=pg->vobs[i];
                return;
            }
    }
    pg->vobs=realloc(pg->vobs,(pg->numvobs+1)*sizeof(struct vob *));
    v=malloc(sizeof(struct vob));
    pg->vobs[pg->numvobs++]=v;
    memset(v,0,sizeof(struct vob));
    v->fname=strdup(s->fname);
    v->p=p;
    s->vob=v;
}

static void pgcgroup_pushci(struct pgcgroup *p,int warn)
{
    int i,j,ii,jj;

    for( i=0; i<p->numpgcs; i++ ) {
        if( !p->pgcs[i]->ci )
            continue;
        for( j=0; j<p->pgcs[i]->numsources; j++ ) {
            struct vob *v=p->pgcs[i]->sources[j]->vob;

            for( ii=0; ii<p->numpgcs; ii++ )
                for( jj=0; jj<p->pgcs[ii]->numsources; jj++ )
                    if( v==p->pgcs[ii]->sources[jj]->vob ) {
                        if( !p->pgcs[ii]->ci )
                            p->pgcs[ii]->ci=p->pgcs[i]->ci;
                        else if( p->pgcs[ii]->ci!=p->pgcs[i]->ci && warn) {
                            fprintf(stderr,"WARN: Conflict in colormap between PGC %d and %d\n",i,ii);
                        }
                    }
        }
    }
}

static void pgcgroup_createvobs(struct pgcgroup *p,struct vobgroup *v)
{
    int i,j;

    v->allpgcs=(struct pgc **)realloc(v->allpgcs,(v->numallpgcs+p->numpgcs)*sizeof(struct pgc *));
    memcpy(v->allpgcs+v->numallpgcs,p->pgcs,p->numpgcs*sizeof(struct pgc *));
    v->numallpgcs+=p->numpgcs;
    for( i=0; i<p->numpgcs; i++ )
        for( j=0; j<p->pgcs[i]->numsources; j++ )
            vobgroup_addvob(v,p->pgcs[i],p->pgcs[i]->sources[j]);
    pgcgroup_pushci(p,0);
    for( i=0; i<p->numpgcs; i++ )
        if( !p->pgcs[i]->ci ) {
            p->pgcs[i]->ci=colorinfo_new();
            pgcgroup_pushci(p,0);
        }
    pgcgroup_pushci(p,1);
}

static void validatesummary(struct pgcgroup *va)
{
    int i,err=0,allowedentries;

    switch(va->pstype) {
    case 1: allowedentries=0xf8; break;
    case 2: allowedentries=4; break;
        // case 0:
    default:
        allowedentries=0; break;
    }

    for( i=0; i<va->numpgcs; i++ ) {
        struct pgc *p=va->pgcs[i];
        if( !p->posti && p->numsources ) {
            struct source *s=p->sources[p->numsources-1];
            s->cells[s->numcells-1].pauselen=255;
        }
        if( va->allentries & p->entries ) {
            int j;

            for( j=0; j<8; j++ )
                if( va->allentries & p->entries & (1<<j) )
                    fprintf(stderr,"ERR:  Multiple definitions for entry %s, 2nd occurance in PGC #%d\n",entries[j],i);
            err=1;
        }
        if( p->entries & ~allowedentries ) {
            int j;

            for( j=0; j<8; j++ )
                if( p->entries & (~allowedentries) & (1<<j) )
                    fprintf(stderr,"ERR:  Entry %s is not allowed for menu type %s\n",entries[j],pstypes[va->pstype]);
            err=1;
        }
        va->allentries|=p->entries;
        if( p->numsources ) {
            int j,first;

            first=1;
            for( j=0; j<p->numsources; j++ ) {
                if( !p->sources[j]->numcells )
                    fprintf(stderr,"WARN: Source has no cells (%s) in PGC %d\n",p->sources[j]->fname,i);
                else if( first ) {
                    if( p->sources[j]->cells[0].ischapter!=1 ) {
                        fprintf(stderr,"WARN: First cell is not marked as a chapter in PGC %d, setting chapter flag\n",i);
                        p->sources[j]->cells[0].ischapter=1;
                    }
                    first=0;
                }
            }
        }
    }
    for( i=1; i<256; i<<=1 )
        if( va->allentries&i )
            va->numentries++;
    if(err)
        exit(1);
}

struct source *source_new()
{
    struct source *v=malloc(sizeof(struct source));
    memset(v,0,sizeof(struct source));
    return v;
}

int source_add_cell(struct source *v,double starttime,double endtime,int chap,int pause,const char *cmd)
{
    struct cell *c;

    v->cells=realloc(v->cells,(v->numcells+1)*sizeof(struct cell));
    c=v->cells+v->numcells;
    v->numcells++;
    c->startpts=starttime*90000+.5;
    c->endpts=endtime*90000+.5;
    c->ischapter=chap;
    c->pauselen=pause;
    if( cmd )
        c->cs=vm_parse(cmd);
    else
        c->cs=0;
    return 0;
}

void source_set_filename(struct source *v,const char *s)
{
    v->fname=strdup(s);
}

struct pgc *pgc_new()
{
    struct pgc *p=malloc(sizeof(struct pgc));
    memset(p,0,sizeof(struct pgc));
    return p;
}

void pgc_set_pre(struct pgc *p,const char *cmd)
{
    assert(!p->prei);
    p->prei=vm_parse(cmd); // this will initialize prei
}

void pgc_set_post(struct pgc *p,const char *cmd)
{
    assert(!p->posti);
    p->posti=vm_parse(cmd); // this will initialize posti
}

void pgc_set_color(struct pgc *p,int index,int color)
{
    assert(index>=0 && index<16);
    if( !p->ci ) p->ci=colorinfo_new();
    p->ci->colors[index]=color;
}

void pgc_set_stilltime(struct pgc *p,int still)
{
    p->pauselen=still;
}

void pgc_add_entry(struct pgc *p,char *entry)
{
    int i;
   
    for( i=2; i<8; i++ )
        if( !strcasecmp(entry,entries[i])) {
            int v=1<<i;
            if( p->entries&v ) {
                fprintf(stderr,"ERR:  Defined entry '%s' multiple times for the same PGC\n",entry);
                exit(1);
            }
            p->entries|=1<<i;
            return;
        }
    fprintf(stderr,"ERR:  Unknown entry '%s'\n",entry);
    exit(1);
}

void pgc_add_source(struct pgc *p,struct source *v)
{
    p->sources=(struct source **)realloc(p->sources,(p->numsources+1)*sizeof(struct source *));
    p->sources[p->numsources++]=v;
}

int pgc_add_button(struct pgc *p,const char *name,const char *cmd)
{
    struct button *bs;

    if( p->numbuttons==36 ) {
        fprintf(stderr,"ERR:  Limit of up to 36 buttons\n");
        exit(1);
    }
    p->buttons=(struct button *)realloc(p->buttons,(p->numbuttons+1)*sizeof(struct button));
    bs=&p->buttons[p->numbuttons++];
    memset(bs,0,sizeof(struct button));
    if( name )
        bs->name=strdup(name);
    else {
        char nm[10];
        sprintf(nm,"%d",p->numbuttons);
        bs->name=strdup(nm);
    }
    bs->cs=vm_parse(cmd);
    return 0;
}

struct pgcgroup *pgcgroup_new(int type)
{
    struct pgcgroup *ps=malloc(sizeof(struct pgcgroup));
    memset(ps,0,sizeof(struct pgcgroup));
    ps->pstype=type;
    if( !type )
        ps->vg=vobgroup_new();
    return ps;
}

void pgcgroup_add_pgc(struct pgcgroup *ps,struct pgc *p)
{
    ps->pgcs=(struct pgc **)realloc(ps->pgcs,(ps->numpgcs+1)*sizeof(struct pgc *));
    ps->pgcs[ps->numpgcs++]=p;
}

int pgcgroup_set_video_attr(struct pgcgroup *va,int attr,char *s)
{
    return vobgroup_set_video_attr(va->vg,attr,s);
}

int pgcgroup_set_audio_attr(struct pgcgroup *va,int attr,char *s,int ch)
{
    return vobgroup_set_audio_attr(va->vg,attr,s,ch);
}

int pgcgroup_set_subpic_attr(struct pgcgroup *va,int attr,char *s,int ch)
{
    return vobgroup_set_subpic_attr(va->vg,attr,s,ch);
}

struct menugroup *menugroup_new()
{
    struct menugroup *mg=malloc(sizeof(struct menugroup));
    memset(mg,0,sizeof(struct menugroup));
    mg->vg=vobgroup_new();
    return mg;
}

void menugroup_add_pgcgroup(struct menugroup *mg,char *lang,struct pgcgroup *pg)
{
    mg->groups=(struct langgroup *)realloc(mg->groups,(mg->numgroups+1)*sizeof(struct langgroup));
    if( strlen(lang)!=2 ) {
        fprintf(stderr,"ERR:  Menu language '%s' is not two letters.\n",lang);
        exit(1);
    }
    mg->groups[mg->numgroups].lang[0]=tolower(lang[0]);
    mg->groups[mg->numgroups].lang[1]=tolower(lang[1]);
    mg->groups[mg->numgroups].pg=pg;
    mg->numgroups++;
}

int menugroup_set_video_attr(struct menugroup *va,int attr,char *s)
{
    return vobgroup_set_video_attr(va->vg,attr,s);
}

int menugroup_set_audio_attr(struct menugroup *va,int attr,char *s,int ch)
{
    return vobgroup_set_audio_attr(va->vg,attr,s,ch);
}

int menugroup_set_subpic_attr(struct menugroup *va,int attr,char *s,int ch)
{
    return vobgroup_set_subpic_attr(va->vg,attr,s,ch);
}

void dvdauthor_enable_jumppad()
{
    jumppad=1;
}

void dvdauthor_vmgm_gen(struct pgc *fpc,struct menugroup *menus,char *fbase)
{
    DIR *d;
    struct dirent *de;
    char *vtsdir;
    int i;
    static struct toc_summary ts;
    static char fbuf[1000];
    static char ifonames[101][14];
    struct workset ws;

    ws.ts=&ts;
    ws.menus=menus;
    ws.titles=0;
    jp_force_menu(menus,2);
    for( i=0; i<menus->numgroups; i++ ) {
        validatesummary(menus->groups[i].pg);
        pgcgroup_createvobs(menus->groups[i].pg,menus->vg);
        forceaddentry(menus->groups[i].pg,4);
    }
    fprintf(stderr,"INFO: dvdauthor creating table of contents\n");
    initdir(fbase);
    // create base entry, if not already existing
    memset(&ts,0,sizeof(struct toc_summary));
    vtsdir=makevtsdir(fbase);
    for( i=0; i<101; i++ )
        ifonames[i][0]=0;
    d=opendir(vtsdir);
    while ((de=readdir(d))!=0) {
        i=strlen(de->d_name);
        if( i==12 && !strcasecmp(de->d_name+i-6,"_0.IFO") &&
            !strncasecmp(de->d_name,"VTS_",4)) {
            i=(de->d_name[4]-'0')*10+(de->d_name[5]-'0');
            if( ifonames[i][0] ) {
                fprintf(stderr,"ERR:  Two different names for the same titleset: %s and %s\n",ifonames[i],de->d_name);
                exit(1);
            }
            if( !i ) {
                fprintf(stderr,"ERR:  Cannot have titleset #0 (%s)\n",de->d_name);
                exit(1);
            }
            strcpy(ifonames[i],de->d_name);
        }
    }
    closedir(d);
    for( i=1; i<=99; i++ ) {
        if( !ifonames[i][0] )
            break;
        sprintf(fbuf,"%s/%s",vtsdir,ifonames[i]);
        fprintf(stderr,"INFO: Scanning %s\n",fbuf);
        ScanIfo(&ts,fbuf);
    }
    for( ; i<=99; i++ )
        if( ifonames[i][0] ) {
            fprintf(stderr,"ERR:  Titleset #%d (%s) does not immediately follow the last titleset\n",i,ifonames[i]);
            exit(1);
        }
    if( !ts.numvts ) {
        fprintf(stderr,"ERR:  No .IFO files to process\n");
        exit(1);
    }
    if( menus->vg->numvobs ) {
        fprintf(stderr,"INFO: Creating menu for TOC\n");
        sprintf(fbuf,"%s/VIDEO_TS.VOB",vtsdir);
        FindVobus(fbuf,menus->vg,2);
        MarkChapters(menus->vg);
        setattr(menus->vg,2);
        fprintf(stderr,"\n");
        FixVobus(fbuf,menus->vg,2);
    }
    sprintf(fbuf,"%s/VIDEO_TS.IFO",vtsdir);
    TocGen(&ws,fpc,fbuf);
    sprintf(fbuf,"%s/VIDEO_TS.BUP",vtsdir);
    TocGen(&ws,fpc,fbuf);
}

void dvdauthor_vts_gen(struct menugroup *menus,struct pgcgroup *titles,char *fbase)
{
    int vtsnum,i;
    static char realfbase[1000];
    struct workset ws;

    fprintf(stderr,"INFO: dvdauthor creating VTS\n");
    initdir(fbase);
    ws.ts=0;
    ws.menus=menus;
    ws.titles=titles;
    jp_force_menu(menus,1);
    for( i=0; i<menus->numgroups; i++ ) {
        validatesummary(menus->groups[i].pg);
        pgcgroup_createvobs(menus->groups[i].pg,menus->vg);
        forceaddentry(menus->groups[i].pg,0x80);
        checkaddentry(menus->groups[i].pg,0x08);
    }
    validatesummary(titles);
    pgcgroup_createvobs(titles,titles->vg);
    if( titles->numpgcs==0 ) {
        fprintf(stderr,"ERR:  no titles defined\n");
        exit(1);
    }
    vtsnum=getvtsnum(fbase);
    sprintf(realfbase,"%s/VIDEO_TS/VTS_%02d",fbase,vtsnum);
    if( menus->vg->numvobs ) {
        FindVobus(realfbase,menus->vg,1);
        MarkChapters(menus->vg);
        setattr(menus->vg,1);
    }
    FindVobus(realfbase,titles->vg,0);
    MarkChapters(titles->vg);
    setattr(titles->vg,0);
    if( !menus->vg->numvobs ) { // for undefined menus, we'll just copy the video type of the title
        menus->vg->vd=titles->vg->vd;
    }
    fprintf(stderr,"\n");
    WriteIFOs(realfbase,&ws);
    if( menus->vg->numvobs )
        FixVobus(realfbase,menus->vg,1);
    FixVobus(realfbase,titles->vg,0);
}
