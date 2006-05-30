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
#include <fcntl.h>
#include <math.h>

#ifdef HAVE_MAGICK
#include <magick/api.h>
#else
#include <png.h>
#endif

#include "subgen.h"
#include "textsub.h"

static const char RCSID[]="$Id: //depot/dvdauthor/src/subgen-image.c#23 $";

#define MAXX 720
#define MAXY 576

static void constructblankpic(pict *p,int w,int h)
{
    w+=w&1;
    h+=h&1;
    p->width=w;
    p->height=h;
    p->fname=0;
    p->img=malloc(w*h);
    p->numpal=1;
    p->pal[0].r=0;
    p->pal[0].g=0;
    p->pal[0].b=0;
    p->pal[0].t=0;
    memset(p->img,0,w*h);
}

typedef struct {
    int numpal;
    int pal[4];
} palgroup;

// ****************************************************
//
// Bitmap reading code

// this function scans through a picture and looks for the color that
// the user indicated should mean transparent
// if the color is found, it is replaced by 0,0,0,0 meaning "transparent"
static void scanpict(stinfo *s,pict *p)
{
    int i;

    // we won't replace the background color if there is already evidence
    // of alpha channel information
    for( i=0; i<p->numpal; i++ )
        if( p->pal[i].t!=255 )
            goto skip_transreplace;
    for( i=0; i<p->numpal; i++ )
        if( !memcmp(p->pal+i,&s->transparentc,sizeof(palt)) ) {
            memset(&p->pal[i],0,sizeof(palt));
            break;
        }

 skip_transreplace:
    for( i=0; i<p->numpal; i++ )
        findmasterpal(s,p->pal+i);
}

static void putpixel(pict *p,int x,palt *c)
{
    int i;

    if( !c->t ) {
        c->r=0;
        c->g=0;
        c->b=0;
    }
    for( i=0; i<p->numpal; i++ )
        if( !memcmp(&p->pal[i],c,sizeof(palt)) ) {
            p->img[x]=i;
            return;
        }
    if( p->numpal==256 ) {
        p->img[x]=0;
        return;
    }
    p->img[x]=p->numpal;
/*    fprintf(stderr,"CREATING COLOR %d,%d,%d %d\n",c->r,c->g,c->b,c->t); */
    p->pal[p->numpal++]=*c;
}

static void createimage(pict *s,int w,int h)
{
    s->numpal=0;
    s->width=w+(w&1);
    s->height=h+(h&1);
    s->img=malloc(s->width*s->height);
    if( w!=s->width ) {
        int y;
        palt t;

        t.r=0;
        t.g=0;
        t.b=0;
        t.t=0;
        for( y=0; y<h; y++ )
            putpixel(s,w+y*s->width,&t);
    }
    if( h!=s->height ) {
        int x;
        palt t;

        t.r=0;
        t.g=0;
        t.b=0;
        t.t=0;
        for( x=0; x<s->width; x++ )
            putpixel(s,x+h*s->width,&t);
    }
}

#ifdef HAVE_MAGICK
static int read_magick(pict *s)
{
    Image *im;
    ImageInfo *ii;
    ExceptionInfo ei;
    int x,y;

    GetExceptionInfo(&ei);
    ii=CloneImageInfo(NULL);
    strcpy(ii->filename,s->fname);
    im=ReadImage(ii,&ei);

    if( !im ) {
        MagickError(ei.severity,"Unable to load file",ii->filename);
        return -1;
    }

    if( im->columns>MAXX || im->rows>MAXY ) {
        fprintf(stderr,"ERR: Picture %s is too big: %lux%lu\n",s->fname,im->columns,im->rows);
        DestroyImage(im);
        return -1;
    }
    createimage(s,im->columns,im->rows);
    for( y=0; y<im->rows; y++ ) {
        char pdata[MAXX*4];

        if(!ExportImagePixels(im,0,y,im->columns,1,"RGBA",CharPixel,pdata,&ei)) {
            fprintf(stderr,"ERR: Extracting row %d from %s (%s,%s)\n",y,s->fname,ei.reason,ei.description);
            CatchException(&ei);
            MagickError(ei.severity,ei.reason,ei.description);
            DestroyImage(im);
            return -1;
        }
        for( x=0; x<im->columns; x++ ) {
            palt p;

            p.r=pdata[x*4];
            p.g=pdata[x*4+1];
            p.b=pdata[x*4+2];
            // the meaning of RGBA swapped with ImageMagick 6.0.0...
#if MagickLibVersion >= 0x600
            p.t=pdata[x*4+3];
#else
            p.t=255-pdata[x*4+3];
#endif
            putpixel(s,y*s->width+x,&p);
        }
    }
    DestroyImage(im);
    DestroyExceptionInfo(&ei);
    fprintf(stderr,"INFO: Picture %s had %d colors\n",s->fname,s->numpal);

    return 0;
}
#else
static int read_png(pict *s)
{
    unsigned char pnghead[8];
    FILE *fp;
    png_struct *ps;
    png_info *pi;
    png_byte **rowp;
    unsigned long width,height;
    int bit_depth,color_type,channels,x,y;

    fp=fopen(s->fname,"rb");
    if( !fp ) {
	fprintf(stderr,"ERR: Unable to open file %s\n",s->fname);
	return -1;
    }
    fread(pnghead,1,8,fp);
    if(png_sig_cmp(pnghead,0,8)) {
	fprintf(stderr,"ERR: File %s isn't a png\n",s->fname);
	fclose(fp);
	return -1;
    }
    ps=png_create_read_struct(PNG_LIBPNG_VER_STRING,NULL,NULL,NULL);
    if( !ps ) {
        fprintf(stderr,"ERR: Initializing png\n");
        fclose(fp);
        return -1;
    }
    pi=png_create_info_struct(ps);
    if( !pi ) {
        fprintf(stderr,"ERR: Initializing png\n");
        png_destroy_read_struct(&ps,NULL,NULL);
        fclose(fp);
        return -1;
    }
    png_init_io(ps,fp);
    png_set_sig_bytes(ps,8);

    png_read_png(ps,pi,PNG_TRANSFORM_PACKING|PNG_TRANSFORM_STRIP_16|PNG_TRANSFORM_EXPAND,NULL);
    rowp=png_get_rows(ps,pi);
    png_get_IHDR(ps,pi,&width,&height,&bit_depth,&color_type,NULL,NULL,NULL);
    // format is now RGB[A] or G[A]
    channels=png_get_channels(ps,pi);
    fclose(fp);
    if(color_type&PNG_COLOR_MASK_COLOR)
        channels-=3;
    else
        channels--;
    if(color_type&PNG_COLOR_MASK_ALPHA)
        channels--;
    assert(bit_depth==8); // 8bpp, not 1, 2, 4, or 16
    assert(!(color_type&PNG_COLOR_MASK_PALETTE)); // not a palette
    if( width>MAXX || height>MAXY ) {
        fprintf(stderr,"ERR: PNG %s is too big: %lux%lu\n",s->fname,width,height);
        png_destroy_read_struct(&ps,&pi,NULL);
        return -1;
    }
    createimage(s,width,height);
    for( y=0; y<height; y++ ) {
        unsigned char *d=rowp[y];
        for( x=0; x<width; x++ ) {
            palt p;

            if(color_type&PNG_COLOR_MASK_COLOR) {
                p.r=*d++;
                p.g=*d++;
                p.b=*d++;
            } else {
                p.r=*d++;
                p.g=p.r;
                p.b=p.r;
            }
            if( color_type&PNG_COLOR_MASK_ALPHA )
                p.t=*d++;
            else
                p.t=255;
            d+=channels;
            putpixel(s,y*s->width+x,&p);
        }
        // free(rowp[y]);
    }
    // free(rowp);
    png_destroy_read_struct(&ps,&pi,NULL);
    fprintf(stderr,"INFO: PNG had %d colors\n",s->numpal);

    return 0;
}
#endif

static int read_frame(pict *s)
{
  int x,y;

  createimage(s,movie_width,movie_height);
  for( y=0; y<movie_height; y++ )
  {
	unsigned char *d=image_buffer+(y*movie_width*3);

    for( x=0; x<movie_width; x++ )
	{
      palt p;
	  p.r=*d++;
      p.g=*d++;
      p.b=*d++;
	  p.t=255;
	  putpixel(s,y*s->width+x,&p);
	}
  }
  return 0;
}

static int read_pic(stinfo *s,pict *p)
{
    int r=0;
    if ( have_textsub)
    {
        textsub_render(s->sub_title);
        r=read_frame(p);
    }
    else
    {
        if( !p->fname )
            return 0;
#ifdef HAVE_MAGICK
        r=read_magick(p);
#else
        r=read_png(p);
#endif
    }
    if( !r )
        scanpict(s,p);
    return r;
}


static int checkcolor(palgroup *p,int c)
{
    int i;

    for( i=0; i<p->numpal; i++ )
        if( p->pal[i]==c )
            return 1;
    if( p->numpal==4 )
        return 0;
    p->pal[p->numpal++]=c;
    return 1;
}

static int gettricolor(stinfo *s,int p,int useimg)
{
    return (useimg?s->img.img[p]<<16:0)|
        (s->hlt.img[p]<<8)|
        (s->sel.img[p]);
}

static int pickbuttongroups(stinfo *s,int ng,int useimg)
{
    palgroup *bpgs,*gs;
    int i,x,y,j,k,enb;

    bpgs=malloc(s->numbuttons*sizeof(palgroup));
    memset(bpgs,0,s->numbuttons*sizeof(palgroup));

    gs=malloc(ng*sizeof(palgroup));
    memset(gs,0,ng*sizeof(palgroup));

    // fprintf(stderr,"attempt %d groups, %d useimg\n",ng,useimg);
    // find unique colors per button
    for( i=0; i<s->numbuttons; i++ ) {
        button *b=&s->buttons[i];
        palgroup *bp=&bpgs[i];

        for( y=b->r.y0; y<b->r.y1; y++ )
            for( x=b->r.x0; x<b->r.x1; x++ )
                if( !checkcolor(bp,gettricolor(s,y*s->xd+x,useimg)) )
                    goto impossible;
        // fprintf(stderr,"pbg: button %d has %d colors\n",i,bp->numpal);
    }

    // assign to groups
    enb=1;
    for( i=0; i<s->numbuttons; i++ )
        enb*=ng;
    for( i=0; i<enb; i++ ) {
        // check that the buttons fit in the buttongroups
        for( j=0; j<ng; j++ )
            gs[j].numpal=0;
        // fprintf(stderr,"trying ");
        k=i;
        for( j=0; j<s->numbuttons; j++ ) {
            int l;
            palgroup *pd=&gs[k%ng],*ps=&bpgs[j];

            s->buttons[j].grp=k%ng+1;
            // fprintf(stderr,"%s%d",j?", ":"",s->buttons[j].grp);
            k/=ng;
            for( l=0; l<ps->numpal; l++ )
                if( !checkcolor(pd,ps->pal[l]) ) {
                    // fprintf(stderr," -- failed mapping button %d\n",j);
                    goto trynext;
                }
        }
        // if necessary, check if the image fits in one palette
        if(useimg) {
            palgroup p;

            p.numpal=s->img.numpal;
            for( j=0; j<p.numpal; j++ )
                p.pal[j]=j;
            for( j=0; j<ng; j++ ) {
                for( k=0; k<4; k++ )
                    s->groupmap[j][k]=-1;
                for( k=0; k<p.numpal; k++ )
                    p.pal[k]&=255;
                for( k=0; k<gs[j].numpal; k++ ) {
                    int l,c;

                    c=(gs[j].pal[k]>>16);
                    for( l=0; l<p.numpal; l++ )
                        if( p.pal[l] == c ) {
                            goto ui_found;
                        }
                    if( p.numpal==4 ) {
                        // fprintf(stderr," -- failed finding unique overall palette\n");
                        goto trynext;
                    }
                    p.numpal++;
                ui_found:
                    p.pal[l]=c|256;
                    s->groupmap[j][l]=gs[j].pal[k];
                }

            }
        } else {
            for( j=0; j<ng; j++ ) {
                for( k=0; k<gs[j].numpal; k++ )
                    s->groupmap[j][k]=gs[j].pal[k];
                for( ; k<4; k++ )
                    s->groupmap[j][k]=-1;
            }
        }
        free(bpgs);
        free(gs);
        fprintf(stderr,"INFO: Pickbuttongroups, success with %d groups, useimg=%d\n",ng,useimg);
        s->numgroups=ng;
        return 1;
    trynext:
        continue; // 'deprecated use of label at end of compound statement'
    }

 impossible:
    free(bpgs);
    free(gs);
    return 0;
}

static void fixnames(stinfo *s)
{
    int i;

    for( i=0; i<s->numbuttons; i++ )
        if( !s->buttons[i].name ) {
            char n[10];
            sprintf(n,"%d",i+1);
            s->buttons[i].name=strdup(n);
        }
}

// a0 .. a1, b0 .. b1 are analogous to y coordinates, positive -> high, negative->low
// d is the distance from a to b (assuming b is to the right of a, i.e. positive x)
// returns angle (0 = straight right, 90 = straight up, -90 = straight down) from a to b
static void brphelp(int a0,int a1,int b0,int b1,int d,int *angle,int *dist)
{
    int d1,d2,m;

    if( a1>b0 && a0<b1 ) {
        *angle=0;
        *dist=d;
        return;
    }
    d1=-(b0-a1+1);
    d2= (a0-b1+1);
    d++;
    if( abs(d2)<abs(d1) ) {
        d1=d2;
    }
    m=1;
    if( d1<0 ) {
        d1=-d1;
        m=-1;
    }
    *angle=m*180/M_PI*atan(((double)d1)/((double)d));
    *dist=sqrt(d1*d1+d*d);
}

static int buttonrelpos(rectangle *a,rectangle *b,int *angle,int *dist)
{
    // from b to a

    if( a->y1<=b->y0 ) {
        brphelp(a->x0,a->x1,b->x0,b->x1,b->y0-a->y1,angle,dist);

        // fprintf(stderr,"from %dx%d-%dx%d to %dx%d-%dx%d is angle %d, dist %d\n",b->x0,b->y0,b->x1,b->y1,a->x0,a->y0,a->x1,a->y1,*angle,*dist);
        return 1;
    }
    if( a->y0>=b->y1 ) {
        brphelp(a->x0,a->x1,b->x0,b->x1,a->y0-b->y1,angle,dist);
        *angle=180-*angle;

        // fprintf(stderr,"from %dx%d-%dx%d to %dx%d-%dx%d is angle %d, dist %d\n",b->x0,b->y0,b->x1,b->y1,a->x0,a->y0,a->x1,a->y1,*angle,*dist);
        return 1;
    }
    if( a->x1<=b->x0 ) {
        brphelp(a->y0,a->y1,b->y0,b->y1,b->x0-a->x1,angle,dist);
        *angle=270-*angle;

        // fprintf(stderr,"from %dx%d-%dx%d to %dx%d-%dx%d is angle %d, dist %d\n",b->x0,b->y0,b->x1,b->y1,a->x0,a->y0,a->x1,a->y1,*angle,*dist);
        return 1;
    }
    if( a->x0>=b->x1 ) {
        brphelp(a->y0,a->y1,b->y0,b->y1,a->x0-b->x1,angle,dist);
        *angle=90+*angle;

        // fprintf(stderr,"from %dx%d-%dx%d to %dx%d-%dx%d is angle %d, dist %d\n",b->x0,b->y0,b->x1,b->y1,a->x0,a->y0,a->x1,a->y1,*angle,*dist);
        return 1;
    }

    // fprintf(stderr,"from %dx%d-%dx%d to %dx%d-%dx%d -- no easy comparison\n",b->x0,b->y0,b->x1,b->y1,a->x0,a->y0,a->x1,a->y1);
    return 0;
}

static void findbestbindir(stinfo *s,button *b,char **dest,int a)
{
    int i,la=0,ld=0;

    if( *dest ) return;
    // fprintf(stderr,"locating nearest button from %s, angle %d\n",b->name,a);
    for( i=0; i<s->numbuttons; i++ )
        if( b!=&s->buttons[i] && !s->buttons[i].autoaction ) {
            int na,nd;

            if( buttonrelpos(&s->buttons[i].r,&b->r,&na,&nd) ) {
                na=abs(na-a);
                if( na >= 90 )
                    continue;
                if( !*dest || na<la || (na==la && nd<ld) ) {
                    // fprintf(stderr,"\tchoosing %s, na=%d, d=%d\n",s->buttons[i].name,na,nd);
                    *dest=s->buttons[i].name;
                    la=na;
                    ld=nd;
                }
            }
        }
    if( *dest )
        *dest=strdup(*dest);
    else
        *dest=strdup(b->name);
}

static void detectdirections(stinfo *s)
{
    int i;

    for( i=0; i<s->numbuttons; i++ ) {
        findbestbindir(s,&s->buttons[i],&s->buttons[i].up,0);
        findbestbindir(s,&s->buttons[i],&s->buttons[i].down,180);
        findbestbindir(s,&s->buttons[i],&s->buttons[i].left,270);
        findbestbindir(s,&s->buttons[i],&s->buttons[i].right,90);
    }
}

#define MAX(a,b) (((a)>(b))?(a):(b))
#define MIN(a,b) (((a)<(b))?(a):(b))

static int scanvline(stinfo *s,unsigned char *v,rectangle *r,int x,int d)
{
    int i,j;

    for( j=1; j<=s->outlinewidth; j++ ) {
        x+=d;
        if( x<0 || x>=s->xd )
            return 0;
        for( i=MAX(r->y0-j,0); i<MIN(r->y1+j,s->yd); i++ )
            if( v[i*s->xd+x] )
                return 1;
    }
    return 0;
}

static int scanhline(stinfo *s,unsigned char *v,rectangle *r,int y,int d)
{
    int i,j;

    for( j=1; j<=s->outlinewidth; j++ ) {
        y+=d;
        if( y<0 || y>=s->yd )
            return 0;
        for( i=MAX(r->x0-j,0); i<MIN(r->x1+j,s->xd); i++ )
            if( v[y*s->xd+i] )
                return 1;
    }
    return 0;
}

static void detectbuttons(stinfo *s)
{
    unsigned char *visitmask=malloc(s->xd*s->yd);
    int i,x,y;
    rectangle *rs=0;
    int numr=0;

    if( !s->outlinewidth )
        s->outlinewidth=1;
    for( i=0; i<s->xd*s->yd; i++ )
        visitmask[i]=( s->hlt.pal[s->hlt.img[i]].t || s->sel.pal[s->sel.img[i]].t ) ? 1 : 0;
    for( y=0; y<s->yd; y++ )
        for( x=0; x<s->xd; x++ )
            if( visitmask[y*s->xd+x] ) {
                rectangle r;
                int didwork;

                r.x0=x;
                r.y0=y;
                r.x1=x+1;
                r.y1=y+1;

                do {
                    didwork=0;
                    while( scanvline(s,visitmask,&r,r.x0,-1) ) {
                        r.x0--;
                        didwork=1;
                    }
                    while( scanvline(s,visitmask,&r,r.x1-1,1) ) {
                        r.x1++;
                        didwork=1;
                    }
                    while( scanhline(s,visitmask,&r,r.y0,-1) ) {
                        r.y0--;
                        didwork=1;
                    }
                    while( scanhline(s,visitmask,&r,r.y1-1,1) ) {
                        r.y1++;
                        didwork=1;
                    }
                } while(didwork);

                // add button r
                rs=realloc(rs,(numr+1)*sizeof(rectangle));
                rs[numr++]=r;

                // reset so we pass over
                for( i=r.y0; i<r.y1; i++ )
                    memset(visitmask+i*s->xd+r.x0,0,r.x1-r.x0);
            }
    free(visitmask);

    while(numr) {
        int j=0;
        // find the left most button on the top row
        if( !s->autoorder ) {
            for( i=1; i<numr; i++ )
                if( rs[i].y0 < rs[j].y0 ||
                    (rs[i].y0==rs[j].y0 && rs[i].x0 < rs[j].x0 ) )
                    j=i;
        } else {
            for( i=1; i<numr; i++ )
                if( rs[i].x0 < rs[j].x0 ||
                    (rs[i].x0==rs[j].x0 && rs[i].y0 < rs[j].y0 ) )
                    j=i;
        }
        // see if there are any buttons to the left, i.e. slightly overlapping vertically, but possibly start a little lower
        for( i=0; i<numr; i++ )
            if( i!=j ) {
                int a,d;
                if(buttonrelpos(rs+i,rs+j,&a,&d))
                    if( a==(s->autoorder?0:270) )
                        j=i;
            }

        // ok add rectangle 'j'

        for( i=0; i<s->numbuttons; i++ )
            if( s->buttons[i].r.x0<0 && !s->buttons[i].autoaction )
                break;
        if( i==s->numbuttons ) {
            s->numbuttons++;
            s->buttons=realloc(s->buttons,s->numbuttons*sizeof(button));
            memset(s->buttons+i,0,sizeof(button));
        }

        fprintf(stderr,"INFO: Autodetect %d = %dx%d-%dx%d\n",i,rs[j].x0,rs[j].y0,rs[j].x1,rs[j].y1);

        s->buttons[i].r=rs[j];
        memmove(rs+j,rs+j+1,(numr-j-1)*sizeof(rectangle));
        numr--;
    }
}

static int imgfix(stinfo *s)
{
    int i,useimg,w,h,x,y,x0,y0;

    w=s->img.width;
    h=s->img.height;
    s->xd=w; // pickbuttongroups needs these values set
    s->yd=h;

    if( s->autooutline )
        detectbuttons(s);

    fixnames(s);
    detectdirections(s);

    s->fimg=malloc(w*h);
    memset(s->fimg,255,w*h);

    // first try not to have multiple palettes for
    useimg=1;
    if( s->numbuttons ) {
        i=0;
        do {
            if( pickbuttongroups(s,1,useimg) ) break;
            if( pickbuttongroups(s,2,useimg) ) break;
            if( pickbuttongroups(s,3,useimg) ) break;
            useimg--;
        } while(useimg>=0);

        // at this point I don't want to deal with blocking the primary subtitle image
        assert(useimg);

        if( useimg<0 ) {
            fprintf(stderr,"ERR: Cannot pick button masks\n");
            return 0;
        }

        for( i=0; i<s->numbuttons; i++ ) {
            button *b=&s->buttons[i];

            for( y=b->r.y0; y<b->r.y1; y++ )
                for( x=b->r.x0; x<b->r.x1; x++ ) {
                    int dc=-1,p=y*w+x,j;
                    int c=gettricolor(s,p,useimg);
                    for( j=0; j<4; j++ )
                        if( s->groupmap[b->grp-1][j]==c ) {
                            dc=j;
                            break;
                        }
                    if( dc == -1 ) {
                        fprintf(stderr,"ERR: Button %d cannot find color %06x in group %d\n",i,c,b->grp-1);
                        assert(dc!=-1);
                    }
                    if( s->fimg[p]!=dc && s->fimg[p]!=255 ) {
                        fprintf(stderr,"ERR: Overlapping buttons\n");
                        return 0;
                    }
                    s->fimg[p]=dc;
                }
        }
    }
    for( i=0; i<4; i++ ) {
        s->pal[i].r=255;
        s->pal[i].g=255;
        s->pal[i].b=255;
        s->pal[i].t=0;
    }
    for( i=0; i<w*h; i++ )
        if( s->fimg[i]!=255 )
            s->pal[s->fimg[i]]=s->img.pal[s->img.img[i]];
    for( i=0; i<w*h; i++ )
        if( s->fimg[i]==255 ) {
            int j;
            palt *p=&s->img.pal[s->img.img[i]];

            for( j=0; j<4; j++ )
                if( !memcmp(&s->pal[j],p,sizeof(palt)) )
                    goto if_found;
            for( j=0; j<4; j++ )
                if( s->pal[j].t==0 && s->pal[j].r==255 ) {
                    s->pal[j]=*p;
                    goto if_found;
                }
            fprintf(stderr,"ERR: Too many colors in base picture\n");
            return 0;

        if_found:
            s->fimg[i]=j;
        }

    // determine minimal visual area, and crop the subtitle accordingly
    x0=w;
    y0=-1;
    s->xd=0;
    s->yd=0;
    for( i=0, y=0; y<h; y++ ) {
        for( x=0; x<w; i++, x++ ) {
            if( s->img.pal[s->img.img[i]].t ||
                s->hlt.pal[s->hlt.img[i]].t ||
                s->sel.pal[s->sel.img[i]].t ) {

                if( y0==-1 ) y0=y;
                s->yd=y;

                if( x<x0 ) x0=x;
                if( x>s->xd ) s->xd=x;
            }
        }
    }
    if( y0==-1 ) { // empty image?
        s->xd=w;
        s->yd=h;
        return 1;
    }
    x0&=-2;
    y0&=-2;
    s->xd=(s->xd+2-x0)&(-2);
    s->yd=(s->yd+2-y0)&(-2);
    for( i=0; i<s->yd; i++ )
        memmove(s->fimg+i*s->xd,s->fimg+i*w+x0+y0*w,s->xd);
    for( i=0; i<s->numbuttons; i++ ) {
        button *b=&s->buttons[i];
        b->r.x0+=s->x0;
        b->r.y0+=s->y0;
        b->r.x1+=s->x0;
        b->r.y1+=s->y0;
    }
    s->x0+=x0;
    s->y0+=y0;
    return 1;
} /* end function imgfix */

int process_subtitle(stinfo *s)
{
    int w=0,h=0;
    int iline=0;

    if( !s ) return 0;
	if( read_pic(s,&s->img) ) {
        if(debug > -1)
            fprintf(stderr, "WARN: Bad image,  skipping line %d\n", iline - 1);
        return 0;
    }
   	if( s->img.img && !w ) {
        w=s->img.width;
        h=s->img.height;
    }
    if( read_pic(s,&s->hlt) ) {
        if(debug > -1)
            fprintf(stderr, "WARN: Bad image,  skipping line %d\n", iline - 1);
        return 0;
    }
    if( s->hlt.img && !w ) {
        w=s->hlt.width;
        h=s->hlt.height;
    }
    if( read_pic(s,&s->sel) ) {
        if(debug > -1)
            fprintf(stderr, "WARN: Bad image,  skipping line %d\n", iline - 1);
        return 0;
    }
    if( s->sel.img && !w ) {
        w=s->sel.width;
        h=s->sel.height;
    }

    if( !w ) {
        fprintf(stderr,"WARN: No picture, skipping line %d\n",iline-1);
        return 0;
    }
    if( !s->img.img ) {
        constructblankpic(&s->img,w,h);
        fprintf(stderr,"INFO: Constructing blank img\n");
    } else if( s->img.width!=w || s->img.height!=h ) {
        fprintf(stderr,"WARN: Inconsistant picture widths, skipping line %d\n",iline-1);
        return 0;
    }
    if( !s->hlt.img ) {
        constructblankpic(&s->hlt,w,h);
        fprintf(stderr,"INFO: Constructing blank hlt\n");
    } else if( s->hlt.width!=w || s->hlt.height!=h ) {
        fprintf(stderr,"WARN: Inconsistant picture widths, skipping line %d\n",iline-1);
        return 0;
    }
    if( !s->sel.img ) {
        constructblankpic(&s->sel,w,h);
        fprintf(stderr,"INFO: Constructing blank sel\n");
    } else if( s->sel.width!=w || s->sel.height!=h ) {
        fprintf(stderr,"WARN: Inconsistant picture widths, skipping line %d\n",iline-1);
        return 0;
    }

    if( !imgfix(s) ) /* data in img to fimg */
    {
        if (debug > -1)
        {
            fprintf(stderr, "ERR: Blank image, skipping line %d\n", iline - 1);
        }
        return 0;
    }

    return 1;
}

void image_init()
{
#ifdef HAVE_MAGICK
    InitializeMagick(NULL);
#endif
}

void image_shutdown()
{
#ifdef HAVE_MAGICK
    DestroyMagick();
#endif
}
