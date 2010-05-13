/*
    Layout and rendering of subtitles for spumux
*/
/* Copyright (C) 2000 - 2003 various authors of the MPLAYER project
 * This module uses various parts of the MPLAYER project (http://www.mplayerhq.hu)
 * With many changes by Sjef van Gool (svangool@hotmail.com) November 2003
 * Major rework by Lawrence D'Oliveiro <ldo@geek-central.gen.nz>
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

#include "subglobals.h"
#include "subrender.h"
#include "subfont.h"

#define NEW_SPLITTING

typedef struct mp_osd_bbox_s
  {
    int x1, y1; /* top left */
    int x2, y2; /* bottom right */
  } mp_osd_bbox_t;

#define MAX_UCS 1600
#define MAX_UCSLINES 16

typedef struct /* for holding and maintaining a rendered subtitle image */
  {
    int topy;
    mp_osd_bbox_t bbox; // bounding box
    union
      {
        struct /* only one used by spumux */
          { /* subtitle text lines already laid out ready for rendering */
            int utbl[MAX_UCS + 1]; /* all display lines concatenated, each terminated by a null */
            int xtbl[MAX_UCSLINES]; /* x-positions of lines for centre alignment */
            int lines;          // no. of lines
          } subtitle;
      } params;
    int stride; /* bytes per row of both alpha and bitmap buffers */
    int allocated; /* size in bytes of each buffer */
    unsigned char *bitmap_buffer; /* four bytes per pixel */
  } mp_osd_obj_t;

static int sub_pos=100;
/* static int sub_width_p=100; */

int sub_justify=1; /* fixme: not user-settable */
int sub_left_margin=60;   /* Size of left horizontal non-display area in pixel units */
int sub_right_margin=60;  /* Size of right horizontal non-display area in pixel units */
int sub_bottom_margin=30; /* Size of bottom horizontal non-display area in pixel units */
int sub_top_margin=20;    /* Size of top horizontal non-display area in pixel units */
int h_sub_alignment = H_SUB_ALIGNMENT_LEFT;  /* Horizontal alignment 0=center, 1=left, 2=right, 4=subtitle default */
int v_sub_alignment = V_SUB_ALIGNMENT_BOTTOM;      /* Vertical alignment 0=top, 1=center, 2=bottom */
/* following default according to default_video_format if not explicitly set by user: */
float movie_fps = 0.0;
int movie_width = 0;
int movie_height = 0;

unsigned char *textsub_image_buffer;
size_t textsub_image_buffer_size;

/* statistics */
int sub_max_chars;
int sub_max_lines;
int sub_max_font_height;
int sub_max_bottom_font_height;

static mp_osd_obj_t* vo_osd = NULL;

static inline void vo_draw_subtitle_line
  (
    int w, /* dimensions of area to copy */
    int h,
    const unsigned char * srcbase, /* source pixels */
    int srcstride, /* for srcbase */
    unsigned char * dstbase, /* where to copy to */
    int dststride /* for dstbase */
  )
  /* copies pixels from srcbase onto dstbase. Used to transfer a complete
    rendered line to textsub_image_buffer. */
  {
    int y;
    for (y = 0; y < h; y++)
      {
        const register unsigned char * src = srcbase;
        register unsigned char * dst = dstbase;
        register int x;
        for (x = 0; x < w; x++)
          {
            *dst++ = *src++;
            *dst++ = *src++;
            *dst++ = *src++;
            *dst++ = *src++;
          } /*for*/
        srcbase += srcstride;
        dstbase += dststride;
      } /*for*/
  } /*vo_draw_subtitle_line*/

static void draw_glyph
  (
    mp_osd_obj_t * obj,
    int x0, /* origin in destination buffer to copy to */
    int y0,
    int w, /* dimensions of area to copy */
    int h,
    const unsigned char * src, /* source pixels */
    const colorspec * srccolors, /* source palette */
    int stride /* of source */
  )
  /* used to assemble complete rendered screen lines in obj by copying individual
    glyph images. */
  {
    int dststride = obj->stride;
    int dstskip = obj->stride - w * 4;
    int srcskip = stride - w;
    int i, j;
    unsigned char * bdst =
            obj->bitmap_buffer
        +
            (y0 - obj->bbox.y1) * dststride
        +
            (x0 - obj->bbox.x1) * 4;
    const unsigned char * bsrc = src;
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
        for (j = 0; j < w; j++)
          {
            const colorspec srccolor = srccolors[*bsrc++];
            if (srccolor.a != 0)
              {
                *bdst++ = srccolor.r;
                *bdst++ = srccolor.g;
                *bdst++ = srccolor.b;
                *bdst++ = srccolor.a;
              }
            else
              {
                bdst += 4;
              } /*if*/
          } /*for*/
        bdst += dstskip;
        bsrc += srcskip;
      } /*for*/
  } /*draw_glyph*/

static void alloc_buf(mp_osd_obj_t * obj)
  /* (re)allocates pixel buffers to be large enough for bbox. */
  {
    int len;
  /* fprintf(stderr,"x1:%d x2:%d y1:%d y2:%d\n",obj->bbox.x1,obj->bbox.x2,obj->bbox.y1,obj->bbox.y2); */
    if (obj->bbox.x2 < obj->bbox.x1)
        obj->bbox.x2 = obj->bbox.x1;
    if (obj->bbox.y2 < obj->bbox.y1)
        obj->bbox.y2 = obj->bbox.y1;
    obj->stride = (obj->bbox.x2 - obj->bbox.x1) * 4 + 7 & ~7; /* round up to multiple of 8 bytes--why bother? */
    len = obj->stride * (obj->bbox.y2 - obj->bbox.y1);
    if (obj->allocated < len)
      {
      /* allocate new, bigger buffers, don't bother preserving contents of old ones */
        obj->allocated = len;
        free(obj->bitmap_buffer);
        obj->bitmap_buffer = (unsigned char *)malloc(len);
      } /*if*/
    memset(obj->bitmap_buffer, 0, len);
  } /*alloc_buf*/

inline static void vo_update_text_sub
  (
    mp_osd_obj_t * obj,
    const subtitle_elt * the_sub
  )
  /* lays out and renders the subtitle text from the_sub using the font settings from vo_font,
    putting the results into obj. */
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
        struct osd_text_word *prev, *next; /* doubly-linked list of all words on all lines */
      };
    struct osd_text_line
      {
        int linewidth;
        struct osd_text_word *words; /* where in word list this line starts */
        struct osd_text_line *prev, *next; /* doubly-linked list */
      };
    int linedone, linesleft;
    bool warn_overlong_word;
    int textlen, sub_totallen;
  /* const int widthlimit = movie_width * sub_width_p / 100; */
    const int widthlimit = movie_width - sub_right_margin - sub_left_margin;
      /* maximum width of display lines after deducting space for margins
        and starting point */
    int xmin = widthlimit, xmax = 0;
    int max_line_height;
    int xtblc, utblc;

    if (!the_sub || !vo_font)
      {
        return;
      } /*if*/
    obj->bbox.y2 = obj->topy = movie_height - sub_bottom_margin;
    obj->params.subtitle.lines = 0;

    // too long lines divide into a smaller ones
    linedone = sub_totallen = 0;
    max_line_height = vo_font->height;
      /* actually a waste of time computing this when I don't allow mixing fonts */
    linesleft = the_sub->lines;
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
            int chindex, prevch, wordlen;
            const unsigned char *text;
            int xsize = -vo_font->charspace;
              /* cancels out extra space left before first word of first line */
            linesleft--;
            text = (const unsigned char *)the_sub->text[linedone++];
            textlen = strlen((const char *)text);
            wordlen = 0;
            wordbuf = (int *)realloc(wordbuf, textlen * sizeof(int));
            prevch = -1;
            osl = NULL;
            osl_tail = NULL;
            warn_overlong_word = true;
            // reading the subtitle words from the_sub->text[]
            chindex = 0;
            for (;;) /* split line into words */
              {
                int curch;
                if (chindex < textlen)
                  {
                    curch = text[chindex];
                    if (curch >= 0x80)
                      {
                      /* fixme: no checking for chindex going out of range */
                        if ((curch & 0xe0) == 0xc0)    /* 2 bytes U+00080..U+0007FF*/
                            curch = (curch & 0x1f) << 6 | (text[++chindex] & 0x3f);
                        else if ((curch & 0xf0) == 0xe0) /* 3 bytes U+00800..U+00FFFF*/
                          {
                            curch = (((curch & 0x0f) << 6) | (text[++chindex] & 0x3f)) << 6;
                            curch |= text[++chindex] & 0x3f;
                          } /*if*/
                      } /*if*/
                    if (sub_totallen == MAX_UCS)
                      {
                        textlen = chindex; // end here
                        fprintf(stderr, "WARN: MAX_UCS exceeded!\n");
                      } /*if*/
                    if (!curch)
                        curch++; // avoid UCS 0
                    render_one_glyph(vo_font, curch); /* ensure I have an image for it */
                  } /*if*/
                if (chindex >= textlen || curch == ' ')
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
                    newelt->text_length = wordlen;
                    newelt->text = (int *)malloc(wordlen * sizeof(int));
                    for (counter = 0; counter < wordlen; ++counter)
                        newelt->text[counter] = wordbuf[counter];
                    wordlen = 0;
                    if (chindex == textlen)
                      {
                        xsize = -vo_font->charspace;
                          /* cancels out extra space left before first word of next line */
                        break;
                      } /*if*/
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
                      /* width which will be added to word by this character */
                    if (xsize + delta_xsize <= widthlimit)
                      {
                      /* word still fits in available width */
                        if (!warn_overlong_word)
                            warn_overlong_word = true;
                        prevch = curch;
                        wordbuf[wordlen++] = curch;
                        xsize += delta_xsize;
                        if (!suboverlap_enabled)
                          {
                          /* keep track of line heights to ensure no overlap */
                            const int font = vo_font->font[curch];
                            if (font >= 0 && vo_font->pic_b[font]->h > max_line_height)
                              {
                                max_line_height = vo_font->pic_b[font]->h;
                              } /*if*/
                          } /*if*/
                      }
                    else
                      {
                      /* truncate word to fit */
                        if (warn_overlong_word)
                          {
                            fprintf(stderr, "WARN: Subtitle word '%s' too long!\n", text);
                            warn_overlong_word = false; /* only warn once per line */
                          } /*if*/
                      } /*if*/
                  } /*if*/
                ++chindex;
              } /*for*/
        // osl holds an ordered (as they appear in the lines) chain of the subtitle words
            if (osl != NULL) /* will always be true! */
              {
              /* collect words of this line into one or more on-screen lines,
                wrapping too-long lines */
                int linewidth = 0, linewidth_variation = 0;
                struct osd_text_line *lastnewelt;
                struct osd_text_word *curword;
                struct osd_text_line *otp_new;
                // otp_new will contain the chain of the osd subtitle lines coming from the single the_sub line.
                otp_new = lastnewelt = (struct osd_text_line *)calloc(1, sizeof(struct osd_text_line));
                lastnewelt->words = osl;
                curword = lastnewelt->words;
                for (;;)
                  {
                    while
                      (
                            curword != NULL
                        &&
                            linewidth + curword->osd_kerning + curword->osd_length <= widthlimit
                      )
                      {
                      /* include another word on this line */
                        linewidth += curword->osd_kerning + curword->osd_length;
                        curword = curword->next;
                      } /*while*/
                    if
                      (
                            curword != NULL
                        &&
                            curword != lastnewelt->words
                              /* ensure new line contains at least one word (fix Ubuntu bug 385187) */
                      )
                      {
                      /* append yet another new display line onto otp_new chain */
                        struct osd_text_line * const nextnewelt =
                            (struct osd_text_line *)calloc(1, sizeof(struct osd_text_line));
                        lastnewelt->linewidth = linewidth;
                        lastnewelt->next = nextnewelt;
                        nextnewelt->prev = lastnewelt;
                        lastnewelt = nextnewelt;
                        lastnewelt->words = curword;
                        linewidth = -2 * vo_font->charspace - vo_font->width[' '];
                      }
                    else
                      {
                        lastnewelt->linewidth = linewidth;
                        break;
                      } /*if*/
                  } /*for*/
#ifdef NEW_SPLITTING
              /* rebalance split among multiple onscreen lines corresponding to a single
                subtitle line */
                // linewidth_variation holds the 'sum of the differences in length among the lines',
                // a measure of the eveness of the lengths of the lines
                  {
                    struct osd_text_line *tmp_otp;
                    for (tmp_otp = otp_new; tmp_otp->next != NULL; tmp_otp = tmp_otp->next)
                      {
                        const struct osd_text_line * pmt = tmp_otp->next;
                        while (pmt != NULL)
                          {
                            linewidth_variation += abs(tmp_otp->linewidth - pmt->linewidth);
                            pmt = pmt->next;
                          } /*while*/
                      } /*for*/
                  }
                if (otp_new->next != NULL) /* line split into more than one display line */
                  {
                    // until the last word of a line can be moved to the beginning of following line
                    // reducing the 'sum of the differences in length among the lines', it is done
                    for (;;)
                      /* even out variations in width of screen lines corresponding to
                        a single subtitle line */
                      {
                        struct osd_text_line *this_display_line;
                        bool exit1 = true; /* initial assumption */
                        struct osd_text_line *rebalance_line = NULL;
                          /* if non-null, then word at end of this line should be moved to
                            following line */
                        for
                          (
                            this_display_line = otp_new;
                            this_display_line->next != NULL;
                            this_display_line = this_display_line->next
                          )
                          {
                            struct osd_text_line *next_display_line = this_display_line->next;
                            struct osd_text_word *prev_word;
                            for
                              (
                                prev_word = this_display_line->words;
                                prev_word->next != next_display_line->words;
                                prev_word = prev_word->next
                              )
                              /* find predecessor word to next_display_line */;
                              /* seems a shame I can't make use of the doubly-linked lists
                                somehow to speed this up */
                            if
                              (
                                        next_display_line->linewidth
                                    +
                                        prev_word->osd_length
                                    +
                                        next_display_line->words->osd_kerning
                                <=
                                    widthlimit
                              )
                              {
                              /* prev_word can be moved from this_display_line line onto
                                next_display_line line; see if doing this improves the layout */
                                struct osd_text_line *that_display_line;
                                int new_variation;
                                int prev_line_width, cur_line_width;
                                prev_line_width = this_display_line->linewidth;
                                cur_line_width = next_display_line->linewidth;
                              /* temporary change to line widths to see effect of new layout */
                                this_display_line->linewidth =
                                        prev_line_width
                                    -
                                        prev_word->osd_length
                                    -
                                        prev_word->osd_kerning;
                                next_display_line->linewidth =
                                        cur_line_width
                                    +
                                        prev_word->osd_length
                                    +
                                        next_display_line->words->osd_kerning;
                                new_variation = 0;
                                for
                                  (
                                    that_display_line = otp_new;
                                    that_display_line->next != NULL;
                                    that_display_line = that_display_line->next
                                  )
                                  {
                                    next_display_line = that_display_line->next;
                                    while (next_display_line != NULL)
                                      {
                                        new_variation +=
                                            abs
                                              (
                                                    that_display_line->linewidth
                                                -
                                                    next_display_line->linewidth
                                              );
                                        next_display_line = next_display_line->next;
                                      } /*while*/
                                  } /*for*/
                                if (new_variation < linewidth_variation)
                                  {
                                  /* implement this new layout unless I find something better */
                                    linewidth_variation = new_variation;
                                    rebalance_line = this_display_line;
                                    exit1 = false;
                                  } /*if*/
                              /* undo the temporary line width changes */
                                this_display_line->linewidth = prev_line_width;
                                this_display_line->next->linewidth = cur_line_width;
                              } /*if*/
                          } /*for*/
                        // merging
                        if (exit1) /* no improvement found */
                            break;
                          {
                          /* word at end of rebalance_line line should be moved to following line */
                            struct osd_text_word *word_to_move;
                            struct osd_text_line *next_display_line;
                            this_display_line = rebalance_line;
                            next_display_line = this_display_line->next;
                            for
                              (
                                word_to_move = this_display_line->words;
                                word_to_move->next != next_display_line->words;
                                word_to_move = word_to_move->next
                              )
                              /* find previous word to be moved to this line */;
                              /* seems a shame I can't make use of the doubly-linked lists
                                somehow to speed this up, not to mention having to do
                                it twice */
                            this_display_line->linewidth -=
                                word_to_move->osd_length + word_to_move->osd_kerning;
                            next_display_line->linewidth +=
                                word_to_move->osd_length + next_display_line->words->osd_kerning;
                            next_display_line->words = word_to_move;
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
        xtblc = 0; /* count of display lines */
        utblc = 0; /* total count of characters in all display lines */
        obj->topy = movie_height - sub_bottom_margin;
        obj->params.subtitle.lines = 0;
          {
          /* collect display line text into obj->params.subtitle.utbl and x-positions
            into obj->params.subtitle.xtbl */
            struct osd_text_line *this_display_line;
            for
              (
                this_display_line = otp_sub;
                this_display_line != NULL;
                this_display_line = this_display_line->next
              )
              {
                struct osd_text_word *this_word, *next_line_words;
                int xsize;
                if (obj->params.subtitle.lines++ >= MAX_UCSLINES)
                  {
                    fprintf(stderr, "WARN: max_ucs_lines\n");
                    break;
                  } /*if*/
                if (max_line_height + sub_top_margin > obj->topy) // out of the screen so end parsing
                  {
                    obj->topy += vo_font->height; /* undo inclusion of last line */
                    fprintf(stderr, "WARN: Out of screen at Y: %d\n", obj->topy);
                    obj->params.subtitle.lines -= 1;
                      /* discard overlong line */
                    break;
                  } /*if*/
                xsize = this_display_line->linewidth;
                obj->params.subtitle.xtbl[xtblc++] = (widthlimit - xsize) / 2 + sub_left_margin;
                if (xmin > (widthlimit - xsize) / 2 + sub_left_margin)
                    xmin = (widthlimit - xsize) / 2 + sub_left_margin;
                if (xmax < (widthlimit + xsize) / 2 + sub_left_margin)
                    xmax = (widthlimit + xsize) / 2 + sub_left_margin;
             /* fprintf(stderr, "lm %d rm: %d xm:%d xs:%d\n", sub_left_margin, sub_right_margin, xmax, xsize); */
                next_line_words =
                    this_display_line->next == NULL ?
                        NULL
                    :
                        this_display_line->next->words;
                for
                  (
                    this_word = this_display_line->words;
                    this_word != next_line_words;
                    this_word = this_word->next
                  )
                  {
                  /* assemble display lines into obj->params.subtitle */
                    int chindex = 0;
                    for (;;)
                      {
                        int curch;
                        if (chindex == this_word->text_length)
                            break;
                        if (utblc > MAX_UCS)
                            break;
                        curch = this_word->text[chindex];
                        render_one_glyph(vo_font, curch); /* fixme: didn't we already do this? */
                        obj->params.subtitle.utbl[utblc++] = curch;
                        sub_totallen++;
                        ++chindex;
                      } /*for*/
                    obj->params.subtitle.utbl[utblc++] = ' '; /* separate from next word */
                  } /*for*/
                obj->params.subtitle.utbl[utblc - 1] = 0;
                  /* overwrite last space with string terminator */
                obj->topy -= vo_font->height; /* adjust top to leave room for another line */
                  /* fixme: shouldn't that be max_line_height? same is true some other places
                    vo_font->height is mentioned */
              } /*for*/
          }
        if (sub_max_lines < obj->params.subtitle.lines)
            sub_max_lines = obj->params.subtitle.lines;
        if (sub_max_font_height < vo_font->height)
            sub_max_font_height = vo_font->height;
        if (sub_max_bottom_font_height < vo_font->pic_b[vo_font->font[40]]->h)
            sub_max_bottom_font_height = vo_font->pic_b[vo_font->font[40]]->h;
        if (obj->params.subtitle.lines)
            obj->topy = movie_height - sub_bottom_margin - (obj->params.subtitle.lines * vo_font->height); /* + vo_font->pic_b[vo_font->font[40]]->h; */

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
      /* work out vertical alignment and final positioning of subtitle */
        const int subs_height =
                (obj->params.subtitle.lines - 1) * vo_font->height
            +
                vo_font->pic_b[vo_font->font[40]]->h;
      /* fprintf(stderr,"^1 bby1:%d bby2:%d h:%d movie_height:%d oy:%d sa:%d sh:%d f:%d\n",obj->bbox.y1,obj->bbox.y2,h,movie_height,obj->topy,v_sub_alignment,subs_height,font); */
        if (v_sub_alignment == V_SUB_ALIGNMENT_BOTTOM)
            obj->topy = movie_height * sub_pos / 100 - sub_bottom_margin - subs_height;
        else if (v_sub_alignment == V_SUB_ALIGNMENT_CENTER)
            obj->topy =
                    (
                        movie_height * sub_pos / 100
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
            obj->topy = sub_top_margin;
        if (obj->topy < sub_top_margin)
            obj->topy = sub_top_margin;
        if (obj->topy > movie_height - sub_bottom_margin - vo_font->height)
            obj->topy = movie_height - sub_bottom_margin - vo_font->height;
        obj->bbox.y2 = obj->topy + subs_height + 3;
        // calculate bbox:
        if (sub_justify)
            xmin = sub_left_margin;
        obj->bbox.x1 = xmin - 3;
        obj->bbox.x2 = xmax + 3 + vo_font->spacewidth;
      /* if (obj->bbox.x2 >= movie_width - sub_right_margin - 20)
           {
             obj->bbox.x2 = movie_width;
           } */
        obj->bbox.y1 = obj->topy - 3;
    //  obj->bbox.y2 = obj->topy + obj->params.subtitle.lines * vo_font->height;
        alloc_buf(obj);
      /* fprintf(stderr,"^2 bby1:%d bby2:%d h:%d movie_height:%d oy:%d sa:%d sh:%d\n",obj->bbox.y1,obj->bbox.y2,h,movie_height,obj->topy,v_sub_alignment,subs_height); */
      }
      {
      /* now to actually render the subtitle */
        int i, chindex, prev_line_end;
        chindex = prev_line_end = 0;
        linesleft = obj->params.subtitle.lines;
        if (linesleft != 0)
          {
            int xtbl_min, x;
            int y = obj->topy;
            for (xtbl_min = widthlimit; linedone < linesleft; ++linedone)
                if (obj->params.subtitle.xtbl[linedone] < xtbl_min)
                    xtbl_min = obj->params.subtitle.xtbl[linedone];
            for (i = 0; i < linesleft; ++i)
              {
                int prevch, curch;
                switch (the_sub->alignment) /* determine start position for rendering line */
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
                while ((curch = obj->params.subtitle.utbl[chindex++]) != 0)
                  {
                  /* collect the rendered characters of this subtitle display line */
                    const int font = vo_font->font[curch];
                    x += kerning(vo_font, prevch, curch);
                    if (font >= 0)
                      {
                      /* fprintf(stderr, "^3 vfh:%d vfh+y:%d odys:%d\n", vo_font->pic_b[font]->h, vo_font->pic_b[font]->h + y, movie_height); */
                        draw_glyph
                          (
                            /*obj =*/ obj,
                            /*x0 =*/ x,
                            /*y0 =*/ y,
                            /*w =*/ vo_font->width[curch],
                            /*h =*/
                                vo_font->pic_b[font]->h + y < movie_height - sub_bottom_margin ?
                                    vo_font->pic_b[font]->h
                                :
                                    movie_height - sub_bottom_margin - y,
                            /*src =*/ vo_font->pic_b[font]->bmp + vo_font->start[curch],
                            /*srccolors =*/ vo_font->pic_b[font]->pal,
                            /*stride =*/ vo_font->pic_b[font]->w
                          );
                      } /*if*/
                    x += vo_font->width[curch] + vo_font->charspace;
                    prevch = curch;
                  } /*while*/
                if (sub_max_chars < chindex - prev_line_end)
                    sub_max_chars = chindex - prev_line_end;
                prev_line_end = chindex;
                y += vo_font->height;
              } /*for*/
            /* Here you could retreive the buffers*/
          } /*if*/
      }
  } /*vo_update_text_sub*/

void vo_update_osd(const subtitle_elt * vo_sub)
  {
    memset(textsub_image_buffer, 0, textsub_image_buffer_size);
      /* fill with transparent colour */
    vo_update_text_sub(vo_osd, vo_sub);
    vo_draw_subtitle_line
      (
        /*w =*/ vo_osd->bbox.x2 - vo_osd->bbox.x1,
        /*h =*/ vo_osd->bbox.y2 - vo_osd->bbox.y1,
        /*srcbase =*/ vo_osd->bitmap_buffer,
        /*srcstride =*/ vo_osd->stride,
        /*dstbase =*/
                textsub_image_buffer
            +
                4 * vo_osd->bbox.x1
            +
                4 * vo_osd->bbox.y1 * movie_width,
        /*dststride =*/ movie_width * 4
      );
  } /*vo_update_osd*/

void vo_init_osd()
  {
    vo_finish_osd(); /* if previously allocated */
    switch (default_video_format)
      {
    case VF_NTSC:
        if (movie_fps == 0.0)
          {
            movie_fps = 29.97;
          } /*if*/
        if (movie_width == 0)
          {
            movie_width = 720;
          }
        if (movie_height == 0)
          {
            movie_height = 478;
          } /*if*/
    break;
    case VF_PAL:
        if (movie_fps == 0.0)
          {
            movie_fps = 25.0;
          } /*if*/
        if (movie_width == 0)
          {
            movie_width = 720;
          }
        if (movie_height == 0)
          {
            movie_height = 574;
          } /*if*/
    break;
    default:
        fprintf(stderr, "ERR:  cannot determine default video size and frame rate--no video format specified\n");
        exit(1);
      } /*switch*/
    textsub_image_buffer_size = sizeof(uint8_t) * 4 * movie_height * movie_width;
    textsub_image_buffer = malloc(textsub_image_buffer_size);
      /* fixme: not freed from previous call! */
    if (textsub_image_buffer == NULL)
     {
        fprintf(stderr, "ERR:  Failed to allocate memory\n");
        exit(1);
      } /*if*/
#ifdef HAVE_FREETYPE
    init_freetype();
    load_font_ft();
#endif
    sub_max_chars = 0;
    sub_max_lines = 0;
    sub_max_font_height = 0;
    sub_max_bottom_font_height = 0;
    vo_osd = malloc(sizeof(mp_osd_obj_t));
    memset(vo_osd, 0, sizeof(mp_osd_obj_t));
    vo_osd->bitmap_buffer = NULL;
    vo_osd->allocated = -1;
  } /*vo_init_osd*/

void vo_finish_osd()
  /* frees up memory allocated for vo_osd. */
  {
    if (vo_osd)
      {
        free(vo_osd->bitmap_buffer);
      } /*if*/
    free(vo_osd);
    vo_osd = NULL;
#ifdef HAVE_FREETYPE
    done_freetype();
#endif
    free(textsub_image_buffer);
    textsub_image_buffer = NULL;
  } /*vo_finish_osd*/
