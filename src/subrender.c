/* Copyright (C) 2000 - 2003 various authors of the MPLAYER project
 * This module uses various parts of the MPLAYER project (http://www.mplayerhq.hu)
 * With many changes by Sjef van Gool (svangool@hotmail.com) November 2003
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

/* Generic alpha renderers for all YUV modes and RGB depths.
 * Optimized by Nick and Michael
 * Code from Michael Niedermayer (michaelni@gmx.at) is under GPL
 */

#include "config.h"

#include "compat.h"

#include "subconfig.h"

#include "subglobals.h"
#include "subrender.h"
#include "subfont.h"




#define NEW_SPLITTING


static int sub_pos=100;
/* static int sub_width_p=100; */
static int sub_visibility=1;
static int vo_osd_changed_status = 0;
static mp_osd_obj_t* vo_osd_list=NULL;

int force_load_font;

static inline void vo_draw_alpha_rgb24
  (
    int w,
    int h,
    const unsigned char * src,
    const unsigned char * srca,
    int srcstride,
    unsigned char * dstbase,
    int dststride
  )
  /* composites pixels from src onto dstbase according to transparency
    taken from srca. */
  {
    int y, i;
    for (y = 0; y < h; y++)
      {
        register unsigned char * dst = dstbase;
        register int x;
        for (x = 0; x < w; x++)
          {
            if (srca[x]) /* not fully transparent */
              {
                dst[0] = (dst[0] * srca[x] >> 8) + src[x];
                dst[1] = (dst[1] * srca[x] >> 8) + src[x];
                dst[2] = (dst[2] * srca[x] >> 8) + src[x];
              /* dst[0] = (src[x] >> 6) << 6;
                dst[1] = (src[x] >> 6) << 6;
                dst[2] = (src[x] >> 6) << 6; */
                for (i = 0; i < 3; i++)
                  {
                    if (dst[i])
                      {
                        if (dst[i] >= 170)
                            dst[i] = 255;
                        else
                          {
                            if (dst[i] >= 127)
                                dst[i] = 127;
                            else
                                dst[i] = 1;
                          } /*if*/
                      } /*if*/
                  } /*for*/
                /* fprintf(stderr,"%d.",src[x]); */
              } /*if*/
            dst += 3; // 24bpp
          } /*for*/
        src += srcstride;
        srca += srcstride;
        dstbase += dststride;
      } /*for*/
  } /*vo_draw_alpha_rgb24*/

// renders char to a big per-object buffer where alpha and bitmap are separated
static void draw_alpha_buf
  (
    mp_osd_obj_t * obj,
    int x0,
    int y0,
    int w,
    int h,
    const unsigned char * src,
    const unsigned char * srca,
    int stride
  )
  {
    int dststride = obj->stride;
    int dstskip = obj->stride-w;
    int srcskip = stride-w;
    int i, j;
    unsigned char * b = obj->bitmap_buffer + (y0 - obj->bbox.y1) * dststride + (x0 - obj->bbox.x1);
    unsigned char * a = obj->alpha_buffer + (y0 - obj->bbox.y1) * dststride + (x0 - obj->bbox.x1);
    const unsigned char * bs = src;
    const unsigned char * as = srca;
    int k = 0;
  /* fprintf(stderr, "***w:%d x0:%d bbx1:%d bbx2:%d dstsstride:%d y0:%d h:%d bby1:%d bby2:%d ofs:%d ***\n",w,x0,obj->bbox.x1,obj->bbox.x2,dststride,y0,h,obj->bbox.y1,obj->bbox.y2,(y0-obj->bbox.y1)*dststride + (x0-obj->bbox.x1));*/
    if (x0 < obj->bbox.x1 || x0 + w > obj->bbox.x2 || y0 < obj->bbox.y1 || y0 + h > obj->bbox.y2)
      {
        fprintf
          (
            stderr,
            "WARN: Text out of range: bbox [%d %d %d %d], txt [%d %d %d %d]\n",
            obj->bbox.x1, obj->bbox.x2, obj->bbox.y1, obj->bbox.y2,
            x0, x0 + w, y0, y0 + h
          );
        return;
      } /*if*/
    for (i = 0; i < h; i++)
      {
        for (j = 0; j < w; j++, b++, a++, bs++, as++)
          {
            if (*b < *bs) /* composite according to max operator */
              *b = *bs;
            if (*as) /* not fully transparent */
              {
                if (*a == 0 || *a > *as)
                  *a = *as;
              } /*if*/
          } /*for*/
        k += dstskip;
        b += dstskip;
        a += dstskip;
        bs += srcskip;
        as += srcskip;
      } /*for*/
  } /*draw_alpha_buf*/

// allocates/enlarges the alpha/bitmap buffer
static void alloc_buf(mp_osd_obj_t * obj)
  {
    int len;
  /* fprintf(stderr,"x1:%d x2:%d y1:%d y2:%d\n",obj->bbox.x1,obj->bbox.x2,obj->bbox.y1,obj->bbox.y2); */
    if (obj->bbox.x2 < obj->bbox.x1)
        obj->bbox.x2 = obj->bbox.x1;
    if (obj->bbox.y2 < obj->bbox.y1)
        obj->bbox.y2 = obj->bbox.y1;
    obj->stride = obj->bbox.x2 - obj->bbox.x1 + 7 & ~7; /* round up to multiple of 8 bytes */
    len = obj->stride * (obj->bbox.y2 - obj->bbox.y1);
    if (obj->allocated < len)
      {
      /* allocate new, bigger buffers */
        obj->allocated = len;
        free(obj->bitmap_buffer);
        free(obj->alpha_buffer);
        obj->bitmap_buffer = (unsigned char *)malloc(len);
        obj->alpha_buffer = (unsigned char *)malloc(len);
      } /*if*/
    memset(obj->bitmap_buffer, sub_bg_color, len);
    memset(obj->alpha_buffer, sub_bg_alpha, len);
  } /*alloc_buf*/

// vo_draw_text_sub(int dxs,int dys,void (*draw_alpha)(int x0,int y0, int w,int h, unsigned char* src, unsigned char *srca, int stride))

inline static void vo_update_text_sub
  (
    mp_osd_obj_t * obj,
    int dxs,
    int dys
  )
  {
    // Structures needed for the new splitting algorithm.
    // osd_text_word contains the single subtitle word.
    // osd_text_line is used to mark the lines of subtitles
    struct osd_text_word
      {
        int osd_kerning; //kerning with the previous word
        int osd_length;  //horizontal length inside the bbox
        int text_length; //number of characters
        int *text;       //characters
        struct osd_text_word *prev, *next; /* doubly-linked list */
      };
    struct osd_text_line
      {
        int value;
        struct osd_text_word *words; /* head of word list */
        struct osd_text_line *prev, *next; /* doubly-linked list */
      };
    int linedone, linesleft, warn_overlong_word;
    int textlen, sub_totallen, xsize;
  /* const int xlimit = dxs * sub_width_p / 100; */
    const int xlimit = dxs - sub_right_margin - sub_left_margin;
    int xmin = xlimit, xmax = 0;
    int max_height;
    int xtblc, utblc;

    obj->flags |= OSDFLAG_CHANGED | OSDFLAG_VISIBLE;
    if (!vo_sub || !vo_font || !sub_visibility)
      {
        obj->flags &= ~OSDFLAG_VISIBLE;
        return;
      } /*if*/
    obj->bbox.y2 = obj->y = dys-sub_bottom_margin;
    obj->params.subtitle.lines = 0;

    // too long lines divide into a smaller ones
    linedone = sub_totallen = 0;
    max_height = vo_font->height;
    linesleft = vo_sub->lines;
      {
        struct osd_text_line
          // these are used to store the whole sub text osd
            *otp_sub = NULL, /* head of list */
            *otp_sub_last = NULL; /* last element of list */
        int *wordbuf = NULL;
        while (linesleft)
          { /* split next subtitle line into words */
            struct osd_text_word
                *osl, /* head of list */
                *osl_tail; /* last element of list */
            int j, prevch, char_position;
            const unsigned char *text;
            xsize = -vo_font->charspace;
            linesleft--;
            text = (const unsigned char *)vo_sub->text[linedone++];
            textlen = strlen((const char *)text) - 1;
            char_position = 0;
            wordbuf = (int *)realloc(wordbuf, (textlen + 1) * sizeof(int));
            prevch = -1;
            osl = NULL;
            osl_tail = NULL;
            warn_overlong_word = 1;
            // reading the subtitle words from vo_sub->text[]
            for (j = 0; j <= textlen; j++)
              {
                int curch = text[j];
                if (curch >= 0x80 && sub_utf8)
                  {
                  /* fixme: no checking for j going out of range */
                    if ((curch & 0xe0) == 0xc0)    /* 2 bytes U+00080..U+0007FF*/
                        curch = (curch & 0x1f) << 6 | (text[++j] & 0x3f);
                    else if ((curch & 0xf0) == 0xe0) /* 3 bytes U+00800..U+00FFFF*/
                      {
                        curch = (((curch & 0x0f) << 6) | (text[++j] & 0x3f)) << 6;
                        curch |= text[++j] & 0x3f;
                      } /*if*/
                  } /*if*/
                if (sub_totallen == MAX_UCS)
                  {
                    textlen = j; // end here
                    fprintf(stderr, "WARN: MAX_UCS exceeded!\n");
                  } /*if*/
                if (!curch)
                    curch++; // avoid UCS 0
                render_one_glyph(vo_font, curch);
                if (curch == ' ')
                  {
                  /* word break */
                    struct osd_text_word * const newelt =
                        (struct osd_text_word *)calloc(1, sizeof(struct osd_text_word));
                    int counter;
                    if (osl == NULL)
                      {
                      /* first element on list */
                        osl = newelt;
                      }
                    else
                      {
                      /* link to previous elements */
                        newelt->prev = osl_tail;
                        osl_tail->next = newelt;
                        newelt->osd_kerning = vo_font->charspace + vo_font->width[' '];
                      } /*if*/
                    osl_tail = newelt;
                    newelt->osd_length = xsize;
                    newelt->text_length = char_position;
                    newelt->text = (int *)malloc(char_position * sizeof(int));
                    for (counter = 0; counter < char_position; ++counter)
                        newelt->text[counter] = wordbuf[counter];
                    char_position = 0;
                    xsize = 0;
                    prevch = curch;
                  }
                else
                  {
                  /* continue accumulating word */
                    const int delta_xsize =
                            vo_font->width[curch]
                        +
                            vo_font->charspace
                        +
                            kerning(vo_font, prevch, curch);
                    if (xsize + delta_xsize <= xlimit)
                      {
                      /* word fits in available space */
                        if (!warn_overlong_word)
                            warn_overlong_word = 1;
                        prevch = curch;
                        wordbuf[char_position++] = curch;
                        xsize += delta_xsize;
                        if (!suboverlap_enabled)
                          {
                            const int font = vo_font->font[curch];
                            if (font >= 0 && vo_font->pic_a[font]->h > max_height)
                              {
                                max_height = vo_font->pic_a[font]->h;
                              } /*if*/
                          } /*if*/
                      }
                    else
                      {
                        if (warn_overlong_word)
                          {
                            fprintf(stderr, "WARN: Subtitle word '%s' too long!\n", text);
                            warn_overlong_word = 0; /* only warn once per line */
                          } /*if*/
                      } /*if*/
                  } /*if*/
              } // for textlen (all words from subtitle line read)
        // osl holds an ordered (as they appear in the lines) chain of the subtitle words
              {
              /* append last/only word */
                struct osd_text_word * const newelt = (struct osd_text_word *)calloc(1, sizeof(struct osd_text_word));
                int counter;
                if (osl == NULL)
                  {
                  /* only element on list */
                    osl = newelt;
                  }
                else
                  {
                    newelt->prev = osl_tail;
                    osl_tail->next = newelt;
                    newelt->osd_kerning = vo_font->charspace + vo_font->width[' '];
                  } /*if*/
                osl_tail = newelt;
                newelt->osd_length = xsize;
                newelt->text_length = char_position;
                newelt->text = (int *)malloc(char_position * sizeof(int));
                for (counter = 0; counter < char_position; ++counter)
                    newelt->text[counter] = wordbuf[counter];
                char_position = 0;
                xsize = -vo_font->charspace;
              }
            if (osl != NULL) /* will always be true! */
              {
                int value = 0, minimum = 0;
                struct osd_text_line *lastnewelt;
                struct osd_text_word *curword;
                struct osd_text_line *otp_new;
                // otp_new will contain the chain of the osd subtitle lines coming from the single vo_sub line.
                otp_new = lastnewelt = (struct osd_text_line *)calloc(1, sizeof(struct osd_text_line));
                lastnewelt->words = osl;
                curword = lastnewelt->words;
                for (;;)
                  {
                    while
                      (
                            curword != NULL
                        &&
                            value + curword->osd_kerning + curword->osd_length <= xlimit
                      )
                      {
                        value += curword->osd_kerning + curword->osd_length;
                        curword = curword->next;
                      } /*while*/
                    if
                      (
                            curword != NULL
                        &&
                            curword != lastnewelt->words
                              /* not sure what this does, but trying to stop it getting stuck in
                                here (fix Ubuntu bug 385187) */
                      )
                      {
                      /* append yet another element onto otp_new chain */
                        struct osd_text_line * const nextnewelt =
                            (struct osd_text_line *)calloc(1, sizeof(struct osd_text_line));
                        lastnewelt->value = value;
                        lastnewelt->next = nextnewelt;
                        nextnewelt->prev = lastnewelt;
                        lastnewelt = nextnewelt;
                        lastnewelt->words = curword;
                        value = -2 * vo_font->charspace - vo_font->width[' '];
                      }
                    else
                      {
                        lastnewelt->value = value;
                        break;
                      } /*if*/
                  } /*for*/

#ifdef NEW_SPLITTING
                // minimum holds the 'sum of the differences in length among the lines',
                // a measure of the eveness of the lengths of the lines
                  {
                    struct osd_text_line *tmp_otp;
                    for (tmp_otp = otp_new; tmp_otp->next != NULL; tmp_otp = tmp_otp->next)
                      {
                        const struct osd_text_line * pmt = tmp_otp->next;
                        while (pmt != NULL)
                          {
                            minimum += abs(tmp_otp->value - pmt->value);
                            pmt = pmt->next;
                          } /*while*/
                      } /*for*/
                  }
                if (otp_new->next != NULL)
                  {
                    int mem1, mem2;
                    // until the last word of a line can be moved to the beginning of following line
                    // reducing the 'sum of the differences in length among the lines', it is done
                    for (;;)
                      {
                        struct osd_text_line *tmp_otp;
                        int exit1 = 1; /* initial assumption */
                        struct osd_text_line *hold = NULL;
                        for (tmp_otp = otp_new; tmp_otp->next != NULL; tmp_otp = tmp_otp->next)
                          {
                            struct osd_text_word *tmp;
                            struct osd_text_line *pmt = tmp_otp->next;
                            for (tmp = tmp_otp->words; tmp->next != pmt->words; tmp = tmp->next)
                              /* find predecessor to pmt */;
                            if (pmt->value + tmp->osd_length + pmt->words->osd_kerning <= xlimit)
                              {
                                struct osd_text_line *mem;
                                mem1 = tmp_otp->value;
                                mem2 = pmt->value;
                                tmp_otp->value = mem1 - tmp->osd_length - tmp->osd_kerning;
                                pmt->value = mem2 + tmp->osd_length + pmt->words->osd_kerning;
                                value = 0;
                                for (mem = otp_new; mem->next != NULL; mem = mem->next)
                                  {
                                    pmt = mem->next;
                                    while (pmt != NULL)
                                      {
                                        value += abs(mem->value - pmt->value);
                                        pmt = pmt->next;
                                      } /*while*/
                                  } /*for*/
                                if (value < minimum)
                                  {
                                    minimum = value;
                                    hold = tmp_otp;
                                    exit1 = 0;
                                  } /*if*/
                                tmp_otp->value = mem1;
                                tmp_otp->next->value = mem2;
                              } /*if*/
                          } /*for*/
                        // merging
                        if (exit1)
                            break;
                          {
                            struct osd_text_word *tmp;
                            struct osd_text_line *pmt;
                            tmp_otp = hold;
                            pmt = tmp_otp->next;
                            for (tmp = tmp_otp->words; tmp->next != pmt->words; tmp = tmp->next)
                              /* find predecessor element */;
                            mem1 = tmp_otp->value;
                            mem2 = pmt->value;
                            tmp_otp->value = mem1 - tmp->osd_length - tmp->osd_kerning;
                            pmt->value = mem2 + tmp->osd_length + pmt->words->osd_kerning;
                            pmt->words = tmp;
                          } //~merging
                      } /*for*/
                  } //~if (otp->next != NULL)
#endif
                // adding otp (containing splitted lines) to otp chain
                if (otp_sub == NULL)
                  {
                    otp_sub = otp_new;
                    for
                      (
                        otp_sub_last = otp_sub;
                        otp_sub_last->next != NULL;
                        otp_sub_last = otp_sub_last->next
                      )
                      /* find last element in chain */;
                  }
                else
                  {
                  /* append otp_new to otp_sub chain */
                    struct osd_text_word * ott_last = otp_sub->words;
                    while (ott_last->next != NULL)
                        ott_last = ott_last->next;
                    ott_last->next = otp_new->words;
                    otp_new->words->prev = ott_last;
                    //attaching new subtitle line at the end
                    otp_sub_last->next = otp_new;
                    otp_new->prev = otp_sub_last;
                    do
                        otp_sub_last = otp_sub_last->next;
                    while (otp_sub_last->next != NULL);
                  } /*if*/
              } //~ if (osl != NULL)
          } // while (linesleft)
        free(wordbuf);
        // write lines into utbl
        xtblc = 0;
        utblc = 0;
        obj->y = dys - sub_bottom_margin;
        obj->params.subtitle.lines = 0;
          {
            struct osd_text_line *tmp_otp;
            for (tmp_otp = otp_sub; tmp_otp != NULL; tmp_otp = tmp_otp->next)
              {
                struct osd_text_word *tmp_ott, *tmp;
                if (obj->params.subtitle.lines++ >= MAX_UCSLINES)
                  {
                    fprintf(stderr, "WARN: max_ucs_lines\n");
                    break;
                  } /*if*/
                if (max_height + sub_top_margin > obj->y)    // out of the screen so end parsing
                  {
                    obj->y += vo_font->height;  // correct the y position
                    fprintf(stderr, "WARN: Out of screen at Y: %d\n", obj->y);
                    obj->params.subtitle.lines -= 1;
                      /* discard overlong line */
                    break;
                  } /*if*/
                xsize = tmp_otp->value;
                obj->params.subtitle.xtbl[xtblc++] = (xlimit - xsize) / 2 + sub_left_margin;
                if (xmin > (xlimit - xsize) / 2 + sub_left_margin)
                    xmin = (xlimit - xsize) / 2 + sub_left_margin;
                if (xmax < (xlimit + xsize) / 2 + sub_left_margin)
                    xmax = (xlimit + xsize) / 2 + sub_left_margin;
             /* fprintf(stderr, "lm %d rm: %d xm:%d xs:%d\n", sub_left_margin, sub_right_margin, xmax, xsize); */
                tmp = tmp_otp->next == NULL ? NULL : tmp_otp->next->words;
                for (tmp_ott = tmp_otp->words; tmp_ott != tmp; tmp_ott = tmp_ott->next)
                  {
                    int counter = 0;
                    for (;;)
                      {
                        int curch;
                        if (counter == tmp_ott->text_length)
                            break;
                        if (utblc > MAX_UCS)
                            break;
                        curch = tmp_ott->text[counter];
                        render_one_glyph(vo_font, curch);
                        obj->params.subtitle.utbl[utblc++] = curch;
                        sub_totallen++;
                        ++counter;
                      } /*for*/
                    obj->params.subtitle.utbl[utblc++] = ' ';
                  } /*for*/
                obj->params.subtitle.utbl[utblc - 1] = 0;
                obj->y -= vo_font->height;
              } /*for*/
          }
        if (sub_max_lines < obj->params.subtitle.lines)
            sub_max_lines = obj->params.subtitle.lines;
        if (sub_max_font_height < vo_font->height)
            sub_max_font_height = vo_font->height;
        if (sub_max_bottom_font_height < vo_font->pic_a[vo_font->font[40]]->h)
            sub_max_bottom_font_height = vo_font->pic_a[vo_font->font[40]]->h;
        if (obj->params.subtitle.lines)
            obj->y = dys - sub_bottom_margin - (obj->params.subtitle.lines * vo_font->height); /* + vo_font->pic_a[vo_font->font[40]]->h; */

        // free memory
        if (otp_sub != NULL)
          {
            struct osd_text_word *tmp;
            struct osd_text_line *pmt;
            for (tmp = otp_sub->words; tmp->next != NULL; free(tmp->prev))
              {
                free(tmp->text);
                tmp = tmp->next;
              } /*for*/
            free(tmp->text);
            free(tmp);
            for (pmt = otp_sub; pmt->next != NULL; free(pmt->prev))
              {
                pmt = pmt->next;
              } /*for*/
            free(pmt);
          }
        else
          {
            fprintf(stderr, "WARN: Subtitles requested but not found.\n");
          } /*if*/
      }
      {
        const int subs_height =
                (obj->params.subtitle.lines - 1) * vo_font->height
            +
                vo_font->pic_a[vo_font->font[40]]->h;
      /* fprintf(stderr,"^1 bby1:%d bby2:%d h:%d dys:%d oy:%d sa:%d sh:%d f:%d\n",obj->bbox.y1,obj->bbox.y2,h,dys,obj->y,v_sub_alignment,subs_height,font); */
        if (v_sub_alignment == V_SUB_ALIGNMENT_BOTTOM)
            obj->y = dys * sub_pos / 100 - sub_bottom_margin - subs_height;
        else if (v_sub_alignment == V_SUB_ALIGNMENT_CENTER)
            obj->y =
                    (
                        dys * sub_pos / 100
                    -
                        sub_bottom_margin
                    -
                        sub_top_margin
                    -
                        subs_height
                    +
                        vo_font->height
                    )
                /
                    2;
        else /* v_sub_alignment = V_SUB_ALIGNMENT_TOP */
            obj->y = sub_top_margin;
        if (obj->y < sub_top_margin)
            obj->y = sub_top_margin;
        if (obj->y > dys - sub_bottom_margin-vo_font->height)
            obj->y = dys - sub_bottom_margin-vo_font->height;

        obj->bbox.y2 = obj->y + subs_height + 3;

        // calculate bbox:
        if (sub_justify)
            xmin = sub_left_margin;
        obj->bbox.x1 = xmin - 3;
        obj->bbox.x2 = xmax + 3 + vo_font->spacewidth;

      /* if ( obj->bbox.x2 >= dxs - sub_right_margin - 20)
           {
             obj->bbox.x2 = dxs;
           }
        */
        obj->bbox.y1 = obj->y-3;
    //  obj->bbox.y2 = obj->y+obj->params.subtitle.lines * vo_font->height;
        obj->flags |= OSDFLAG_BBOX;

        alloc_buf(obj);
      /* fprintf(stderr,"^2 bby1:%d bby2:%d h:%d dys:%d oy:%d sa:%d sh:%d\n",obj->bbox.y1,obj->bbox.y2,h,dys,obj->y,v_sub_alignment,subs_height); */
      }
    switch (vo_sub->alignment)
      {
    case H_SUB_ALIGNMENT_LEFT:
        obj->alignment |= H_SUB_ALIGNMENT_LEFT;
    break;
    case H_SUB_ALIGNMENT_CENTER:
        obj->alignment |= H_SUB_ALIGNMENT_CENTER;
    break;
    case H_SUB_ALIGNMENT_RIGHT:
    default:
        obj->alignment |= H_SUB_ALIGNMENT_RIGHT;
    break;
      } /*switch*/
      {
        int i, j, prev_j;
        j = prev_j = 0;
        linesleft = obj->params.subtitle.lines;
        if (linesleft != 0)
          {
            int xtbl_min, x;
            int y = obj->y;
            for (xtbl_min = xlimit; linedone < linesleft; ++linedone)
                if (obj->params.subtitle.xtbl[linedone] < xtbl_min)
                    xtbl_min = obj->params.subtitle.xtbl[linedone];
            for (i = 0; i < linesleft; ++i)
              {
                int prevch, curch;
                switch (obj->alignment & 0x3)
                  {
                case H_SUB_ALIGNMENT_LEFT:
                    if (sub_justify)
                        x = xmin;
                    else
                        x = xtbl_min;
                break;
                case H_SUB_ALIGNMENT_RIGHT:
                    x =
                            2 * obj->params.subtitle.xtbl[i]
                        -
                            xtbl_min
                        -
                            (obj->params.subtitle.xtbl[i] == xtbl_min ? 0 : 1);
                break;
                case H_SUB_ALIGNMENT_CENTER:
                default:
                    x = obj->params.subtitle.xtbl[i];
                break;
                  } /*switch*/
                prevch = -1;
                while ((curch = obj->params.subtitle.utbl[j++]) != 0)
                  {
                    const int font = vo_font->font[curch];
                    x += kerning(vo_font, prevch, curch);
                    if (font >= 0)
                      {
                      /* fprintf(stderr, "^3 vfh:%d vfh+y:%d odys:%d\n", vo_font->pic_a[font]->h, vo_font->pic_a[font]->h + y, obj->dys); */
                        draw_alpha_buf
                          (
                            /*obj =*/ obj,
                            /*x0 =*/ x,
                            /*y0 =*/ y,
                            /*w =*/ vo_font->width[curch],
                            /*h =*/
                                vo_font->pic_a[font]->h + y < obj->dys-sub_bottom_margin ?
                                    vo_font->pic_a[font]->h
                                :
                                    obj->dys-sub_bottom_margin - y,
                            /*src =*/ vo_font->pic_b[font]->bmp + vo_font->start[curch],
                            /*srca =*/ vo_font->pic_a[font]->bmp + vo_font->start[curch],
                            /*stride =*/ vo_font->pic_a[font]->w
                          );
                      } /*if*/
                    x += vo_font->width[curch] + vo_font->charspace;
                    prevch = curch;
                  } /*while*/
                if (sub_max_chars < j - prev_j)
                    sub_max_chars = j - prev_j;
                prev_j = j;
                y += vo_font->height;
              } /*for*/
            /* Here you could retreive the buffers*/
          } /*if*/
      }
  } /*vo_update_text_sub*/

mp_osd_obj_t * new_osd_obj(int type)
  {
    mp_osd_obj_t * const osd = malloc(sizeof(mp_osd_obj_t));
    memset(osd, 0, sizeof(mp_osd_obj_t));
    osd->next = vo_osd_list;
    vo_osd_list = osd;
    osd->type = type;
    osd->alpha_buffer = NULL;
    osd->bitmap_buffer = NULL;
    osd->allocated = -1;
    return osd;
  } /*new_osd_obj*/

void free_osd_list()
  /* frees up memory allocated for vo_osd_list. */
  {
    mp_osd_obj_t * obj = vo_osd_list;
    while (obj)
      {
        mp_osd_obj_t * const next = obj->next;
        if (obj->alpha_buffer)
            free(obj->alpha_buffer);
        if (obj->bitmap_buffer)
            free(obj->bitmap_buffer);
        free(obj);
        obj = next;
      } /*while*/
    vo_osd_list = NULL;
  } /*free_osd_list*/

int vo_update_osd(int dxs, int dys)
  {
    mp_osd_obj_t * obj = vo_osd_list;
    int chg = 0;

#ifdef HAVE_FREETYPE
    // here is the right place to get screen dimensions
    if (!vo_font || force_load_font)
      {
        force_load_font = 0;
        load_font_ft(dxs, dys);
      } /*if*/
#endif

    while(obj)
      {
        if (dxs != obj->dxs || dys != obj->dys || obj->flags & OSDFLAG_FORCE_UPDATE)
          {
            int vis;
            obj->flags = obj->flags | OSDFLAG_VISIBLE;
            vis = obj->flags & OSDFLAG_VISIBLE;
            obj->flags &= ~OSDFLAG_BBOX;
            switch (obj->type)
              {
            case OSDTYPE_SUBTITLE:
                if (vo_sub)
                  {
                    obj->dxs = dxs;
                    obj->dys = dys;
                    vo_update_text_sub(obj, dxs ,dys);
                  /* obj->dxs = dxs; obj->dys = dys;
                    fprintf(stderr, "x1:%d x2:%d y1:%d y2:%d\n", obj->bbox.x1, obj->bbox.x2, obj->bbox.y1, obj->bbox.y2); */
                    vo_draw_alpha_rgb24
                      (
                        /*w =*/ obj->bbox.x2 - obj->bbox.x1,
                        /*h =*/ obj->bbox.y2 - obj->bbox.y1,
                        /*src =*/ obj->bitmap_buffer,
                        /*srca =*/ obj->alpha_buffer,
                        /*srcstride =*/ obj->stride,
                        /*dstbase =*/ textsub_image_buffer + 3 * obj->bbox.x1 + 3 * obj->bbox.y1 * movie_width,
                        /*dststride =*/ movie_width * 3
                      );
                  } /*if*/
            break;
              } /*switch*/
            // check if visibility changed:
            if (vis != (obj->flags & OSDFLAG_VISIBLE))
                obj->flags |= OSDFLAG_CHANGED;
            // remove the cause of automatic update:
            obj->flags &= ~OSDFLAG_FORCE_UPDATE;
          } /*if*/
        if (obj->flags & OSDFLAG_CHANGED)
          {
            chg |= 1 << obj->type;
          /* fprintf(stderr, "DEBUG:OSD chg: %d  V: %s  \n", obj->type, (obj->flags & OSDFLAG_VISIBLE) ? "yes" : "no"); */
          } /*if*/
        obj = obj->next;
      } /*while*/
    return chg;
  } /*vo_update_osd*/

void vo_init_osd()
  {
    if (vo_osd_list)
        free_osd_list();
  // temp hack, should be moved to mplayer/mencoder later
  /* new_osd_obj(OSDTYPE_OSD); */
    new_osd_obj(OSDTYPE_SUBTITLE);
  /* new_osd_obj(OSDTYPE_PROGBAR);
    new_osd_obj(OSDTYPE_SPU); */
#ifdef HAVE_FREETYPE
    force_load_font = 1;
#endif
  } /*vo_init_osd*/

int vo_osd_changed(int new_value)
  {
    mp_osd_obj_t * obj = vo_osd_list;
    const int previous_value = vo_osd_changed_status;
    vo_osd_changed_status = new_value;
    while (obj)
      {
        if (obj->type == new_value)
            obj->flags |= OSDFLAG_FORCE_UPDATE;
        obj = obj->next;
      } /*while*/
    return previous_value;
  } /*vo_osd_changed*/

