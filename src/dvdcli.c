/*
    dvdauthor mainline, interpretation of command-line options and parsing of
    dvdauthor XML control files
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

#include <assert.h>
#include <ctype.h>
#include <fcntl.h>

#include "conffile.h"
#include "dvdauthor.h"
#include "readxml.h"
#include "rgb.h"

int default_video_format = VF_NONE;
char provider_str[PROVIDER_SIZE];

/* common parsing bits for both command line and XML file */

#define RGB2YCrCb(R,G,B) ((((int)RGB2Y(R,G,B))<<16)|(((int)RGB2Cr(R,G,B))<<8)|(((int)RGB2Cb(R,G,B))))

static int readdvdauthorxml(const char *xmlfile,const char *fb);

static enum
  {
    chapters_neither, /* neither of following seen yet */
    chapters_cells, /* vob chapters specified via cells (and possibly also "chapters" attribute) */
    chapters_chapters, /* only via "chapters" attribute */
  }
    hadchapter = chapters_neither; /* info about last VOB in current PGC */
static int
    pauselen=0;
static bool
    writeoutput = true;
static char
    *cur_chapters = 0;

static void parsevideoopts(struct pgcgroup *va,const char *o)
{
    char *s;
    while(NULL!=(s=str_extract_until(&o,"+"))) {
        if(pgcgroup_set_video_attr(va,VIDEO_ANY,s)) {
            fprintf(stderr,"ERR:  Video option '%s' overrides previous option\n",s);
            exit(1);
        }
        free(s);
    }
}

static void parseaudiotrack(struct pgcgroup *va,const char *o,int c)
{
    char *s;
    while(NULL!=(s=str_extract_until(&o,"+"))) {
        if(pgcgroup_set_audio_attr(va,AUDIO_ANY,s,c)) {
            fprintf(stderr,"ERR:  Audio option '%s' on track %d overrides previous option\n",s,c);
            exit(1);
        }
        free(s);
    }
}

static void parseaudioopts(struct pgcgroup *va,const char *o)
{
    char *s;
    int ch=0;
    while(NULL!=(s=str_extract_until(&o,", "))) {
        parseaudiotrack(va,s,ch);
        free(s);
        ch++;
    }
}

static void parsesubpicturetrack(struct pgcgroup *va,const char *o,int c)
{
    char *s;
    while(NULL!=(s=str_extract_until(&o,"+"))) {
        if(pgcgroup_set_subpic_attr(va,SPU_ANY,s,c)) {
            fprintf(stderr,"ERR:  Subpicture option '%s' on track %d overrides previous option\n",s,c);
            exit(1);
        }
        free(s);
    }
}

static void parsesubpictureopts(struct pgcgroup *va,const char *o)
{
    char *s;
    int ch=0;
    while(NULL!=(s=str_extract_until(&o,", "))) {
        parsesubpicturetrack(va,s,ch);
        free(s);
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

static void parseinstructions(struct pgc *va,const char *b)
{
    char *c=str_extract_until(&b,"=");
    if(!strcasecmp(c,"post")) {
        pgc_set_post(va,b);
    } else if(!strcasecmp(c,"pre")) {
        pgc_set_pre(va,b);
    } else {
        fprintf(stderr,"Unknown instruction block: %s\n",c);
        exit(1);
    }
    free(c);
}

static void parseentries(struct pgc *p, vtypes ismenuf, const char *b)
  {
    char *v;
    while (NULL != (v = str_extract_until(&b, ", ")))
      {
        pgc_add_entry(p, ismenuf, v);
        free(v);
      } /*while*/
  } /*parseentries*/

static double parsechapter(const char *s)
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

static void parsechapters(const char *o, struct source *src, int pauselen)
  /* parses a chapter spec and adds corresponding cell definitions. */
  {
    char *s;
    double last = 0;
    cell_chapter_types lastchap = CELL_NEITHER;
    while (NULL != (s = str_extract_until(&o, ", ")))
      {
        const double total = parsechapter(s);
        if (total > last)
          {
            source_add_cell(src, last, total, lastchap, 0, 0);
            last = total;
            lastchap = CELL_CHAPTER_PROGRAM;
          }
        else if (total == last)
            lastchap = CELL_CHAPTER_PROGRAM;
      /* else report error? user specified chapter times in wrong order? */
        free(s);
      } /*while*/
    source_add_cell(src, last, -1, lastchap, pauselen, 0);
  } /*parsechapters*/

static void readpalette(struct pgc *p,const char *fname)
{
    int i,rgbf;
    struct vfile h;

    h=varied_open(fname, O_RDONLY, "palette file");

    /* write out colors, the hex is the 0yuv combined in one integer 00yyuuvv */
    i=strlen(fname);
    rgbf=( i>=4 && !strcasecmp(fname+i-4,".rgb") );
    for( i=0; i<16; i++ ) {
        int pcolor;
        fscanf(h.h, "%x", &pcolor);
        if( rgbf ) {
            const int r=(pcolor>>16)&255,
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
    while(fscanf(h.h,"%x",&b) == 1 && i<24 ) {
        groups[i++]=b>>24;
        groups[i++]=b>>16;
        groups[i++]=b>>8;
        groups[i++]=b;
        if( !(i&7) )
            pgc_set_buttongroup(p,(i-8)/8,groups+(i-8));
    }
#endif
    varied_close(h);
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
            "syntax: dvdauthor [-o VTSBASE | -n] [options] VOBFILE(s)\n"
            "\n\t-x XMLFILE where XMLFILE is a configuration file describing the\n"
            "\t    structure of the DVD to create.  If you use a config file, then you\n"
            "\t    do not need to specify any other options, except -o and -n.\n"
            "\n\t-n skips writing any files, for testing purposes.  MUST occur before any\n"
            "\t    other options.\n"
            "\n\t" LONGOPT("--video=VOPTS or ") "-v VOPTS where VOPTS is a plus (+) separated list of\n"
            "\t    video options.  dvdauthor will try to infer any unspecified options.\n"
            "\t\tpal, ntsc, 4:3, 16:9, 720xfull, 720x576, 720x480, 704xfull,\n"
            "\t\t704x576, 704x480, 352xfull, 352x576, 352x480, 352xhalf,\n"
            "\t\t352x288, 352x240, nopanscan, noletterbox, crop.\n"
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
            "\t    flexibility in choosing jump/call destinations.\n"
            "\n\t" LONGOPT("--allgprm or ") "-g enables the use of all 16 general purpose registers.\n"
            "\n\t" LONGOPT("--help or ") "-h displays this screen.\n"
        );
    exit(1);
}

/* bits for authoring via command line */
#define NOXML                                                                          \
    if (xmlfile)                                                                       \
      {                                                                                \
        fprintf                                                                        \
          (                                                                            \
            stderr,                                                                    \
            "ERR:  Cannot use command line options after specifying XML config file\n" \
          );                                                                           \
        return 1;                                                                      \
      } /*if*/
#define MAINDEF                                                               \
            if (!usedtocflag)                                                 \
              {                                                               \
                fprintf(stderr, "ERR:  Must first specify -t, -m, or -x.\n"); \
                return 1;                                                     \
              } /*if*/                                                        \
            if (istoc && istitle)                                             \
              {                                                               \
                fprintf(stderr, "ERR:  TOC cannot have titles\n");            \
                return 1;                                                     \
              } /*if*/                                                        \
            if (!va[istitle])                                                          \
              {                                                               \
                va[istitle] = pgcgroup_new((int)istoc + 1 - (int)istitle); \
              } /*if*/

#define MAINDEFPGC                                                   \
            MAINDEF                                                  \
            if (!curpgc)                                             \
                curpgc = pgc_new();

#define MAINDEFVOB                                                   \
            MAINDEFPGC                                               \
            if (!curvob)                                             \
                curvob = source_new();

#define FLUSHPGC                                                     \
            if (curpgc)                                              \
              {                                                      \
                pgcgroup_add_pgc(va[istitle], curpgc);               \
                curpgc = 0;                                          \
              } /*if*/

int main(int argc, char **argv)
  {
    struct pgcgroup *va[2]; /* element 0 for doing menus, 1 for doing titles */
    struct menugroup *mg;
    char *fbase = 0; /* output directory name */
    char * xmlfile = 0; /* name of XML control file, if any */
    bool istitle = true; /* index into va */
    bool istoc = false, /* true if doing VMG, false if doing titleset */
        usedtocflag = false; /* indicates that istoc can no longer be changed */
    struct pgc *curpgc = 0,* fpc = 0;
    struct source *curvob = 0;
#ifdef HAVE_GETOPT_LONG
    const static struct option longopts[]={
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
        {"allgprm",0,0,'g'},
        {0,0,0,0}
    };
#define GETOPTFUNC(x,y,z) getopt_long(x,y,"-" z,longopts,NULL)
#else
#define GETOPTFUNC(x,y,z) getopt(x,y,z)
#endif

    default_video_format = get_video_format();
    init_locale();
    fputs(PACKAGE_HEADER("dvdauthor"), stderr);
    if (default_video_format != VF_NONE)
      {
        fprintf
          (
            stderr,
            "INFO: default video format is %s\n",
            default_video_format == VF_PAL ? "PAL" : "NTSC"
          );
      }
    else
      {
#if defined(DEFAULT_VIDEO_FORMAT)
#    if DEFAULT_VIDEO_FORMAT == 1
        fprintf(stderr, "INFO: default video format is NTSC\n");
#    elif DEFAULT_VIDEO_FORMAT == 2
        fprintf(stderr, "INFO: default video format is PAL\n");
#    endif
#else
        fprintf(stderr, "INFO: no default video format, must explicitly specify NTSC or PAL\n");
#endif
      } /*if*/

    if (argc < 1)
      {
        fprintf(stderr, "ERR:  No arguments!\n");
        return 1;
      } /*if*/
    memset(va, 0, sizeof(struct pgcgroup *) * 2);

    while (true)
      {
        int c = GETOPTFUNC(argc, argv, "f:o:O:v:a:s:hc:Cp:Pmtb:Ti:e:x:jgn");
        if (c == -1)
            break;
        switch (c)
          {
        case 'h':
            usage();
        break;

        case 'x':
            if (usedtocflag)
              {
                fprintf
                  (
                    stderr,
                    "ERR:  Cannot specify XML config file after using command line options\n"
                  );
                return 1;
              } /*if*/
            if (xmlfile)
              {
                fprintf(stderr, "ERR:  only one XML file allowed\n");
                return 1;
              } /*if*/
            xmlfile = optarg;
        break;

        case 'n':
            writeoutput = false;
        break;

        case 'j':
            dvdauthor_enable_jumppad();
        break;

        case 'g':
            dvdauthor_enable_allgprm();
        break;

        case 'T':
            NOXML
            if (usedtocflag)
              {
                fprintf
                  (
                    stderr,
                    "ERR:  TOC (-T) option must come first because I am a lazy programmer.\n"
                  );
                return 1;
              } /*if*/
            istoc = true;
            usedtocflag = true; // just for completeness (also see -x FOO)
        break;

        case 'O':
            delete_output_dir = true;
        /* and fallthru */
        case 'o':
            fbase = optarg;
        break;

        case 'm':
            NOXML
            FLUSHPGC
            usedtocflag = true; // force -T to occur before -m
            hadchapter = chapters_neither; /* reset for new menu */
            istitle = false;
        break;

        case 't':
            NOXML
            if (istoc)
              {
                fprintf(stderr, "ERR:  TOC cannot have titles\n");
                return 1;
              } /*if*/
            FLUSHPGC
            usedtocflag = true;
            hadchapter = chapters_neither; /* reset for new title */
            istitle = true;
        break;

        case 'a':
            NOXML
            MAINDEF
            parseaudioopts(va[istitle], optarg);
        break;

        case 'v':
            NOXML
            MAINDEF
            parsevideoopts(va[istitle], optarg);
        break;

        case 's':
            NOXML
            MAINDEF
            parsesubpictureopts(va[istitle], optarg);
        break;

        case 'b':
            NOXML
            MAINDEFPGC
            parsebutton(curpgc, optarg);
        break;

        case 'i':
            NOXML
            MAINDEFPGC
            parseinstructions(curpgc, optarg);
        break;

        case 'F':
            NOXML
            if (!istoc)
              {
                fprintf(stderr, "ERR:  You may only specify FPC commands on the VMGM\n");
                return 1;
              } /*if*/
            usedtocflag = true;
            if (fpc)
              {
                fprintf(stderr,"ERR:  FPC commands already specified\n");
                return 1;
              } /*if*/
            fpc = pgc_new();
            pgc_set_pre(fpc, optarg);
        break;

        case 'e':
            NOXML
            MAINDEFPGC
            if (istitle)
              {
                fprintf(stderr, "ERR:  Cannot specify an entry for a title.\n");
                return 1;
              } /*if*/
            parseentries(curpgc, istoc ? VTYPE_VMGM : VTYPE_VTSM, optarg);
        break;

        case 'P':
          /* NOXML */
            optarg = 0; /* use default palette name */
      /* fallthru */
        case 'p':
            NOXML
            MAINDEFPGC
            readpalette(curpgc, optarg ? optarg : "xste-palette.dat");
        break;

        case 1:
        case 'f':
            NOXML
            MAINDEFVOB
            source_set_filename(curvob, optarg);
            if (hadchapter == chapters_chapters )
                hadchapter = chapters_cells; /* chapters already specified */
            else
                source_add_cell
                  (
                    /*source =*/ curvob,
                    /*starttime =*/ 0,
                    /*endtime =*/ -1,
                    /*chap =*/
                        hadchapter == chapters_neither ?
                            CELL_CHAPTER_PROGRAM /* first vob in pgc */
                        :
                            CELL_NEITHER, /* subsequent vobs in pgc */
                    /*pause =*/ 0,
                    /*cmd =*/ 0
                  );
                  /* default to single chapter/cell for entire source file */
            pgc_add_source(curpgc, curvob);
            curvob = 0;
        break;

        case 'C':
          /* NOXML */
            optarg = 0;
      /* fallthru */
        case 'c':
            NOXML
            if (curvob)
              {
                fprintf(stderr, "ERR:  cannot list -c twice for one file.\n");
                return 1;
              } /*if*/
            MAINDEFVOB
            hadchapter = chapters_chapters;
            if (optarg)
                parsechapters(optarg, curvob, 0);
            else
                source_add_cell(curvob, 0, -1, CELL_CHAPTER_PROGRAM, 0, 0);
                  /* default to single chapter for entire source file */
        break;

        default:
            fprintf(stderr, "ERR:  getopt returned bad code %d\n", c);
            return 1;
          } /*switch*/
      } /*while*/
    if (xmlfile)
      {
        return readdvdauthorxml(xmlfile, fbase);
      }
    else
      {
        if (curvob)
          {
            fprintf(stderr, "ERR:  Chapters defined without a file source.\n");
            return 1;
          } /*if*/
        FLUSHPGC
        if (!va[0])
          {
            va[0] = pgcgroup_new(istoc ? VTYPE_VMGM : VTYPE_VTSM);
            mg = menugroup_new();
            menugroup_add_pgcgroup(mg, "en", va[0]);
              /* fixme: would be nice to make language a preference setting, perhaps default to locale? */
          }
        else
            mg = 0;
        if (!va[1] && !istoc)
            va[1] = pgcgroup_new(VTYPE_VTS);
        if (!fbase && writeoutput)
          {
            fbase = get_outputdir();
            if (!fbase)
                usage();
          } /*if*/
        if (optind != argc)
          {
            fprintf(stderr, "ERR:  bad version of getopt; please precede all sources with '-f'\n");
            return 1;
          } /*if*/
        if (fbase)
          {
            const int l = strlen(fbase);
            if (l && fbase[l - 1] == '/')
                fbase[l - 1] = 0;
          } /*if*/
        if (istoc)
            dvdauthor_vmgm_gen(fpc, mg, fbase);
        else
            dvdauthor_vts_gen(mg, va[1], fbase);
        pgc_free(fpc);
        menugroup_free(mg);
        pgcgroup_free(va[1]);
        return 0;
      } /*if*/
  } /*main*/

/* authoring via XML file */

enum {
    DA_BEGIN=0,
    DA_ROOT,
    DA_SET,
    DA_PGCGROUP,
    DA_PGC,
    DA_VOB,
    DA_SUBP,
    DA_NOSUB
};

static struct pgcgroup
    *titles=0, /* titles saved here (only one set allowed) */
    *curgroup=0; /* current menus or titles */
static struct menugroup
    *mg=0, /* current menu group (for titleset or vmgm) */
    *vmgmmenus=0; /* vmgm menu group saved here on completion */
static struct pgc *curpgc=0,*fpc=0;
static struct source *curvob=0;
static const char
    *fbase = 0, /* output directory name */
    *buttonname = 0; /* name of button currently being defined */
static vtypes
    ismenuf = VTYPE_VTS; /* type of current pgcgroup structure being parsed */
static bool
    istoc = false, /* true for vmgm, false for titleset */
    hadtoc = false; /* set to true when vmgm seen */
static int
    setvideo=0, /* to keep count of <video> tags */
    setaudio=0, /* to keep count of <audio> tags */
    setsubpicture=0, /* to keep count of <subpicture> tags */
    subpmode=DA_NOSUB,
      /* for determining context of a <subpicture> tag:
        DA_NOSUB -- invalid
        DA_PGC -- inside a <pgc> tag (no "lang" attribute allowed)
        DA_PGCGROUP -- inside a <menus> or <titles> tag, must come before the <pgc> tags
      */
    subpstreamid=-1; /* id attribute of current <stream> saved here */
static char *subpstreammode=0; /* mode attribute of current <stream> saved here */
static enum
  {
    vob_has_neither, /* neither of following seen yet */
    vob_has_chapters_pause, /* vob has "chapters" or "pause" attribute */
    vob_has_cells, /* vob has <cell> subtags */
  }
    vobbasic; /* info about each VOB */
static cell_chapter_types
    cell_chapter;
static double
    cell_starttime,
    cell_endtime;
static char
    menulang[3];

static void set_video_attr(int attr,const char *s)
{
    if (ismenuf != VTYPE_VTS)
        menugroup_set_video_attr(mg, attr, s);
    else
        pgcgroup_set_video_attr(titles, attr, s);
}

static void set_audio_attr(int attr,const char *s,int ch)
{
    if (ismenuf != VTYPE_VTS)
        menugroup_set_audio_attr(mg,attr,s,ch);
    else
        pgcgroup_set_audio_attr(titles,attr,s,ch);
}

static void set_subpic_attr(int attr,const char *s,int ch)
{
    if (ismenuf != VTYPE_VTS)
        menugroup_set_subpic_attr(mg,attr,s,ch);
    else
        pgcgroup_set_subpic_attr(titles,attr,s,ch);
}

static void set_subpic_stream(int ch,const char *m,int id)
{
    if (ismenuf != VTYPE_VTS)
        menugroup_set_subpic_stream(mg,ch,m,id);
    else
        pgcgroup_set_subpic_stream(titles,ch,m,id);
}

static int parse_pause(const char *f)
{
    if (!strcmp(f,"inf"))
        return 255;
    else
        return strtounsigned(f, "pause time"); /* should check it's in [0 .. 254] */
}

static void dvdauthor_outputdir(const char *s)
  {
    if (!fbase)
      {
        fbase = localize_filename(s);
      } /*if*/
  }

static void dvdauthor_jumppad(const char *s)
{
    if(xml_ison(s, "jumppad"))
        dvdauthor_enable_jumppad();
}

static void dvdauthor_allgprm(const char *s)
{
    if (xml_ison(s, "allgprm"))
        dvdauthor_enable_allgprm();
}

static void dvdauthor_video_format
  (
    const char * s
  )
  {
    if (!strcmp(s, "ntsc") || !strcmp(s, "NTSC"))
      {
        default_video_format = VF_NTSC;
      }
    else if (!strcmp(s, "pal") || !strcmp(s, "PAL"))
      {
        default_video_format = VF_PAL;
      }
    else
      {
        fprintf(stderr, "ERR:  Unrecognized video format \"%s\"\n", s);
        parser_err = true;
      } /*if*/
  } /*dvdauthor_video_format*/

static void dvdauthor_provider
  (
    const char * s
  )
  {
    strncpy(provider_str, s, PROVIDER_SIZE);
    provider_str[PROVIDER_SIZE - 1] = 0;
  } /*dvdauthor_provider*/

static void getfbase()
  {
    if (!writeoutput)
        fbase = 0;
    else if (!fbase)
      {
        fprintf(stderr, "ERR:  Must specify working directory\n");
        parser_err = true;
      } /*if*/
  } /*getfbase*/

static void dvdauthor_start(void)
  {
    strncpy(provider_str, PACKAGE_STRING, PROVIDER_SIZE);
    provider_str[PROVIDER_SIZE - 1] = 0;
  } /*dvdauthor_start*/

static void dvdauthor_end(void)
/* called on </dvdauthor> end tag, generates the VMGM if specified.
  This needs to be done after all the titles, so it can include
  information about them. */
  {
    if (hadtoc)
      {
        dvdauthor_vmgm_gen(fpc, vmgmmenus, fbase);
        pgc_free(fpc);
        menugroup_free(vmgmmenus);
        fpc = 0;
        vmgmmenus = 0;
      } /*if*/
  } /*dvdauthor_end*/

static void titleset_start()
{
    mg=menugroup_new();
    istoc = false;
}

static void titleset_end()
  { /* called on </titles> end tag, generates the output titleset. */
    getfbase();
    if (!parser_err)
      {
        if (!titles)
            titles = pgcgroup_new(VTYPE_VTS);
        dvdauthor_vts_gen(mg, titles, fbase);
        menugroup_free(mg);
        pgcgroup_free(titles);
        mg = 0;
        titles = 0;
      } /*if*/
  } /*titleset_end*/

static void vmgm_start()
{
    if (hadtoc) {
        fprintf(stderr,"ERR:  Can only define one VMGM\n");
        parser_err = true;
        return;
    }
    mg=menugroup_new();
    istoc = true;
    hadtoc = true;
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
        parser_err = true;
        return;
    }
    if( fpc ) {
        fprintf(stderr,"ERR:  Already defined <fpc>\n");
        parser_err = true;
        return;
    }
    fpc=pgc_new();
    parser_acceptbody = true;
}

static void fpc_end()
{
    pgc_set_pre(fpc,parser_body);
}

static void pgcgroup_start()
  /* common part of both <menus> and <titles> start. */
  {
    setvideo = 0;
    setaudio = -1;
    setsubpicture = -1;
    subpmode = DA_PGCGROUP;
  } /*pgcgroup_start*/

static void titles_start()
  /* called on a <titles> tag. */
  {
    if (titles)
      {
        fprintf(stderr, "ERR:  Titles already defined\n");
        parser_err = true;
      }
    else if (istoc)
      {
        fprintf(stderr, "ERR:  Cannot have titles in a VMGM\n");
        parser_err = true;
      }
    else
      {
        titles = pgcgroup_new(VTYPE_VTS);
        curgroup = titles;
        ismenuf = VTYPE_VTS;
        pgcgroup_start();
      } /*if*/
  }

static void menus_start()
  /* called on a <menus> tag. */
  {
    ismenuf = (istoc ? VTYPE_VMGM : VTYPE_VTSM);
    curgroup = pgcgroup_new(ismenuf);
    pgcgroup_start();
    strcpy(menulang, "en"); /* default. */
      /* fixme: would be nice to make this a preference setting, perhaps default to locale? */
      /* fixme: no check for buffer overflow! */
  }

static void menus_lang(const char *lang)
  {
    strcpy(menulang, lang);
      /* fixme: no check for buffer overflow! */
  } /*menus_lang*/

static void menus_end()
  {
    menugroup_add_pgcgroup(mg, menulang, curgroup);
    curgroup = 0;
  } /*menus_end*/

static void video_start()
  /* called on a <video> tag. */
  {
    if (setvideo)
      {
        fprintf(stderr, "ERR:  Already defined video characteristics for this PGC group\n");
          /* only one video stream allowed */
        parser_err = true;
      }
    else
        setvideo = 1;
  }

static void video_format(const char *c)
  {
    set_video_attr(VIDEO_FORMAT, c);
  }

static void video_aspect(const char *c)
  {
    set_video_attr(VIDEO_ASPECT, c);
  }

static void video_resolution(const char *c)
  {
    set_video_attr(VIDEO_RESOLUTION, c);
  }

static void video_widescreen(const char *c)
  {
    set_video_attr(VIDEO_WIDESCREEN, c);
  }

static void video_caption(const char *c)
  {
    set_video_attr(VIDEO_CAPTION, c);
  }

static void audio_start()
  /* called on an <audio> tag. */
  {
    setaudio++;
    if (setaudio >= 8)
      {
        fprintf(stderr, "ERR:  Attempting to define too many audio streams for this PGC group\n");
        parser_err = true;
      } /*if*/
  }

static void audio_format(const char *c)
  {
    set_audio_attr(AUDIO_FORMAT, c, setaudio);
  }

static void audio_quant(const char *c)
  {
    set_audio_attr(AUDIO_QUANT, c, setaudio);
  }

static void audio_dolby(const char *c)
  {
    set_audio_attr(AUDIO_DOLBY, c, setaudio);
  }

static void audio_lang(const char *c)
  {
    set_audio_attr(AUDIO_LANG, c, setaudio);
  }

static void audio_samplerate(const char *c)
  {
    set_audio_attr(AUDIO_SAMPLERATE, c, setaudio);
  }

static void audio_content(const char *c)
  {
    set_audio_attr(AUDIO_CONTENT, c, setaudio);
  } /*audio_content*/

static void audio_channels(const char *c)
  {
    char ch[4];
    if (strlen(c) == 1)
      {
        ch[0] = c[0];
        ch[1] = 'c';
        ch[2] = 'h';
        ch[3] = 0;
        c = ch;
      } /*if*/
    set_audio_attr(AUDIO_CHANNELS, c, setaudio);
  }

static void subattr_group_start()
  /* called for a <subpicture> tag immediately within a pgcgroup (<menus> or <titles>). */
 {
    if (subpmode != DA_PGCGROUP)
      {
        fprintf(stderr, "ERR:  Define all the subpictures before defining PGCs\n");
        parser_err = true;
        return;
      } /*if*/
    setsubpicture++;
    if (setsubpicture >= 32)
      {
        fprintf(stderr, "ERR:  Attempting to define too many subpicture streams for this PGC group\n");
        parser_err = true;
      } /*if*/
  } /*subattr_group_start*/

static void subattr_pgc_start()
  /* called for a <subpicture> tag immediately within a <pgc>. */
  {
    setsubpicture++;
    if (setsubpicture >= 32)
      {
        fprintf(stderr, "ERR:  Attempting to define too many subpicture streams for this PGC group\n");
        parser_err = true;
      } /*if*/
  } /*subattr_pgc_start*/

static void subattr_lang(const char *c)
  {
    if (subpmode == DA_PGCGROUP)
        set_subpic_attr(SPU_LANG, c, setsubpicture);
    else
      {
        fprintf
          (
            stderr,
            "ERR:  Cannot set subpicture language within a pgc; do it within titles or menus\n"
          );
        parser_err = true;
      } /*if*/
  } /*subattr_lang*/

static void subattr_content(const char * c)
  {
    if (subpmode == DA_PGCGROUP)
      {
        set_subpic_attr(SPU_CONTENT, c, setsubpicture);
      }
    else
      {
        fprintf
          (
            stderr,
            "ERR:  Cannot set subpicture content type within a pgc; do it within titles or menus\n"
          );
        parser_err = true;
      } /*if*/
  } /*subattr_content*/

static void stream_start()
  /* called on a <stream> tag. */
  {
    subpstreamid = -1;
    subpstreammode = 0;
  } /*stream_start*/

static void substream_id(const char *c)
  /* saves the id attribute for the current <stream> tag. */
  {
    subpstreamid = strtounsigned(c, "subpicture stream id");
    if (subpstreamid < 0 || subpstreamid >= 32)
      {
        fprintf(stderr, "ERR:  Subpicture stream id must be 0-31: '%s'.\n", c);
        parser_err = true;
      } /*if*/
  } /*substream_id*/

static void substream_mode(const char *c)
  /* saves the mode attribute for the current <stream> tag. */
  {
    subpstreammode = strdup(c); /* won't leak, because I won't be called more than once */
  }

static void stream_end()
  {
    if (subpstreamid == -1 || !subpstreammode)
      {
        fprintf(stderr, "ERR:  Must define the mode and id for the stream.\n");
        parser_err = true;
      }
    else if (subpmode == DA_PGCGROUP)
      {
        set_subpic_stream(setsubpicture, subpstreammode, subpstreamid);
      }
    else
        pgc_set_subpic_stream(curpgc, setsubpicture, subpstreammode, subpstreamid);
    free(subpstreammode);
  } /*stream_end*/

static void pgc_start()
{
    curpgc = pgc_new();
    hadchapter = chapters_neither; /* reset for new menu/title */
    setsubpicture = -1;
    subpmode = DA_PGC;
}

static void pgc_entry(const char *e)
{
    // xml attributes can only be defined once, so entry="foo" entry="bar" won't work
    // instead, use parseentries...
    parseentries(curpgc, ismenuf, e);
}

static void pgc_palette(const char *p)
{
    readpalette(curpgc,p);
}

static void pgc_pause(const char *c)
{
    pgc_set_stilltime(curpgc,parse_pause(c));
}

static void pgc_end()
{
    pgcgroup_add_pgc(curgroup,curpgc);
    curpgc=0;
    subpmode=DA_NOSUB;
}

static void pre_start()
{
    parser_acceptbody = true;
}

static void pre_end()
{
    pgc_set_pre(curpgc,parser_body);
}

static void post_start()
{
    parser_acceptbody = true;
}

static void post_end()
{
    pgc_set_post(curpgc,parser_body);
}

static void vob_start()
{
    curvob = source_new();
    pauselen = 0;
    vobbasic = vob_has_neither; /* to begin with */
  /* but note hadchapter keeps its value from previous VOB in this PGC
    if no chapters attribute or cells are seen */
    cell_endtime = 0;
}

static void vob_file(const char *f)
{
    f = localize_filename(f);
    source_set_filename(curvob, f);
    free(f);
}

static void vob_chapters(const char *c)
  {
    vobbasic = vob_has_chapters_pause;
    hadchapter = chapters_chapters;
    cur_chapters = strdup(c); /* won't leak, because I won't be called more than once per <vob> */
  }

static void vob_pause(const char *c)
  {
    vobbasic = vob_has_chapters_pause;
    pauselen = parse_pause(c);
  }

static void vob_end()
  {
    if (vobbasic != vob_has_cells) /* = vob_has_chapters_pause or vob_has_neither */
      {
        if (hadchapter == chapters_chapters) /* vob has chapters attribute */
          {
            parsechapters(cur_chapters, curvob, pauselen);
            hadchapter = chapters_cells;
              /* subsequent <vob>s in this PGC shall be only cells, unless
                they have their own "chapter" attribute */
          }
        else /* hadchapter = chapters_neither or chapters_cells */
            source_add_cell
              (
                /*source =*/ curvob,
                /*starttime =*/ 0,
                /*endtime =*/ -1,
                /*chap =*/
                    hadchapter == chapters_neither ?
                        CELL_CHAPTER_PROGRAM /* first <vob> in <pgc> */
                    :
                        CELL_NEITHER, /* subsequent <vob>s in <pgc> */
                /*pause =*/ pauselen,
                /*cmd =*/ 0
              );
              /* default to single chapter/cell for entire source file */
      } /*if*/
    pgc_add_source(curpgc, curvob);
    free(cur_chapters);
    cur_chapters = 0;
    curvob = 0;
  } /*vob_end*/

static void cell_start()
  {
    parser_acceptbody = true; /* collect cell commands */
    assert(vobbasic != vob_has_chapters_pause);
      /* no "chapters" or "pause" attribute on containing <vob> allowed */
    vobbasic = vob_has_cells;
    cell_starttime = cell_endtime; /* new cell starts by default where previous one ends */
    cell_endtime = -1;
    cell_chapter = CELL_NEITHER; /* to begin with */
    pauselen = 0;
    hadchapter = chapters_cells;
  }

static void cell_parsestart(const char *f)
  {
    if (f[0] == 0)
      {
        fprintf(stderr,"ERR:  Empty cell start time\n");
        exit(1);
      } /*if*/
    cell_starttime = parsechapter(f);
  }

static void cell_parseend(const char *f)
  {
    if (f[0] == 0)
      {
        fprintf(stderr,"ERR:  Empty cell end time\n");
        exit(1);
      } /*if*/
    cell_endtime = parsechapter(f);
  }

static void cell_parsechapter(const char *f)
  {
    if (xml_ison(f, "chapter"))
        cell_chapter = CELL_CHAPTER_PROGRAM;
  }

static void cell_parseprogram(const char *f)
  {
    if (xml_ison(f, "program") && cell_chapter != CELL_CHAPTER_PROGRAM)
        cell_chapter = CELL_PROGRAM;
  }

static void cell_pauselen(const char *f)
  {
    pauselen=parse_pause(f);
  }

static void cell_end()
  {
    assert (cell_starttime >= 0);
    source_add_cell(curvob, cell_starttime, cell_endtime, cell_chapter, pauselen, parser_body);
    pauselen = 0;
  }

static void button_start()
{
    parser_acceptbody = true;
    buttonname=0;
}

static void button_name(const char *f)
{
    buttonname=strdup(f);
}

static void button_end()
{
    pgc_add_button(curpgc,buttonname,parser_body);
    if(buttonname) free((char *)buttonname);
}

static struct elemdesc elems[]={
    {"dvdauthor", DA_BEGIN,   DA_ROOT,    dvdauthor_start, dvdauthor_end},
    {"titleset",  DA_ROOT,    DA_SET,     titleset_start,  titleset_end},
    {"vmgm",      DA_ROOT,    DA_SET,     vmgm_start,      vmgm_end},
    {"fpc",       DA_SET,     DA_NOSUB,   fpc_start,       fpc_end},
    {"titles",    DA_SET,     DA_PGCGROUP,titles_start,    0},
    {"menus",     DA_SET,     DA_PGCGROUP,menus_start,     menus_end},
    {"video",     DA_PGCGROUP,DA_NOSUB,   video_start,     0},
    {"audio",     DA_PGCGROUP,DA_NOSUB,   audio_start,     0},
    {"subpicture",DA_PGCGROUP,DA_SUBP,    subattr_group_start, 0},
    {"pgc",       DA_PGCGROUP,DA_PGC,     pgc_start,       pgc_end},
    {"pre",       DA_PGC,     DA_NOSUB,   pre_start,       pre_end},
    {"post",      DA_PGC,     DA_NOSUB,   post_start,      post_end},
    {"button",    DA_PGC,     DA_NOSUB,   button_start,    button_end},
    {"vob",       DA_PGC,     DA_VOB,     vob_start,       vob_end},
    {"subpicture",DA_PGC,     DA_SUBP,    subattr_pgc_start, 0},
    {"cell",      DA_VOB,     DA_NOSUB,   cell_start,      cell_end},
    {"stream",    DA_SUBP,    DA_NOSUB,   stream_start,    stream_end},
    {0,0,0,0,0}
};

static struct elemattr attrs[]={
    {"dvdauthor","dest",dvdauthor_outputdir},
    {"dvdauthor","jumppad",dvdauthor_jumppad},
    {"dvdauthor","allgprm",dvdauthor_allgprm},
    {"dvdauthor","format",dvdauthor_video_format},
    {"dvdauthor","provider",dvdauthor_provider},

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
    {"audio","content",audio_content},

    {"subpicture","lang",subattr_lang},
    {"subpicture","content",subattr_content},
    {"stream","mode",substream_mode},
    {"stream","id",substream_id},

    {"pgc","entry",pgc_entry},
    {"pgc","palette",pgc_palette},
    {"pgc","pause",pgc_pause},
    {0,0,0}
};

static int readdvdauthorxml(const char *xmlfile, const char *fb)
{
    fbase = fb;
    if (!fbase)
      {
        fbase = get_outputdir();
      } /*if*/
    return readxml(xmlfile, elems, attrs);
}
