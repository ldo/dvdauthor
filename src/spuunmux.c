/*
    spuunmux mainline
*/
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
 * Svcd decoding by Giacomo Comes <encode2mpeg@users.sourceforge.net>
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
#include <errno.h>

#include <netinet/in.h>

#include <png.h>
#include <zlib.h>

#include "rgb.h"
#include "common.h"
#include "conffile.h"

#define CBUFSIZE 65536 /* big enough for any MPEG packet */
#define PSBUFSIZE 10

static unsigned int add_offset;

static int debug = 0;

static int video_format = VF_NONE;
static bool full_size = false;
static unsigned int pts, spts, subi, subs, subno;
static int ofs, ofs1;
  /* offsets from beginning of SPU to bottom and top field data set by last SPU_SET_DSPXA command */
static unsigned char sub[65536];
static unsigned char next_bits;
static const char *base_name;
static unsigned int have_bits;
static FILE *fdo;
static unsigned char svcd_adjust;

static colorspec
    current_palette[16]; /* current PGC colour table, alpha unused */

struct spu /* data for one subpicture unit (SPU) */
  {
    unsigned char *img; /* image data */
    unsigned int x0, y0, xd, yd; /* display bounds */
    unsigned int pts[2]; /* start time, end time */
    unsigned int subno; /* index used for generating unique filenames */
    unsigned int force_display;
    unsigned int nummap; /* length of map array */
    struct colormap *map;
      /* array where entry 0 is for colours as specified in SPU, other entries
        are for button colours as specified in PGC, so if they overlap, the
        last entry takes precedence */
    struct spu *next; /* linked list */
  };

static struct spu
    *pending_spus = 0;

struct button /* information about a button */
  {
    char *name;
    bool autoaction;
    int x1, y1, x2, y2;
    char *up, *down, *left, *right; /* names of neighbouring buttons */
    int grp; /* which group button belongs to */
  };

struct dispdetails /* information about button grouping */
  {
    int pts[2]; /* start time, end time */
    int numpal; /* nr used entries in palette */
    uint32_t palette[16]; /* RGB colours */
    int numcoli; /* nr of SL_COLI entries present, not checked! */
    uint32_t coli[6]; /* up to 3 8-byte SL_COLI entries */
    int numbuttons; /* length of buttons array */
    struct button *buttons; /* array */
    struct dispdetails *next; /* linked list */
  };

static struct dispdetails
    *pending_buttons = 0;

struct colormap /* for determining which colours take precedence in overlapping areas */
  {
    uint16_t color; /* four 4-bit colour indexes */
    uint16_t contrast; /* four 4-bit contrast (transparency) values */
    int x1, y1, x2, y2; /* bounds of area over which entries are valid */
  };

static unsigned int read4(const unsigned char *p)
  /* decodes 4 bytes as a big-endian integer starting at p. */
  {
    return
            p[0] << 24
        |
            p[1] << 16
        |
            p[2] << 8
        |
            p[3];
  } /*read4*/

static unsigned int read2(const unsigned char *p)
  /* decodes 2 bytes as a big-endian integer starting at p. */
  {
    return
            p[0] << 8
        |
            p[1];
  } /*read2*/

static char *readpstr(const unsigned char *b, int *i)
  /* extracts a null-terminated string beginning at b[*i], advances *i past it and returns
    a copy of the string. */
  {
    char * const s = strdup((const char *)b + i[0]);
    i[0] += strlen(s) + 1;
    return s;
  } /*readpstr*/

static unsigned char get_next_bits()
  /* returns next nibble from sub at offset ofs. */
  {
    if (!have_bits)
      {
        next_bits = sub[ofs++];
        have_bits = true;
        return next_bits >> 4;
      } /*if*/
    have_bits = false;
    return next_bits & 15;
  } /*get_next_bits*/

static unsigned char get_next_svcdbits()
  /* returns next two bits from sub at offset ofs. */
  {
    switch (have_bits)
      {
    case 0:
        ++have_bits;
        return sub[++ofs] >> 6;
    break;
    case 1:
        ++have_bits;
        return (sub[ofs] & 0x30) >> 4;
    break;
    case 2:
        ++have_bits;
        return (sub[ofs] & 0x0c) >> 2;
    break;
    default:
        have_bits = 0;
        return sub[ofs] & 0x03;
    break;
      } /*switch*/
  } /*get_next_svcdbits*/

static unsigned int getpts(const unsigned char *buf)
  /* decodes a presentation time stamp (PTS) beginning at location buf. */
  {
    if
      (
            !(buf[1] & 0xc0)
        ||
            buf[2] < 4
        ||
            (buf[3] & 0xe1) != 0x21
        ||
            (buf[5] & 1) != 1
        ||
            (buf[7] & 1) != 1
      )
        return -1; /* doesn't look like a proper PTS */
    return
            (buf[7] >> 1)
        +
            ((unsigned int)buf[6] << 7)
        +
            (((unsigned int)buf[5] & 254) << 14)
        +
            ((unsigned int)buf[4] << 22)
        +
            (((unsigned int)buf[3] & 14) << 29);
  } /*getpts*/

static void addspu(struct spu *s)
  /* appends s onto pending_spus. */
  {
    struct spu **f = &pending_spus;
    while (*f)
        f = &f[0]->next;
    *f = s;
  } /*addspu*/

static void add_pending_buttons(struct dispdetails *d)
  /* appends d onto pending_buttons. */
  {
    struct dispdetails **dp = &pending_buttons;
    while (*dp)
        dp = &dp[0]->next;
    *dp = d;
  } /*add_pending_buttons*/

static int dvddecode()
  /* decodes DVD-Video subpicture data from sub and appends a new entry containing
    the results onto pending_spus. */
  {
    unsigned int io;
    uint16_t total_spu_size, dsize, thiscmdoffset, nextcmdoffset, i, x, y, t;
    unsigned char c;
    struct spu *newspu;
    total_spu_size = read2(sub); /* SPDSZ = size of SPU */
    dsize = read2(sub + 2); /* SP_DCSQTA = offset to SP_DCSQT */
    ofs = -1;
    if (debug > 1)
        fprintf(stderr, "packet: %d bytes, first block offset=%d\n", total_spu_size, dsize);
    newspu = malloc(sizeof(struct spu));
    memset(newspu, 0, sizeof(struct spu));
    newspu->subno = subno++;
    newspu->pts[0] = newspu->pts[1] = -1;
    newspu->nummap = 1;
    newspu->map = malloc(sizeof(struct colormap));
    memset(newspu->map, 0, sizeof(struct colormap));
    newspu->map[0].x2 = 0x7fffffff;
    newspu->map[0].y2 = 0x7fffffff;
    i = dsize + 4; /* start of commands */
    thiscmdoffset = dsize;
    nextcmdoffset = read2(sub + thiscmdoffset + 2);
    if (nextcmdoffset < dsize)
      {
        if (debug > 0)
          {
            fprintf
              (
                stderr,
                "invalid control header nextcommand=%d dsize=%d!\n",
                nextcmdoffset,
                dsize
              );
          } /*if*/
        nextcmdoffset = thiscmdoffset;
      } /*if*/
    t = read2(sub + dsize); /* SP_DCSQ_STM = delay in 90kHz units / 1024 before executing commands */
    if (debug > 2)
        fprintf
          (
            stderr,
            "\tBLK(%5d): time offset: %d; next: %d\n",
            dsize, t, read2(sub + dsize + 2)
          );
  /* decode the commands, including finding out where the image data is */
    while (i < total_spu_size)
      {
        c = sub[i]; /* get next command */
        switch (c)
          {
        case SPU_FSTA_DSP:
            if (debug > 4)
                fprintf(stderr, "\tcmd(%5d): force start display\n", i);
            newspu->force_display = true;
        // fall through
        case SPU_STA_DSP:
            if (debug > 4 && c == SPU_STA_DSP)
                fprintf(stderr, "\tcmd(%5d): start display\n", i);
            i++;
            newspu->pts[0] = t * 1024 + spts;
        break;

        case SPU_STP_DSP:
            if (debug > 4)
                fprintf(stderr, "\tcmd(%5d): end display\n", i);
            newspu->pts[1] = t * 1024 + spts;
            i++;
        break;

        case SPU_SET_COLOR:
            if (debug > 4)
                fprintf(stderr, "\tcmd(%5d): palette=%02x%02x\n", i, sub[i + 1], sub[i + 2]);
            newspu->map[0].color = read2(sub + i + 1);
            i += 3;
        break;

        case SPU_SET_CONTR:
            if (debug > 4)
                fprintf(stderr, "\tcmd(%5d): transparency=%02x%02x\n", i, sub[i + 1], sub[i + 2]);
            newspu->map[0].contrast = read2(sub + i + 1);
            i += 3;
        break;

        case SPU_SET_DAREA:
            newspu->x0 = ((((unsigned int)sub[i + 1]) << 4) + (sub[i + 2] >> 4));
            newspu->xd = (((sub[i + 2] & 0x0f) << 8) + sub[i + 3]) - newspu->x0 + 1;
            newspu->y0 = ((((unsigned int)sub[i + 4]) << 4) + (sub[i + 5] >> 4));
            newspu->yd = (((sub[i + 5] & 0x0f) << 8) + sub[i + 6]) - newspu->y0 + 1;
            if (debug > 4)
                fprintf
                  (
                    stderr,
                    "\tcmd(%5d): image corner=%d,%d, size=%d,%d\n",
                    i, newspu->x0, newspu->y0, newspu->xd, newspu->yd
                  );
            i += 7;
        break;

        case SPU_SET_DSPXA:
            if (ofs >= 0)
                fprintf(stderr, "WARN: image pointer already supplied for this subpicture\n");
                  /* not necessarily wrong, it's just I can't handle it */
            ofs = read2(sub + i + 1); /* offset to top field data */
            ofs1 = read2(sub + i + 3); /* offset to bottom field data */
            if (debug > 4)
                fprintf(stderr, "\tcmd(%5d): image offsets=%d,%d\n", i, ofs, ofs1);
            i += 5;
        break;

        case SPU_CMD_END:
            if (thiscmdoffset == nextcmdoffset) /* no next SP_DCSQ */
              {
                if (debug > 4)
                  {
                    fprintf(stderr, "cmd: last end command\n");
                    if (i + 1 < total_spu_size)
                      {
                        fprintf
                          (
                            stderr,
                            "data present after last command (%d bytes, size=%d)\n",
                            total_spu_size - (i + 1),
                            total_spu_size
                          );
                      } /*if*/
                  } /*if*/
                i = total_spu_size; /* indicate I'm finished */
                break;
              } /*if*/
            if (debug > 4)
                fprintf(stderr, "\tcmd(%5d): end cmd\n", i);
          /* another SP_DCSQT follows */
            thiscmdoffset = nextcmdoffset;
            nextcmdoffset = read2(sub + thiscmdoffset + 2);
            if (nextcmdoffset < dsize)
              {
                if (debug > 0)
                  {
                    fprintf
                      (
                        stderr,
                        "invalid control header nextcommand=%d dsize=%d!\n",
                        nextcmdoffset,
                        dsize
                      );
                  } /*if*/
                nextcmdoffset = thiscmdoffset;
                i = total_spu_size; /* force an end to all this */
                break;
              } /*if*/
            t = read2(sub + thiscmdoffset); /* SP_DCSQ_STM = delay in 90kHz units / 1024 before executing commands */
            if (debug > 4)
              {
                fprintf(stderr, "\tcmd(%5d): end cmd\n", i);
                fprintf(stderr, "\tBLK(%5d): time offset: %d; next: %d\n", i + 1, t, read2(sub + i + 3));
              } /*if*/
            if (debug > 4 && i + 1 < thiscmdoffset)
              {
                fprintf(stderr, "next packet jump: %d bytes\n", thiscmdoffset - (i + 1));
              } /*if*/
            i = thiscmdoffset + 4; /* start of next lot of commands */
        break;

      /* case SPU_CHG_COLCON: */ /* fixme: not handled */
        default:
            if (debug > 4)
                fprintf(stderr, "\tcmd(%5d): 0x%x\n", i, c);
            if (debug > 0)
                fprintf(stderr, "invalid sequence in control header (%02x)!\n", c);
            return -1;
          } /*switch*/
      } /*while*/

  /* now to decode the actual image data */
    have_bits = false;
    x = y = 0;
    io = 0;
    newspu->img = malloc(newspu->xd * newspu->yd);
    if (ofs < 0 && y < newspu->yd)
      {
        fprintf(stderr, "WARN: No image data supplied for this subtitle\n");
      }
    else
      {
      /* decode image data */
        while (ofs < dsize && y < newspu->yd)
          {
            i = get_next_bits();
            if (i < 4)
              {
                i = (i << 4) + get_next_bits();
                if (i < 16)
                  {
                    i = (i << 4) + get_next_bits();
                    if (i < 0x40)
                      {
                        i = (i << 4) + get_next_bits();
                        if (i < 4)
                          {
                            i = i + (newspu->xd - x) * 4; /* run ends at end of line */
                          } /*if*/
                      } /*if*/
                  } /*if*/
              } /*if*/
            c = i & 3; /* pixel value */
            i = i >> 2; /* count */
            while (i--)
              {
                newspu->img[io++] = c;
                if (++x == newspu->xd)
                  {
                  /* end of scanline */
                    y += 2;
                    x = 0;
                    if (y >= newspu->yd && !(y & 1))
                      {
                      /* end of top (odd) field, now do bottom (even) field */
                        y = 1;
                        io = newspu->xd;
                        ofs = ofs1;
                      }
                    else
                        io += newspu->xd; /* next scanline */
                    have_bits = false;
                  } /*if*/
              } /*while*/
          } /*while*/
      } /*if*/
    if (newspu->pts[0] == -1)
        return 0; /* fixme: free newspu or report error? */
    newspu->pts[0] += add_offset;
    if (newspu->pts[1] != -1)
        newspu->pts[1] += add_offset;
    addspu(newspu);
    return 0;
  } /*dvddecode*/

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

static void absorb_palette(const struct dispdetails *d)
  /* extracts the colour palette from d and puts it in RGB format into current_palette. */
  {
    int i;
    for (i = 0; i < d->numpal; i++)
      {
        int Y, Cr, Cb;
        Y = d->palette[i] >> 16 & 255;
        Cr = d->palette[i] >> 8 & 255;
        Cb = d->palette[i] & 255;
        current_palette[i].r = YCrCb2R(Y, Cr, Cb);
        current_palette[i].g = YCrCb2G(Y, Cr, Cb);
        current_palette[i].b = YCrCb2B(Y, Cr, Cb);
      } /*for*/
  } /*absorb_palette*/

static void pluck_pending_buttons()
  /* removes the head of pending_buttons, copies its palette into current_palette,
    and gets rid of it. */
  {
    struct dispdetails * const d = pending_buttons;
    int i;
    pending_buttons = d->next;
    absorb_palette(d);
    for (i = 0; i < d->numbuttons; i++)
      {
        free(d->buttons[i].name);
        free(d->buttons[i].up);
        free(d->buttons[i].down);
        free(d->buttons[i].left);
        free(d->buttons[i].right);
      } /*for*/
    free(d->buttons);
    free(d);
  } /*pluck_pending_buttons*/

static unsigned char cmap_find
  (
    int x,
    int y,
    const struct colormap *map, /* array */
    int nummap, /* length of map array */
    int ci /* pixel value */
  )
  /* returns the colour index (low nibble) and contrast (high nibble) of the
    specified pixel, as determined from the highest-numbered entry of map that
    covers its coordinates. Returns 0 if no entry is found. */
  {
    int i;
    unsigned char cix = 0;
    for (i = 0; i < nummap; i++)
      /* fixme: why not just start from the other end and terminate when a match is found? */
        if
          (
                x >= map[i].x1
            &&
                y >= map[i].y1
            &&
                x <= map[i].x2
            &&
                y <= map[i].y2
          )
            cix =
                    (map[i].contrast >> ci * 4 & 15) << 4
                |
                    map[i].color >> ci * 4 & 15;
    return cix;
  } /*cmap_find*/

static int write_png
  (
    const char *file_name,
    const struct spu *s,
    const struct colormap *map, /* array of entries for assigning colours to overlapping areas */
    int nummap /* length of map array */
  )
  /* outputs the contents of s as a PNG file, converting pixels to colours
    according to map. */
  {
    int status = -1; /* to begin with */
    unsigned char *out_buf = NULL;
    FILE *fp = NULL;
    png_structp png_ptr = NULL;
    png_infop info_ptr = NULL;
    const unsigned short subwidth = svcd_adjust ? 704 : 720;
    const unsigned short subheight = video_format == VF_NTSC ? 480 : 576;
    do /*once*/
      {
        out_buf = malloc(s->xd * s->yd * 4);
        if (out_buf == NULL)
          {
            fprintf(stderr, "ERR:  unable allocate %d-byte PNG buffer\n", s->xd * s->yd * 4);
            break;
          } /*if*/
          {
            unsigned char *temp = out_buf;
            bool nonzero = false;
            unsigned int x, y;
            for (y = 0; y < s->yd; y++)
              {
                for (x = 0; x < s->xd; x++)
                  {
                    const unsigned char cix =
                        cmap_find(x + s->x0, y + s->y0, map, nummap, s->img[y * s->xd + x]);
                    *temp++ = current_palette[cix & 15].r;
                    *temp++ = current_palette[cix & 15].g;
                    *temp++ = current_palette[cix & 15].b;
                    *temp++ = (cix >> 4) * 17;
                    if (cix & 0xf0)
                        nonzero = true;
                  } /*for*/
              } /*for*/
            if (!nonzero)
              {
              /* all transparent, don't bother writing any image */
                status = 1;
                break;
              } /*if*/
          }
        fp = fopen(file_name, "wb");
        if (fp == NULL)
          {
            fprintf(stderr, "ERR:  error %s trying to open/create file: %s\n", strerror(errno), file_name);
            break;
          } /*if*/
        png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
        if (png_ptr == NULL)
            break;
        info_ptr = png_create_info_struct(png_ptr);
        if (info_ptr == NULL)
            break;
        if (setjmp(png_jmpbuf(png_ptr)))
            break;
        png_init_io(png_ptr, fp);
        png_set_filter(png_ptr, 0, PNG_FILTER_NONE);
        png_set_compression_level(png_ptr, Z_BEST_COMPRESSION);
        png_set_compression_mem_level(png_ptr, 8);
        png_set_compression_strategy(png_ptr, Z_DEFAULT_STRATEGY);
        png_set_compression_window_bits(png_ptr, 15);
        png_set_compression_method(png_ptr, 8);
        if (full_size)
          {
            png_set_IHDR
              (
                /*png_ptr =*/ png_ptr,
                /*info_ptr =*/ info_ptr,
                /*width =*/ subwidth,
                /*height =*/ subheight,
                /*bit_depth =*/ 8,
                /*color_type =*/ PNG_COLOR_TYPE_RGB_ALPHA,
                /*interlace_method =*/ PNG_INTERLACE_NONE,
                /*compression_method =*/ PNG_COMPRESSION_TYPE_DEFAULT,
                /*filter_method =*/ PNG_FILTER_TYPE_DEFAULT
              );
          }
        else
          {
            png_set_IHDR
              (
                /*png_ptr =*/ png_ptr,
                /*info_ptr =*/ info_ptr,
                /*width =*/ s->xd,
                /*height =*/ s->yd,
                /*bit_depth =*/ 8,
                /*color_type =*/ PNG_COLOR_TYPE_RGB_ALPHA,
                /*interlace_method =*/ PNG_INTERLACE_NONE,
                /*compression_method =*/ PNG_COMPRESSION_TYPE_DEFAULT,
                /*filter_method =*/ PNG_FILTER_TYPE_DEFAULT
              );
          } /*if*/
        png_write_info(png_ptr, info_ptr);
        png_set_packing(png_ptr);
          {
            unsigned int xd = s->xd, yd = s->yd;
            png_byte *row_pointers[576]; /* big enough for both PAL and NTSC */
            unsigned int a, x, y;
            if (full_size)
              {
                unsigned char *image;
                const unsigned char *temp = out_buf;
                image = malloc(subwidth * subheight * 4);
                memset(image, 0, subwidth * subheight * 4);    // fill image full transparent
                // insert image on the correct position
                for (y = s->y0; y < s->y0 + s->yd; y++)
                  {
                    unsigned char *to = &image[y * subwidth * 4 + s->x0 * 4];
                    if (y >= subheight)
                      {
                        fprintf(stderr, "WARN: subtitle %s truncated\n", file_name);
                        break;
                      } /*if*/
                    for (x = 0; x < s->xd; x++)
                      {
                        *to++ = *temp++;
                        *to++ = *temp++;
                        *to++ = *temp++;
                        *to++ = *temp++;
                      } /*for*/
                  } /*for*/
                yd = subheight;
                xd = subwidth;
                free(out_buf);
                out_buf = image;
              } /*if*/
            for (a = 0; a < yd; a++)
              {
                row_pointers[a] = out_buf + a * (xd * 4);
              } /*for*/
            png_write_image(png_ptr, row_pointers);
          }
        png_write_end(png_ptr, info_ptr);
      /* all successfully done */
        status = 0;
      }
    while (false);
    if (png_ptr != NULL)
      {
        png_destroy_write_struct(&png_ptr, &info_ptr);
      } /*if*/
    if (fp != NULL)
      {
        fclose(fp);
      } /*if*/
    free(out_buf);
    return
        status;
  } /*write_png*/

static void write_pts(const char *preamble, int pts)
  /* outputs a formatted representation of a timestamp to fdo. */
  {
    fprintf
      (
        fdo,
        " %s=\"%02d:%02d:%02d.%02d\"",
        preamble,
        (pts / (60 * 60 * 90000)) % 24,
        (pts / (60 * 90000)) % 60,
        (pts / 90000) % 60,
        (pts / 900) % 100
      );
  } /*write_pts*/

/*
  copy the content of buf to expbuf converting '&' '<' '>' '"'
  expbuf must be big enough to contain the expanded buffer
*/
static void xml_buf
  (
    unsigned char *expbuf,
    const unsigned char *buf
  )
  {
    const unsigned char *p;
    do
      {
        switch (*buf)
          {
        case '&':
            p = (const unsigned char *)"&amp;";
        break;
        case '<':
            p = (const unsigned char *)"&lt;";
        break;
        case '>':
            p = (const unsigned char *)"&gt;";
        break;
        case '"':
            p = (const unsigned char *)"&quot;";
        break;
        default:
            p = NULL;
        break;
          } /*switch*/
        if (p)
          {
            while ((*expbuf++ = *p++))
               /* copy the representation */;
            --expbuf;
          }
        else
            *expbuf++ = *buf; /* copy as is */
      }
    while (*buf++);
  } /*xml_buf*/

static void write_menu_image
  (
    const struct spu *s,
    const struct dispdetails *d,
    const char *type, /* name of attribute to fill in with name of generated file */
    int offset /* 0 => highlighted, 1 => selected */
  )
  /* outputs the subpicture image with the buttons in the highlighted or selected state. */
  {
    unsigned char nbuf[256];
    int nummap = d->numbuttons + 1, i;
    struct colormap *map = malloc(sizeof(struct colormap) * nummap);
    memset(map, 0, sizeof(struct colormap)); // set the first one blank
    map[0].x2 = 0x7fffffff;
    map[0].y2 = 0x7fffffff;
    for (i = 0; i < d->numbuttons; i++)
      {
        const uint32_t cc = d->coli[2 * d->buttons[i].grp - 2 + offset];
        map[i + 1].x1 = d->buttons[i].x1;
        map[i + 1].y1 = d->buttons[i].y1;
        map[i + 1].x2 = d->buttons[i].x2;
        map[i + 1].y2 = d->buttons[i].y2;
        map[i + 1].color = cc >> 16;
        map[i + 1].contrast = cc;
      } /*for*/    
    sprintf((char *)nbuf, "%s%05d%c.png", base_name, s->subno, type[0]);
    if (!write_png((char *)nbuf, s, map, nummap))
      {
        unsigned char ebuf[sizeof nbuf * 6];
        xml_buf(ebuf, nbuf);
        fprintf(fdo," %s=\"%s\"", type, ebuf);
      } /*if*/
    free(map);
  } /*write_menu_image*/

static void write_spu
  (
    const struct spu * curspu,
    const struct dispdetails * buttons /* applicable button highlight info, if any */
  )
  /* writes out all information about a subpicture unit as an <spu> tag. */
  {
    unsigned char nbuf[256];
    int i;
    if (buttons)
        absorb_palette(buttons);
    fprintf(fdo, "\t\t<spu");
    sprintf((char *)nbuf, "%s%05d.png", base_name, curspu->subno);
    if (!write_png((char *)nbuf, curspu, curspu->map, curspu->nummap))
      {
        unsigned char ebuf[sizeof nbuf * 6];
        xml_buf(ebuf, nbuf);
        fprintf(fdo, " image=\"%s\"", ebuf);
      } /*if*/
    if (buttons && buttons->numbuttons)
      {
        write_menu_image(curspu, buttons, "highlight", 0);
        write_menu_image(curspu, buttons, "select", 1);
      } /*if*/
    write_pts("start", curspu->pts[0]);
    if (curspu->pts[1] != -1)
        write_pts("end", curspu->pts[1]);
    if (curspu->x0 || curspu->y0)
        fprintf(fdo, " xoffset=\"%d\" yoffset=\"%d\"", curspu->x0, curspu->y0);
    if (curspu->force_display)
        fprintf(fdo, " force=\"yes\"");
    if (buttons && buttons->numbuttons)
      {
        fprintf(fdo, ">\n");
        for (i = 0; i < buttons->numbuttons; i++)
          {
            const struct button * const b = buttons->buttons + i;
            fprintf
              (
                fdo,
                "\t\t\t<%s name=\"%s\" x0=\"%d\" y0=\"%d\" x1=\"%d\" y1=\"%d\""
                    " up=\"%s\" down=\"%s\" left=\"%s\" right=\"%s\" />\n",
                b->autoaction ? "action" : "button", b->name,
                b->x1, b->y1, b->x2, b->y2,
                b->up, b->down, b->left, b->right
              );
          } /*for*/
        fprintf(fdo, "\t\t</spu>\n");
      }
    else
        fprintf(fdo, " />\n");
  } /*write_spu*/

static void flushspus(unsigned int lasttime)
  /* pops and outputs elements from pending_spus and pending_buttons that start
    prior to lasttime. */
  {
    while (pending_spus)
      {
        const struct spu * const curspu = pending_spus;
        if (curspu->pts[0] >= lasttime)
            return; /* next entry not yet due */
        pending_spus = pending_spus->next;
        while
          (
                pending_buttons
            &&
                pending_buttons->pts[1] < curspu->pts[0]
            &&
                pending_buttons->pts[1] != -1
          )
          /* merge colours from expired entries into colour table, but otherwise ignore them */
            pluck_pending_buttons();
        if
          (
                pending_buttons
            &&
                (pending_buttons->pts[0] < curspu->pts[1] || curspu->pts[1] == -1)
            &&
                (pending_buttons->pts[1] > curspu->pts[0] || pending_buttons->pts[1] == -1)
          )
          /* head of pending_buttons overlaps duration of curspu */
            write_spu(curspu, pending_buttons);
        else
            write_spu(curspu, 0);
        free(curspu->img);
        free(curspu->map);
        free((void *)curspu);
      } /*while*/
  } /*flushspus*/

#define bps(n,R,G,B) do { current_palette[n].r = R; current_palette[n].g = G; current_palette[n].b = B; } while (false)

static int svcddecode()
  {
    unsigned int io;
    unsigned short int size, i, x, y;
    unsigned char c;
    struct spu *s;
    int n;
    size = read2(sub);
    if (debug > 1)
        fprintf(stderr, "packet: 0x%x bytes\n", size);
    s = malloc(sizeof(struct spu));
    memset(s, 0, sizeof(struct spu));
    s->subno = subno++;
    s->pts[0] = spts;
    s->pts[1] = -1;
    s->nummap = 1;
    s->map = malloc(sizeof(struct colormap));
    memset(s->map, 0, sizeof(struct colormap));
    s->map[0].x2 = 0x7ffffff; /* single colour map covers entire picture */
    s->map[0].y2 = 0x7ffffff;
    i = 2;
    if (sub[i] & 0x08) /* timestamp present */
      {
        s->pts[1] = spts + read4(sub + i + 2);
        i += 4;
      } /*if*/
    i += 2;
    s->x0 = read2(sub + i);
    s->y0 = read2(sub + i + 2);
    s->xd = read2(sub + i + 4);
    s->yd = read2(sub + i + 6);
    i += 8;
    if (debug > 4)
        fprintf(stderr, "img ofs: %d,%d  size: %d,%d\n", s->x0, s->y0, s->xd, s->yd);
    for (n = 0; n < 4; n++)
      {
      /* collect colour table */
        int r, g, b;
        r = sub[i + 0 + n * 4];
        g = sub[i + 1 + n * 4];
        b = sub[i + 2 + n * 4];
        ycrcb_to_rgb(&r, &g, &b);
        bps(n, r, g, b);
        if (debug > 4)
          {
            fprintf
              (
                stderr,
                "palette: %d => 0x%02x 0x%02x 0x%02x 0x%02x => (%d, %d, %d)\n",
                n,
                sub[i + 0 + n * 4],
                sub[i + 1 + n * 4],
                sub[i + 2 + n * 4],
                sub[i + 3 + n * 4],
                r,
                g,
                b
              );
          } /*if*/
      } /*for*/
    s->map[0].color = 0x3210;
    s->map[0].contrast =
            (sub[i + 3] >> 4)
        +
            (sub[i + 7] & 0xf0)
        +
            ((sub[i + 11] & 0xf0) << 4)
        +
            ((sub[i + 15] & 0xf0) << 8);
    if (debug > 4)
        fprintf(stderr, "tpalette: %04x\n", s->map[0].contrast);
    i += 16;
    if (sub[i++] >> 6)
      {
        if (debug > 4)
          {
            fprintf
              (
                stderr,
                "cmd: shift (unsupported), direction=%d time=%f\n",
                sub[i - 1] >> 4 & 0x3,
                read4(sub) / 90000.0
              );
          } /*if*/
        i += 4;
      } /*if*/
    ofs = i + 2 - 1; // get_next_svcdbits will increment ofs by 1
    ofs1 = ofs + read2(sub + i);
    i += 2;
    if (debug > 4)
        fprintf(stderr, "cmd: image offsets 0x%x 0x%x\n", ofs, ofs1);
    have_bits = 0;
    x = y = 0;
    io = 0;
    s->img = malloc(s->xd * s->yd);
    memset(s->img, 0, s->xd * s->yd);
  /* decode the pixels */
    while (ofs < size && y < s->yd)
      {
        if ((c = get_next_svcdbits()) != 0)
          {
            s->img[io++] = c;
            ++x;
          }
        else
          {
            c = get_next_svcdbits() + 1;
            x += c;
            io += c;
          } /*if*/
        if (x >= s->xd)
          {
            y += 2;
            x = 0;
            if (y >= s->yd && !(y & 1))
              {
                y = 1;
                ofs = ofs1;
              } /*if*/
            io = s->xd * y;
            have_bits = 0;
          } /*if*/
      } /*while*/
    s->pts[0] += add_offset;
    if (s->pts[1] != -1)
        s->pts[1] += add_offset;
    addspu(s);
    if (debug > 2)
        fprintf(stderr, "ofs: 0x%x y: %d\n", ofs, y);
    return 0;
   } /*svcddecode*/

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
        "-f          resize images to full size          [720x576 or 720x480]\n");
    fprintf(stderr,
        "-F <format> specify video format, NTSC or PAL\n");
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
  } /*usage*/

int main(int argc, char **argv)
  {
    int option, n;
    int firstvideo = -1;
    unsigned int pid, next_word, stream_number, fileindex, nrinfiles;
    unsigned char cbuf[CBUFSIZE];
    unsigned char psbuf[PSBUFSIZE];
    char *palet_file;
    char *iname[256]; /* names of input files -- fixme: no range checking */
    unsigned int last_system_time = -1;

    video_format = get_video_format();
    fputs(PACKAGE_HEADER("spuunmux"), stderr);
    if (video_format != VF_NONE)
      {
        fprintf
          (
            stderr,
            "INFO: default video format is %s\n",
            video_format == VF_PAL ? "PAL" : "NTSC"
          );
      }
    else
      {
#if defined(DEFAULT_VIDEO_FORMAT)
#    if DEFAULT_VIDEO_FORMAT == 1
        fprintf(stderr, "INFO: default video format is NTSC\n");
        video_format = VF_NTSC;
#    elif DEFAULT_VIDEO_FORMAT == 2
        fprintf(stderr, "INFO: default video format is PAL\n");
        video_format = VF_PAL;
#    endif
#else
        fprintf(stderr, "INFO: no default video format, must explicitly specify NTSC or PAL\n");
#endif
      } /*if*/
    base_name = "sub";
    stream_number = 0;
    palet_file = 0;
    nrinfiles = 0;
    while ((option = getopt(argc, argv, "o:v:fF:s:p:Vh")) != -1)
      {
        switch (option)
          {
        case 'o':
            base_name = optarg;
        break;
        case 'v':
            debug = strtounsigned(optarg, "verbosity");
        break;
        case 'f':
            full_size = true;
        break;
        case 'F':
            if (!strcasecmp(optarg, "ntsc"))
              {
                video_format = VF_NTSC;
              }
            else if (!strcasecmp(optarg, "pal"))
              {
                video_format = VF_PAL;
              }
            else
              {
                fprintf(stderr, "ERR:  Unrecognized video format \"%s\"\n", optarg);
                exit(-1);
              } /*if*/
        break;
        case 's':
            stream_number = strtounsigned(optarg, "stream number");
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
          } /*switch*/
      } /*while*/

    if (optind < argc)
      {
      /* remaining args are input filenames */
        int n, i;
        for (i = 0, n = optind; n < argc; n++, i++)
            iname[i] = argv[n];
        nrinfiles = i;
      }
    else
      {
        usage();
        return -1;
      } /*if*/
    if (full_size && video_format == VF_NONE)
      {
        fprintf(stderr, "ERR:  cannot determine meaning of full size without knowing if it's NTSC or PAL\n");
        exit(-1);
      } /*if*/

  /* initialize current_palette to default palette */
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

    if (palet_file)
      {
        bool rgb = false;
        char * const temp = strrchr(palet_file, '.');
        if (temp != NULL)
          {
            if (strcmp(temp, ".rgb") == 0)
                rgb = true;
          } /*if*/        
        fdo = fopen(palet_file, "r");
        if (fdo != NULL)
          {
            for (n = 0; n < 16; n++)
              {
                int r, g, b;
                fscanf(fdo, "%02x%02x%02x", &r, &g, &b);
                if (!rgb)
                    ycrcb_to_rgb(&r, &g, &b);
                current_palette[n].r = r;
                current_palette[n].g = g;
                current_palette[n].b = b;
                if (debug > 3)
                    fprintf
                      (
                        stderr,
                        "pal: %d #%02x%02x%02x\n",
                        n, current_palette[n].r, current_palette[n].g, current_palette[n].b
                      );
              } /*for*/
            fclose(fdo);
          }
        else
          {
            fprintf(stderr, "unable to open %s, using defaults\n", palet_file);
          } /*if*/
      } /*if*/
    if (strlen(base_name) > 246)
      {
        fprintf(stderr,
            "error: max length of base for filename creation is 246 characters\n");
        return -1;
      } /*if*/
      {
        char nbuf[256];
        sprintf(nbuf, "%s.xml", base_name);
        fdo = fopen(nbuf, "w+");
      }
    fprintf(fdo, "<subpictures>\n\t<stream>\n");
    pts = 0;
    subno = 0;
    subi = 0;
    add_offset = 450; // for rounding purposes
    fileindex = 0;
    while (fileindex < nrinfiles)
      {
        struct vfile fd = varied_open(iname[fileindex], O_RDONLY, "input file");
        if (debug > 0)
            fprintf(stderr, "file: %s\n", iname[fileindex]);
        fileindex++;
        while (fread(&pid, 1, 4, fd.h) == 4)
          {
            pid = ntohl(pid);
            if (pid == 0x00000100 + MPID_PACK)
              {  // start PS (Program stream)
                unsigned int new_system_time, stuffcount;
l_01ba:
                if (debug > 5)
                    fprintf(stderr, "pack_start_code\n");
                if (fread(psbuf, 1, PSBUFSIZE, fd.h) < 1)
                    break;
                if ((psbuf[0] & 0xc0) != 0x40)
                  {
                    if (debug > 1)
                        fprintf(stderr, "not a MPEG-2 file, skipping.\n");
                    break;
                  } /*if*/
                new_system_time =
                        (psbuf[4] >> 3)
                    +
                        psbuf[3] * 32
                    +
                        (psbuf[2] & 3) * 32 * 256
                    +
                        (psbuf[2] & 0xf8) * 32 * 128
                    +
                        psbuf[1] * 1024 * 1024
                    +
                        (psbuf[0] & 3) * 1024 * 1024 * 256
                    +
                        (psbuf[0] & 0x38) * 1024 * 1024 * 128;
                if (new_system_time < last_system_time)
                  {
                    if (last_system_time != -1)
                      {
                        if (debug > 0)
                            fprintf
                              (
                                stderr,
                                "Time changed in stream header, use old time as offset for"
                                    " timecode in subtitle stream\n"
                              );
                        add_offset += last_system_time;
                      } /*if*/
                  } /*if*/
                last_system_time = new_system_time;
                flushspus(last_system_time);
                if (debug > 5)
                  {
                    fprintf(stderr, "system time: %u\n", new_system_time);
                  } /*if*/
                stuffcount = psbuf[9] & 7;
                if (stuffcount != 0)
                  {
                    char stuff[7];
                    if (debug > 5)
                        fprintf(stderr, "found %d stuffing bytes\n", stuffcount);
                    if (fread(stuff, 1, stuffcount, fd.h) < stuffcount)
                        break;
                  } /*if*/
              }
            else if (pid == 0x100 + MPID_PROGRAM_END)
              {
                if (debug > 5)
                    fprintf(stderr, "end packet\n");
              }
            else /* packet with a length field */
              {
                unsigned short int package_length;
                fread(&package_length, 1, 2, fd.h);
                package_length = ntohs(package_length);
                if (package_length != 0)
                  {
                    switch (pid)
                      {
                    case 0x0100 + MPID_SYSTEM:
                        if (debug > 5)
                            fprintf(stderr, "system header\n");
                    break;
                    case 0x0100 + MPID_PRIVATE2: /* PCI & DSI packets, not my problem */
                        if (debug > 5)
                            fprintf(stderr, "private stream 2\n");
                    break;
                    case 0x0100 + MPID_PRIVATE1: /* subpicture or audio stream */
                        if (debug > 5)
                            fprintf(stderr, "private stream 1\n");
                        fread(cbuf, 1, package_length, fd.h);
                        next_word = getpts(cbuf);
                        if (next_word != -1)
                          {
                            pts = next_word;
                          } /*if*/
                        next_word = cbuf[2] /* additional data length */ + 3 /* length of fixed part of MPEG-2 extension */;
                        if (debug > 5)
                          {
                          /* dump PES header + extension */
                            int c;
                            for (c = 0; c < next_word; c++)
                                fprintf(stderr, "0x%02x ", cbuf[c]);
                          } /*if*/
                        if (debug > 5)
                            fprintf(stderr, "tid: %d\n", pts);
                        if
                          (
                                cbuf[next_word] == stream_number + 32 /* DVD-Video stream nr */
                            ||
                                cbuf[next_word] == 0x70 && cbuf[next_word + 1] == stream_number
                                  /* SVCD stream nr */
                          )
                          {
                          /* this is the subpicture stream the user wants dumped */
                            svcd_adjust = cbuf[next_word] == 0x70 ? 4 : 0;
                            if (/*debug < 6 &&*/ debug > 1)
                              {
                                fprintf(stderr,
                                    "id: 0x%x 0x%x %d  tid: %d\n",
                                    cbuf[next_word], package_length,
                                    next_word, pts);
                              } /*if*/
                            if (!subi)
                              {
                              /* starting a new SPU */
                                subs =
                                        ((unsigned int)cbuf[next_word + 1 + svcd_adjust] << 8)
                                    +
                                        cbuf[next_word + 2 + svcd_adjust];
                                  /* SPDSZ, size of total subpicture data */
                                spts = pts;
                              } /*if*/
                            memcpy
                              (
                                /*dest =*/ sub + subi,
                                /*src =*/ cbuf + next_word + 1 + svcd_adjust,
                                /*n =*/ package_length - next_word - 1 - svcd_adjust
                              );
                              /* collect the subpicture data */
                            if (debug > 1)
                              {
                                fprintf(stderr, "found %d bytes of data\n",
                                    package_length - next_word - 1 - svcd_adjust);
                              } /*if*/
                            subi += package_length - next_word - 1 - svcd_adjust;
                              /* how much I just collected */
                            if (debug > 2)
                              {
                                fprintf(stderr,
                                    "subi: %d (0x%x)  subs: %d (0x%x) b-a-1: %d (0x%x)\n",
                                    subi, subi, subs, subs,
                                    package_length - next_word - 1 - svcd_adjust,
                                    package_length - next_word - 1 - svcd_adjust);
                              } /*if*/
                            if (svcd_adjust)
                              {
                                if (cbuf[next_word + 2] & 0x80)
                                  {
                                    subi = 0;
                                    next_word = svcddecode();
                                    if (next_word)
                                      {
                                        fprintf
                                          (
                                            stderr,
                                            "found unreadable subtitle at %.2fs, skipping\n",
                                            (double) spts / 90000
                                          );
                                        continue;
                                      } /*if*/
                                  } /*if*/
                              }
                            else if (subs == subi)
                              {
                              /* got a complete SPU */
                                subi = 0;
                                if (dvddecode())
                                  {
                                    fprintf(stderr,
                                        "found unreadable subtitle at %.2fs, skipping\n",
                                        (double) spts / 90000);
                                    continue;
                                  } /*if*/
                              } /*if*/
                          } /*if dump the stream*/
                        package_length = 0;
                    break;
                    case 0x0100 + MPID_VIDEO_FIRST:
                        if (firstvideo == -1)
                          {
                            fread(cbuf, 1, package_length, fd.h);
                            firstvideo = getpts(cbuf);
                            add_offset -= firstvideo;
                            package_length = 0;
                          } /*if*/
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
                            fprintf(stderr, "video stream %d\n", pid - 0x100 - MPID_VIDEO_FIRST);
                    break;
                    case 0x0100 + MPID_PAD:
                        if (debug > 5)
                            fprintf(stderr, "padding stream %d bytes\n", package_length);
                        fread(cbuf, 1, package_length, fd.h);
                        if (package_length > 30)
                          {
                            int i;
                            package_length = 0;
                            i = 0;
                            if (strcmp((const char *)cbuf + i, "dvdauthor-data"))
                                break;
                          /* pad packet contains DVDAuthor private data */
                            i = 15;
                            if (cbuf[i] != 2)
                                break;
                            switch(cbuf[i + 1])
                              {
                            case 1: // subtitle/menu color and button information
                              {
                                // int st = cbuf[i + 2] & 31; // we ignore which subtitle stream for now
                                struct dispdetails *buttons;
                                i += 3;
                                buttons = malloc(sizeof(struct dispdetails));
                                memset(buttons, 0, sizeof(struct dispdetails));
                                buttons->pts[0] = read4(cbuf + i);
                                buttons->pts[1] = read4(cbuf + i + 4);
                                i += 8;
                                while(cbuf[i] != 0xff)
                                  {
                                    switch(cbuf[i])
                                      {
                                    case 1: /* colour table */
                                      {
                                        int j;
                                        buttons->numpal = 0;
                                        for (j = 0; j < cbuf[i + 1]; j++)
                                          {
                                            const int c = read4(cbuf + i + 1 + 3 * j) & 0xffffff;
                                            buttons->palette[j] = c;
                                            buttons->numpal++;
                                          } /*for*/
                                        i += 2 + 3 * buttons->numpal;
                                      }
                                    break;
                                    case 2: /* button groups */
                                      {
                                        int j;
                                        buttons->numcoli = cbuf[i + 1];
                                        for (j = 0; j < 2 * buttons->numcoli; j++)
                                            buttons->coli[j] = read4(cbuf + i + 2 + j * 4);
                                        i += 2 + 8 * buttons->numcoli;
                                      }
                                    break;
                                    case 3: /* button placement */
                                      {
                                        int j;
                                        buttons->numbuttons = cbuf[i + 1];
                                        buttons->buttons = malloc(buttons->numbuttons * sizeof(struct button));
                                        i += 2;
                                        for (j = 0; j < buttons->numbuttons; j++)
                                          {
                                            struct button *b = &buttons->buttons[j];
                                            b->name = readpstr(cbuf, &i);
                                            i += 2;
                                            b->autoaction = cbuf[i++] != 0;
                                            b->grp = cbuf[i];
                                            b->x1 = read2(cbuf + i + 1);
                                            b->y1 = read2(cbuf + i + 3);
                                            b->x2 = read2(cbuf + i + 5);
                                            b->y2 = read2(cbuf + i + 7);
                                            i += 9;
                                            // up down left right
                                            b->up = readpstr(cbuf, &i);
                                            b->down = readpstr(cbuf, &i);
                                            b->left = readpstr(cbuf, &i);
                                            b->right = readpstr(cbuf, &i);
                                          } /*for*/
                                      }
                                    break;
                                    default:
                                        fprintf(stderr,"ERR:  unknown dvd info packet command: %d, offset %d\n",cbuf[i], i);
                                        exit(1);
                                      } /*switch*/
                                  } /*while*/
                                add_pending_buttons(buttons);
                              } /*case 1*/
                            break;
                              } /*switch*/
                          } /*if*/
                        package_length = 0;
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
                            fprintf(stderr, "audio stream %d\n", pid - 0x100 - MPID_AUDIO_FIRST);
                    break;
                    default:
                        if (debug > 0)
                            fprintf(stderr, "unknown header %x\n", pid);
                        next_word = pid << 16 | package_length;
                        package_length = 2;
                        while (next_word != 0x100 + MPID_PACK)
                          {
                            next_word = next_word << 8;
                            if (fread(&next_word, 1, 1, fd.h) < 1)
                                break;
                            package_length++;
                          } /*while*/
                        if (debug > 0)
                            fprintf(stderr, "skipped %d bytes of garbage\n", package_length);
                        goto l_01ba;
                      } /*switch*/
                    fread(cbuf, 1, package_length, fd.h);
                  } /*if*/
              } /*if*/
          } /*while read next packet header*/
        varied_close(fd);
      } /*while fileindex < nrinfiles*/
    flushspus(0x7fffffff); /* ensure all remaining spus elements are output */
    fprintf(fdo, "\t</stream>\n</subpictures>\n");
    fclose(fdo);
    return 0;
  } /*main*/
