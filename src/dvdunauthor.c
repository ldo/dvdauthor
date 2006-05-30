/*
 * Copyright (C) 2002 Scott Smith (trckjunky@users.sourceforge.net)
 * 13.11.2004, Ralf Engels <ralf-engels@gmx.de> added lang options for titles,
 *                                              conversion to write-xml2 lib
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

#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_types.h>
#include <dvdread/ifo_read.h>
#include <dvdread/nav_read.h>

#include <libxml/tree.h>

static const char RCSID[]="$Id: //depot/dvdauthor/src/dvdunauthor.c#15 $";

#define BIGBLOCKSECT 512
#define BIGBLOCKLEN (DVD_VIDEO_LB_LEN*BIGBLOCKSECT)
static unsigned char bigblock[BIGBLOCKLEN];

static int numtitlesets=0; // set in dvdump

struct cellstarttime {
    int vob,cell,pts;
} *cellstarttimes=0;

int numcst=0;

static int lasthlstart;

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

static void printtime(int v,int c, char* buf, int bufLen )
{
    int t1=getpts(v,c),t2=getpts(v,1);
    if( t1>=0 && t2>=0 ) {
        int t=t1-t2;
        snprintf(buf, bufLen,
                 "%d:%02d:%02d.%03d",
                 (t/90/1000/60/60),
                 (t/90/1000/60)%60,
                 (t/90/1000)%60,
                 (t/90)%1000);
        return;
    }
    snprintf(buf, bufLen,"-1");
}

/** Add a language attribute to the node */
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

/** Output Video Title Set Attributes */
static void dump_vtsi_mat( vtsi_mat_t *vtsi_mat,
                           xmlNodePtr node ) 
{
  int i;
  xmlNodePtr newNode;

  for(i = 0; i < vtsi_mat->nr_of_vts_audio_streams; i++) {
    newNode = xmlNewTextChild( node, NULL, "audio", NULL );
    addLangAttr( newNode, vtsi_mat->vts_audio_attr[i].lang_code);
  }

  for(i = 0; i < vtsi_mat->nr_of_vts_subp_streams; i++) {
    xmlNodePtr newNode = xmlNewTextChild( node, NULL, "subpicture", NULL );
    addLangAttr( newNode, vtsi_mat->vts_subp_attr[i].lang_code);
  }

}

/** Output attributes for a pgcs (program group chain sequence) */
static void dump_pgcs(ifo_handle_t *ifo,pgcit_t *pgcs,int titleset,int titlef, xmlNodePtr titleNode)
{
    if( pgcs ) {
        int i, j;

        for( i=0; i<pgcs->nr_of_pgci_srp; i++ ) {
            pgc_t      *p      = pgcs->pgci_srp[i].pgc;
            xmlNodePtr pgcNode = xmlNewTextChild( titleNode, NULL, "pgc", NULL );

            for( j=0; j<p->nr_of_cells; j++ ) {
              char buffer[128];
              xmlNodePtr vobNode = xmlNewTextChild( pgcNode, NULL, "vob", NULL );
              xmlNodePtr cellNode  = xmlNewTextChild( vobNode, NULL, "cell", NULL );

              /* add vob file name attribute */
              snprintf( buffer, 128, "vob_%02d_%03d%c.vob", titleset,p->cell_position[j].vob_id_nr,titlef?'t':'m' );
              xmlNewProp( vobNode, "file", buffer );

              /* add cell time attribute */
              printtime(p->cell_position[j].vob_id_nr,p->cell_position[j].cell_nr, buffer, 128);
              xmlNewProp( cellNode, "start", buffer );

              printtime(p->cell_position[j].vob_id_nr,p->cell_position[j].cell_nr+1, buffer, 128);
              xmlNewProp( cellNode, "end", buffer );


              switch( getprogramtype( titlef?ifo->vts_ptt_srpt:0,p,i+1,j+1) ) {
                case 1:
                  xmlNewProp( cellNode, "chapter", "1" );
                  break;
              case 2:
                  xmlNewProp( cellNode, "program", "1" );
                  break;
                  /* default: 
                     do nothing */
              }

            } /* end for */

        }
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

static void writepalette(int h,uint32_t *palette,unsigned char *vobu)
{
    static unsigned char sector[2048];
    int i,j;

    if( vobu ) {
        if( !vobu[0x8e] )
            return;
        if( (int)read4(vobu+0x8f) <= lasthlstart )
            return;
        if( !palette ) {
            fprintf(stderr,"ERR:  How can there be buttons but no palette?\n");
            exit(1);
        }
        memcpy(sector,vobu,14); // copy pack header
        lasthlstart=read4(vobu+0x8f);
    } else {
        if( !palette )
            return;
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
    }
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
    if( vobu ) {
        wdlong(read4(vobu+0x8f)); // start pts
        wdlong(read4(vobu+0x93)); // end pts
    } else {
        wdlong(0);
        wdlong(0);
    }

    wdbyte(1); // colormap
    wdbyte(16); // number of colors
    for( i=0; i<16; i++ ) {  
        wdbyte(palette[i]>>16);
        wdbyte(palette[i]>>8);
        wdbyte(palette[i]);
    }

    if( vobu ) {
        wdbyte(2); // st_coli
        wdbyte(3);
        for( i=0; i<24; i++ )
            wdbyte(vobu[0xa3+i]);

        wdbyte(3);
        wdbyte(vobu[0x9e]);
        for( i=0; i<vobu[0x9e]; i++ ) {
            unsigned char *b=vobu+0xbb+18*i;
            char nm1[10];

            sprintf(nm1,"%d",i+1);
            wdstr(nm1);
            wdshort(0);
            wdbyte(b[3]>>6);
            if( !(b[3]&0xc0) ) {
                wdbyte(b[0]>>6);
                wdshort((read2(b+0)>>4)&1023);
                wdshort((read2(b+3)>>4)&1023);
                wdshort(read2(b+1)&1023);
                wdshort(read2(b+4)&1023);
                for( j=6; j<=9; j++ ) {
                    sprintf(nm1,"%d",(b[j]&63)+1);
                    wdstr(nm1);
                }
            }
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
  int numcells,i,j;

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
    
  for( i=0; i<numcells; i++ ) {
        char fname[100];
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

        sprintf(fname,"vob_%02d_%03d%c.vob",titleset,cells[i].vob_id,titlef?'t':'m');
        if( vobexists(cells,i,cells[i].vob_id) )
            h=open(fname,O_CREAT|O_APPEND|O_WRONLY|O_BINARY,0666);
        else {
            h=open(fname,O_CREAT|O_TRUNC|O_WRONLY|O_BINARY,0666);
            writepalette(h,palette,0);
            lasthlstart=-1;
        }
        
        fprintf(stderr,"[%d] VOB %d, Cell %d, Size %d kbytes\n",i,cells[i].vob_id,cells[i].cell_id,(cells[i].last_sector-cells[i].start_sector+1)*DVD_VIDEO_LB_LEN/1024);
        if( h<0 ) {
            fprintf(stderr,"ERR:  Cannot open %s for writing\n",fname);
            exit(1);
        }
        for( b=cells[i].start_sector; b<=cells[i].last_sector; b+=BIGBLOCKSECT ) {
            int rl=cells[i].last_sector+1-b;
            if( rl > BIGBLOCKSECT ) rl = BIGBLOCKSECT;
            if( DVDReadBlocks(vobs,b,rl,bigblock) < rl ) {
                fprintf(stderr,"ERR:  Error reading data: %s\n",strerror(errno));
                exit(1);
            }
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
                    unsigned char *pts=bigblock+j*DVD_VIDEO_LB_LEN+0x39;
                    addcst(cells[i].vob_id,cells[i].cell_id,(pts[0]<<24)|(pts[1]<<16)|(pts[2]<<8)|pts[3]);
                    writepalette(h,palette,bigblock+j*DVD_VIDEO_LB_LEN);
                }
                if( write(h,bigblock+j*DVD_VIDEO_LB_LEN,DVD_VIDEO_LB_LEN) < DVD_VIDEO_LB_LEN ) {
                    fprintf(stderr,"ERR:  Error writing data: %s\n",strerror(errno));
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


    getVobs( dvd, ifo, titleset, titlef );

    if( titlef ) {
      xmlNodePtr titleNode = xmlNewTextChild( titlesetNode, NULL, "titles", NULL );

      if( ifo->vtsi_mat )
        dump_vtsi_mat( ifo->vtsi_mat, titleNode );
      dump_pgcs(ifo,ifo->vts_pgcit,titleset,titlef, titleNode );

    } else {
        if( ifo->pgci_ut ) {
          int i;

          for( i=0; i<ifo->pgci_ut->nr_of_lus; i++ ) {
            pgci_lu_t *lu=&ifo->pgci_ut->lu[i];
            xmlNodePtr menusNode = xmlNewTextChild( titlesetNode, NULL, "menus", NULL );
            addLangAttr( menusNode, lu->lang_code );
            
            dump_pgcs(ifo,lu->pgcit,titleset,titlef, menusNode);
          }
        }
    }

    ifoClose(ifo);
}

int main(int argc,char **argv)
{
    xmlDocPtr  myXmlDoc;
    xmlNodePtr mainNode;
    xmlNodePtr titlesetNode;

    dvd_reader_t *dvd;
    int i;

    fputs(PACKAGE_HEADER("dvdunauthor"),stderr);

    if( argc!=2) {
        fprintf(stderr,"syntax: dvdunauthor path\n");
        return 0;
    }
    dvd=DVDOpen(argv[1]);
    if(!dvd) {
        fprintf(stderr,"ERR:  Cannot open path '%s'\n",argv[1]);
        return 1;
    }


    myXmlDoc = xmlNewDoc( "1.0" );
    mainNode = xmlNewDocNode( myXmlDoc, NULL, "dvdauthor", NULL );
    xmlDocSetRootElement(myXmlDoc, mainNode);
      
    for( i=0; i<=numtitlesets/*0*/; i++ ) {
      if( i )
        titlesetNode = xmlNewTextChild( mainNode, NULL, "titleset", NULL );
      else
        titlesetNode = xmlNewTextChild( mainNode, NULL, "vmgm", NULL );

      dump_dvd(dvd,i,0, titlesetNode );
      if( i ) {
        dump_dvd(dvd,i,1, titlesetNode );
      }
    }
     
    xmlSaveFormatFile( "dvdauthor.xml", myXmlDoc, 1 );
    xmlFreeDoc( myXmlDoc );

    DVDClose(dvd);
    return 0;
}
