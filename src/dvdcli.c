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

#include "conffile.h"
#include "dvdauthor.h"
#include "readxml.h"
#include "rgb.h"

static const char RCSID[]="$Id: //depot/dvdauthor/src/dvdcli.c#52 $";

#define RGB2YCrCb(R,G,B) ((((int)RGB2Y(R,G,B))<<16)|(((int)RGB2Cr(R,G,B))<<8)|(((int)RGB2Cb(R,G,B))))

static int readdvdauthorxml(const char *xmlfile,char *fb);

static int hadchapter=0,pauselen=0;
static char *chapters=0;

static void parsevideoopts(struct pgcgroup *va,char *o)
{
    char *s;
    while(NULL!=(s=strsep(&o,"+"))) {
        if(pgcgroup_set_video_attr(va,VIDEO_ANY,s)) {
            fprintf(stderr,"ERR:  Video option '%s' overrides previous option\n",s);
            exit(1);
        }
    }
}

static void parseaudiotrack(struct pgcgroup *va,char *o,int c)
{
    char *s;
    while(NULL!=(s=strsep(&o,"+"))) {
        if(pgcgroup_set_audio_attr(va,AUDIO_ANY,s,c)) {
            fprintf(stderr,"ERR:  Audio option '%s' on track %d overrides previous option\n",s,c);
            exit(1);
        }
    }
}

static void parseaudioopts(struct pgcgroup *va,char *o)
{
    char *s;
    int ch=0;
    while(NULL!=(s=strsep(&o,", "))) {
        parseaudiotrack(va,s,ch);
        ch++;
    }
}

static void parsesubpicturetrack(struct pgcgroup *va,char *o,int c)
{
    char *s;
    while(NULL!=(s=strsep(&o,"+"))) {
        if(pgcgroup_set_subpic_attr(va,SPU_ANY,s,c)) {
            fprintf(stderr,"ERR:  Subpicture option '%s' on track %d overrides previous option\n",s,c);
            exit(1);
        }
    }
}

static void parsesubpictureopts(struct pgcgroup *va,char *o)
{
    char *s;
    int ch=0;
    while(NULL!=(s=strsep(&o,", "))) {
        parsesubpicturetrack(va,s,ch);
        ch++;
    }
}

static void parsebutton(struct pgc *va,char *b)
{
    char *bnm=0;

    if( strchr(b,'=') ) {
        char *p=strchr(b,'=');
        p[0]=0;
        bnm=b;
        b=p+1;
    }
    pgc_add_button(va,bnm,b);
}

static void parseinstructions(struct pgc *va,char *b)
{
    char *c=strsep(&b,"=");
    if(!strcasecmp(c,"post")) {
        pgc_set_post(va,b);
    } else if(!strcasecmp(c,"pre")) {
        pgc_set_pre(va,b);
    } else {
        fprintf(stderr,"Unknown instruction block: %s\n",c);
        exit(1);
    }
}

static void parseentries(struct pgc *p,char *b)
{
    char *v;

    while(NULL!=(v=strsep(&b,", ")))
        pgc_add_entry(p,v);
}

static double parsechapter(char *s)
{
    double total=0,field=0;
    int i;

    for( i=0; s[i]; i++ ) {
        if(!strchr(s+i,':')) {
            char *last;
            field=strtod(s+i,&last);
            if( *last ) {
                fprintf(stderr,"ERR:  Cannot parse chapter timestamp '%s'\n",s);
                exit(1);
            }
            break;
        } else if(isdigit(s[i]))
            field=field*10+s[i]-'0';
        else if(s[i]==':') {
            total=total*60+field;
            field=0;
        } else {
            fprintf(stderr,"ERR:  Cannot parse chapter timestamp '%s'\n",s);
            exit(1);
        }
    }
    return total*60+field;
}

static void parsechapters(char *o,struct source *src,int pauselen)
{
    char *s;
    double last=0;
    int lastchap=0;

    while(NULL!=(s=strsep(&o,", "))) {
        double total=parsechapter(s);
        if( total>last ) {
            source_add_cell(src,last,total,lastchap,0,0);
            last=total;
            lastchap=1;
        } else if( total==last )
            lastchap=1;
    }
    source_add_cell(src,last,-1,lastchap,pauselen,0);
}

static void readpalette(struct pgc *p,const char *fname)
{
    int i,rgbf;
    FILE *h;

    if(fname[0] == '&' && isdigit(fname[1]) )
        h=fdopen(atoi(&fname[1]),"rb");
    else
        h=fopen(fname,"rb");
    
    if( !h ) {
        fprintf(stderr,"ERR:  Cannot open palette file '%s'\n",fname);
        exit(1);
    }
    /* write out colors, the hex is the 0yuv combined in one integer 00yyuuvv */
    i=strlen(fname);
    rgbf=( i>=4 && !strcasecmp(fname+i-4,".rgb") );
    for( i=0; i<16; i++ ) {
        int pcolor;
        fscanf(h, "%x", &pcolor);
        if( rgbf ) {
            int r=(pcolor>>16)&255,
                g=(pcolor>>8)&255,
                b=(pcolor)&255;
            pcolor=RGB2YCrCb(r,g,b);
        }
        pgc_set_color(p,i,pcolor);
    }

#if 0
    int b;
    unsigned char groups[24];

    memset(groups,0,24);
    i=0;
    while(fscanf(h,"%x",&b) == 1 && i<24 ) {
        groups[i++]=b>>24;
        groups[i++]=b>>16;
        groups[i++]=b>>8;
        groups[i++]=b;
        if( !(i&7) )
            pgc_set_buttongroup(p,(i-8)/8,groups+(i-8));
    }
#endif
    fclose(h);
}

static void usage()
{
#ifdef HAVE_GETOPT_LONG
#define LONGOPT(x) x
#define LONGOPT2(x,y) x
#else
#define LONGOPT(x)
#define LONGOPT2(x,y) y
#endif

    fprintf(stderr,
            "syntax: dvdauthor [-o VTSBASE] [options] VOBFILE(s)\n"
            "\n\t-x XMLFILE where XMLFILE is a configuration file describing the\n"
            "\t    structure of the DVD to create.  If you use a config file, then you\n"
            "\t    do not need to specify any other options, except -o.\n"
            "\n\t" LONGOPT("--video=VOPTS or ") "-v VOPTS where VOPTS is a plus (+) separated list of\n"
            "\t    video options.  dvdauthor will try to infer any unspecified options.\n"
            "\t\tpal, ntsc, 4:3, 16:9, 720xfull, 720x576, 720x480, 704xfull,\n"
            "\t\t704x576, 704x480, 352xfull, 352x576, 352x480, 352xhalf,\n"
            "\t\t352x288, 352x240, nopanscan, noletterbox.\n"
            "\t    Default is ntsc, 4:3, 720xfull\n"
            "\n\t" LONGOPT("--audio=AOPTS or ") "-a AOPTS where AOPTS is a plus (+) separated list of\n"
            "\t    options for an audio track, with each track separated by a\n"
            "\t    comma (,).  For example -a ac3+en,mp2+de specifies two audio\n"
            "\t    tracks: the first is an English track encoded in AC3, the second is\n"
            "\t    a German track encoded using MPEG-1 layer 2 compression.\n"
            "\t\tac3, mp2, pcm, dts, 16bps, 20bps, 24bps, drc, surround, nolang,\n"
            "\t\t1ch, 2ch, 3ch, 4ch, 5ch, 6ch, 7ch, 8ch, and any two letter\n"
            "\t\tISO 639 language abbreviation.\n"
            "\t    Default is 1 track, mp2, 20bps, nolang, 2ch.\n"
            "\t    'ac3' implies drc, 6ch.\n"
            "\n\t" LONGOPT("--subpictures=SOPTS or ") "-s SOPTS where SOPTS is a plus (+) separated list\n"
            "\t    of options for a subpicture track, with each track separated by a\n"
            "\t    comma (,).\n"
            "\t\tnolang and any two letter language abbreviation (see -a)\n"
            "\t    Default is no subpicture tracks.\n"
            "\n\t" LONGOPT("--palette[=FILE] or ") "-p FILE or -P where FILE specifies where to get the\n"
            "\t    subpicture palette.  Settable per title and per menu.  If the\n"
            "\t    filename ends in .rgb (case insensitive) then it is assumed to be\n"
            "\t    RGB, otherwise it is YUV.  Entries should be 6 hexadecimal digits.\n"
            "\t    FILE defaults to xste-palette.dat\n"
            "\n\t" LONGOPT2("--file=FILE or -f FILE or FILE","-f FILE") " where FILE is either a file, a pipe, or a\n"
            "\t    shell command ending in | which supplies an MPEG-2 system stream\n"
            "\t    with VOB sectors inserted in the appropriate places\n"
            "\t    (using mplex -f 8 to generate)\n"
            "\n\t" LONGOPT("--chapter[s][=COPTS] or ") "-c COPTS or -C where COPTS is a comma (,)\n"
            "\t    separated list of chapter markers.  Each marker is of the form\n"
            "\t    [[h:]mm:]ss[.frac] and is relative to the SCR of the next file\n"
            "\t    listed (independent of any timestamp transposing that occurs within\n"
            "\t    dvdauthor).  The chapter markers ONLY apply to the next file listed.\n"
            "\t    COPTS defaults to 0\n"
            "\n\t" LONGOPT("--menu or ") "-m creates a menu.\n"
            "\n\t" LONGOPT("--title or ") "-t creates a title.\n"
            "\n\t" LONGOPT("--toc or ") "-T creates the table of contents file instead of a titleset.\n"
            "\t    If this option is used, it should be listed first, and you may not\n"
            "\t    specify any titles.\n"
            "\n\t" LONGOPT("--entry=EOPTS or ") "-e EOPTS makes the current menu the default for\n"
            "\t    certain circumstances.  EOPTS is a comma separated list of any of:\n"
            "\t\tfor TOC menus: title\n"
            "\t\tfor VTS menus: root, ptt, audio, subtitle, angle\n"
            "\n\t" LONGOPT("--button or ") "-b DEST specifies what command to issue for each button.\n"
            "\t    See " LONGOPT("--instructions or ") "-i for a description of\n"
            "\t    DEST.\n"
            "\n\t" LONGOPT("--instructions or ") "-i post=DEST executes the DEST instructions at the\n"
            "\t    end of the title.\n"
            "\n\t" LONGOPT("--fpc or ") "-F CMD sets the commands to be executed when the disc is first\n"
            "\t    inserted.\n"
            "\n\t" LONGOPT("--jumppad or ") "-j enables the creation of jumppads, which allow greater\n"
            "\t    flexibility in choosing jump/call desinations.\n"
            "\n\t" LONGOPT("--help or ") "-h displays this screen.\n"
        );
    exit(1);
}

#define MAINDEF                                                      \
            if( istoc && curva ) {                                   \
                fprintf(stderr,"ERR:  TOC cannot have titles\n");    \
                return 1;                                            \
            }                                                        \
            usedtocflag=1;                                           \
            if( !vc ) {                                              \
                va[curva]=vc=pgcgroup_new(istoc+1-curva);             \
            }

#define MAINDEFPGC                                                   \
            MAINDEF;                                                 \
            if( !curpgc )                                            \
                curpgc=pgc_new();

#define MAINDEFVOB                                                   \
            MAINDEFPGC;                                              \
            if( !curvob )                                            \
                curvob=source_new();

#define FLUSHPGC                                                     \
            if( curpgc ) {                                           \
                pgcgroup_add_pgc(va[curva],curpgc);                   \
                curpgc=0;                                            \
            }

int main(int argc,char **argv)
{
    struct pgcgroup *va[2];
    struct menugroup *mg;
    char *fbase=0;
    int curva=1,istoc=0,l,
        usedtocflag=0; // whether the 'istoc' value has been used or not
    struct pgc *curpgc=0,*fpc=0;
    struct source *curvob=0;
#ifdef HAVE_GETOPT_LONG
    static struct option longopts[]={
        {"video",1,0,'v'},
        {"audio",1,0,'a'},
        {"subpictures",1,0,'s'},
        {"palette",2,0,'p'},
        {"file",1,0,'f'},
        {"chapters",2,0,'c'},
        {"help",0,0,'h'},
        {"menu",0,0,'m'},
        {"title",0,0,'t'},
        {"button",1,0,'b'},
        {"toc",0,0,'T'},
        {"instructions",1,0,'i'},
        {"entry",1,0,'e'},
        {"fpc",1,0,'F'},
        {"jumppad",0,0,'j'},
        {0,0,0,0}
    };
#define GETOPTFUNC(x,y,z) getopt_long(x,y,"-" z,longopts,NULL)
#else
#define GETOPTFUNC(x,y,z) getopt(x,y,z)
#endif

    fputs(PACKAGE_HEADER("dvdauthor"),stderr);

    if( argc<1 ) {
        fprintf(stderr,"ERR:  No arguments!\n");
        return 1;
    }
    memset(va,0,sizeof(struct pgcgroup *)*2);

    while(1) {
        struct pgcgroup *vc=va[curva];
        int c=GETOPTFUNC(argc,argv,"f:o:v:a:s:hc:Cp:Pmtb:Ti:e:x:");
        if( c == -1 )
            break;
        switch(c) {
        case 'h':
            usage();
            break;

        case 'x':
            if( usedtocflag ) {
                fprintf(stderr,"ERR:  Cannot specify XML config file after using command line options\n");
                return 1;
            }
            return readdvdauthorxml(optarg,fbase);

        case 'j':
            dvdauthor_enable_jumppad();
            break;

        case 'T':
            if( usedtocflag ) {
                fprintf(stderr,"ERR:  TOC (-T) option must come first because I am a lazy programmer.\n");
                return 1;
            }
            istoc=1;
            usedtocflag=1; // just for completeness (also see -x FOO)
            break;

        case 'o': 
            fbase=optarg;
            break;

        case 'm':
            FLUSHPGC;
            usedtocflag=1; // force -T to occur before -m
            hadchapter=0;
            curva=0;
            break;

        case 't':
            if( istoc ) {
                fprintf(stderr,"ERR:  TOC cannot have titles\n");
                return 1;
            }
            FLUSHPGC;
            usedtocflag=1;
            hadchapter=0;
            curva=1;
            break;
            
        case 'a':
            MAINDEF;
            parseaudioopts(vc,optarg);
            break;
        
        case 'v':
            MAINDEF;
            parsevideoopts(vc,optarg);
            break;

        case 's':
            MAINDEF;
            parsesubpictureopts(vc,optarg);
            break;

        case 'b':
            MAINDEFPGC;
            parsebutton(curpgc,optarg);
            break;

        case 'i':
            MAINDEFPGC;
            parseinstructions(curpgc,optarg);
            break;

        case 'F':
            if( !istoc ) {
                fprintf(stderr,"ERR:  You may only specify FPC commands on the VMGM\n");
                return 1;
            }
            usedtocflag=1;
            if( fpc ) {
                fprintf(stderr,"ERR:  FPC commands already specified\n");
                return 1;
            }
            fpc=pgc_new();
            pgc_set_pre(fpc,optarg);
            break;

        case 'e':
            MAINDEFPGC;
            if( curva ) {
                fprintf(stderr,"ERR:  Cannot specify an entry for a title.\n");
                return 1;
            }
            parseentries(curpgc,optarg);
            break;

        case 'P': optarg=0;
        case 'p':
            MAINDEFPGC;
            readpalette(curpgc,optarg?optarg:"xste-palette.dat");
            break;

        case 1:
        case 'f':
            MAINDEFVOB;
            
            source_set_filename(curvob,optarg);
            if( hadchapter==2 )
                hadchapter=1;
            else
                source_add_cell(curvob,0,-1,!hadchapter,0,0);
            pgc_add_source(curpgc,curvob);
            curvob=0;
            break;

        case 'C': optarg=0;
        case 'c':
            if( curvob ) {
                fprintf(stderr,"ERR:  cannot list -c twice for one file.\n");
                return 1;
            }
            MAINDEFVOB;

            hadchapter=2;
            if( optarg )
                parsechapters(optarg,curvob,0);
            else
                source_add_cell(curvob,0,-1,1,0,0);
            break;

        default:
            fprintf(stderr,"ERR:  getopt returned bad code %d\n",c);
            return 1;
        }
    }
    if( curvob ) {
        fprintf(stderr,"ERR:  Chapters defined without a file source.\n");
        return 1;
    }
    FLUSHPGC;
    if( !va[0] ) {
        va[0]=pgcgroup_new(istoc+1);
        mg=menugroup_new();
        menugroup_add_pgcgroup(mg,"en",va[0]);
    } else
        mg=0;
    if( !va[1] && !istoc )
        va[1]=pgcgroup_new(0);

    if( !fbase ) {
        fbase=readconfentry("WORKDIR");
        if( !fbase )
            usage();
    }
    if( optind != argc ) {
        fprintf(stderr,"ERR:  bad version of getopt; please precede all sources with '-f'\n");
        return 1;
    }
    l=strlen(fbase);
    if( l && fbase[l-1]=='/' )
        fbase[l-1]=0;

    if( istoc )
        dvdauthor_vmgm_gen(fpc,mg,fbase);
    else
        dvdauthor_vts_gen(mg,va[1],fbase);

    return 0;
}

static struct pgcgroup *titles=0, *curgroup=0;
static struct menugroup *mg=0, *vmgmmenus=0;
static struct pgc *curpgc=0,*fpc=0;
static struct source *curvob=0;
static char *fbase=0,*buttonname=0;
static int ismenuf=0,istoc=0,setvideo=0,setaudio=0,setsubpicture=0,hadtoc=0;
static int vobbasic,cell_chapter;
static double cell_starttime,cell_endtime;
static char menulang[3];

static void set_video_attr(int attr,char *s)
{
    if( ismenuf )
        menugroup_set_video_attr(mg,attr,s);
    else
        pgcgroup_set_video_attr(titles,attr,s);
}

static void set_audio_attr(int attr,char *s,int ch)
{
    if( ismenuf )
        menugroup_set_audio_attr(mg,attr,s,ch);
    else
        pgcgroup_set_audio_attr(titles,attr,s,ch);
}

static void set_subpic_attr(int attr,char *s,int ch)
{
    if( ismenuf )
        menugroup_set_subpic_attr(mg,attr,s,ch);
    else
        pgcgroup_set_subpic_attr(titles,attr,s,ch);
}

static int parse_pause(char *f)
{
    if( !strcmp(f,"inf") )
        return 255;
    else
        return atoi(f);
}

static void dvdauthor_workdir(char *s)
{
    fbase=utf8tolocal(s);
}

static void dvdauthor_jumppad(char *s)
{
    int i=xml_ison(s);
    if( i==1 )
        dvdauthor_enable_jumppad();
    else if( i==-1 ) {
        fprintf(stderr,"ERR:  Unknown jumppad cmd '%s'\n",s);
        exit(1);
    }
}

static void getfbase()
{
    if( !fbase ) {
        fbase=readconfentry("WORKDIR");
        if( !fbase ) {
            fprintf(stderr,"ERR:  Must specify working directory\n");
            parser_err=1;
        }
    }
}

static void dvdauthor_end()
{
    if( fbase && hadtoc ) {
        dvdauthor_vmgm_gen(fpc,vmgmmenus,fbase);
        vmgmmenus=0;
        fpc=0;
    }
}

static void titleset_start()
{
    mg=menugroup_new();
    istoc=0;
}

static void titleset_end()
{
    getfbase();
    if( fbase ) {
        if( !titles )
            titles=pgcgroup_new(0);
        dvdauthor_vts_gen(mg,titles,fbase);
        mg=0;
        titles=0;
    }
}

static void vmgm_start()
{
    if( hadtoc ) {
        fprintf(stderr,"ERR:  Can only define one VMGM\n");
        parser_err=1;
        return;
    }
    mg=menugroup_new();
    istoc=1;
    hadtoc=1;
}

static void vmgm_end()
{
    getfbase();
    // we put off compilation of vmgm until after the titlesets
    vmgmmenus=mg;
    mg=0;
}

static void fpc_start()
{
    if( !istoc ) {
        fprintf(stderr,"ERR:  Can only specify <fpc> under <vmgm>\n");
        parser_err=1;
        return;
    }
    if( fpc ) {
        fprintf(stderr,"ERR:  Already defined <fpc>\n");
        parser_err=1;
        return;
    }
    fpc=pgc_new();
    parser_acceptbody=1;
}

static void fpc_end()
{
    pgc_set_pre(fpc,parser_body);
}

static void pgcgroup_start()
{
    setvideo=0;
    setaudio=-1;
    setsubpicture=-1;
}

static void titles_start()
{
    if( titles ) {
        fprintf(stderr,"ERR:  Titles already defined\n");
        parser_err=1;
    } else if( istoc ) {
        fprintf(stderr,"ERR:  Cannot have titles in a VMGM\n");
        parser_err=1;
    } else {
        titles=pgcgroup_new(0);
        curgroup=titles;
        ismenuf=0;
        pgcgroup_start();
    }
}

static void menus_start()
{
    ismenuf=(istoc?2:1);
    curgroup=pgcgroup_new(ismenuf);
    pgcgroup_start();
    strcpy(menulang,"en");
}

static void menus_lang(char *lang)
{
    strcpy(menulang,lang);
}

static void menus_end()
{
    menugroup_add_pgcgroup(mg,menulang,curgroup);
    curgroup=0;
}

static void video_start()
{
    if( setvideo ) {
        fprintf(stderr,"ERR:  Already defined video characteristics for this PGC group\n");
        parser_err=1;
    } else
        setvideo=1;
}

static void video_format(char *c)
{
    set_video_attr(VIDEO_FORMAT,c);
}

static void video_aspect(char *c)
{
    set_video_attr(VIDEO_ASPECT,c);
}

static void video_resolution(char *c)
{
    set_video_attr(VIDEO_RESOLUTION,c);
}

static void video_widescreen(char *c)
{
    set_video_attr(VIDEO_WIDESCREEN,c);
}

static void video_caption(char *c)
{
    set_video_attr(VIDEO_CAPTION,c);
}

static void audio_start()
{
    setaudio++;
    if( setaudio>=8 ) {
        fprintf(stderr,"ERR:  Attempting to define too many audio streams for this PGC group\n");
        parser_err=1;
    }
}

static void audio_format(char *c)
{
    set_audio_attr(AUDIO_FORMAT,c,setaudio);
}

static void audio_quant(char *c)
{
    set_audio_attr(AUDIO_QUANT,c,setaudio);
}

static void audio_dolby(char *c)
{
    set_audio_attr(AUDIO_DOLBY,c,setaudio);
}

static void audio_lang(char *c)
{
    set_audio_attr(AUDIO_LANG,c,setaudio);
}

static void audio_samplerate(char *c)
{
    set_audio_attr(AUDIO_SAMPLERATE,c,setaudio);
}

static void audio_channels(char *c)
{
    char ch[4];

    if( strlen(c) == 1 ) {
        ch[0]=c[0];
        ch[1]='c';
        ch[2]='h';
        ch[3]=0;
        c=ch;
    }
    set_audio_attr(AUDIO_CHANNELS,ch,setaudio);
}

static void subattr_start()
{
    setsubpicture++;
    if( setsubpicture>=32 ) {
        fprintf(stderr,"ERR:  Attempting to define too many subpicture streams for this PGC group\n");
        parser_err=1;
    }
}

static void subattr_lang(char *c)
{
    set_subpic_attr(SPU_LANG,c,setsubpicture);
}

static void pgc_start()
{
    curpgc=pgc_new();
    hadchapter=0;
}

static void pgc_entry(char *e)
{
    // xml attributes can only be defined once, so entry="foo" entry="bar" won't work
    // instead, use parseentries...
    parseentries(curpgc,e);
}

static void pgc_palette(char *p)
{
    readpalette(curpgc,p);
}

static void pgc_pause(char *c)
{
    pgc_set_stilltime(curpgc,parse_pause(c));
}

static void pgc_end()
{
    pgcgroup_add_pgc(curgroup,curpgc);
    curpgc=0;
}

static void pre_start()
{
    parser_acceptbody=1;
}

static void pre_end()
{
    pgc_set_pre(curpgc,parser_body);
}

static void post_start()
{
    parser_acceptbody=1;
}

static void post_end()
{
    pgc_set_post(curpgc,parser_body);
}

static void vob_start()
{
    curvob=source_new();
    pauselen=0;
    vobbasic=0;
    cell_endtime=0;
}

static void vob_file(char *f)
{
    source_set_filename(curvob,utf8tolocal(f));
}

static void vob_chapters(char *c)
{
    vobbasic=1;
    hadchapter=2;
    chapters=strdup(c);
}

static void vob_pause(char *c)
{
    vobbasic=1;
    pauselen=parse_pause(c);
}

static void vob_end()
{
    if( vobbasic!=2 ) {
        if( hadchapter==2 ) {
            parsechapters(chapters,curvob,pauselen);
            hadchapter=1;
        } else
            source_add_cell(curvob,0,-1,!hadchapter,pauselen,0);
    }
    pgc_add_source(curpgc,curvob);
    curvob=0;
}

static void cell_start()
{
    parser_acceptbody=1;
    assert(vobbasic!=1);
    vobbasic=2;
    cell_starttime=cell_endtime;
    cell_endtime=-1;
    cell_chapter=0;
    pauselen=0;
    hadchapter=1;
}

static void cell_parsestart(char *f)
{
    cell_starttime=parsechapter(f);
}

static void cell_parseend(char *f)
{
    cell_endtime=parsechapter(f);
}

static void cell_parsechapter(char *f)
{
    int i=xml_ison(f);
    if(i==-1) {
        fprintf(stderr,"ERR:  Unknown chapter cmd '%s'\n",f);
        exit(1);
    } else if (i)
        cell_chapter=1;
}

static void cell_parseprogram(char *f)
{
    int i=xml_ison(f);
    if(i==-1) {
        fprintf(stderr,"ERR:  Unknown program cmd '%s'\n",f);
        exit(1);
    } else if (i && cell_chapter!=1 )
        cell_chapter=2;
}

static void cell_pauselen(char *f)
{
    pauselen=parse_pause(f);
}

static void cell_end()
{
    assert( cell_starttime>=0 );
    source_add_cell(curvob,cell_starttime,cell_endtime,cell_chapter,pauselen,parser_body);
    pauselen=0;
}

static void button_start()
{
    parser_acceptbody=1;
    buttonname=0;
}

static void button_name(char *f)
{
    buttonname=strdup(f);
}

static void button_end()
{
    pgc_add_button(curpgc,buttonname,parser_body);
    if(buttonname) free(buttonname);
}

enum {
    DA_BEGIN=0,
    DA_ROOT,
    DA_SET,
    DA_PGCGROUP,
    DA_PGC,
    DA_VOB,
    DA_NOSUB
};

static struct elemdesc elems[]={
    {"dvdauthor", DA_BEGIN,   DA_ROOT,    0,               dvdauthor_end},
    {"titleset",  DA_ROOT,    DA_SET,     titleset_start,  titleset_end},
    {"vmgm",      DA_ROOT,    DA_SET,     vmgm_start,      vmgm_end},
    {"fpc",       DA_SET,     DA_NOSUB,   fpc_start,       fpc_end},
    {"titles",    DA_SET,     DA_PGCGROUP,titles_start,    0},
    {"menus",     DA_SET,     DA_PGCGROUP,menus_start,     menus_end},
    {"video",     DA_PGCGROUP,DA_NOSUB,   video_start,     0},
    {"audio",     DA_PGCGROUP,DA_NOSUB,   audio_start,     0},
    {"subpicture",DA_PGCGROUP,DA_NOSUB,   subattr_start,   0},
    {"pgc",       DA_PGCGROUP,DA_PGC,     pgc_start,       pgc_end},
    {"pre",       DA_PGC,     DA_NOSUB,   pre_start,       pre_end},
    {"post",      DA_PGC,     DA_NOSUB,   post_start,      post_end},
    {"button",    DA_PGC,     DA_NOSUB,   button_start,    button_end},
    {"vob",       DA_PGC,     DA_VOB,     vob_start,       vob_end},
    {"cell",      DA_VOB,     DA_NOSUB,   cell_start,      cell_end},
    {0,0,0,0,0}
};

static struct elemattr attrs[]={
    {"dvdauthor","dest",dvdauthor_workdir},
    {"dvdauthor","jumppad",dvdauthor_jumppad},

    {"menus","lang",menus_lang},

    {"vob","file",vob_file},
    {"vob","chapters",vob_chapters},
    {"vob","pause",vob_pause},

    {"cell","start",cell_parsestart},
    {"cell","end",cell_parseend},
    {"cell","chapter",cell_parsechapter},
    {"cell","program",cell_parseprogram},
    {"cell","pause",cell_pauselen},

    {"button","name",button_name},

    {"video","format",video_format},
    {"video","aspect",video_aspect},
    {"video","resolution",video_resolution},
    {"video","widescreen",video_widescreen},
    {"video","caption",video_caption},
    {"audio","format",audio_format},
    {"audio","quant",audio_quant},
    {"audio","dolby",audio_dolby},
    {"audio","lang",audio_lang},
    {"audio","channels",audio_channels},
    {"audio","samplerate",audio_samplerate},
    {"subpicture","lang",subattr_lang},
    {"pgc","entry",pgc_entry},
    {"pgc","palette",pgc_palette},
    {"pgc","pause",pgc_pause},
    {0,0,0}
};

static int readdvdauthorxml(const char *xmlfile,char *fb)
{
    fbase=fb;
    return readxml(xmlfile,elems,attrs);
}
