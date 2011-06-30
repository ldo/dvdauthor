/*
    Reading of subpicture images and handling of buttons and associated palettes
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307
 * USA
 */

#include "config.h"

#include "compat.h"

#include <assert.h>
#include <fcntl.h>
#include <math.h>

#if defined(HAVE_MAGICK) || defined(HAVE_GMAGICK)
#include <stdarg.h>
#include <magick/api.h>
#else
#include <png.h>
#endif

#include "subglobals.h"
#include "subrender.h"
#include "subgen.h"


#define MAXX 720
#define MAXY 576

bool text_forceit = false;     /* Forcing of the subtitles */
sub_data *textsub_subdata;

static void constructblankpic(pict *p,int w,int h)
  /* allocates and fills in p with an image consisting entirely of transparent pixels */
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
    p->pal[0].a=0;
    memset(p->img,0,w*h);
}

typedef struct { /* a set of colours in a subpicture or button */
    int numpal; /* nr entries used in pal */
    int pal[4]; /* that's all the DVD-video spec allows */
} palgroup;

// ****************************************************
//
// Bitmap reading code

// this function scans through a picture and looks for the color that
// the user indicated should mean transparent
// if the color is found, it is replaced by 0,0,0,0 meaning "transparent"
static void scanpict(stinfo *s, pict *p)
  {
    int i;
    // we won't replace the background color if there is already evidence
    // of alpha channel information
    for (i = 0; i < p->numpal; i++)
        if (p->pal[i].a != 255)
            goto skip_transreplace;
    for (i = 0; i < p->numpal; i++)
        if (!memcmp(p->pal + i, &s->transparentc, sizeof(colorspec)))
          { /* found matching entry */
            memset(&p->pal[i], 0, sizeof(colorspec)); /* zap the RGB components */
            break;
          } /*if; for*/
 skip_transreplace:
    for (i = 0; i < p->numpal; i++)
        (void)findmasterpal(s, p->pal + i);
          /* just make sure all the colours are in the colour table */
  } /*scanpict*/

static void putpixel(pict *p, int x, const colorspec *c)
  /* stores another pixel into pict p at offset x with colour c. Adds a new
    entry into the colour table if not already present and there's room. */
  {
    int i;
    colorspec ct;
    if (!c->a && (c->r || c->g || c->b))
      {
      /* all transparent pixels look alike to me */
        ct.a = 0;
        ct.r = 0;
        ct.g = 0;
        ct.b = 0;
        c = &ct;
      } /*if*/
    for (i = 0; i < p->numpal; i++)
        if (!memcmp(&p->pal[i], c, sizeof(colorspec)))
          {
          /* matches existing palette entry */
            p->img[x] = i;
            return;
          } /*if; for*/
    if (p->numpal == 256)
      {
      /* too many colours */
        p->img[x] = 0;
        return;
      } /*if*/
  /* allocate new palette entry */
    p->img[x] = p->numpal;
/*  fprintf(stderr, "CREATING COLOR %d,%d,%d %d\n", c->r, c->g, c->b, c->a); */
    p->pal[p->numpal++] = *c;
  } /*putpixel*/

static void createimage(pict *s, int w, int h)
  /* allocates memory for pixels in s with dimensions w and h. */
  {
    s->numpal = 0;
  /* ensure allocated dimensions are even */
    s->width = w + (w & 1);
    s->height = h + (h & 1);
    s->img = malloc(s->width * s->height);
    if (w != s->width)
      {
      /* set padding pixels along side to transparent */
        int y;
        colorspec t;
        t.r = 0;
        t.g = 0;
        t.b = 0;
        t.a = 0;
        for (y = 0; y < h; y++)
            putpixel(s, w + y * s->width, &t);
      } /*if*/
    if (h != s->height)
      {
      /* set padding pixels along bottom to transparent */
        int x;
        colorspec t;
        t.r = 0;
        t.g = 0;
        t.b = 0;
        t.a = 0;
        for (x = 0; x < s->width; x++)
            putpixel(s, x + h * s->width, &t);
      } /*if*/
  } /*createimage*/

#if defined(HAVE_MAGICK) || defined(HAVE_GMAGICK)
// meaning of A in RGBA swapped in ImageMagick 6.0.0 and GraphicsMagick 1.3.8
#if defined(HAVE_MAGICK)
#define XMAGICK_NEW_RGBA_MINVER 0x600
#else // HAVE_GMAGICK
#define XMAGICK_NEW_RGBA_MINVER 0x060300
#define ExportImagePixels DispatchImage
#endif
static int read_magick(pict *s)
/* uses ImageMagick/GraphicsMagick to read image s from s->fname. */
{
    Image *im;
    ImageInfo *ii;
    ExceptionInfo ei;
    int x,y;
    unsigned long magickver;
    unsigned char amask;

    GetExceptionInfo(&ei);
    ii=CloneImageInfo(NULL);
    strcpy(ii->filename,s->fname);
    im=ReadImage(ii,&ei);

    if( !im ) {
        MagickError(ei.severity,"Unable to load file",ii->filename);
        return -1;
    }

    if( im->columns>MAXX || im->rows>MAXY ) {
        fprintf(stderr,"ERR:  Picture %s is too big: %lux%lu\n",s->fname,im->columns,im->rows);
        DestroyImage(im);
        return -1;
    }
    createimage(s,im->columns,im->rows);
    GetMagickVersion(&magickver);
    amask = magickver < XMAGICK_NEW_RGBA_MINVER ? 255 : 0;
    for( y=0; y<im->rows; y++ ) {
        char pdata[MAXX*4];

        if(!ExportImagePixels(im,0,y,im->columns,1,"RGBA",CharPixel,pdata,&ei)) {
            fprintf(stderr,"ERR:  Extracting row %d from %s (%s,%s)\n",y,s->fname,ei.reason,ei.description);
            CatchException(&ei);
            MagickError(ei.severity,ei.reason,ei.description);
            DestroyImage(im);
            return -1;
        }
        for( x=0; x<im->columns; x++ ) {
            colorspec p;
            p.r=pdata[x*4];
            p.g=pdata[x*4+1];
            p.b=pdata[x*4+2];
            p.a = pdata[x*4+3] ^ amask;
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
/* uses libpng to read image s from s->fname. */
{
    unsigned char pnghead[8];
    FILE *fp;
    png_struct *ps;
    png_info *pi;
    png_byte **rowp;
    png_uint_32 width,height;
    int bit_depth,color_type,channels,x,y;

    fp=fopen(s->fname,"rb");
    if( !fp ) {
    fprintf(stderr,"ERR:  Unable to open file %s\n",s->fname);
    return -1;
    }
    fread(pnghead,1,8,fp);
    if(png_sig_cmp(pnghead,0,8)) {
    fprintf(stderr,"ERR:  File %s isn't a png\n",s->fname);
    fclose(fp);
    return -1;
    }
    ps=png_create_read_struct(PNG_LIBPNG_VER_STRING,NULL,NULL,NULL);
    if( !ps ) {
        fprintf(stderr,"ERR:  Initializing png\n");
        fclose(fp);
        return -1;
    }
    pi=png_create_info_struct(ps);
    if( !pi ) {
        fprintf(stderr,"ERR:  Initializing png\n");
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
        fprintf(stderr,"ERR:  PNG %s is too big: %lux%lu\n",s->fname,width,height);
        png_destroy_read_struct(&ps,&pi,NULL);
        return -1;
    }
    createimage(s,width,height);
    for( y=0; y<height; y++ ) {
        unsigned char *d=rowp[y];
        for( x=0; x<width; x++ ) {
            colorspec p;
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
                p.a=*d++;
            else
                p.a=255;
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
  /* fills in s from textsub_image_buffer. */
  {
    int x, y;
    createimage(s, movie_width, movie_height);
    for (y = 0; y < movie_height; y++)
      {
          const unsigned char * d = textsub_image_buffer + y * movie_width * 4;
          for (x = 0; x < movie_width; x++)
            {
              colorspec p;
              p.r = *d++;
              p.g = *d++;
              p.b = *d++;
              p.a = *d++;
              putpixel(s, y * s->width + x, &p);
            } /*for*/
      } /*for*/
    return 0;
  } /*read_frame*/

static int read_pic(stinfo *s, pict *p)
  /* gets the image(s) specified by p into s. */
  {
    int r = 0;
    if (have_textsub)
      {
        vo_update_osd(s->sub_title); /* will allocate and render into textsub_image_buffer */
        s->forced = text_forceit;
        r = read_frame(p);
      }
    else /* read image file */
      {
        if (!p->fname)
            return 0;
#if defined(HAVE_MAGICK) || defined(HAVE_GMAGICK)
        r = read_magick(p);
#else
        r = read_png(p);
#endif
      } /*if*/
    if (!r)
        scanpict(s, p);
    return r;
  } /*read_pic*/

static bool checkcolor(palgroup *p,int c)
  /* tries to put colour c into palette p, returning true iff successful or already there. */
{
    int i;
    for( i=0; i<p->numpal; i++ )
        if( p->pal[i]==c )
            return true;
    if( p->numpal==4 )
        return false;
    p->pal[p->numpal++]=c;
    return true;
}

static int gettricolor(stinfo *s,int p,int useimg)
  /* returns an index used to represent the particular combination of colours used
    in s->img, s->hlt and s->sel at offset p. */
  {
    return
            (useimg ? s->img.img[p] << 16 : 0)
        |
            s->hlt.img[p] << 8
        |
            s->sel.img[p];
  } /*gettricolor*/

static bool pickbuttongroups(stinfo *s, int ng, int useimg)
  /* tries to assign the buttons in s to ng unique groups. useimg indicates
    whether to look at the pixels in s->img in addition to s->hlt and s->sel. */
  {
    palgroup *bpgs, *gs;
    int i, x, y, j, k, enb;

    bpgs = malloc(s->numbuttons * sizeof(palgroup)); /* colour tables for each button */
    memset(bpgs, 0, s->numbuttons * sizeof(palgroup));

    gs = malloc(ng * sizeof(palgroup)); /* colour tables for each button group */
    memset(gs, 0, ng * sizeof(palgroup));

    assert(!useimg || (s->xd <= s->img.width && s->yd <= s->img.height));
    assert(s->xd <= s->hlt.width && s->yd <= s->hlt.height);
    assert(s->xd <= s->sel.width && s->yd <= s->sel.height);

    // fprintf(stderr,"attempt %d groups, %d useimg\n",ng,useimg);
    // find unique colors per button
    for (i = 0; i < s->numbuttons; i++)
      {
      /* check coordinates of buttons make sense, and determine their colour tables */
        button *b = &s->buttons[i];
        palgroup *bp = &bpgs[i]; /* button's combined colour table for all images */

        if
          (
                b->r.x0 != b->r.x1
            &&
                b->r.y0 != b->r.y1
            &&
                (
                    b->r.x0 < 0
                ||
                    b->r.x0 > b->r.x1
                ||
                    b->r.x1 > s->xd
                ||
                    b->r.y0 < 0
                ||
                    b->r.y0 > b->r.y1
                ||
                    b->r.y1 > s->yd
                )
          )
          {
            if (debug > -1)
                fprintf
                  (
                    stderr,
                    "ERR:  Button coordinates out of range (%d,%d): (%d,%d)-(%d,%d)\n",
                    s->xd, s->yd,
                    b->r.x0, b->r.y0, b->r.x1, b->r.y1
                  );
            exit(1);
          } /*if*/
        for (y = b->r.y0; y < b->r.y1; y++)
            for (x = b->r.x0; x < b->r.x1; x++)
                if (!checkcolor(bp, gettricolor(s, y * s->xd + x, useimg)))
                    goto impossible;
        // fprintf(stderr, "pbg: button %d has %d colors\n", i, bp->numpal);
      } /*for i*/

    // assign to groups
    enb = 1;
    for (i = 0; i < s->numbuttons; i++)
        enb *= ng; /* how many combinations to try--warning! combinatorial explosion! */
    for (i = 0; i < enb; i++)
      {
      /* try another combination for assigning buttons to groups */
        for (j = 0; j < ng; j++)
            gs[j].numpal = 0; /* clear all button group palettes for new attempt */
        // fprintf(stderr, "trying ");
        k = i; /* combinator */
        for (j = 0; j < s->numbuttons; j++)
          {
          /* assemble this combination of merging button palettes into group palettes */
            int l;
            palgroup *pd = &gs[k % ng];
              /* palette for group to which to try to assign this button */
            palgroup *ps = &bpgs[j]; /* palette for button */

            s->buttons[j].grp = k % ng + 1; /* assign button group */
            // fprintf(stderr, "%s%d",j?", ":"", s->buttons[j].grp);
            k /= ng;
            for (l = 0; l < ps->numpal; l++)
              /* try to merge button palette into group palette */
                if (!checkcolor(pd, ps->pal[l]))
                  {
                    // fprintf(stderr, " -- failed mapping button %d\n", j);
                    goto trynext;
                  } /*if; for*/
          } /*for j*/
        if (useimg)
          {
          /* save the final button group palettes, ensuring the colours used in s->img
            for all the buttons fit in a single palette */
            palgroup p;

            p.numpal = s->img.numpal;
            for (j = 0; j < p.numpal; j++)
                p.pal[j] = j;
            for (j = 0; j < ng; j++)
              {
                for (k = 0; k < 4; k++)
                    s->groupmap[j][k] = -1; /* button group palette initially empty */
                for (k = 0; k < p.numpal; k++)
                    p.pal[k] &= 255; /* clear colour-already-matched flag */
                for (k = 0; k < gs[j].numpal; k++)
                  {
                    int l, c;

                    c = gs[j].pal[k] >> 16; /* s->img component of tricolor from button group palette */
                    for (l = 0; l < p.numpal; l++)
                        if (p.pal[l] == c)
                          {
                            goto ui_found;
                          } /*if; for */
                    if (p.numpal == 4)
                      {
                        // fprintf(stderr, " -- failed finding unique overall palette\n");
                        goto trynext;
                      } /*if*/
                    p.numpal++;
ui_found:
                    p.pal[l] = c | 256; /* mark palette entry as already matched */
                    s->groupmap[j][l] = gs[j].pal[k]; /* merge colour into button group palette */
                  } /*for*/
              } /*for j*/
          }
        else
          {
          /* save the final button group palettes */
            for (j = 0; j < ng; j++)
              {
                for (k = 0; k < gs[j].numpal; k++)
                    s->groupmap[j][k] = gs[j].pal[k];
                for (; k < 4; k++)
                    s->groupmap[j][k] = -1; /* unused palette entries */
              } /*for*/
          } /*if*/
        free(bpgs);
        free(gs);

        // If possible, make each palette entry 0 transparent in
        // all states, since some players may pad buttons with 0
        // and we also pad with 0 in some cases.
        for (j = 0; j < ng; j++)
          {
            int spare = -1;
            int tri; /* tricolor */

            // search for an unused color, or one that is already fully transparent
            for (k = 0; k < 4; k++)
              {
                tri = s->groupmap[j][k];
                if
                  (
                        tri == -1
                    ||
                            s->img.pal[(tri >> 16) & 0xFF].a == 0
                        &&
                            s->hlt.pal[(tri >> 8) & 0xFF].a == 0
                        &&
                            s->sel.pal[tri & 0xFF].a == 0
                  )
                  {
                    spare = k;
                    break;
                  } /*if*/
              } /*for*/
          /* at this point, if spare = 0 then nothing to do, entry if any at location 0
            is already transparent */
            if (spare > 0 && tri == -1)
              {
              /* got an unused slot, make up a transparent entry to exchange it with */
                tri = 0;
                for (k = 0; k < s->img.numpal; ++k)
                    if (s->img.pal[k].a == 0)
                      {
                        tri |= k << 16;
                        break;
                      } /*if; for*/
                for (k = 0; k < s->hlt.numpal; ++k)
                    if (s->hlt.pal[k].a == 0)
                      {
                        tri |= k << 8;
                        break;
                      } /*if; for*/
                for (k = 0; k < s->sel.numpal; ++k)
                    if (s->sel.pal[k].a == 0)
                      {
                        tri |= k;
                        break;
                      } /*if*/
              } /*if*/
            if (spare > 0)
              {
              /* move transparent colour to location 0 */
                s->groupmap[j][spare] = s->groupmap[j][0]; /* move nontransparent colour */
                s->groupmap[j][0] = tri; /* to make way for transparent one */
              } /*if*/
          } /*for j*/

        fprintf(stderr, "INFO: Pickbuttongroups, success with %d groups, useimg=%d\n", ng, useimg);
        s->numgroups = ng;
        return true;
trynext:
        continue; // 'deprecated use of label at end of compound statement'
      } /*for i*/

impossible:
    free(bpgs);
    free(gs);
    return false;
  } /*pickbuttongroups*/

static void fixnames(stinfo *s)
  /* assigns default names to buttons that don't already have them. */
  {
    int i;
    for (i = 0; i < s->numbuttons; i++)
        if (!s->buttons[i].name)
          {
            char n[10]; /* should be enough! */
            sprintf(n, "%d", i + 1);
            s->buttons[i].name = strdup(n);
          } /*if; for*/
  } /*fixnames*/

// a0 .. a1, b0 .. b1 are analogous to y coordinates, positive -> high, negative->low
// d is the distance from a to b (assuming b is to the right of a, i.e. positive x)
// returns angle (0 = straight right, 90 = straight up, -90 = straight down) from a to b
static void brphelp(int a0, int a1, int b0, int b1, int d, int *angle, int *dist)
  {
    int d1, d2, m;
    if (a1 > b0 && a0 < b1)
      {
        *angle = 0;
        *dist = d;
        return;
      } /*if*/
    d1 = -(b0 - a1 + 1);
    d2 = (a0 - b1 + 1);
    d++;
    if (abs(d2) <abs(d1))
      {
        d1 = d2;
      } /*if*/
    m = 1;
    if (d1 < 0)
      {
        d1 = -d1;
        m = -1;
      } /*if*/
    *angle = m * 180 / M_PI * atan(((double)d1) / ((double)d));
    *dist = sqrt(d1 * d1+d * d);
  } /*brphelp*/

static bool buttonrelpos(const rectangle *a, const rectangle *b, int *angle, int *dist)
  /* computes an angle and distance from the position of rectangle a to that of rectangle b,
    if I can figure one out. Returns true if the case is sufficiently simple for me to handle,
    false otherwise. */
  {
    // from b to a
    if (a->y1 <= b->y0)
      {
      /* a lies above b with no vertical overlap */
        brphelp(a->x0, a->x1, b->x0, b->x1, b->y0 - a->y1, angle, dist);
        // fprintf(stderr,"from %dx%d-%dx%d to %dx%d-%dx%d is angle %d, dist %d\n",b->x0,b->y0,b->x1,b->y1,a->x0,a->y0,a->x1,a->y1,*angle,*dist);
        return true;
      } /*if*/
    if (a->y0 >= b->y1)
      {
      /* a lies below b with no vertical overlap */
        brphelp(a->x0, a->x1, b->x0, b->x1, a->y0 - b->y1, angle, dist);
        *angle = 180 - *angle;
        // fprintf(stderr,"from %dx%d-%dx%d to %dx%d-%dx%d is angle %d, dist %d\n",b->x0,b->y0,b->x1,b->y1,a->x0,a->y0,a->x1,a->y1,*angle,*dist);
        return true;
      } /*if*/
    if (a->x1 <= b->x0)
      {
      /* a lies to the left of b with no horizontal overlap */
        brphelp(a->y0, a->y1, b->y0, b->y1, b->x0 - a->x1, angle, dist);
        *angle = 270 - *angle;
        // fprintf(stderr,"from %dx%d-%dx%d to %dx%d-%dx%d is angle %d, dist %d\n",b->x0,b->y0,b->x1,b->y1,a->x0,a->y0,a->x1,a->y1,*angle,*dist);
        return true;
      } /*if*/
    if (a->x0 >= b->x1)
      {
      /* a lies to the right of b with no horizontal overlap */
        brphelp(a->y0, a->y1, b->y0, b->y1, a->x0 - b->x1, angle, dist);
        *angle = 90 + *angle;
        // fprintf(stderr,"from %dx%d-%dx%d to %dx%d-%dx%d is angle %d, dist %d\n",b->x0,b->y0,b->x1,b->y1,a->x0,a->y0,a->x1,a->y1,*angle,*dist);
        return true;
      } /*if*/
  /* none of the above */
    // fprintf(stderr,"from %dx%d-%dx%d to %dx%d-%dx%d -- no easy comparison\n",b->x0,b->y0,b->x1,b->y1,a->x0,a->y0,a->x1,a->y1);
    return false;
  } /*buttonrelpos*/

static void findbestbindir(stinfo *s, const button *b, char **dest, int a)
  /* finds the best button to go to in the specified direction and returns its
    name in *dest, if that has not already been set. */
  {
    int i, la = 0, ld = 0;
    if (*dest) /* already got one */
        return;
    // fprintf(stderr,"locating nearest button from %s, angle %d\n",b->name,a);
    for (i = 0; i < s->numbuttons; i++)
        if (b != &s->buttons[i])
          {
            int na, nd;
            if (buttonrelpos(&s->buttons[i].r, &b->r, &na, &nd))
              {
                na = abs(na - a); /* error from desired direction */
                if (na >= 90) /* completely wrong direction */
                    continue;
                if (!*dest || na < la || (na == la && nd < ld))
                  {
                  /* first candidate, or better score than previous candidate */
                    // fprintf(stderr,"\tchoosing %s, na=%d, d=%d\n",s->buttons[i].name,na,nd);
                    *dest = s->buttons[i].name;
                    la = na;
                    ld = nd;
                  } /*if*/
              } /*if*/
          } /*if; for*/
    if (*dest)
        *dest = strdup(*dest); /* copy the name I found */
    else
        *dest = strdup(b->name); /* back to same button in this direction */
  } /*findbestbindir*/

static void detectdirections(stinfo *s)
  /* automatically detects the neighbours of each button in each direction, where
    these have not already been specified. */
  {
    int i;
    for (i = 0; i < s->numbuttons; i++)
      {
        findbestbindir(s, &s->buttons[i], &s->buttons[i].up, 0);
        findbestbindir(s, &s->buttons[i], &s->buttons[i].down, 180);
        findbestbindir(s, &s->buttons[i], &s->buttons[i].left, 270);
        findbestbindir(s, &s->buttons[i], &s->buttons[i].right, 90);
      } /*for*/
  } /*detectdirections*/

#define MAX(a,b) (((a)>(b))?(a):(b))
#define MIN(a,b) (((a)<(b))?(a):(b))

static bool scanvline(stinfo *s,unsigned char *v,rectangle *r,int x,int d)
{
    int i,j;

    for( j=1; j<=s->outlinewidth; j++ ) {
        x+=d;
        if( x<0 || x>=s->xd )
            return false;
        for( i=MAX(r->y0-j,0); i<MIN(r->y1+j,s->yd); i++ )
            if( v[i*s->xd+x] )
                return true;
    }
    return false;
}

static bool scanhline(stinfo *s,unsigned char *v,rectangle *r,int y,int d)
{
    int i,j;

    for( j=1; j<=s->outlinewidth; j++ ) {
        y+=d;
        if( y<0 || y>=s->yd )
            return false;
        for( i=MAX(r->x0-j,0); i<MIN(r->x1+j,s->xd); i++ )
            if( v[y*s->xd+i] )
                return true;
    }
    return false;
}

static void detectbuttons(stinfo *s)
  /* does automatic detection of button outlines. */
{
    unsigned char *visitmask=malloc(s->xd*s->yd);
    int i,x,y;
    rectangle *rs=0;
    int numr=0;

    if( !s->outlinewidth )
        s->outlinewidth=1;
    for( i=0; i<s->xd*s->yd; i++ )
        visitmask[i]=( s->hlt.pal[s->hlt.img[i]].a || s->sel.pal[s->sel.img[i]].a ) ? 1 : 0;
    for( y=0; y<s->yd; y++ )
        for( x=0; x<s->xd; x++ )
            if( visitmask[y*s->xd+x] ) {
                rectangle r;
                bool didwork;

                r.x0=x;
                r.y0=y;
                r.x1=x+1;
                r.y1=y+1;

                do {
                    didwork = false;
                    while( scanvline(s,visitmask,&r,r.x0,-1) ) {
                        r.x0--;
                        didwork = true;
                    }
                    while( scanvline(s,visitmask,&r,r.x1-1,1) ) {
                        r.x1++;
                        didwork = true;
                    }
                    while( scanhline(s,visitmask,&r,r.y0,-1) ) {
                        r.y0--;
                        didwork = true;
                    }
                    while( scanhline(s,visitmask,&r,r.y1-1,1) ) {
                        r.y1++;
                        didwork = true;
                    }
                } while(didwork);

                r.y0-=r.y0&1; // buttons need even 'y' coordinates
                r.y1+=r.y1&1;

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
            if( s->buttons[i].r.x0<0 )
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

static bool imgfix(stinfo *s)
  /* fills in the subpicture/button details. */
{
    int i, useimg, w, h, x, y, x0, y0;

    w = s->img.width;
    h = s->img.height;
    s->xd = w; // pickbuttongroups needs these values set
    s->yd = h;

    if (s->autooutline)
        detectbuttons(s);

    fixnames(s);
    detectdirections(s);

    s->fimg = malloc(w * h);
    memset(s->fimg, 255, w * h); /* mark all pixels as uninitialized */

    // first try not to have multiple palettes for
    useimg = 1;
    if (s->numbuttons)
      {
        i = 0;
        do
          {
            if (pickbuttongroups(s, 1, useimg))
                break;
            if (pickbuttongroups(s, 2, useimg))
                break;
            if (pickbuttongroups(s, 3, useimg))
                break;
            useimg--;
          }
        while (useimg >= 0);
        assert(useimg); // at this point I don't want to deal with blocking the primary subtitle image
        if (useimg < 0)
          {
            fprintf(stderr, "ERR:  Cannot pick button masks\n");
            return false;
          } /*if*/

        for (i = 0; i < s->numbuttons; i++)
          {
          /* fill in button areas of fimg */
            button *b = &s->buttons[i];

            for (y = b->r.y0; y < b->r.y1; y++)
                for (x = b->r.x0; x < b->r.x1; x++)
                  {
                    int dc = -1, p = y * w + x, j;
                    int c = gettricolor(s, p, useimg);
                    for (j = 0; j < 4; j++)
                        if (s->groupmap[b->grp - 1][j] == c)
                          {
                            dc = j;
                            break;
                          } /*if; for*/
                    if (dc == -1)
                      { /* shouldn't occur */
                        fprintf(stderr, "ERR:  Button %d cannot find color %06x in group %d\n",
                            i, c, b->grp - 1);
                        assert(dc != -1); /* instant assertion failure */
                      } /*if*/
                    if (s->fimg[p] != dc && s->fimg[p] != 255)
                      { /* pixel already occupied by another button */
                        fprintf(stderr, "ERR:  Overlapping buttons\n");
                        return false;
                      } /*if*/
                    s->fimg[p] = dc;
                  } /*for; for*/
          } /*for*/
      } /*if s->numbuttons*/
    for (i = 0; i < 4; i++)
      { /* initially mark all s->pal entries as "unused" (transparent) */
        s->pal[i].r = 255;
        s->pal[i].g = 255;
        s->pal[i].b = 255;
        s->pal[i].a = 0;
      } /*for*/
    for (i = 0; i < w * h; i++)
        if (s->fimg[i] != 255)
            s->pal[s->fimg[i]] = s->img.pal[s->img.img[i]];
    for (i = 0; i < w * h; i++)
      /* fill in rest of fimg with "normal" image (s->img) */
        if (s->fimg[i] == 255)
          { /* haven't already done this pixel */
            int j;
            const colorspec * const p = &s->img.pal[s->img.img[i]];
            for (j = 0; j < 4; j++)
              /* see if colour is already in s->pal */
                if (!memcmp(&s->pal[j], p, sizeof(colorspec)))
                    goto if_found;
            for (j = 0; j < 4; j++) /* insert new colour in place of unused/transparent entry */
                if (s->pal[j].a == 0 && s->pal[j].r == 255) /* not checking all the components? */
                  {
                    s->pal[j] = *p;
                    goto if_found;
                  } /*if*/
          /* no room in s->pal */
            fprintf(stderr, "ERR:  Too many colors in base picture\n");
            return false;
if_found:
            s->fimg[i] = j;
          } /*if; for*/

    // determine minimal visual area, and crop the subtitle accordingly
    x0 = w;
    y0 = -1;
    s->xd = 0;
    s->yd = 0;
    for (i = 0, y = 0; y < h; y++)
      {
        for (x = 0; x < w; i++, x++)
          {
            if
              (
                    s->img.pal[s->img.img[i]].a
                ||
                    s->hlt.pal[s->hlt.img[i]].a
                ||
                    s->sel.pal[s->sel.img[i]].a
              )
              {
                if (y0 == -1)
                    y0 = y;
                s->yd = y;
                if (x < x0)
                    x0 = x;
                if (x > s->xd)
                    s->xd = x;
              } /*if*/
          } /*for*/
      } /*for*/
    if (y0 == -1)
      { // empty image?
        s->xd = w;
        s->yd = h;
        return true;
      } /*if*/
    x0 &= -2;
    y0 &= -2;
    s->xd = (s->xd + 2 - x0) & (-2);
    s->yd = (s->yd + 2 - y0) & (-2);
    for (i = 0; i < s->yd; i++)
        memmove(s->fimg + i * s->xd, s->fimg + i * w + x0 + y0 * w, s->xd);
    for (i = 0; i < s->numbuttons; i++)
      {
        button *b = &s->buttons[i];
        b->r.x0 += s->x0;
        b->r.y0 += s->y0;
        b->r.x1 += s->x0;
        b->r.y1 += s->y0;
      } /*for*/
    s->x0 += x0;
    s->y0 += y0;
    return true;
  } /*imgfix*/

bool process_subtitle(stinfo *s)
  /* loads the specified image files and builds the subpicture in memory. */
{
    int w=0,h=0;
    int iline=0;

    if( !s ) return false;
    if( read_pic(s,&s->img) ) {
        if(debug > -1)
            fprintf(stderr, "WARN: Bad image,  skipping line %d\n", iline - 1);
        return false;
    }
    if( s->img.img && !w ) {
        w=s->img.width;
        h=s->img.height;
    }
    if( read_pic(s,&s->hlt) ) {
        if(debug > -1)
            fprintf(stderr, "WARN: Bad image,  skipping line %d\n", iline - 1);
        return false;
    }
    if( s->hlt.img && !w ) {
        w=s->hlt.width;
        h=s->hlt.height;
    }
    if( read_pic(s,&s->sel) ) {
        if(debug > -1)
            fprintf(stderr, "WARN: Bad image,  skipping line %d\n", iline - 1);
        return false;
    }
    if( s->sel.img && !w ) {
        w=s->sel.width;
        h=s->sel.height;
    }

    if( !w ) {
        fprintf(stderr,"WARN: No picture, skipping line %d\n",iline-1);
        return false;
    }
  /* check consistent dimensions, fill in missing images with blanks */
    if( !s->img.img ) {
        constructblankpic(&s->img,w,h);
        fprintf(stderr,"INFO: Constructing blank img\n");
    } else if( s->img.width!=w || s->img.height!=h ) {
        fprintf(stderr,"WARN: Inconsistent picture widths, skipping line %d\n",iline-1);
        return false;
    }
    if( !s->hlt.img ) {
        constructblankpic(&s->hlt,w,h);
        fprintf(stderr,"INFO: Constructing blank hlt\n");
    } else if( s->hlt.width!=w || s->hlt.height!=h ) {
        fprintf(stderr,"WARN: Inconsistent picture widths, skipping line %d\n",iline-1);
        return false;
    }
    if( !s->sel.img ) {
        constructblankpic(&s->sel,w,h);
        fprintf(stderr,"INFO: Constructing blank sel\n");
    } else if( s->sel.width!=w || s->sel.height!=h ) {
        fprintf(stderr,"WARN: Inconsistent picture widths, skipping line %d\n",iline-1);
        return false;
    }

    if( !imgfix(s) ) /* data in img to fimg */
    {
        if (debug > -1)
        {
            fprintf(stderr, "ERR:  Blank image, skipping line %d\n", iline - 1);
        }
        return false;
    }

    return true;
}

void image_init()
{
#if defined(HAVE_MAGICK) || defined(HAVE_GMAGICK)
    InitializeMagick(NULL);
#endif
}

void image_shutdown()
{
#if defined(HAVE_MAGICK) || defined(HAVE_GMAGICK)
    DestroyMagick();
#endif
}
