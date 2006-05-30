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

// this is needed for FreeBSD and Windows
#include <sys/time.h>

#include <netinet/in.h>

static const char RCSID[]="$Id: //depot/dvdauthor/src/mpeg2desc.c#33 $";

// #define SHOWDATA

int pos=0;
int queuedlen=0;

char *frametype="0IPB4567";

#define SCRTIME 27000000
#define PTSTIME 90000

#define WAITLEN (256*1024)

#define BUFLEN (65536)

struct fdbuf {
    int pos,len;
    struct fdbuf *next;
    unsigned char buf[BUFLEN];
};

struct ofd {
    int fd;
    char *fname;
    struct fdbuf *firstbuf,**lastbufptr;
    int len,isvalid;
} outputfds[256];

int ofdlist[256],numofd;

int firstpts[256];

int closing=0, outputmplex=0;

fd_set rfd,wfd;


int64_t readpts(unsigned char *buf)
{
    int64_t a1,a2,a3,pts;
    
    a1=(buf[0]&0xf)>>1;
    a2=((buf[1]<<8)|buf[2])>>1;
    a3=((buf[3]<<8)|buf[4])>>1;
    pts=(((int64_t)a1)<<30)|
        (a2<<15)|
        a3;
    return pts;
}

int hasbecomevalid(int stream,struct ofd *o)
{
    unsigned char quad[4];
    struct fdbuf *f1=o->firstbuf,*f2;
    int i;
    unsigned int realquad;

    if( f1 )
        f2=f1->next;
    else
        f2=0;
    for( i=0; i<4; i++ ) {
        if( f1->len-f1->pos-i > 0 )
            quad[i]=f1->buf[f1->pos+i];
        else
            quad[i]=f2->buf[f2->pos+i-(f1->len-f1->pos)];
    }
    realquad=(quad[0]<<24)|
        (quad[1]<<16)|
        (quad[2]<<8)|
        quad[3];
    if( stream>=0xC0 && stream<0xE0 && (realquad&0xFFE00000)==0xFFE00000 )
        return 1;
    if( stream>=0xE0 && realquad==0x1B3 )
        return 1;
    return 0;
}

int dowork(int checkin)
{
    int i,n=-1;
    struct timeval tv;
    
    if( !numofd )
        return checkin;
    if( checkin ) {
        FD_SET(STDIN_FILENO,&rfd);
        n=STDIN_FILENO;
    } else {
        FD_CLR(STDIN_FILENO,&rfd);
    }
    while(1) {
        int minq=-1;
        for( i=0; i<numofd; i++ ) {
            struct ofd *o=&outputfds[ofdlist[i]];

            if( o->fd != -1 ) {
                if( o->fd == -2 ) {
                    int fd;
                    fd=open(o->fname,O_CREAT|O_WRONLY|O_NONBLOCK,0666);
                    if( fd == -1 && errno == ENXIO ) {
                        continue;
                    }
                    if( fd == -1 ) {
                        fprintf(stderr,"Cannot open %s: %s\n",o->fname,strerror(errno));
                        exit(1);
                    }
                    o->fd=fd;
                }
                // at this point, fd >= 0 
                if( minq == -1 || o->len < minq ) {
                    minq=o->len;
                }
                if( (o->len > 0 && o->isvalid) || o->len >= 4 ) {
                    if( o->fd>n )
                        n=o->fd;
                    FD_SET(o->fd,&wfd);
                } else {
                    FD_CLR(o->fd,&wfd);
                    if( closing ) {
                        close(o->fd);
                        o->fd=-1;
                    }
                }
            }
        }
        // if all the open files have more then WAITLEN bytes of data
        // queued up, then don't process anymore
        if( minq >= WAITLEN ) {
            FD_CLR(STDIN_FILENO,&rfd);
            break;
        } else if( minq >= 0 || outputmplex ) // as long as one file is open, continue
            break;
        sleep(1);
    }
    if( n == -1 )
        return 0;
    tv.tv_sec=1; // set timeout to 1 second just in case any files need to be opened
    tv.tv_usec=0;
    i=select(n+1,&rfd,&wfd,NULL,&tv);
    if( i > 0 ) {
        for( i=0; i<numofd; i++ ) {
            struct ofd *o=&outputfds[ofdlist[i]];
            if( o->fd >= 0 && FD_ISSET( o->fd, &wfd ) ) {
                struct fdbuf *f=o->firstbuf;
                if( !o->isvalid && hasbecomevalid(ofdlist[i],o) )
                    o->isvalid=1;

                if( o->isvalid )
                    n=write(o->fd,f->buf+f->pos,f->len-f->pos);
                else if( f->len-f->pos > 0 )
                    n=1;
                else
                    n=0;

                if( n == -1 ) {
                    fprintf(stderr,"Error writing to fifo: %s\n",strerror(errno));
                    exit(1);
                }
                queuedlen-=n;
                f->pos+=n;
                if( f->pos == f->len ) {
                    o->firstbuf=f->next;
                    if( o->lastbufptr == &f->next )
                        o->lastbufptr=&o->firstbuf;
                    free(f);
                }
                o->len-=n;
            }
        }
        if( FD_ISSET( STDIN_FILENO, &rfd ) )
            return 1;
    }
    return 0;
}

int forceread(void *ptr,int len,FILE *h)
{
    while(!dowork(1));
    if( fread(ptr,1,len,h) != len ) {
        fprintf(stderr,"Could not read\n");
        closing=1;
        while( queuedlen )
            dowork(0);
        exit(1);
    }
    pos+=len;
    return len;
}

int forceread1(void *ptr,FILE *h)
{
    int v=fgetc(h);

    if( v<0 ) {
        fprintf(stderr,"Could not read\n");
        closing=1;
        while( queuedlen )
            dowork(0);
        exit(1);
    }
    ((unsigned char *)ptr)[0]=v;
    pos+=1;
    return 1;
}

void writetostream(int stream,unsigned char *buf,int len)
{
    struct ofd *o=&outputfds[stream];

    if( o->fd == -1 )
        return;

    while( len > 0 ) {
        int thislen;
        struct fdbuf *fb;
        if( !o->lastbufptr[0] ) {
            o->lastbufptr[0]=malloc(sizeof(struct fdbuf));
            o->lastbufptr[0]->pos=0;
            o->lastbufptr[0]->len=0;
            o->lastbufptr[0]->next=0;
        }
        fb=o->lastbufptr[0];
        thislen=BUFLEN-fb->len;
        if( !thislen ) {
            o->lastbufptr=&fb->next;
            continue;
        }
        if( thislen > len )
            thislen=len;
        
        o->len+=thislen;
        memcpy(fb->buf+fb->len,buf,thislen);
        fb->len+=thislen;
        len-=thislen;
        buf+=thislen;
        queuedlen+=thislen;
    }
}

int main(int argc,char **argv)
{
    unsigned int hdr=0, mpeg2=1;
    unsigned char c,buf[200];
    int outputenglish=1,outputstream=0,oc, i,skiptohdr=0,audiodrop=0,nounknown=0;

    for( oc=0; oc<256; oc++ )
        outputfds[oc].fd=-1;

    while( -1 != (oc=getopt(argc,argv,"ha:v:o:msd:u")) ) {
        switch(oc) {
        case 'd':
            audiodrop=atoi(optarg);
            break;

        case 'a':
        case 'v':
            if( outputstream ) {
                fprintf(stderr,"can only output one stream to stdout at a time\n; use -o to output more than\none stream\n");
                exit(1);
            }
            outputstream=((oc=='a')?0xc0:0xe0)+atoi(optarg);
            break;

        case 'm':
            outputmplex=1;
            break;

        case 's':
            skiptohdr=1;
            break;

        case 'o':
            if( !outputstream ) {
                fprintf(stderr,"no stream selected for '%s'\n",optarg);
                exit(1);
            }
            outputfds[outputstream].fd=-2;
            outputfds[outputstream].fname=optarg;
            outputstream=0;
            break;

        case 'u':
            nounknown=1;
            break;

            // case 'h':
        default:
            fprintf(stderr,
                    "usage: mpeg2desc [options] < movie.mpg\n"
                    "\t-a #: output audio stream # to stdout\n"
                    "\t-v #: output video stream # to stdout\n"
                    "\t-o FILE: output previous stream to FILE instead of stdout\n"
                    "\t-s: skip to first valid header -- ensures mplex can handle output\n"
                    "\t-m: output mplex offset to stdout\n"
                    "\t-u: ignore unknown hdrs\n"
                    "\t-h: help\n"
                );
            exit(1);
        }
    }
    if( outputstream ) {
        outputenglish=0;
        outputfds[outputstream].fd=STDOUT_FILENO;
    }
    if( outputmplex ) {
        if( !outputenglish ) {
            fprintf(stderr,"Cannot output a stream and the mplex offset at the same time\n");
            exit(1);
        }
        outputenglish=0;
    }
    numofd=0;
    for( oc=0; oc<256; oc++ )
        if( outputfds[oc].fd != -1 ) {
            ofdlist[numofd++]=oc;
            outputfds[oc].firstbuf=0;
            outputfds[oc].lastbufptr=&outputfds[oc].firstbuf;
            outputfds[oc].len=0;
            outputfds[oc].isvalid=!skiptohdr;
        }
    FD_ZERO(&rfd);
    FD_ZERO(&wfd);    

    for( i=0; i<256; i++ ) {
        firstpts[i]=-1;
    }

    forceread(&hdr,4,stdin);
    while(1) {
        int disppos=pos-4;
        switch( ntohl(hdr) ) {
            // start codes:
        case 0x100: // picture header
            forceread(buf,4,stdin);
            if( outputenglish )
                printf("%08x: picture hdr, frametype=%c, temporal=%d\n",disppos,frametype[(buf[1]>>3)&7],(buf[0]<<2)|(buf[1]>>6));
            forceread(&hdr,4,stdin);
            break;

        case 0x1b3: // sequence header
            forceread(buf,8,stdin);
            if( outputenglish )
                printf("%08x: sequence hdr: %dx%d, a/f:%02x, bitrate=%d\n"
                       ,disppos
                       ,(buf[0]<<4)|(buf[1]>>4)
                       ,((buf[1]<<8)&0xf00)|(buf[2])
                       ,buf[3]
                       ,(buf[4]<<10)|(buf[5]<<2)|(buf[6]>>6)
                    );
            if( buf[7]&2 )
                forceread(buf+8,64,stdin);
            if( buf[7]&1 )
                forceread(buf+8,64,stdin);
            forceread(&hdr,4,stdin);
            break;

        case 0x1b5: // extension header
            forceread(buf,1,stdin);
            switch( buf[0]>>4 ) {
            case 1:
                if( outputenglish )
                    printf("%08x: sequence extension hdr\n",disppos);
                forceread(buf+1,5,stdin);
                break;
            case 2:
                if( outputenglish )
                    printf("%08x: sequence display extension hdr\n",disppos);
                forceread(buf+1,(buf[0]&1)?7:3,stdin);
                break;
            case 7:
                if( outputenglish )
                    printf("%08x: picture display extension hdr\n",disppos);
                break;
            case 8:
                forceread(buf+1,4,stdin);
                if( buf[4]&64 )
                    forceread(buf+5,2,stdin);
                if( outputenglish ) {
                    printf("%08x: picture coding extension hdr%s\n",disppos,(buf[3]&2)?", repeat":"");
                }
                break;
            default:
                if( outputenglish )
                    printf("%08x: extension hdr %x\n",disppos,buf[0]>>4);
                break;
            }
            forceread(&hdr,4,stdin);
            break;

        case 0x1b8: // group of pictures
            forceread(buf,4,stdin);
            if( outputenglish ) {
                printf("%08x: GOP: %s%d:%02d:%02d.%02d, %s%s\n"
                       ,disppos
                       ,buf[0]&128?"drop, ":""
                       ,(buf[0]>>2)&31
                       ,((buf[0]<<4)|(buf[1]>>4))&63
                       ,((buf[1]<<3)|(buf[2]>>5))&63
                       ,((buf[2]<<1)|(buf[3]>>7))&63
                       ,buf[3]&64?"closed":"open"
                       ,buf[3]&32?", broken":""
                    );
            }
            forceread(&hdr,4,stdin);
            break;

        case 0x1b9: // end of program stream
            if( outputenglish )
                printf("%08x: end of program stream\n",disppos);
            forceread(&hdr,4,stdin);
            break;

        case 0x1ba: // mpeg_pack_header
        {
            long scr,scrhi,scrext;
            int64_t fulltime;
            forceread(buf,8,stdin);
            if((buf[0] & 0xC0) == 0x40) {
	        forceread(buf+8,2,stdin);
                scrhi=(buf[0]&0x20)>>5;
                scr=((buf[0]&0x18)<<27)|
                    ((buf[0]&3)<<28)|
                    (buf[1]<<20)|
                    ((buf[2]&0xf8)<<12)|
                    ((buf[2]&3)<<13)|
                    (buf[3]<<5)|
                    ((buf[4]&0xf8)>>3);
                scrext=((buf[4]&3)<<7)|
                    (buf[5]>>1);
                if( scrext >= 300 && outputenglish ) {
                    printf("WARN: scrext in pack hdr > 300: %ld\n",scrext);
                }
                fulltime=((int64_t)scrhi)<<32|((int64_t)scr);
                fulltime*=300;
                fulltime+=scrext;
                mpeg2 = 1;
	    } else if((buf[0] & 0xF0) == 0x20) {
                mpeg2 = 0;
		fulltime=readpts(buf);
                fulltime*=300;
	    } else {
                if( outputenglish )
                    printf("WARN: unknown pack header version\n");
                fulltime=0;
            }

            if( outputenglish )
                printf("%08x: mpeg%c pack hdr, %lld.%03lld sec\n",disppos,mpeg2?'2':'1',fulltime/SCRTIME,(fulltime%SCRTIME)/(SCRTIME/1000));
            
            forceread(&hdr,4,stdin);
            break;
        }

        case 0x1bb: // mpeg_system_header
        case 0x1bd:
        case 0x1be:
        case 0x1bf:
        case 0x1c0:
        case 0x1c1:
        case 0x1c2:
        case 0x1c3:
        case 0x1c4:
        case 0x1c5:
        case 0x1c6:
        case 0x1c7:
        case 0x1c8:
        case 0x1c9:
        case 0x1ca:
        case 0x1cb:
        case 0x1cc:
        case 0x1cd:
        case 0x1ce:
        case 0x1cf:
        case 0x1d0:
        case 0x1d1:
        case 0x1d2:
        case 0x1d3:
        case 0x1d4:
        case 0x1d5:
        case 0x1d6:
        case 0x1d7:
        case 0x1d8:
        case 0x1d9:
        case 0x1da:
        case 0x1db:
        case 0x1dc:
        case 0x1dd:
        case 0x1de:
        case 0x1df:
        case 0x1e0:
        case 0x1e1:
        case 0x1e2:
        case 0x1e3:
        case 0x1e4:
        case 0x1e5:
        case 0x1e6:
        case 0x1e7:
        case 0x1e8:
        case 0x1e9:
        case 0x1ea:
        case 0x1eb:
        case 0x1ec:
        case 0x1ed:
        case 0x1ee:
        case 0x1ef:
        {
            unsigned char c;
            int ext=0,extra=0,readlen,dowrite=1;

            c=ntohl(hdr);
            if( outputenglish )
                printf("%08x: ",disppos);
            if( c == 0xBB ) {
                if( outputenglish )
                    printf("system header");
            } else if( c == 0xBD ) {
                if (outputenglish)
                    printf("pes private1");
                ext=1;
            } else if( c == 0xBE ) {
                if( outputenglish )
                    printf("pes padding");
            } else if( c == 0xBF ) {
                if( outputenglish )
                    printf("pes private2");
            } else if( c >= 0xC0 && c <= 0xDF ) {
                if( outputenglish )
                    printf("pes audio %d",c-0xC0);
                if( audiodrop ) {
                    dowrite=0;
                    audiodrop--;
                }
                ext=1;
            } else if( c >= 0xE0 && c <= 0xEF ) {
                if( outputenglish )
                    printf("pes video %d",c-0xE0);
                ext=1;
            }
            forceread(buf,2,stdin); // pes packet length
            extra=(buf[0]<<8)|buf[1];
            readlen=forceread(buf,(extra>sizeof(buf))?sizeof(buf):extra,stdin);
            extra-=readlen;
            if( outputenglish ) {
                if( ntohl(hdr)==0x1bd ) { // private stream 1
                    int sid=buf[3+buf[2]];
                    switch( sid&0xe0 ) {
                    case 0x20: printf(", subpicture %d",sid&0x1f); break;
                    case 0x80: printf(", audio %d",sid&0x1f); break;
                    case 0xa0: printf(", lpcm %d",sid&0x1f); break;
                    default: printf(", substream id 0x%02x",sid); break;
                    }
                }
                printf("; length=%d",extra+readlen);
                if( ext ) {
                    int eptr=3;
		    int hdr=0, has_pts, has_dts, has_std=0, std=0, std_scale=0;
		    
		    if((buf[0] & 0xC0) == 0x80) {
		        mpeg2 = 1;
			hdr = buf[2]+3;
			eptr = 3;
			has_pts = buf[1] & 128;
			has_dts = buf[1] & 64;
                    }
                    else {
		        mpeg2 = 0;
			while((buf[hdr] == 0xff) && (hdr<sizeof(buf)))
			    hdr++;
                        if((buf[hdr] & 0xC0) == 0x40) {
			    has_std = 1;
			    std_scale = ((buf[hdr]&32)?1024:128);
			    std = ((buf[hdr]&31)*256+buf[hdr+1])*std_scale;
			    hdr+=2;
                        } else has_std = 0;
                        eptr = hdr;
			has_pts = (buf[hdr] & 0xE0) == 0x20;
			has_dts = (buf[hdr] & 0xF0) == 0x30;
                    }

                    printf("; hdr=%d",hdr);
                    if( has_pts ) {
                        int64_t pts;
                        
                        pts=readpts(buf+eptr);
                        eptr+=5;
                        printf("; pts %lld.%03lld sec",pts/PTSTIME,(pts%PTSTIME)/(PTSTIME/1000));
                    }
                    if( has_dts ) {
                        int64_t dts;
                    
                        dts=readpts(buf+eptr);
                        eptr+=5;
                        printf("; dts %lld.%03lld sec",dts/PTSTIME,(dts%PTSTIME)/(PTSTIME/1000));
                    }
		    if(mpeg2) {
                        if( buf[1] & 32 ) {
                            printf("; escr");
                            eptr+=6;
                        }
                        if( buf[1] & 16 ) {
                            printf("; es");
                            eptr+=2;
                        }
                        if( buf[1] & 4 ) {
                            printf("; ci");
                            eptr++;
                        }
                        if( buf[1] & 2 ) {
                            printf("; crc");
                            eptr+=2;
                        }
                        if( buf[1] & 1 ) {
                            int pef=buf[eptr];
                            eptr++;
                            printf("; (pext)");
                            if( pef & 128 ) {
                                printf("; user");
                                eptr+=16;
                            }
                            if( pef & 64 ) {
                                printf("; pack");
                                eptr++;
                            }
                            if( pef & 32 ) {
                                printf("; ppscf");
                                eptr+=2;
                            }
                            if( pef & 16 ) {
                                std_scale=((buf[eptr]&32)?1024:128);
                                printf("; pstd=%d (scale=%d)",((buf[eptr]&31)*256+buf[eptr+1])*std_scale, std_scale);
                                eptr+=2;
                            }
                            if( pef & 1 ) {
                                printf("; (pext2)");
                                eptr+=2;
                            }
                        }
		    } else {
                        if(has_std)
                            printf("; pstd=%d (scale=%d)",std, std_scale);
		    }
                }
                printf("\n");
            }
            if( outputmplex && ext ) {
                if( buf[1]&128 && firstpts[c] == -1 )
                    firstpts[c]=readpts(buf+3);
                if( firstpts[0xC0] != -1 &&
                    firstpts[0xE0] != -1 ) {
                    printf("%d\n",firstpts[0xE0]-firstpts[0xC0]);
                    fflush(stdout);
                    close(1);
                    outputmplex=0;
                    if( !numofd )
                        exit(0);
                }
            }
#ifdef SHOWDATA
            if( ext && outputenglish ) {
                int i=3+buf[2], j;
                printf("  ");
                for( j=0; j<16; j++ )
                    printf(" %02x",buf[j+i]);
                printf("\n");
            }
#endif
            if( ext ) {
                if( dowrite )
                    writetostream(ntohl(hdr)&255,buf+3+buf[2],readlen-3-buf[2]);
            }

            while( extra ) {
                readlen=forceread(buf,(extra>sizeof(buf))?sizeof(buf):extra,stdin);
                if( dowrite )
                    writetostream(ntohl(hdr)&255,buf,readlen);
                extra-=readlen;
            }
            forceread(&hdr,4,stdin);
            break;
        }

        default:
            do {
                if( outputenglish && !nounknown )
                    printf("%08x: unknown hdr: %08x\n",disppos,ntohl(hdr));
                hdr>>=8;
                forceread1(&c,stdin);
                hdr|=c<<24;
            } while( (ntohl(hdr)&0xffffff00)!=0x100 );
            break;
        }
    }
}
