/*
 * Copyright (C) 2002, 2003 Jan Panteltje <panteltje@yahoo.com>,
 *
 * Modified by Zachary Brewster-Geisz, 2003, to work on big-endian
 * machines.
 *
 * Modified by Henry Mason, 2003, to use both PNG and BMP, and to use
 * the dvdauthor submux format.
 *
 * Modified and copy right Jan Panteltje 2002
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * With many changes by Scott Smith (trckjunky@users.sourceforge.net)
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

#include <fcntl.h>

#include <netinet/in.h>

#include <png.h>

#include "rgb.h"

#define FALSE 0
#define TRUE (!FALSE)

#define CBUFSIZE 65536
#define PSBUFSIZE 10

static const char RCSID[]="$Id: //depot/dvdauthor/src/spuunmux.c#21 $";

static unsigned int add_offset;

static int debug = 0;

static int full_size = FALSE;
static unsigned int ofs, ofs1, pts, spts, subi, subs, subno;
static unsigned char sub[65536];
static unsigned char next_bits;
static char *base_name;
static int have_bits;
static FILE *fdo;

typedef struct {
    unsigned char r, g, b, t;
} palt;
static palt bpal[16];

struct spu {
    unsigned char *img;
    unsigned int x0, y0, xd, yd, pts[2], subno, force_display, nummap;
    struct colormap *map;
    struct spu *next;
};

static struct spu *spus=0;

struct button {
    char *name;
    int autoaction;
    int x1,y1,x2,y2;
    char *up,*down,*left,*right;
    int grp;
};

struct dispdetails {
    int pts[2];
    int numpal;
    u_int32_t palette[16];
    int numcoli;
    u_int32_t coli[6];
    int numbuttons;
    struct button *buttons;
    struct dispdetails *next;
};

static struct dispdetails *dd=0;

struct colormap {
    u_int16_t color;
    u_int16_t contrast;
    int x1,y1,x2,y2;
};

static unsigned int read4(unsigned char *p)
{
    return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3];
}

static unsigned int read2(unsigned char *p)
{
    return (p[0]<<8)|p[1];
}

static char *readpstr(char *b,int *i)
{
    char *s=strdup(b+i[0]);
    i[0]+=strlen(s)+1;
    return s;
}

static unsigned char get_next_bits()
{
    if (!have_bits) {
	next_bits = sub[ofs++];
	have_bits = TRUE;
	return next_bits >> 4;
    }
    have_bits = FALSE;
    return next_bits & 15;
}

static unsigned int getpts(unsigned char *buf)
{
    if (!(buf[1] & 0xc0) ||
	(buf[2] < 4) || ((buf[3] & 0xe1) != 0x21) ||
	((buf[5] & 1) != 1) || ((buf[7] & 1) != 1))
	return -1;
    return (buf[7] >> 1) + ((unsigned int) buf[6] << 7) +
	(((unsigned int) buf[5] & 254) << 14) +
	((unsigned int) buf[4] << 22) +
	(((unsigned int) buf[3] & 14) << 29);
}

static void addspu(struct spu *s)
{
    struct spu **f=&spus;

    while( *f )
        f=&(f[0]->next);
    *f=s;
}

static void adddd(struct dispdetails *d)
{
    struct dispdetails **dp=&dd;

    while (*dp )
        dp=&(dp[0]->next);
    *dp=d;
}

static int dvddecode()
{
    unsigned int io;
    unsigned short int size, dsize, i, x, y, t;
    unsigned char c;
    struct spu *s;

    size = (((unsigned int) sub[0]) << 8) + sub[1];
    dsize = (((unsigned int) sub[2]) << 8) + sub[3];

    if (debug > 1)
	fprintf(stderr, "packet: 0x%x bytes, 0x%x bytes data\n", size,
		dsize);

    s=malloc(sizeof(struct spu));
    memset(s,0,sizeof(struct spu));

    s->subno=subno++;

    s->pts[0] = s->pts[1] = -1;
    s->nummap=1;
    s->map=malloc(sizeof(struct colormap));
    memset(s->map,0,sizeof(struct colormap));
    s->map[0].x2=0x7ffffff;
    s->map[0].y2=0x7ffffff;
    i = dsize + 4;

    t = sub[dsize] * 256 + sub[dsize + 1];

    if (debug > 2)
	fprintf(stderr, "time offset: %d sub[%d]=0x%02x sub[%d]=0x%02x\n",
		t, dsize, sub[dsize], dsize + 1, sub[dsize + 1]);

    while (i < size) {
	c = sub[i];
	if (debug > 4)
	    fprintf(stderr, "i: %d cmd: 0x%x\n", i, c);

	switch (c) {
	case 0x0:		//force start display
	    i++;
	    s->force_display = TRUE;
	    s->pts[0] = t * 1024 + spts;
	    if (debug > 4)
		fprintf(stderr, "cmd: force start display\n");
	    break;

	case 0x01:
	    i++;
	    s->pts[0] = t * 1024 + spts;
	    if (debug > 4)
		fprintf(stderr, "cmd: start display\n");
	    break;

	case 0x02:
	    if (debug > 4)
		fprintf(stderr, "cmd: end display\n");
	    s->pts[1] = t * 1024 + spts;
	    i++;
	    break;

	case 0x03:
	    if (debug > 4)
		fprintf(stderr, "cmd: palette %02x%02x\n", sub[i + 1],
			sub[i + 2]);

            s->map[0].color=read2(sub+i+1);
	    i += 3;
	    break;

	case 0x04:
	    if (debug > 4)
		fprintf(stderr, "cmd: t-palette %02x%02x\n", sub[i + 1],
			sub[i + 2]);

            s->map[0].contrast=read2(sub+i+1);
	    i += 3;
	    break;

	case 0x05:
	    s->x0 = ((((unsigned int) sub[i + 1]) << 4) + (sub[i + 2] >> 4));
	    s->xd = (((sub[i + 2] & 0x0f) << 8) + sub[i + 3]) - s->x0 + 1;

	    s->y0 = ((((unsigned int) sub[i + 4]) << 4) + (sub[i + 5] >> 4));
	    s->yd = (((sub[i + 5] & 0x0f) << 8) + sub[i + 6]) - s->y0 + 1;

	    if (debug > 4)
		fprintf(stderr, "cmd: img ofs %d,%d  size: %d,%d\n", s->x0,
			s->y0, s->xd, s->yd);
	    i += 7;
	    break;

	case 0x06:
	    ofs = (((unsigned int) sub[i + 1]) << 8) + sub[i + 2];
	    ofs1 = (((unsigned int) sub[i + 3]) << 8) + sub[i + 4];
	    if (debug > 4)
		fprintf(stderr, "cmd: image offsets 0x%x 0x%x\n", ofs,
			ofs1);
	    i += 5;
	    break;

	case 0xff:
	    if (debug > 4)
		fprintf(stderr, "cmd: end cmd\n");

	    if (i + 5 > size) {
		if (debug > 4)
		    fprintf(stderr,
			    "short end command i=%d size=%d (%d bytes)\n",
			    i, size, size - i);

		i = size;
		break;
	    }

	    t = sub[i + 1] * 256 + sub[i + 2];
	    if (debug > 4) {
		fprintf(stderr, "next packet time: %d  end: 0x%02x%02x\n",
			t, sub[i + 3], sub[i + 4]);
	    }

	    if ((sub[i + 3] != sub[dsize + 2])
		|| (sub[i + 4] != sub[dsize + 3])) {
		if (debug > 0) {
		    fprintf(stderr,
			    "invalid control header (%02x%02x != %02x%02x) dsize=%d!\n",
			    sub[i + 3], sub[i + 4], sub[dsize + 2],
			    sub[dsize + 3], dsize);
		}

		if (debug > 3)
		    fprintf(stderr, "i: %d\n", i);

		i = size;
		break;
	    }

	    i += 5;
	    break;

	default:
	    if (debug > 0)
		fprintf(stderr,
			"invalid sequence in control header (%02x)!\n", c);
	    return -1;
	}			/* end switch command */
    }				/* end while i < size */

    have_bits = FALSE;
    x = y = 0;
    io = 0;
    s->img = malloc( s->xd*s->yd);

    while ((ofs < dsize) && (y < s->yd)) {
	i = get_next_bits();

	if (i < 4) {
	    i = (i << 4) + get_next_bits();
	    if (i < 16) {
		i = (i << 4) + get_next_bits();
		if (i < 0x40) {
		    i = (i << 4) + get_next_bits();
		    if (i < 256) {
			have_bits = FALSE;

			y += 2;
			while (x++ != s->xd)
			    s->img[io++] = i;
			x = 0;
			if ((y >= s->yd) && !(y & 1)) {
			    y = 1;
			    io = s->xd;
			    ofs = ofs1;
			} else
			    io += s->xd;
			continue;
		    }
		}
	    }
	}

	c = i & 3;
	i = i >> 2;
	while (i--) {
	    s->img[io++] = c;
	    if (++x == s->xd) {
		y += 2;
		x = 0;
                if ((y >= s->yd) && !(y & 1)) {
                    y = 1;
                    io = s->xd;
                    ofs = ofs1;
                } else
                    io += s->xd;
		have_bits = FALSE;
	    }
	}
    }
    
    if (s->pts[0] == -1)
        return 0;
    
    s->pts[0] += add_offset;
    if( s->pts[1] != -1 )
        s->pts[1] += add_offset;
    
    addspu(s);

    if (debug > 2)
	fprintf(stderr, "ofs: 0x%x y: %d\n", ofs, y);

    return 0;
}				/* end fuction dvd_decode */



 /*
  * from Y -> R
  * from V -> G
  * from U -> B
  */

static void ycrcb_to_rgb(int *Y, int *Cr, int *Cb)
{
    int R, G, B;
    R = YCrCb2R(*Y,*Cr,*Cb);
    G = YCrCb2G(*Y,*Cr,*Cb);
    B = YCrCb2B(*Y,*Cr,*Cb);
    *Y = R;
    *Cr = G;
    *Cb = B;
}

static void absorb_palette(struct dispdetails *d)
{
    int i;
    for( i=0; i<d->numpal; i++ ) {
        int Y,Cr,Cb;

        Y=(d->palette[i]>>16)&255;
        Cr=(d->palette[i]>>8)&255;
        Cb=(d->palette[i])&255;
        bpal[i].r=YCrCb2R(Y,Cr,Cb);
        bpal[i].g=YCrCb2G(Y,Cr,Cb);
        bpal[i].b=YCrCb2B(Y,Cr,Cb);
    }
}

static void pluck_dd()
{
    struct dispdetails *d=dd;
    int i;

    dd=d->next;
    absorb_palette(d);
    for( i=0; i<d->numbuttons; i++ ) {
        free(d->buttons[i].name);
        free(d->buttons[i].up);
        free(d->buttons[i].down);
        free(d->buttons[i].left);
        free(d->buttons[i].right);
    }
    free(d->buttons);
    free(d);
}

static unsigned char cmap_find(int x,int y,struct colormap *map,int nummap,int ci)
{
    int i;
    unsigned char cix=0;

    for( i=0; i<nummap; i++ )
        if( x>=map[i].x1 &&
            y>=map[i].y1 &&
            x<=map[i].x2 &&
            y<=map[i].y2 )
            cix=(((map[i].contrast>>(ci*4))&15)<<4) |
                (((map[i].color>>(ci*4))&15));
    return cix;
}

static int write_png(char *file_name,struct spu *s,struct colormap *map,int nummap)
{
    unsigned int a, x, y, nonzero;
    unsigned char *out_buf, *temp;
    FILE *fp;
    png_structp png_ptr;
    png_infop info_ptr;

    temp = out_buf = malloc(s->xd * s->yd * 4);
    nonzero=0;
    for (y = 0; y < s->yd; y++) {
        for (x = 0; x < s->xd; x++) {
            unsigned char cix=cmap_find(x+s->x0,y+s->y0,map,nummap,s->img[y * s->xd + x]);
            *temp++ = bpal[cix&15].r;
            *temp++ = bpal[cix&15].g;
            *temp++ = bpal[cix&15].b;
            *temp++ = (cix>>4)*17;
            if( cix&0xf0 ) nonzero=1;
        }
    }
    if( !nonzero ) {
        free(out_buf);
        return 1;
    }

    fp = fopen(file_name, "wb");
    if (!fp) {
	fprintf(stderr, "error, unable to open/create file: %s\n",
		file_name);
	return -1;
    }

    png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);

    if (!png_ptr)
	return -1;

    info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
	png_destroy_write_struct(&png_ptr, (png_infopp) NULL);
	return -1;
    }

    if (setjmp(png_ptr->jmpbuf)) {
	png_destroy_write_struct(&png_ptr, &info_ptr);
	fclose(fp);
	return -1;
    }

    png_init_io(png_ptr, fp);

    /* turn on or off filtering, and/or choose specific filters */
    png_set_filter(png_ptr, 0, PNG_FILTER_NONE);

    /* set the zlib compression level */
    png_set_compression_level(png_ptr, Z_BEST_COMPRESSION);

    /* set other zlib parameters */
    png_set_compression_mem_level(png_ptr, 8);
    png_set_compression_strategy(png_ptr, Z_DEFAULT_STRATEGY);
    png_set_compression_window_bits(png_ptr, 15);
    png_set_compression_method(png_ptr, 8);

    if (full_size) {
	png_set_IHDR(png_ptr, info_ptr, 720, 576,
		     8, PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE,
		     PNG_COMPRESSION_TYPE_DEFAULT,
		     PNG_FILTER_TYPE_DEFAULT);
    } else {
	png_set_IHDR(png_ptr, info_ptr, s->xd, s->yd,
		     8, PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE,
		     PNG_COMPRESSION_TYPE_DEFAULT,
		     PNG_FILTER_TYPE_DEFAULT);
    }

    png_write_info(png_ptr, info_ptr);

    png_set_packing(png_ptr);

    if (out_buf != NULL) {
	png_byte *row_pointers[576];

	if (full_size) {
	    char *image;
	    temp = out_buf;
	    image = malloc(720 * 576 * 4);
	    memset(image, 0, 720 * 576 * 4);	// fill image full transparrent
	    // insert image on the correct position
	    for (y = s->y0; y < s->y0 + s->yd; y++) {
		char *to = &image[y * 720 * 4 + s->x0 * 4];
		for (x = 0; x < s->xd; x++) {
		    *to++ = *temp++;
		    *to++ = *temp++;
		    *to++ = *temp++;
		    *to++ = *temp++;
		}
	    }

            s->y0 = 0;
            s->x0 = 0;
	    s->yd = 576;
	    s->xd = 720;
	    free(out_buf);
	    out_buf = image;
	}


	for (a = 0; a < s->yd; a++) {
	    row_pointers[a] = out_buf + a * (s->xd * 4);
	}

	png_write_image(png_ptr, row_pointers);

	png_write_end(png_ptr, info_ptr);
	free(out_buf);
    }

    png_destroy_write_struct(&png_ptr, &info_ptr);
    fclose(fp);

    return 0;
}

static void write_pts(char *preamble,int pts)
{
    fprintf(fdo,
            " %s=\"%02d:%02d:%02d.%02d\"",preamble,
            (pts / (60 * 60 * 90000)) % 24,
            (pts / (60 * 90000)) % 60,
            (pts / 90000) % 60,
            (pts / 900) % 100);
}

static void write_menu_image(struct spu *s,struct dispdetails *d,char *type,int offset)
{
    unsigned char nbuf[256];
    int nummap=d->numbuttons+1, i;
    struct colormap *map=malloc(sizeof(struct colormap)*nummap);
    memset(map,0,sizeof(struct colormap)); // set the first one blank
    map[0].x2=0x7fffffff;
    map[0].y2=0x7fffffff;
    for( i=0; i<d->numbuttons; i++ ) {
        u_int32_t cc=d->coli[2*d->buttons[i].grp-2+offset];
        map[i+1].x1=d->buttons[i].x1;
        map[i+1].y1=d->buttons[i].y1;
        map[i+1].x2=d->buttons[i].x2;
        map[i+1].y2=d->buttons[i].y2;
        map[i+1].color=cc>>16;
        map[i+1].contrast=cc;
    }
    
    sprintf(nbuf, "%s%05d%c.png", base_name, s->subno, type[0]);
    if( !write_png(nbuf,s,map,nummap) )
        fprintf(fdo," %s=\"%s\"",type,nbuf);
    free(map);
}

static void write_spu(struct spu *s,struct dispdetails *d)
{
    unsigned char nbuf[256];
    int i;

    if( d )
        absorb_palette(d);
    fprintf(fdo,"\t\t<spu");

    sprintf(nbuf, "%s%05d.png", base_name, s->subno);
    if( !write_png(nbuf,s,s->map,s->nummap) )
        fprintf(fdo," image=\"%s\"",nbuf);

    if( d && d->numbuttons ) {
        write_menu_image(s,d,"highlight",0);
        write_menu_image(s,d,"select",1);
    }

    write_pts("start",s->pts[0]);
    if( s->pts[1] != -1 )
        write_pts("end",s->pts[1]);
    if( s->x0 || s->y0 )
        fprintf(fdo," xoffset=\"%d\" yoffset=\"%d\"",s->x0,s->y0);
    if (s->force_display)
        fprintf(fdo, " force=\"yes\"");
    if( d && d->numbuttons ) {
        fprintf(fdo,">\n");
        for( i=0; i<d->numbuttons; i++ ) {
            struct button *b=d->buttons+i;
            if( b->autoaction ) 
                fprintf(fdo,"\t\t\t<action label=\"%s\" />\n",b->name);
            else {
                fprintf(fdo,"\t\t\t<button label=\"%s\" x0=\"%d\" y0=\"%d\" x1=\"%d\" y1=\"%d\" up=\"%s\" down=\"%s\" left=\"%s\" right=\"%s\" />\n",
                        b->name,b->x1,b->y1,b->x2,b->y2,
                        b->up,b->down,b->left,b->right);
            }
        }
        fprintf(fdo,"\t\t</spu>\n");
    } else
        fprintf(fdo, " />\n");

}

static void flushspus(unsigned int lasttime)
{
    while(spus) {
        struct spu *s=spus;
        if( s->pts[0]>=lasttime )
            return;
        spus=spus->next;

        while( dd && dd->pts[1]<s->pts[0] && dd->pts[1]!=-1 )
            pluck_dd();

        if( dd && (dd->pts[0]<s->pts[1] || s->pts[1]==-1) && 
            (dd->pts[1]>s->pts[0] || dd->pts[1]==-1) )
            write_spu(s,dd);
        else
            write_spu(s,0);
        if(s->img) free(s->img);
        if(s->map) free(s->map);
        free(s);
    }
}

#define bps(n,R,G,B) do { bpal[n].r=R; bpal[n].g=G; bpal[n].b=B; } while (0)

static void usage(void)
{
    fprintf(stderr,
	    "\nUse: %s [options] [input file] [input file] ...\n\n",
	    "spuunmux");
    fprintf(stderr, "options:\n");
    fprintf(stderr,
	    "-o <name>   base name for script and images     [sub]\n");
    fprintf(stderr,
	    "-v <level>  verbosity level                     [0]\n");
    fprintf(stderr,
	    "-f          resize images to full size          [720x576]\n");
    fprintf(stderr,
	    "-s <stream> number of the substream to extract  [0]\n");
    fprintf(stderr,
	    "-p <file>   name of file with dvd palette       [none]\n");
    fprintf(stderr, "            if palette file ends with .rgb\n");
    fprintf(stderr, "                treated as a RGB\n");
    fprintf(stderr, "                else as a YCbCr color\n");
    fprintf(stderr, "-h          print this help\n");
    fprintf(stderr, "-V          print version number\n");
    fprintf(stderr, "\n");
}


int main(int argc, char **argv)
{
    int fd;
    int option, n;
    int rgb;
    char *temp;
    int firstvideo=-1;
    unsigned int c, next_word, stream_number, inc, Inc;
    unsigned short int package_length;
    unsigned char cbuf[CBUFSIZE];
    unsigned char psbuf[PSBUFSIZE];
    unsigned char nbuf[256], *palet_file, *iname[256];

    fputs(PACKAGE_HEADER("spuunmux"),stderr);

    base_name = "sub";
    stream_number = 0;
    palet_file = 0;
    Inc = inc = 0;

    while ((option = getopt(argc, argv, "o:v:fs:p:Vh")) != -1) {
	switch (option) {
	case 'o':
	    base_name = optarg;
	    break;
	case 'v':
	    debug = atoi(optarg);
	    break;
	case 'f':
	    full_size = TRUE;
	    break;
	case 's':
	    stream_number = atoi(optarg);
	    break;
	case 'p':
	    palet_file = optarg;
	    break;
	case 'V':
	    exit(-1);

	case 'h':
	default:
	    usage();
	    return -1;
	}
    }

    if (optind < argc) {
	int n, i;
	for (i = 0, n = optind; n < argc; n++, i++)
	    iname[i] = argv[n];
	Inc = i;
    } else {
	usage();
	return -1;
    }

    bps(0, 0, 0, 0);
    bps(1, 127, 0, 0);
    bps(2, 0, 127, 0);
    bps(3, 127, 127, 0);
    bps(4, 0, 0, 127);
    bps(5, 127, 0, 127);
    bps(6, 0, 127, 127);
    bps(7, 127, 127, 127);
    bps(8, 192, 192, 192);
    bps(9, 128, 0, 0);
    bps(10, 0, 128, 0);
    bps(11, 128, 128, 0);
    bps(12, 0, 0, 128);
    bps(13, 128, 0, 128);
    bps(14, 0, 128, 128);
    bps(15, 128, 128, 128);

    if( palet_file ) {
        rgb = FALSE;
        temp = strrchr(palet_file, '.');
        if (temp != NULL) {
            if (strcmp(temp, ".rgb") == 0)
                rgb = TRUE;
        }
        
        fdo = fopen(palet_file, "r");
        if (fdo != NULL) {
            for (n = 0; n < 16; n++) {
                int r, g, b;
                fscanf(fdo, "%02x%02x%02x", &r, &g, &b);
                if (!rgb)
                    ycrcb_to_rgb(&r, &g, &b);
                bpal[n].r = r;
                bpal[n].g = g;
                bpal[n].b = b;
                
                if (debug > 3)
                    fprintf(stderr, "pal: %d #%02x%02x%02x\n", n,
                            bpal[n].r, bpal[n].g, bpal[n].b);

            }
            fclose(fdo);
        } else {
            fprintf(stderr, "unable to open %s, using defaults\n", palet_file);
        }
    }

    if (strlen(base_name) > 246) {
	fprintf(stderr,
		"error: max length of base for filename creation is 246 characters\n");
	return -1;
    }

    sprintf(nbuf, "%s.xml", base_name);
    fdo = fopen(nbuf, "w+");
    fprintf(fdo, "<subpictures>\n\t<stream>\n");

    pts = 0;
    subno = 0;
    subi = 0;

    add_offset = 450; // for rounding purposes

    while (inc < Inc) {
	fd = open(iname[inc], O_RDONLY | O_BINARY);
	if (fd < 0) {
	    fprintf(stderr, "error opening file %s\n", iname[inc]);

	    exit(-1);
	}

	if (debug > 0)
	    fprintf(stderr, "file: %s\n", iname[inc]);

	inc++;

	while (read(fd, &c, 4) == 4) {
            c=ntohl(c);
	    if (c == 0x000001ba) {	// start PS (Program stream)
		static unsigned int old_system_time = -1;
		unsigned int new_system_time;
	      l_01ba:
		if (debug > 5)
		    fprintf(stderr, "pack_start_code\n");

		if (read(fd, psbuf, PSBUFSIZE) < 1)
		    break;

		new_system_time = (psbuf[4] >> 3) + (psbuf[3] * 32) +
		    ((psbuf[2] & 3) * 32 * 256) +
		    ((psbuf[2] & 0xf8) * 32 * 128) +
		    (psbuf[1] * 1024 * 1024) +
		    ((psbuf[0] & 3) * 1024 * 1024 * 256) +
		    ((psbuf[0] & 0x38) * 1024 * 1024 * 256 * 2);

		if (new_system_time < old_system_time) {
		    if (old_system_time != -1) {
			if (debug > 0)
			    printf
				("Time changed in stream header, use old time as offset for timecode in subtitle stream\n");
			add_offset += old_system_time;
		    }
		}
		old_system_time = new_system_time;

                flushspus(old_system_time);

		if (debug > 5) {
		    fprintf(stderr, "system time: %d\n", new_system_time);
		}
            } else if( c==0x1b9 ) {
                if (debug > 5)
                    fprintf(stderr, "end packet\n");
	    } else {
		read(fd, &package_length, 2);
                package_length=ntohs(package_length);
		if (package_length != 0) {

		    switch (c) {
		    case 0x01bb:
			if (debug > 5)
			    fprintf(stderr, "system header\n");
			break;
		    case 0x01bf:
			if (debug > 5)
			    fprintf(stderr, "private stream 2\n");
			break;
		    case 0x01bd:
			if (debug > 5)
			    fprintf(stderr, "private stream\n");
			read(fd, cbuf, package_length);

			next_word = getpts(cbuf);
			if (next_word != -1) {
			    pts = next_word;
			}

			next_word = cbuf[2] + 3;

			if (debug > 5)
			    for (c = 0; c < next_word; c++)
				fprintf(stderr, "0x%02x ", cbuf[c]);

			if (debug > 5)
			    fprintf(stderr, "tid: %d\n", pts);

			if ( /*(debug > 1) && */ (cbuf[next_word] == 0x70))
			    fprintf(stderr, "substr: %d\n",
				    cbuf[next_word + 1]);

			if (cbuf[next_word] == stream_number + 32) {
			    if ((debug < 6) && (debug > 1)) {
				fprintf(stderr,
					"id: 0x%x 0x%x %d  tid: %d\n",
					cbuf[next_word], package_length,
					next_word, pts);
			    }

			    if (!subi) {
				subs =
				    ((unsigned int) cbuf[next_word + 1] <<
				     8) + cbuf[next_word + 2];

				spts = pts;
			    }

			    memcpy(sub + subi, cbuf + next_word + 1,
				   package_length - next_word - 1);

			    if (debug > 1) {
				fprintf(stderr, "found %d bytes of data\n",
					package_length - next_word - 1);
			    }

			    subi += package_length - next_word - 1;

			    if (debug > 2) {
				fprintf(stderr,
					"subi: %d (0x%x)  subs: %d (0x%x) b-a-1: %d (0x%x)\n",
					subi, subi, subs, subs,
					package_length - next_word - 1,
					package_length - next_word - 1);
			    }

			    if (subs == subi) {
				subi = 0;

				next_word = dvddecode();

				if (next_word) {
				    fprintf(stderr,
					    "found unreadable subtitle at %.2fs, skipping\n",
					    (double) spts / 90000);
				    continue;
				}
			    }	/* end if subs == subi */
			}
                        package_length=0;
			break;
		    case 0x01e0:
                        if( firstvideo==-1 ) {
                            read(fd, cbuf, package_length);
                            firstvideo=getpts(cbuf);
                            add_offset-=firstvideo;
                            package_length=0;
                        }
			if (debug > 5)
			    fprintf(stderr, "video stream 0\n");
			break;
                    case 0x01e1:
		    case 0x01e2:
		    case 0x01e3:
		    case 0x01e4:
		    case 0x01e5:
		    case 0x01e6:
		    case 0x01e7:
		    case 0x01e8:
		    case 0x01e9:
		    case 0x01ea:
		    case 0x01eb:
		    case 0x01ec:
		    case 0x01ed:
		    case 0x01ee:
		    case 0x01ef:
			if (debug > 5)
			    fprintf(stderr, "video stream %d\n",c-0x1e0);
			break;
		    case 0x01be:
			if (debug > 5)
			    fprintf(stderr, "padding stream %d bytes\n",
				    package_length);
			read(fd, cbuf, package_length);
                        if( package_length > 30 ) {
                            int i;

                            package_length=0;
                            i=0;
                            if( strcmp(cbuf+i,"dvdauthor-data") )
                                break;
                            i=15;
                            if( cbuf[i]!=1 )
                                break;
                            switch(cbuf[i+1]) {
                            case 1: // subtitle/menu color and button information
                            {
                                // int st=cbuf[i+2]&31; // we ignore which subtitle stream for now
                                struct dispdetails *d;
                                i+=3;
                                d=malloc(sizeof(struct dispdetails));
                                memset(d,0,sizeof(struct dispdetails));
                                d->pts[0]=read4(cbuf+i);
                                d->pts[1]=read4(cbuf+i+4);
                                i+=8;
                                while(cbuf[i]!=0xff) {
                                    switch(cbuf[i]) {
                                    case 1:
                                    {
                                        int j;

                                        d->numpal=0;
                                        for( j=0; j<cbuf[i+1]; j++ ) {
                                            int c=(cbuf[i+2+3*j]<<16)|(cbuf[i+3+3*j]<<8)|(cbuf[i+4+3*j]);
                                            d->palette[j]=c;
                                            d->numpal++;
                                        }
                                        i+=2+3*d->numpal;
                                        break;
                                    }
                                    case 2:
                                    {
                                        int j;

                                        d->numcoli=cbuf[i+1];
                                        for( j=0; j<2*d->numcoli; j++ )
                                            d->coli[j]=read4(cbuf+i+2+j*4);
                                        i+=2+8*d->numcoli;
                                        break;
                                    }
                                    case 3:
                                    {
                                        int j;
                                        d->numbuttons=cbuf[i+1];
                                        d->buttons=malloc(d->numbuttons*sizeof(struct button));
                                        for( j=0; j<d->numbuttons; j++ ) {
                                            struct button *b=&d->buttons[j];
                                            b->name=readpstr(cbuf,&i);
                                            i+=2;
                                            b->autoaction=cbuf[i++];
                                            if(!b->autoaction) {
                                                b->grp=cbuf[i];
                                                b->x1=read2(cbuf+i+1);
                                                b->y1=read2(cbuf+i+3);
                                                b->x2=read2(cbuf+i+5);
                                                b->y2=read2(cbuf+i+7);
                                                i+=9;
                                                // up down left right
                                                b->up=readpstr(cbuf,&i);
                                                b->down=readpstr(cbuf,&i);
                                                b->left=readpstr(cbuf,&i);
                                                b->right=readpstr(cbuf,&i);
                                            }
                                        }
                                        break;
                                    }
                                        
                                    default:
                                        fprintf(stderr,"ERR:  unknown dvd info packet command: %d\n",cbuf[i]);
                                        exit(1);
                                    }
                                }
                                adddd(d);
                                break;
                            }
                            }
                        }
                        package_length=0;
			break;
		    case 0x01c0:
                    case 0x01c1:
		    case 0x01c2:
		    case 0x01c3:
		    case 0x01c4:
		    case 0x01c5:
		    case 0x01c6:
		    case 0x01c7:
		    case 0x01c8:
		    case 0x01c9:
		    case 0x01ca:
		    case 0x01cb:
		    case 0x01cc:
		    case 0x01cd:
		    case 0x01ce:
		    case 0x01cf:
		    case 0x01d0:
                    case 0x01d1:
		    case 0x01d2:
		    case 0x01d3:
		    case 0x01d4:
		    case 0x01d5:
		    case 0x01d6:
		    case 0x01d7:
		    case 0x01d8:
		    case 0x01d9:
		    case 0x01da:
		    case 0x01db:
		    case 0x01dc:
		    case 0x01dd:
		    case 0x01de:
		    case 0x01df:
			if (debug > 5)
			    fprintf(stderr, "audio stream %d\n",c-0x1c0);
			break;
		    default:
			if (debug > 0)
			    fprintf(stderr, "unknown header %x\n", c);
			next_word = (c<<16) | package_length;
			package_length = 2;
			while (next_word != 0x1ba) {
			    next_word = next_word << 8;
			    if (read(fd, &next_word, 1) < 1)
				break;
			    package_length++;
			}

			if (debug > 0)
			    fprintf(stderr,
				    "skipped %d bytes of garbage\n",
				    package_length);
			goto l_01ba;
		    }		/* end switch */
                    read(fd, cbuf, package_length);
		}

	    }			/* end if 0xbd010000 */

	}			/* end while read 4 */

	close(fd);
    }				/* end while inc < Inc */

    flushspus(0x7fffffff);

    fprintf(fdo, "\t</stream>\n</subpictures>\n");
    fclose(fdo);

    return 0;
}				/* end function main */
