/*
    Reading of subtitle files for spumux
*/
/* Copyright (C) 2000 - 2003 various authors of the MPLAYER project
 * This module uses various parts of the MPLAYER project (http://www.mplayerhq.hu)
 * With many changes by Sjef van Gool (svangool@hotmail.com) November 2003
 * And many changes by Lawrence D'Oliveiro <ldo@geek-central.gen.nz> April 2010.
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

/*
 * Subtitle reader with format autodetection
 *
 * Written by laaz
 * Some code cleanup & realloc() by A'rpi/ESP-team
 * pjs sub format by szabi
 *
 */

#include "config.h"

#include "compat.h"

#include <ctype.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <assert.h>

#include "subglobals.h"
#include "subreader.h"

#define ERR ((void *) -1)

#ifdef HAVE_FRIBIDI
#include <fribidi/fribidi.h>
#endif

// subtitle formats
#define SUB_INVALID   -1
#define SUB_MICRODVD  0 /* see <http://en.wikipedia.org/wiki/MicroDVD> */
#define SUB_SUBRIP    1 /* see <http://wiki.multimedia.cx/index.php?title=SubRip> */
#define SUB_SUBVIEWER 2 /* see <http://en.wikipedia.org/wiki/SubViewer>, sample <http://wiki.videolan.org/SubViewer> */
#define SUB_SAMI      3 /* see <http://en.wikipedia.org/wiki/SAMI>, sample <http://www.titlefactory.com/TitleFactoryDocs/sami_format.htm> */
#define SUB_VPLAYER   4
#define SUB_RT        5
#define SUB_SSA       6 /* spec is at <http://moodub.free.fr/video/ass-specs.doc>, or see <http://www.matroska.org/technical/specs/subtitles/ssa.html> */
#define SUB_PJS       7 /* Phoenix Japanimation Society */
#define SUB_MPSUB     8
#define SUB_AQTITLE   9
#define SUB_SUBVIEWER2 10 /* see <http://en.wikipedia.org/wiki/SubViewer>, sample <http://wiki.videolan.org/SubViewer> */
#define SUB_SUBRIP09 11
#define SUB_JACOSUB  12  /* spec is at <http://unicorn.us.com/jacosub/jscripts.html>. */

/* Maximal length of line of a subtitle */
#define LINE_LEN 1000

static float
  /* global state for mpsub subtitle format */
    mpsub_position = 0.0,
      /* for determining next start time, because file format only gives incremental durations */
    mpsub_multiplier = 1.0; /* for scaling times */
static int
  /* global state for sami subtitle format */
    sami_sub_slacktime = 20000; //20 sec

static int sub_no_text_pp=0;   // 1 => do not apply text post-processing
                        // like {\...} elimination in SSA format.
  /* never set to any other value */

#define USE_SORTSUB 1
  /* whether to ensure that subtitle entries are sorted into time order */
#ifdef USE_SORTSUB
/*
   Some subtitling formats, namely AQT and Subrip09, define the end of a
   subtitle as the beginning of the following. Since currently we read one
   subtitle at time, for these format we keep two global *subtitle,
   previous_aqt_sub and previous_subrip09_sub, pointing to previous subtitle,
   so we can change its end when we read current subtitle starting time.
   When USE_SORTSUB is defined, we use a single global unsigned long,
   previous_sub_end, for both (and even future) formats, to store the end of
   the previous sub: it is initialized to 0 in sub_read_file and eventually
   modified by sub_read_aqt_line or sub_read_subrip09_line.
 */
static unsigned long previous_sub_end;
#endif

/*
    Caller-controllable settings
*/

char* subtitle_charset = NULL;
  /* subtitle_charset (char) contains "from character-set" for ICONV like ISO8859-1 and UTF-8, */
  /* "to character-set" is set to UTF-8. If not specified, then defaults to locale */

float sub_delay=0.0;
/* sub_delay (float) contains delay for subtitles in 10 msec intervals (optional user parameter, 0.0 for no delay)*/
float sub_fps=0.0;
/* sub_fps (float)contains subtitle frames per second, only applicable when we have no timed subs (detection from
   video stream, 0.0 for setting taken over from fps otherwise subtitle fps)*/
int suboverlap_enabled=1;
/* suboverlap_enabled (int) indicates overlap if the user forced it (suboverlap_enabled == 2) or
   the user didn't forced no-overlapsub and the format is Jacosub or Ssa.
   this is because usually overlapping subtitles are found in these formats,
   while in others they are probably result of bad timing (set by subtile file type if
   initialized on 1) */
 /* never set to any other value */

/*
    Useful string-handling routines
*/

static int eol(char p)
  /* does p indicate the end of a line. */
  {
    return p == '\r' || p == '\n' || p == '\0';
  } /*eol*/

/* Remove leading and trailing space */
static void trail_space(char *s)
  {
    int i = 0;
    while (isspace(s[i]))
        ++i;
    if (i)
        strcpy(s, s + i);
    i = strlen(s) - 1;
    while (i > 0 && isspace(s[i]))
        s[i--] = '\0';
  } /*trail_space*/

static const char *stristr(const char *haystack, const char *needle)
  /* case-insensitive substring search. */
  {
    int len = 0;
    const char *p = haystack;
    if (!(haystack && needle))
        return NULL;
    len = strlen(needle);
    while (*p != '\0')
      {
        if (strncasecmp(p, needle, len) == 0)
            return p;
        p++;
      } /*while*/
    return NULL;
  } /*stristr*/

/*
    Input-file reading
*/

enum
  {
    sub_buf_size = 2048,
  };
struct vfile
    subfile;
static char *
    sub_buf = NULL;
static size_t
    sub_out_size,
    sub_next_out,
    sub_end_out;
static bool
    sub_buf_rewindable = false;
static int
    in_charno = 0,
    in_lineno = 1;

static void sub_open(const char * filename)
  {
    subfile = varied_open(filename, O_RDONLY, "subtitle file");
    sub_out_size = sub_buf_size;
    sub_buf = malloc(sub_out_size);
    sub_next_out = 0;
    sub_end_out = 0;
    in_charno = 0;
    in_lineno = 1;
  } /*sub_open*/

static void sub_close(void)
  {
    varied_close(subfile);
    free(sub_buf);
    sub_buf = NULL;
  } /*sub_close*/

#ifdef HAVE_ICONV

static iconv_t
    icdsc = ICONV_NULL; /* for converting subtitle text encoding to UTF-8 */
static char
    ic_inbuf[sub_buf_size]; /* size to minimize reads from disk */
static size_t
    ic_next_in,
    ic_end_in;
static int
    ic_needmore,
    ic_eof;

static void subcp_open(void)
  /* opens an iconv context for converting subtitles from subtitle_charset to UTF-8 if appropriate. */
  {
    const char * const fromcp = subtitle_charset != NULL ? subtitle_charset : default_charset;
    const char * const tocp = "UTF-8";
    icdsc = iconv_open(tocp, fromcp);
    if (icdsc != ICONV_NULL)
      {
        fprintf(stderr, "INFO: Opened iconv descriptor. *%s* <= *%s*\n", tocp, fromcp);
        ic_next_in = 0;
        ic_end_in = 0;
        ic_needmore = false;
        ic_eof = false;
      }
    else
      {
        fprintf
          (
            stderr,
            "ERR:  Error %d -- %s opening iconv descriptor for charset \"%s\".\n",
            errno, strerror(errno), fromcp
          );
        exit(1);
      } /*if*/
  } /*subcp_open*/

static void subcp_close(void)
  /* closes the previously-opened iconv context, if any. */
  {
    if (icdsc != ICONV_NULL)
      {
        (void)iconv_close(icdsc);
        icdsc = ICONV_NULL;
/*      fprintf(stderr, "INFO: Closed iconv descriptor.\n"); */
      } /*if*/
  } /*subcp_close*/

#endif /*HAVE_ICONV*/

static int sub_getc()
  /* gets the next decoded UTF-8 byte from the input file, or EOF if the
    end of the file has been reached. */
  {
    int result;
    if (sub_next_out == sub_end_out)
      {
#ifdef HAVE_ICONV
        if (icdsc != ICONV_NULL)
          {
            if ((ic_next_in == ic_end_in || ic_needmore) && !ic_eof)
              {
              /* refill the input buffer */
                size_t bytesread;
                if (ic_next_in < ic_end_in)
                  {
                  /* move down remaining unprocessed data from last read */
                    ic_end_in -= ic_next_in;
                    memcpy(ic_inbuf, ic_inbuf + ic_next_in, ic_end_in);
                  }
                else
                  {
                    ic_end_in = 0;
                  } /*if*/
                bytesread = fread(ic_inbuf + ic_end_in, 1, sizeof ic_inbuf - ic_end_in, subfile.h);
                ic_end_in += bytesread;
                if (ic_end_in < sizeof ic_inbuf)
                  {
                    ic_eof = true;
                  } /*if*/
                ic_next_in = 0;
                ic_needmore = false;
              } /*if*/
            if (ic_next_in < ic_end_in)
              {
              /* refill sub_buf with more decoded characters */
                const char * nextin;
                char * nextout;
                size_t inleft, outleft, prev_sub_end_out;
                bool convok;
                nextin = ic_inbuf + ic_next_in;
                if (sub_buf_rewindable)
                  {
                    if (sub_out_size - sub_end_out < sub_buf_size)
                      {
                      /* make room for more */
                        sub_out_size += sub_buf_size;
                        sub_buf = realloc(sub_buf, sub_out_size);
                      } /*if*/
                    nextout = sub_buf + sub_end_out; /* keep what's in buffer */
                  }
                else
                  {
                    if (sub_out_size > sub_buf_size) /* don't need bigger buffer any more */
                      {
                        sub_out_size = sub_buf_size;
                        sub_buf = realloc(sub_buf, sub_out_size);
                      } /*if*/
                    nextout = sub_buf;
                  } /*if*/
                inleft = ic_end_in - ic_next_in; /* won't be zero */
                outleft = sub_out_size - (nextout - sub_buf);
                prev_sub_end_out = outleft;
                convok = iconv(icdsc, (char **)&nextin, &inleft, &nextout, &outleft) != (size_t)-1;
                if (!convok && errno == E2BIG && outleft < prev_sub_end_out)
                  { /* no room to completely convert input, but got something so that's OK */
                    convok = true;
                    errno = 0;
                  } /*if*/
                if (!convok)
                  {
                    if (!ic_eof && errno == EINVAL)
                      {
                        errno = 0;
                        ic_needmore = true;
                          /* can't decode what's left in ic_inbuf without reading more */
                      }
                    else /* E2BIG (shouldn't occur), EILSEQ, EINVAL at end of file */
                      {
                        fprintf
                          (
                            stderr,
                            "ERR:  Error %d -- %s -- decoding subtitle file at approx"
                                " line pos %d + char pos %d\n",
                            errno,
                            strerror(errno),
                            in_lineno, /* might be wrong */
                            in_charno + (int)(nextin - ic_inbuf) /* hopefully this is better */
                          );
                        exit(1);
                      } /*if*/
                  } /*if*/
                ic_next_in = nextin - ic_inbuf;
                prev_sub_end_out = sub_buf_rewindable ? sub_end_out : 0;
                sub_end_out = nextout - sub_buf;
                assert(sub_end_out != prev_sub_end_out);
                  /* because I gave it plenty of input data to work on */
                if (!sub_buf_rewindable)
                  {
                    sub_next_out = 0;
                  } /*if*/
              } /*if*/
          }
        else
#endif /*HAVE_ICONV*/
          {
            char * nextout;
            size_t bytesread;
            if (sub_buf_rewindable)
              {
                if (sub_out_size - sub_end_out < sub_buf_size)
                  {
                  /* make room for more */
                    sub_out_size += sub_buf_size;
                    sub_buf = realloc(sub_buf, sub_out_size);
                  } /*if*/
                nextout = sub_buf + sub_end_out; /* keep what's in buffer */
              }
            else
              {
                if (sub_out_size > sub_buf_size) /* don't need bigger buffer any more */
                  {
                    sub_out_size = sub_buf_size;
                    sub_buf = realloc(sub_buf, sub_out_size);
                  } /*if*/
                nextout = sub_buf;
              } /*if*/
            if (!sub_buf_rewindable)
              {
                sub_end_out = 0;
                sub_next_out = 0;
              } /*if*/
            bytesread = fread(nextout, 1, sub_out_size - sub_end_out, subfile.h);
            sub_end_out += bytesread;
          } /*if*/
      } /*if*/
    if (sub_next_out < sub_end_out)
      {
        result = sub_buf[sub_next_out];
        ++in_charno;
        ++sub_next_out;
        if (result == '\n')
          {
            ++in_lineno;
          } /*if*/
      }
    else
      {
        result = EOF;
      } /*if*/
    return result;
  } /*sub_getc*/

static void sub_rewind()
  /* rewinds to the beginning of the input subtitle file. */
  {
    if (!sub_buf_rewindable)
      {
        fprintf(stderr, "ERR:  trying to rewind subtitle file when not in rewindable state\n");
        exit(1);
      } /*if*/
    sub_next_out = 0;
    in_charno = 0;
    in_lineno = 1;
  } /*sub_rewind*/

static char * sub_fgets
  (
    char * dst,
    size_t dstsize /* must be at least 1, probably should be at least 2 */
  )
  /* reads a whole line from the input subtitle file into dst, returning dst if
    at least one character was read. */
  {
    const char * dstend = dst + dstsize - 1;
    char * dstnext = dst;
    bool warned_truncated = false;
    for (;;)
      {
        const int nextch = sub_getc();
        if (nextch == EOF)
          {
            if (dstnext == dst)
              {
                dst = NULL; /* indicate nothing read */
              } /*if*/
            break;
          } /*if*/
        if (dstnext < dstend)
          {
            *dstnext = nextch;
            ++dstnext;
          }
        else if (!warned_truncated)
          {
            fprintf(stderr, "WARN: input subtitle line too long on line %d\n", in_lineno);
            warned_truncated = true;
          /* and continue gobbling rest of line */
          } /*if*/
        if (nextch == '\n')
            break; /* end of line */
      } /*for*/
    if (dst != NULL)
      {
        *dstnext = 0; /* terminating null */
      } /*if*/
    return dst;
  } /*sub_fgets*/

#ifdef HAVE_FRIBIDI
#ifndef max
#define max(a,b)  (((a)>(b))?(a):(b))
#endif
subtitle_elt *sub_fribidi(subtitle_elt *sub)
  /* reorders character codes as necessary so right-to-left text will come out
    in the correct order when rendered left-to-right. */
  {
    FriBidiChar logical[LINE_LEN + 1], visual[LINE_LEN + 1]; // Hopefully these two won't smash the stack
    char *ip = NULL, *op = NULL;
    FriBidiParType base;
    size_t len, orig_len;
    int l = sub->lines;
    fribidi_boolean log2vis;
    while (l)
      {
        ip = sub->text[--l];
        orig_len = len = strlen(ip);
          // We assume that we don't use full unicode, only UTF-8
        if (len > LINE_LEN)
          {
            fprintf(stderr, "WARN: Sub->text is longer than LINE_LEN.\n");
            l++;
            break;
          } /*if*/
        len = fribidi_charset_to_unicode(FRIBIDI_CHAR_SET_UTF8, ip, len, logical);
          /* fixme: how do I know it will fit? */
        base = FRIBIDI_TYPE_ON; /* request order-neutral */
        log2vis = fribidi_log2vis
          (
            /*str =*/ logical, /* input logical string */
            /*len =*/ len, /* input logical string length */
            /*pbase_dir =*/ &base, /* requested and resolved paragraph base direction */
            /*visual_str =*/ visual, /* output visual string */
            /*positions_L_to_V =*/ NULL,
              /* output mapping from logical to visual string positions */
            /*positions_V_to_L =*/ NULL,
              /* output mapping from visual string back to the logical string positions */
            /*embedding_levels =*/ NULL /* output list of embedding levels */
          );
          /* result is maximum level found plus one, or zero if any error occurred */
          /* Note this function is deprecated because it only handles one-line paragraphs */
        if (log2vis)
          {
            len = fribidi_remove_bidi_marks
              (
                /*str =*/ visual,
                /*len =*/ len,
                /*positions_to_this =*/ NULL,
                /*positions_from_this_list =*/ NULL,
                /*embedding_levels =*/ NULL
              );
            op = (char*)malloc(sizeof(char) * (max(2 * orig_len, 2 * len) + 1));
            if (op == NULL)
              {
                fprintf(stderr, "ERR:  Error allocating mem.\n");
                l++;
                break;
              } /*if*/
            fribidi_unicode_to_charset(FRIBIDI_CHAR_SET_UTF8, visual, len, op);
              /* fixme: how do I know it will fit? */
            free(ip);
            sub->text[l] = op;
          } /*if*/
      } /*while*/
    if (l) /* unprocessed lines remaining */
      {
        for (l = sub->lines; l;)
          free(sub->text[--l]);
        return ERR;
      } /*if*/
    return sub;
  } /*sub_fribidi*/

#endif /*HAVE_FRIBIDI*/

/*
    Decoders for various subtitle formats
*/

subtitle_elt *sub_read_line_sami(subtitle_elt *current)
  {
  /* yuk--internal static state */
    static char line[LINE_LEN + 1];
    static const char *s = NULL;
  /* to get rid of above, simply use sub_getc and process input one character
    at a time, rather than a whole line at a time */
    char text[LINE_LEN + 1], *p = NULL;
    const char *q;
    int state;
    current->lines = current->start = current->end = 0;
    state = 0;
  /* read the first line */
    if (!s)
        if (!(s = sub_fgets(line, LINE_LEN)))
            return 0;
    do
      {
        switch (state)
          {
        case 0: /* find "START=" or "Slacktime:" */
          {
            const char * const slacktime_s = stristr(s, "Slacktime:");
            if (slacktime_s)
                sami_sub_slacktime = strtol(slacktime_s + 10, NULL, 0) / 10;
            s = stristr(s, "Start=");
            if (s)
              {
                current->start = strtol(s + 6, (char **)&s, 0) / 10;
                state = 1;
                continue;
              } /*if*/
          }
        break;

        case 1: /* find first "<P" */
            s = stristr(s, "<P");
            if (s != 0)
              {
                s += 2;
                state = 2;
                continue;
              } /*if*/
        break;

        case 2: /* find ">" */
            s = strchr (s, '>');
            if (s != 0)
              {
                s++;
                state = 3;
                p = text; /* start collecting text here */
                continue;
              } /*if*/
        break;

        case 3: /* get all text until '<' appears */
            if (*s == '\0')
                break;
            else if (!strncasecmp (s, "<br>", 4))
              {
                *p = '\0'; /* end of collected text */
                p = text; /* point to whole thing */
                trail_space(text); /* less whitespace */
                if (text[0] != '\0') /* what's left is nonempty */
                    current->text[current->lines++] = strdup(text);
                s += 4; /* skip "<br>" tag and stay in this state */
              }
          /* wot, no recognition of "<p>" here? */
            else if (*s == '<')
              {
                state = 4;
              }
            else if (!strncasecmp (s, "&nbsp;", 6))
              {
                *p++ = ' '; /* treat as space */
                s += 6;
              }
            else if (*s == '\t')
              {
                *p++ = ' '; /* treat as space */
                s++;
              }
            else if (*s == '\r' || *s == '\n')
              {
                s++; /* ignore line breaks */
              }
            else
              {
                *p++ = *s++; /* preserve everything else */
              } /*if*/
          /* skip duplicated space */
            if (p > text + 2 && p[-1] == ' ' && p[-2] == ' ')
                p--;
        continue;

        case 4: /* get current->end or skip rest of <TAG> */
            q = stristr(s, "Start=");
            if (q)
              {
                current->end = strtol(q + 6, (char **)&q, 0) / 10 - 1;
                  /* start time of new line is end time of previous line */
                *p = '\0';
                trail_space(text);
                if (text[0] != '\0')
                    current->text[current->lines++] = strdup(text);
                if (current->lines > 0) /* got one line, leave new one for next call */
                  {
                    state = 99;
                    break;
                  } /*if*/
                state = 0;
                continue;
              } /*if*/
            s = strchr (s, '>');
            if (s)
              {
                s++;
                state = 3; /* back to collecting text */
                continue;
              } /*if*/
        break;
          } /*switch*/
        /* read next line */
        if (state != 99 && !(s = sub_fgets(line, LINE_LEN)))
          {
            if (current->start > 0)
              {
                break; // if it is the last subtitle
              }
            else
              {
                return 0;
              } /*if*/
          } /*if*/
      }
    while (state != 99);
    // For the last subtitle
    if (current->end <= 0)
      {
        current->end = current->start + sami_sub_slacktime;
        *p = '\0';
        trail_space(text);
        if (text[0] != '\0')
            current->text[current->lines++] = strdup(text);
      } /*if*/
    return current;
  } /*sub_read_line_sami*/

static const char *sub_readtext(const char *source, char **dest)
  /* extracts the next text item in source, and returns a copy of it in *dest
    (could be the empty string). Returns a pointer into the unprocessed remainder
    of source, or NULL if there is nothing left. */
  {
    int len = 0;
    const char *p = source;
//  fprintf(stderr, "src=%p  dest=%p  \n", source, dest);
    while (!eol(*p) && *p!= '|')
      {
        p++, len++;
      } /*while*/
    *dest = (char *)malloc(len + 1);
    if (!dest)
      {
        return ERR;
      } /*if*/
    strncpy(*dest, source, len);
    (*dest)[len] = 0;
    while (*p == '\r' || *p == '\n' || *p == '|')
        p++;
    if (*p)
        return p;  // not-last text field
    else
        return NULL;  // last text field
  } /*sub_readtext*/

subtitle_elt *sub_read_line_microdvd(subtitle_elt *current)
  {
    char line[LINE_LEN + 1];
    char line2[LINE_LEN + 1];
    const char *p, *next;
    int i;
    do /* look for valid timing line */
      {
        if (!sub_fgets(line, LINE_LEN))
            return NULL;
     }
    while
      (
                sscanf(line, "{%ld}{}%[^\r\n]", &current->start, line2)
            <
                2

        &&
                 sscanf(line, "{%ld}{%ld}%[^\r\n]", &current->start, &current->end, line2)
            <
                 3
      );
    p = line2;
    next = p, i = 0;
    while ((next = sub_readtext(next, &current->text[i])) != 0)
      {
        if (current->text[i] == ERR)
          {
            return ERR;
          } /*if*/
        i++;
        if (i >= SUB_MAX_TEXT)
          {
            fprintf(stderr, "WARN: Too many lines in a subtitle\n");
            current->lines = i;
            return current;
          } /*if*/
      } /*while*/
    current->lines = ++i;
    return current;
  } /*sub_read_line_microdvd*/

subtitle_elt *sub_read_line_subrip(subtitle_elt *current)
  {
    char line[LINE_LEN + 1];
    int a1, a2, a3, a4, b1, b2, b3, b4;
    const char *p = NULL, *q = NULL;
    int len;
    while (true)
      {
        if (!sub_fgets(line, LINE_LEN))
            return NULL;
        if (sscanf(line, "%d:%d:%d.%d,%d:%d:%d.%d", &a1, &a2, &a3, &a4, &b1, &b2, &b3, &b4) < 8)
          /* start and end times in hours:minutes:seconds.hundredths */
            continue;
        current->start = a1 * 360000 + a2 * 6000 + a3 * 100 + a4;
        current->end = b1 * 360000 + b2 * 6000 + b3 * 100 + b4;
        if (!sub_fgets(line, LINE_LEN))
            return NULL;
        p = q = line;
        for (current->lines = 1; current->lines < SUB_MAX_TEXT; current->lines++)
          {
            for
              (
                q = p, len = 0;
                *p && *p != '\r' && *p != '\n' && *p != '|' && strncmp(p, "[br]", 4);
                p++, len++
              )
              /* include in current subtitle line until end of line or "|" or "[br]" markers */;
            current->text[current->lines - 1] = (char *)malloc(len + 1);
            if (!current->text[current->lines - 1])
                return ERR;
            strncpy(current->text[current->lines - 1], q, len);
            current->text[current->lines-1][len] = '\0';
            if (!*p || *p == '\r' || *p == '\n')
                break;
            if (*p == '|')
                p++;
            else
                while (*p++ != ']')
                  /* skip "[br]" marker */;
          } /*for*/
        break;
      } /*while*/
    return current;
  } /*sub_read_line_subrip*/

subtitle_elt *sub_read_line_subviewer(subtitle_elt *current)
  {
    char line[LINE_LEN + 1];
    int a1, a2, a3, a4, b1, b2, b3, b4;
    const char *p = NULL;
    int i, len;
    while (!current->text[0])
      {
        if (!sub_fgets(line, LINE_LEN))
            return NULL;
        if
          (
                (len = sscanf
                  (
                    line,
                    "%d:%d:%d%[,.:]%d --> %d:%d:%d%[,.:]%d",
                    &a1, &a2, &a3, (char *)&i, &a4,
                    &b1, &b2, &b3, (char *)&i, &b4
                  ))
            <
                10
          )
            continue;
        current->start = a1 * 360000 + a2 * 6000 + a3 * 100 + a4 / 10;
        current->end = b1 * 360000 + b2 * 6000 + b3 * 100 + b4 / 10;
        for (i = 0; i < SUB_MAX_TEXT;)
          {
            if (!sub_fgets(line, LINE_LEN))
                break;
            len = 0;
            for (p = line; *p != '\n' && *p != '\r' && *p; p++, len++)
              /* find end of line */;
            if (len) /* nonempty line */
              {
                int j = 0;
                bool skip = false;
                char *curptr = current->text[i] = (char *)malloc(len + 1);
                if (!current->text[i])
                    return ERR;
                //strncpy(current->text[i], line, len); current->text[i][len] = '\0';
                for(; j < len; j++)
                  {
                  /* let's filter html tags ::atmos */
                  /* fixme: if you're going to filter out "<" characters,
                    shouldn't you provide a way to escape them? For example,
                    using HTML-style "&"-escapes? */
                    if (line[j] == '>')
                      {
                        skip = false;
                        continue;
                      } /*if*/
                    if (line[j] == '<')
                      {
                        skip = true;
                        continue;
                      } /*if*/
                    if (skip)
                      {
                        continue;
                      } /*if*/
                    *curptr = line[j];
                    curptr++;
                  } /*for*/
                *curptr = '\0';
                i++;
              }
            else
              {
                break;
              } /*if*/
          } /*for*/
        current->lines = i;
      } /*while*/
    return current;
  } /*sub_read_line_subviewer*/

subtitle_elt *sub_read_line_subviewer2(subtitle_elt *current)
  {
    char line[LINE_LEN + 1];
    int a1, a2, a3, a4;
    const char *p = NULL;
    int i, len;
    while (!current->text[0])
      {
        if (!sub_fgets(line, LINE_LEN))
            return NULL;
        if (line[0] != '{')
            continue;
        if ((len = sscanf(line, "{T %d:%d:%d:%d", &a1, &a2, &a3, &a4)) < 4)
            continue;
        current->start = a1 * 360000 + a2 * 6000 + a3 * 100 + a4 / 10;
        for (i = 0; i < SUB_MAX_TEXT;)
          {
            if (!sub_fgets(line, LINE_LEN))
                break;
            if (line[0] == '}')
                break;
            len = 0;
            for (p = line; *p != '\n' && *p != '\r' && *p; ++p, ++len)
              /* find end of line */;
            if (len) /* nonempty line */
              {
                current->text[i] = (char *)malloc(len + 1);
                if (!current->text[i])
                    return ERR;
                strncpy(current->text[i], line, len);
                current->text[i][len] = '\0';
                ++i;
              }
            else
              {
                break;
              } /*if*/
          } /*for*/
        current->lines = i;
      } /*while*/
    return current;
  } /*sub_read_line_subviewer2*/

subtitle_elt *sub_read_line_vplayer(subtitle_elt *current)
  {
    char line[LINE_LEN + 1];
    int a1, a2, a3;
    const char *p = NULL, *next;
    char separator;
    int i, len, plen;
    while (!current->text[0])
      {
        if (!sub_fgets(line, LINE_LEN))
            return NULL;
        if ((len = sscanf(line, "%d:%d:%d%c%n", &a1, &a2, &a3, &separator, &plen)) < 4)
            continue;
        if (!(current->start = a1 * 360000 + a2 * 6000 + a3 * 100))
            continue;
#if 0 /*removed by wodzu*/
        p = line;
        // finds the body of the subtitle
        for (i = 0; i < 3; i++)
          {
           p = strchr(p, ':');
           if (p == NULL)
               break;
           ++p;
          } /*for*/
        if (p == NULL)
          {
            fprintf(stderr, "SUB: Skipping incorrect subtitle line!\n");
            continue;
          } /*if*/
#else
        // by wodzu: hey! this time we know what length it has! what is
        // that magic for? it can't deal with space instead of third
        // colon! look, what simple it can be:
        p = &line[plen];
#endif
        i = 0;
        if (*p != '|')
          {
            //
            next = p, i = 0;
            while ((next = sub_readtext(next, &current->text[i])) != 0)
              {
                if (current->text[i] == ERR)
                  {
                    return ERR;
                  } /*if*/
                i++;
                if (i >= SUB_MAX_TEXT)
                  {
                    fprintf(stderr, "WARN: Too many lines in a subtitle\n");
                    current->lines = i;
                    return current;
                  } /*if*/
              } /*while*/
            current->lines = i + 1;
          } /*if*/
      } /*while*/
    return current;
  } /*sub_read_line_vplayer*/

subtitle_elt *sub_read_line_rt(subtitle_elt *current)
  {
    //TODO: This format uses quite rich (sub/super)set of xhtml
    // I couldn't check it since DTD is not included.
    // WARNING: full XML parses can be required for proper parsing
    char line[LINE_LEN + 1];
    int a1, a2, a3, a4, b1, b2, b3, b4;
    const char *p = NULL, *next = NULL;
    int i, len, plen;
    while (!current->text[0])
      {
        if (!sub_fgets(line, LINE_LEN))
            return NULL;
        //TODO: it seems that format of time is not easily determined, it may be 1:12, 1:12.0 or 0:1:12.0
        //to describe the same moment in time. Maybe there are even more formats in use.
        //if ((len = sscanf(line, "<Time Begin=\"%d:%d:%d.%d\" End=\"%d:%d:%d.%d\"",&a1,&a2,&a3,&a4,&b1,&b2,&b3,&b4)) < 8)
        plen = a1 = a2 = a3 = a4 = b1 = b2 = b3 = b4 = 0;
        if
          (
                    (len = sscanf
                      (
                        line,
                        "<%*[tT]ime %*[bB]egin=\"%d.%d\" %*[Ee]nd=\"%d.%d\"%*[^<]<clear/>%n",
                        &a3, &a4, &b3, &b4, &plen
                     ))
                 <
                    4
            &&
                    (len = sscanf
                      (
                        line,
                        "<%*[tT]ime %*[bB]egin=\"%d.%d\" %*[Ee]nd=\"%d:%d.%d\"%*[^<]<clear/>%n",
                        &a3, &a4, &b2, &b3, &b4, &plen
                     ))
                <
                    5
            &&
                    (len = sscanf
                      (
                        line,
                        "<%*[tT]ime %*[bB]egin=\"%d:%d\" %*[Ee]nd=\"%d:%d\"%*[^<]<clear/>%n",
                        &a2, &a3, &b2, &b3, &plen
                     ))
                <
                    4
            &&
                    (len = sscanf
                      (
                        line,
                        "<%*[tT]ime %*[bB]egin=\"%d:%d\" %*[Ee]nd=\"%d:%d.%d\"%*[^<]<clear/>%n",
                        &a2, &a3, &b2, &b3, &b4, &plen
                    ))
                <
                    5
#if 0
            &&
                    (len = sscanf
                      (
                        line,
                        "<%*[tT]ime %*[bB]egin=\"%d:%d.%d\" %*[Ee]nd=\"%d:%d\"%*[^<]<clear/>%n",
                        &a2, &a3, &a4, &b2, &b3, &plen
                    ))
                <
                    5
#endif
            &&
                    (len = sscanf
                      (
                        line,
                        "<%*[tT]ime %*[bB]egin=\"%d:%d.%d\" %*[Ee]nd=\"%d:%d.%d\"%*[^<]<clear/>%n",
                        &a2, &a3, &a4, &b2, &b3, &b4, &plen
                    ))
                <
                    6
            &&
                    (len = sscanf
                      (
                        line,
                        "<%*[tT]ime %*[bB]egin=\"%d:%d:%d.%d\" %*[Ee]nd=\"%d:%d:%d.%d\"%*[^<]<clear/>%n",
                        &a1, &a2, &a3, &a4, &b1, &b2, &b3, &b4, &plen
                    ))
                <
                    8
            &&
            //now try it without end time
                    (len = sscanf
                      (
                        line,
                        "<%*[tT]ime %*[bB]egin=\"%d.%d\"%*[^<]<clear/>%n",
                        &a3, &a4, &plen
                    ))
                <
                    2
            &&
                    (len = sscanf
                      (
                        line,
                        "<%*[tT]ime %*[bB]egin=\"%d:%d\"%*[^<]<clear/>%n",
                        &a2, &a3, &plen
                    ))
                <
                    2
            &&
                    (len = sscanf
                      (
                        line,
                        "<%*[tT]ime %*[bB]egin=\"%d:%d.%d\"%*[^<]<clear/>%n",
                        &a2, &a3, &a4, &plen
                    ))
                <
                    3
            &&
                    (len = sscanf
                      (
                        line,
                        "<%*[tT]ime %*[bB]egin=\"%d:%d:%d.%d\"%*[^<]<clear/>%n",
                        &a1, &a2, &a3, &a4, &plen
                    ))
                <
                    4
          )
            continue; /* couldn't match any of the above */
        current->start = a1 * 360000 + a2 * 6000 + a3 * 100 + a4 / 10;
        current->end = b1 * 360000 + b2 * 6000 + b3 * 100 + b4 / 10;
        if (b1 == 0 && b2 == 0 && b3 == 0 && b4 == 0)
          current->end = current->start + 200;
        p = line;
        p += plen;
        i = 0;
        // TODO: I don't know what kind of convention is here for marking
        // multiline subs, maybe <br/> like in xml?
        next = strstr(line, "<clear/>");
        if (next && strlen(next) > 8)
          {
            next += 8; /* skip "<clear/>" tag */
            i = 0;
            while ((next = sub_readtext(next, &current->text[i])) != 0)
              {
                if (current->text[i] == ERR)
                  {
                    return ERR;
                  } /*if*/
                  i++;
                if (i >= SUB_MAX_TEXT)
                  {
                    fprintf(stderr, "WARN: Too many lines in a subtitle\n");
                    current->lines = i;
                    return current;
                  } /*if*/
              } /*while*/
          } /*if*/
        current->lines = i + 1;
      } /*while*/
    return current;
  } /*sub_read_line_rt*/

subtitle_elt *sub_read_line_ssa(subtitle_elt *current)
  {
/*
 * Sub Station Alpha v4 (and v2?) scripts have 9 commas before subtitle
 * other Sub Station Alpha scripts have only 8 commas before subtitle
 * Reading the "ScriptType:" field is not reliable since many scripts appear
 * w/o it
 *
 * http://www.scriptclub.org is a good place to find more examples
 * http://www.eswat.demon.co.uk is where the SSA specs can be found
 */
    int comma;
    static int max_comma = 32; /* let's use 32 for the case that the */
                /*  amount of commas increase with newer SSA versions */

    int hour1, min1, sec1, hunsec1,
        hour2, min2, sec2, hunsec2, nothing;
    int num;
    char line[LINE_LEN + 1], line3[LINE_LEN + 1];
    const char *line2;
    const char *tmp;
    do /* look for valid timing line */
      {
        if (!sub_fgets(line, LINE_LEN))
            return NULL;
      }
    while
      (
                sscanf
                  (
                    line,
                    "Dialogue: Marked=%d,%d:%d:%d.%d,%d:%d:%d.%d,%[^\n\r]",
                    &nothing,
                    &hour1, &min1, &sec1, &hunsec1,
                    &hour2, &min2, &sec2, &hunsec2,
                    line3
                  )
            <
                9
        &&
                sscanf
                  (
                    line, "Dialogue: %d,%d:%d:%d.%d,%d:%d:%d.%d,%[^\n\r]",
                    &nothing,
                    &hour1, &min1, &sec1, &hunsec1,
                    &hour2, &min2, &sec2, &hunsec2,
                    line3
                  )
            <
                9
      );
    line2 = strchr(line3, ',');
    for (comma = 4; comma < max_comma; comma ++)
      {
        tmp = line2;
        if (!(tmp = strchr(++tmp, ',')))
            break;
        if (*++tmp == ' ')
            break;
              /* a space after a comma means we're already in a sentence */
        line2 = tmp;
      } /*for*/
    if (comma < max_comma)
        max_comma = comma;
  /* eliminate the trailing comma */
    if (*line2 == ',')
        line2++;
    current->lines = 0;
    num = 0;
    current->start = 360000 * hour1 + 6000 * min1 + 100 * sec1 + hunsec1;
    current->end = 360000 * hour2 + 6000 * min2 + 100 * sec2 + hunsec2;
    while ((tmp = strstr(line2, "\\n")) != NULL || (tmp = strstr(line2, "\\N")) != NULL)
      {
        current->text[num] = (char *)malloc(tmp - line2 + 1);
        strncpy(current->text[num], line2, tmp - line2);
        current->text[num][tmp - line2] = '\0';
        line2 = tmp + 2;
        num++;
        current->lines++;
        if (current->lines >= SUB_MAX_TEXT)
            return current;
      } /*while*/
    current->text[num] = strdup(line2);
    current->lines++;
    return current;
  } /*sub_read_line_ssa*/

void sub_pp_ssa(subtitle_elt *sub)
  {
    int l = sub->lines;
    const char *so, *start;
    char *de;
    while (l)
      {
      /* eliminate any text enclosed with {}, they are font and color settings */
        so = de = sub->text[--l];
        while (*so)
          {
            if (*so == '{' && so[1] == '\\')
              {
                for (start = so; *so && *so != '}'; so++);
                if (*so)
                    so++;
                else
                    so = start;
              } /*if*/
            if (*so)
              {
                *de = *so;
                so++;
                de++;
              } /*if*/
          } /*while*/
        *de = *so;
      } /*while*/
  } /*sub_pp_ssa*/

subtitle_elt *sub_read_line_pjs(subtitle_elt *current)
  {
    char line[LINE_LEN + 1];
    char text[LINE_LEN + 1];
    if (!sub_fgets(line, LINE_LEN))
        return NULL;
    if (sscanf(line, "%ld,%ld,\"%[^\"]",  &current->start, &current->end, text) < 3)
        return ERR;
    current->text[0] = strdup(text);
    current->lines = 1;
    return current;
  } /*sub_read_line_pjs*/

subtitle_elt *sub_read_line_mpsub(subtitle_elt *current)
  {
    char line[LINE_LEN + 1];
    float startdelay, duration;
    int num = 0;
    char *p, *q;
    do /* look for valid timing line */
      {
        if (!sub_fgets(line, LINE_LEN))
            return NULL;
      }
    while (sscanf(line, "%f %f", &startdelay, &duration) != 2);
    mpsub_position += startdelay * mpsub_multiplier;
    current->start = (int)mpsub_position;
    mpsub_position += duration * mpsub_multiplier;
    current->end = (int)mpsub_position;
    while (num < SUB_MAX_TEXT)
      {
        if (!sub_fgets(line, LINE_LEN))
          {
            if (num == 0)
                return NULL;
            else
                return current;
          } /*if*/
        p = line;
        while (isspace(*p))
            p++;
        if (eol(*p) && num > 0)
            return current;
        if (eol(*p))
            return NULL;
        for (q = p; !eol(*q); q++)
          /* look for end of line */;
        *q = '\0';
        if (strlen(p)) /* nonempty line */
          {
            current->text[num] = strdup(p);
//          fprintf(stderr, ">%s<\n", p);
            current->lines = ++num;
          }
        else
          {
            if (num)
                return current;
            else
                return NULL;
          } /*if*/
      } /*while*/
    return NULL; // we should have returned before if it's OK
  } /*sub_read_line_mpsub*/

#ifndef USE_SORTSUB
//we don't need this if we use previous_sub_end
static subtitle_elt *previous_aqt_sub = NULL;
#endif

subtitle_elt *sub_read_line_aqt(subtitle_elt *current)
  {
    char line[LINE_LEN + 1];
    const char *next;
    int i;
    while (true)
      {
      // try to locate next subtitle
        if (!sub_fgets(line, LINE_LEN))
            return NULL;
        if (sscanf(line, "-->> %ld", &current->start) == 1)
            break;
      } /*while*/
#ifdef USE_SORTSUB
    previous_sub_end = current->start ? current->start - 1 : 0;
#else
    if (previous_aqt_sub != NULL)
        previous_aqt_sub->end = current->start - 1;
    previous_aqt_sub = current;
#endif
    if (!sub_fgets(line, LINE_LEN))
        return NULL;
    (void)sub_readtext(line, &current->text[0]);
    current->lines = 1;
    current->end = current->start; // will be corrected by next subtitle
    if (!sub_fgets(line, LINE_LEN))
        return current;
    next = line, i = 1;
    while ((next = sub_readtext (next, &current->text[i])) != 0)
      {
        if (current->text[i] == ERR)
          {
            return ERR;
          } /*if*/
        i++;
        if (i >= SUB_MAX_TEXT)
          {
            fprintf(stderr, "WARN: Too many lines in a subtitle\n");
            current->lines = i;
            return current;
          } /*if*/
      } /*while*/
    current->lines = i + 1;
    if (current->text[0][0] == 0 && current->text[1][0] == 0)
      {
#ifdef USE_SORTSUB
        previous_sub_end = 0;
#else
    // void subtitle -> end of previous marked and exit
        previous_aqt_sub = NULL;
#endif
        return NULL;
      } /*if*/
    return current;
  } /*sub_read_line_aqt*/

#ifndef USE_SORTSUB
static subtitle_elt *previous_subrip09_sub = NULL;
#endif

subtitle_elt *sub_read_line_subrip09(subtitle_elt *current)
  {
    char line[LINE_LEN + 1];
    int a1, a2, a3;
    const char * next = NULL;
    int i, len;
    while (true)
      {
      // try to locate next subtitle
        if (!sub_fgets(line, LINE_LEN))
            return NULL;
        len = sscanf(line, "[%d:%d:%d]", &a1, &a2, &a3);
        if (len == 3)
            break;
      } /*while*/
    current->start = a1 * 360000 + a2 * 6000 + a3 * 100;
#ifdef USE_SORTSUB
    previous_sub_end = current->start ? current->start - 1 : 0;
#else
    if (previous_subrip09_sub != NULL)
        previous_subrip09_sub->end = current->start - 1;
    previous_subrip09_sub = current;
#endif
    if (!sub_fgets(line, LINE_LEN))
        return NULL;
    next = line, i = 0;
    while ((next = sub_readtext(next, &current->text[i])) != 0)
      {
        if (current->text[i] == ERR)
          {
            return ERR;
           } /*if*/
        i++;
        if (i >= SUB_MAX_TEXT)
          {
            fprintf(stderr, "WARN: Too many lines in a subtitle\n");
            current->lines = i;
            return current;
          } /*if*/
      } /*while*/
    current->lines = i + 1;
    if (current->text[0][0] == 0 && i == 0)
      {
#ifdef USE_SORTSUB
        previous_sub_end = 0;
#else
        // void subtitle -> end of previous marked and exit
        previous_subrip09_sub = NULL;
#endif
        return NULL;
      } /*if*/
    return current;
  } /*sub_read_line_subrip09*/

subtitle_elt *sub_read_line_jacosub(subtitle_elt * current)
  {
    char line1[LINE_LEN], line2[LINE_LEN], directive[LINE_LEN];
    unsigned a1, a2, a3, a4, b1, b2, b3, b4, comment = 0;
    static unsigned jacoTimeres = 30;
    static int jacoShift = 0;
    const char *p;
    char *q;

    bzero(current, sizeof(subtitle_elt));
    bzero(line1, LINE_LEN);
    bzero(line2, LINE_LEN);
    bzero(directive, LINE_LEN);
    while (!current->text[0])
      {
        if (!sub_fgets(line1, LINE_LEN))
          {
            return NULL;
          } /*if*/
        if
          (
                sscanf
                  (
                    line1,
                    "%u:%u:%u.%u %u:%u:%u.%u %[^\n\r]",
                    &a1, &a2, &a3, &a4, &b1, &b2, &b3, &b4, line2
                  )
            <
                9
          )
          {
            if (sscanf(line1, "@%u @%u %[^\n\r]", &a4, &b4, line2) < 3)
              {
                if (line1[0] == '#')
                  {
                    int hours = 0, minutes = 0, seconds, delta, inverter = 1;
                    unsigned units = jacoShift;
                    switch (toupper(line1[1]))
                      {
                    case 'S':
                        if (isalpha(line1[2]))
                          {
                            delta = 6;
                          }
                        else
                          {
                            delta = 2;
                          } /*if*/
                        if (sscanf(&line1[delta], "%d", &hours))
                          {
                            if (hours < 0)
                              {
                                hours *= -1;
                                inverter = -1;
                              } /*if*/
                            if (sscanf(&line1[delta], "%*d:%d", &minutes))
                              {
                                if
                                  (
                                    sscanf
                                      (
                                        &line1[delta], "%*d:%*d:%d",
                                        &seconds
                                      )
                                  )
                                  {
                                    sscanf(&line1[delta], "%*d:%*d:%*d.%d", &units);
                                  }
                                else
                                  {
                                    hours = 0;
                                    sscanf(&line1[delta], "%d:%d.%d", &minutes, &seconds, &units);
                                    minutes *= inverter;
                                  } /*if*/
                              }
                            else
                              {
                                hours = minutes = 0;
                                sscanf(&line1[delta], "%d.%d", &seconds, &units);
                                seconds *= inverter;
                              } /*if*/
                            jacoShift =
                                    (
                                        (hours * 3600 + minutes * 60 + seconds) * jacoTimeres
                                    +
                                        units
                                    )
                                *
                                    inverter;
                          } /*if*/
                    break;
                    case 'T':
                        if (isalpha(line1[2]))
                          {
                            delta = 8;
                          }
                        else
                          {
                            delta = 2;
                          } /*if*/
                        sscanf(&line1[delta], "%u", &jacoTimeres);
                    break;
                      } /*switch*/
                  } /*if*/
                continue;
              }
            else /* timing line */
              {
                current->start =
                    (unsigned long)((a4 + jacoShift) * 100.0 / jacoTimeres);
                current->end =
                    (unsigned long)((b4 + jacoShift) * 100.0 / jacoTimeres);
              } /*if*/
          }
        else /* timing line */
          {
            current->start =
                (unsigned long)
                    (
                        ((a1 * 3600 + a2 * 60 + a3) * jacoTimeres + a4 + jacoShift)
                    *
                        100.0
                    /
                        jacoTimeres
                    );
            current->end =
                (unsigned long)
                    (
                        ((b1 * 3600 + b2 * 60 + b3) * jacoTimeres + b4 + jacoShift)
                    *
                        100.0
                    /
                        jacoTimeres
                    );
          } /*if*/
        current->lines = 0;
        p = line2;
        while (*p == ' ' || *p == '\t')
          {
            ++p; /* ignore leading whitespace */
          } /*while*/
        if (isalpha(*p) || *p == '[')
          {
            int cont, jLength;
            if (sscanf(p, "%s %[^\n\r]", directive, line1) < 2)
                return (subtitle_elt *)ERR;
            jLength = strlen(directive);
            for (cont = 0; cont < jLength; ++cont)
              {
                if (isalpha(directive[cont]))
                    directive[cont] = toupper(directive[cont]);
              } /*for*/
            if
              (
                    strstr(directive, "RDB") != NULL
                ||
                    strstr(directive, "RDC") != NULL
                ||
                    strstr(directive, "RLB") != NULL
                ||
                    strstr(directive, "RLG") != NULL
              )
              {
                continue;
              } /*if*/
            if (strstr(directive, "JL") != NULL)
              {
                current->alignment = H_SUB_ALIGNMENT_LEFT;
              }
            else if (strstr(directive, "JR") != NULL)
              {
                current->alignment = H_SUB_ALIGNMENT_RIGHT;
              }
            else
              {
                current->alignment = H_SUB_ALIGNMENT_CENTER;
              } /*if*/
            strcpy(line2, line1);
            p = line2;
          } /*if*/
        for (q = line1; !eol(*p) && current->lines < SUB_MAX_TEXT; ++p)
          { /* collect text from p into q */
            switch (*p)
              {
            case '{':
                comment++;
            break;
            case '}':
                if (comment)
                  {
                    --comment;
                    //the next line to get rid of a blank after the comment
                    if (p[1] == ' ')
                        p++;
                  } /*if*/
            break;
            case '~':
                if (!comment)
                  {
                    *q = ' ';
                    ++q;
                  } /*if*/
            break;
            case ' ':
            case '\t':
                if (p[1] == ' ' || p[1] == '\t') /* ignore duplicated space/tab */
                    break;
                if (!comment)
                  {
                    *q = ' '; /* whitespace => single space */
                    ++q;
                  } /*if*/
            break;
            case '\\':
                if (p[1] == 'n') /* non-literal line break */
                  {
                    *q = '\0';
                    q = line1;
                    current->text[current->lines++] = strdup(line1);
                    ++p;
                    break;
                  } /*if*/
                if (toupper(p[1]) == 'C' || toupper(p[1]) == 'F')
                  {
                    ++p, ++p; /* ignore following character as well */
                    break;
                  } /*if*/
                if
                  (
                        p[1] == 'B'
                    ||
                        p[1] == 'b'
                    ||
                        p[1] == 'D'
                    || //actually this means "insert current date here"
                        p[1] == 'I'
                    ||
                        p[1] == 'i'
                    ||
                        p[1] == 'N'
                    ||
                        p[1] == 'T'
                    || //actually this means "insert current time here"
                        p[1] == 'U'
                    ||
                        p[1] == 'u'
                  )
                  {
                    ++p; /* ignore */
                    break;
                  } /*if*/
                if
                  (
                        p[1] == '\\'
                    ||
                        p[1] == '~'
                    ||
                        p[1] == '{'
                  )
                  {
                    ++p; /* fallthrough to insert char following "\" literally */
                  }
                else if (eol(p[1]))
                  {
                    if (!sub_fgets(directive, LINE_LEN))
                        return NULL;
                    trail_space(directive);
                    strconcat(line2, LINE_LEN, directive);
                    break;
                  } /*if*/
          /* fallthrough */
            default: /* copy character *p literally */
                if (!comment)
                  {
                    *q = *p;
                    ++q;
                  } /*if*/
              } /*switch*/
          } /*for*/
        *q = '\0';
        current->text[current->lines] = strdup(line1);
      } /*while*/
    current->lines++;
    return current;
  } /*sub_read_line_jacosub*/

static int sub_autodetect(bool * uses_time)
  /* scans the first few lines of the file to try to determine what format it is. */
  {
    char line[LINE_LEN + 1];
    int i, j = 0;
    char p;
    while (j < 100)
      {
        j++;
        if (!sub_fgets(line, LINE_LEN))
            return SUB_INVALID;
        if (sscanf(line, "{%d}{%d}", &i, &i) == 2)
          {
            *uses_time = false;
            return SUB_MICRODVD;
          } /*if*/
        if (sscanf(line, "{%d}{}", &i) == 1)
          {
            *uses_time = false;
            return SUB_MICRODVD;
          } /*if*/
        if (sscanf(line, "%d:%d:%d.%d,%d:%d:%d.%d", &i, &i, &i, &i, &i, &i, &i, &i) == 8)
          {
            *uses_time = true;
            return SUB_SUBRIP;
          } /*if*/
        if
          (
                sscanf
                  (
                    line,
                    "%d:%d:%d%[,.:]%d --> %d:%d:%d%[,.:]%d",
                    &i, &i, &i, (char *)&i, &i, &i, &i, &i, (char *)&i, &i
                  )
            ==
                10
          )
          {
            *uses_time = true;
            return SUB_SUBVIEWER;
          } /*if*/
        if (sscanf(line, "{T %d:%d:%d:%d", &i, &i, &i, &i))
          {
            *uses_time = true;
            return SUB_SUBVIEWER2;
          } /*if*/
        if (strstr(line, "<SAMI>"))
          {
            *uses_time = true;
            return SUB_SAMI;
          } /*if*/
        if (sscanf(line, "%d:%d:%d.%d %d:%d:%d.%d", &i, &i, &i, &i, &i, &i, &i, &i) == 8)
          {
            *uses_time = true;
            return SUB_JACOSUB;
          } /*if*/
        if (sscanf(line, "@%d @%d", &i, &i) == 2)
          {
            *uses_time = true;
            return SUB_JACOSUB;
          } /*if*/
        if (sscanf(line, "%d:%d:%d:", &i, &i, &i ) == 3)
          {
            *uses_time = true;
            return SUB_VPLAYER;
          } /*if*/
        if (sscanf(line, "%d:%d:%d ", &i, &i, &i ) == 3)
          {
            *uses_time = true;
            return SUB_VPLAYER;
          } /*if*/
        //TODO: just checking if first line of sub starts with "<" is WAY
        // too weak test for RT
        // Please someone who knows the format of RT... FIX IT!!!
        // It may conflict with other sub formats in the future (actually it doesn't)
        if (*line == '<')
          {
            *uses_time = true;
            return SUB_RT;
          } /*if*/
        if (!memcmp(line, "Dialogue: Marked", 16))
          {
            *uses_time = true;
            return SUB_SSA;
          } /*if*/
        if (!memcmp(line, "Dialogue: ", 10))
          {
            *uses_time = true;
            return SUB_SSA;
          } /*if*/
        if (sscanf(line, "%d,%d,\"%c", &i, &i, (char *)&i) == 3)
          {
            *uses_time = false;
            return SUB_PJS;
          } /*if*/
        if (sscanf(line, "FORMAT=%d", &i) == 1)
          {
            *uses_time = false; /* actually means that durations are in seconds */
            return SUB_MPSUB;
          } /*if*/
        if (sscanf(line, "FORMAT=TIM%c", &p) == 1 && p == 'E')
          {
            *uses_time = true; /* actually means that durations are in hundredths of a second */
            return SUB_MPSUB;
          } /*if*/
        if (strstr(line, "-->>"))
          {
            *uses_time = false;
            return SUB_AQTITLE;
          } /*if*/
        if (sscanf(line, "[%d:%d:%d]", &i, &i, &i) == 3)
          {
            *uses_time = true;
            return SUB_SUBRIP09;
          } /*if*/
      } /*while*/
    return SUB_INVALID;  // too many bad lines
  } /*sub_autodetect*/

/*
    Common subtitle-handling code
*/

static void adjust_subs_time
  (
    subtitle_elt * sub, /* array of subtitle_elts */
    float subtime, /* duration to truncate overlapping subtitle to--why? */
    float fps,
    int block, /* whether to check for overlapping subtitles (false if caller will fix them up) */
    int sub_num, /* nr entries in sub array */
    bool sub_uses_time
  )
  /* adjusts for overlapping subtitle durations, and also for sub_fps if specified. */
  {
    int nradjusted, adjusted;
    subtitle_elt * nextsub;
    int i = sub_num;
    unsigned long const subfms = (sub_uses_time ? 100 : fps) * subtime;
      /* subtime converted to subtitle duration units */
    unsigned long const short_overlap = (sub_uses_time ? 100 : fps) / 5; // 0.2s
    nradjusted = 0;
    if (i)
        for (;;)
          {
            adjusted = 0; /* to begin with */
            if (sub->end <= sub->start)
              {
                sub->end = sub->start + subfms;
                adjusted++;
                nradjusted++;
              } /*if*/
            if (!--i)
                break; /* no nextsub */
            nextsub = sub + 1;
            if (block)
              {
                if (sub->end > nextsub->start && sub->end <= nextsub->start + short_overlap)
                  {
                    // these subtitles overlap for less than 0.2 seconds
                    // and would result in very short overlapping subtitle
                    // so let's fix the problem here, before overlapping code
                    // get its hands on them
                    const unsigned delta = sub->end - nextsub->start, half = delta / 2;
                  /* remove the overlap by splitting the difference */
                    sub->end -= half + 1;
                    nextsub->start += delta - half;
                  } /*if*/
                if (sub->end >= nextsub->start)
                  {
                  /* either exact abut, or overlap by more than short_overlap */
                    sub->end = nextsub->start - 1;
                    if (sub->end - sub->start > subfms)
                        sub->end = sub->start + subfms;
                          /* maximum subtitle duration -- why? */
                    if (!adjusted)
                        nradjusted++;
                  } /*if*/
              } /*if*/
            /* Theory:
             * Movies are often converted from FILM (24 fps)
             * to PAL (25) by simply speeding it up, so we
             * to multiply the original timestmaps by
             * (Movie's FPS / Subtitle's (guessed) FPS)
             * so eg. for 23.98 fps movie and PAL time based
             * subtitles we say -subfps 25 and we're fine!
             */
            /* timed sub fps correction ::atmos */
            if (sub_uses_time && sub_fps)
              {
                sub->start *= sub_fps / fps;
                sub->end *= sub_fps / fps;
              } /*if*/
            sub = nextsub;
          } /*for; if*/
    if (nradjusted != 0)
        fprintf(stderr, "INFO: Adjusted %d subtitle(s).\n", nradjusted);
  } /*adjust_subs_time*/

struct subreader { /* describes a subtitle format */
    subtitle_elt * (*read)(subtitle_elt *dest); /* file reader routine */
    void       (*post)(subtitle_elt *dest); /* optional post-processor routine */
    const char *name; /* descriptive name */
};

sub_data *sub_read_file(const char *filename, float movie_fps)
  /* parses the contents of filename, auto-recognizing the subtitle file format,
    and returns the result. The subtitles will be translated from the subtitle_charset
    character set to Unicode if specified, unless the file name ends in ".utf",
    ".utf8" or ".utf-8" (case-insensitive), in which case they will be assumed
    to already be in UTF-8. movie_fps is the movie frame rate, needed for subtitle
    formats which specify fractional-second durations in frames. */
  {
    //filename is assumed to be malloc'ed, free() is used in sub_free()
    int sub_format;
    int n_max;
    subtitle_elt *first, *second, *new_sub, *return_sub;
#ifdef USE_SORTSUB
    subtitle_elt temp_sub;
#endif
    sub_data *subt_data;
    bool uses_time = false;
    int sub_num = 0, sub_errs = 0;
    struct subreader const sr[] =
      /* all the subtitle formats I know about, indexed by the codes defined in subreader.h */
      {
        {sub_read_line_microdvd, NULL, "microdvd"},
        {sub_read_line_subrip, NULL, "subrip"},
        {sub_read_line_subviewer, NULL, "subviewer"},
        {sub_read_line_sami, NULL, "sami"},
        {sub_read_line_vplayer, NULL, "vplayer"},
        {sub_read_line_rt, NULL, "rt"},
        {sub_read_line_ssa, sub_pp_ssa, "ssa"},
        {sub_read_line_pjs, NULL, "pjs"},
        {sub_read_line_mpsub, NULL, "mpsub"},
        {sub_read_line_aqt, NULL, "aqt"},
        {sub_read_line_subviewer2, NULL, "subviewer 2.0"},
        {sub_read_line_subrip09, NULL, "subrip 0.9"},
        {sub_read_line_jacosub, NULL, "jacosub"},
      };
    const struct subreader *srp; /* the autodetected format */

    if (filename == NULL)
        return NULL; //qnx segfault
    sub_open(filename);
#ifdef HAVE_ICONV
      {
        int l, k;
        k = -1;
        l = strlen(filename);
        if (l > 4) /* long enough to have an extension */
          {
            const char * const exts[] = {".utf", ".utf8", ".utf-8"};
              /* if filename ends with one of these extensions, then its contents
                are assumed to be in UTF-8 encoding, and subtitle_charset is ignored */
            for (k = 3; --k >= 0;)
                if (l > strlen(exts[k]) && !strcasecmp(filename + (l - strlen(exts[k])), exts[k]))
                  {
                    break;
                  } /*if; for*/
          } /*if*/
        if (k < 0) /* assume it's not UTF-8 */
            subcp_open(); /* to convert the text to UTF-8 */
      }
#endif
    sub_buf_rewindable = true;
    sub_format = sub_autodetect(&uses_time);
    mpsub_multiplier = (uses_time ? 100.0 : 1.0);
    if (sub_format == SUB_INVALID)
      {
        fprintf(stderr, "ERR:  Could not determine format of subtitle file \"%s\"\n", filename);
        exit(1);
      } /*if*/
    srp = sr + sub_format;
    fprintf(stderr, "INFO: Detected subtitle file format: %s\n", srp->name);
    sub_rewind();
    sub_buf_rewindable = false;
    sub_num = 0;
    n_max = 32; /* initial size of "first" array */
    first = (subtitle_elt *)malloc(n_max * sizeof(subtitle_elt));
  /* fixme: don't bother recovering from any of the following allocation etc failures, just die */
    if(!first)
      {
#ifdef HAVE_ICONV
        subcp_close();
#endif
        return NULL;
      } /*if*/
#ifdef USE_SORTSUB
    //This is to deal with those formats (AQT & Subrip) which define the end of a subtitle
    //as the beginning of the following
    previous_sub_end = 0;
#endif
    while (true)
      {
      /* read subtitle entries from input file */
        if (sub_num == n_max) /* need more room in "first" array */
          {
            n_max += 16;
            first = realloc(first, n_max * sizeof(subtitle_elt));
          } /*if*/
#ifdef USE_SORTSUB
        new_sub = &temp_sub;
          /* temporary holding area before copying entry into right place in first array */
#else
        new_sub = &first[sub_num];
          /* just put it directly on the end */
#endif
        memset(new_sub, '\0', sizeof(subtitle_elt));
        new_sub = srp->read(new_sub);
        if (!new_sub)
            break;   // EOF
        if (h_sub_alignment != H_SUB_ALIGNMENT_DEFAULT)
          {
            new_sub->alignment = h_sub_alignment; /* override settings from subtitle file */
          } /*if*/
#ifdef HAVE_FRIBIDI
        if (new_sub != ERR)
            new_sub = sub_fribidi(new_sub);
#endif
        if (new_sub == ERR)
          {
#ifdef HAVE_ICONV
            subcp_close();
#endif
            if (first)
                free(first);
            return NULL;
           } /*if*/
        // Apply any post processing that needs recoding first
        if (new_sub != ERR && !sub_no_text_pp && srp->post)
            srp->post(new_sub);
#ifdef USE_SORTSUB
      /* fixme: this will all crash if new_sub == ERR */
        if (!sub_num || first[sub_num - 1].start <= new_sub->start)
          {
          /* append contents of new_sub to first */
            int i;
            first[sub_num].start = new_sub->start;
            first[sub_num].end = new_sub->end;
            first[sub_num].lines = new_sub->lines;
            first[sub_num].alignment = new_sub->alignment;
            for (i = 0; i < new_sub->lines; ++i)
              {
                first[sub_num].text[i] = new_sub->text[i];
              }/*for*/
            if (previous_sub_end)
              {
                first[sub_num - 1].end = previous_sub_end;
                previous_sub_end = 0;
              } /*if*/
          }
        else
          {
          /* insert new_sub into first to keep it sorted by start time */
            int i, j;
            for (j = sub_num - 1; j >= 0; --j)
              {
                first[j + 1].start = first[j].start;
                first[j + 1].end = first[j].end;
                first[j + 1].lines = first[j].lines;
                first[j + 1].alignment = first[j].alignment;
                for (i = 0; i < first[j].lines; ++i)
                  {
                    first[j + 1].text[i] = first[j].text[i];
                  } /*for*/
                if (!j || first[j - 1].start <= new_sub->start)
                  {
                    first[j].start = new_sub->start;
                    first[j].end = new_sub->end;
                    first[j].lines = new_sub->lines;
                    first[j].alignment = new_sub->alignment;
                    for (i = 0; i < SUB_MAX_TEXT; ++i)
                      {
                        first[j].text[i] = new_sub->text[i];
                      } /*for*/
                    if (previous_sub_end)
                      {
                        first[j].end = first[j - 1].end;
                        first[j - 1].end = previous_sub_end;
                        previous_sub_end = 0;
                      } /*if*/
                    break;
                  } /*if*/
              } /*for*/
          } /*if*/
#endif
        if (new_sub == ERR)
            ++sub_errs;
        else
            ++sub_num; // Error vs. Valid
      } /*while*/
#ifdef HAVE_ICONV
    subcp_close();
#endif
    sub_close();
//  fprintf(stderr, "SUB: Subtitle format %s time.\n", uses_time ? "uses" : "doesn't use");
    fprintf(stderr, "INFO: Read %i subtitles\n", sub_num);
    if (sub_errs)
        fprintf(stderr, "INFO: %i bad line(s).\n", sub_errs);
    if (sub_num <= 0)
      {
        free(first);
        return NULL;
      } /*if*/
    // we do overlap if the user forced it (suboverlap_enable == 2) or
    // the user didn't forced no-overlapsub and the format is Jacosub or Ssa.
    // this is because usually overlapping subtitles are found in these formats,
    // while in others they are probably result of bad timing
    if
      (
            suboverlap_enabled == 2
        ||
                suboverlap_enabled
            &&
                (sub_format == SUB_JACOSUB || sub_format == SUB_SSA)
      )
      {
      // here we manage overlapping subtitles
        int n_first, sub_first, sub_orig, i, j;
        adjust_subs_time(first, 6.0, movie_fps, 0, sub_num, uses_time); /*~6 secs AST*/
        sub_orig = sub_num;
        n_first = sub_num;
        sub_num = 0;
        second = NULL;
        // for each subtitle in first[] we deal with its 'block' of
        // bonded subtitles
        for (sub_first = 0; sub_first < n_first; ++sub_first)
          {
            unsigned long
                global_start = first[sub_first].start,
                global_end = first[sub_first].end,
                local_start,
                local_end;
            int
                lines_to_add = first[sub_first].lines, /* total nr lines in block */
                sub_to_add = 0,
                **placeholder = NULL,
                highest_line = 0,
                nr_placeholder_entries,
                subs_done;
            const int
                start_block_sub = sub_num;
            bool real_block = true;
            // here we find the number of subtitles inside the 'block'
            // and its span interval. this works well only with sorted
            // subtitles
            while
              (
                    sub_first + sub_to_add + 1 < n_first
                &&
                    first[sub_first + sub_to_add + 1].start < global_end
              )
              {
              /* another subtitle overlapping sub_first--include in block */
                ++sub_to_add;
                lines_to_add += first[sub_first + sub_to_add].lines;
              /* extend block duration to include this subtitle: */
                if (first[sub_first + sub_to_add].start < global_start)
                  {
                    global_start = first[sub_first + sub_to_add].start;
                  } /*if*/
                if (first[sub_first + sub_to_add].end > global_end)
                  {
                    global_end = first[sub_first + sub_to_add].end;
                  } /*if*/
              } /*while*/
            // we need a structure to keep trace of the screen lines
            // used by the subs, a 'placeholder'
            nr_placeholder_entries = 2 * sub_to_add + 1;
              // the maximum number of subs derived from a block of sub_to_add + 1 subs
              /* fixme: but only two entries are ever accessed in the following loop:
                the previous one and the current one */
            placeholder = (int **)malloc(sizeof(int *) * nr_placeholder_entries);
            for (i = 0; i < nr_placeholder_entries; ++i)
              {
                placeholder[i] = (int *)malloc(sizeof(int) * lines_to_add);
                for (j = 0; j < lines_to_add; ++j)
                  {
                    placeholder[i][j] = -1;
                  } /*for*/
              } /*for*/
            subs_done = 0;
            local_end = global_start - 1;
            do
              {
                // here we find the beginning and the end of a new
                // subtitle in the block
                local_start = local_end + 1; /* start after previous duration done */
                local_end = global_end;
                for (j = 0; j <= sub_to_add; ++j)
                  {
                    if
                      (
                            first[sub_first + j].start - 1 > local_start
                        &&
                            first[sub_first + j].start - 1 < local_end
                      )
                      {
                        local_end = first[sub_first + j].start - 1;
                          /* local_end becomes earliest start if after local_start? */
                      }
                    else if
                      (
                            first[sub_first + j].end > local_start
                        &&
                            first[sub_first + j].end < local_end
                      )
                      {
                        local_end = first[sub_first + j].end;
                          /* local_end becomes earliest end if after local_start? */
                      } /*if*/
                  } /*for*/
                // here we allocate the screen lines to subs we must
                // display in current local_start-local_end interval.
                // if the subs were yet presents in the previous interval
                // they keep the same lines, otherside they get unused lines
                for (j = 0; j <= sub_to_add; ++j)
                  {
                    if
                      (
                            first[sub_first + j].start <= local_end
                        &&
                            first[sub_first + j].end > local_start
                      )
                      {
                      /* this one overlaps (local_start .. local_end] */
                        const unsigned long sub_lines = first[sub_first + j].lines;
                        unsigned long
                            fragment_length = lines_to_add + 1,
                            blank_lines_avail = 0;
                        bool wasinprev = false;
                        int fragment_position = -1;
                        // if this is not the first new sub of the block
                        // we find if this sub was present in the previous
                        // new sub
                        if (subs_done)
                            for (i = 0; i < lines_to_add; ++i)
                              {
                                if (placeholder[subs_done - 1][i] == sub_first + j)
                                  {
                                    placeholder[subs_done][i] = sub_first + j;
                                    wasinprev = true;
                                  } /*if*/
                              } /*for; if*/
                        if (wasinprev)
                            continue; /* already processed this subtitle */
                        // we are looking for the shortest among all groups of
                        // sequential blank lines whose length is greater than or
                        // equal to sub_lines. we store in fragment_position the
                        // position of the shortest group, in fragment_length its
                        // length, and in blank_lines_avail the length of the group currently
                        // examinated
                        for (i = 0; i < lines_to_add; ++i)
                          {
                            if (placeholder[subs_done][i] == -1)
                              {
                                // placeholder[subs_done][i] is part of the current group
                                // of blank lines
                                ++blank_lines_avail;
                              }
                            else /* end of a run of empty lines */
                              {
                                if (blank_lines_avail == sub_lines)
                                  {
                                    // current group's size fits exactly the one we
                                    // need, so we stop looking
                                    fragment_position = i - blank_lines_avail;
                                      /* starting line position */
                                    blank_lines_avail = 0;
                                    break;
                                  } /*if*/
                                if
                                  (
                                        blank_lines_avail
                                    &&
                                        blank_lines_avail > sub_lines
                                    &&
                                        blank_lines_avail < fragment_length
                                  )
                                  {
                                    // current group is the best we found till here,
                                    // but is still bigger than the one we are looking
                                    // for, so we keep on looking
                                    fragment_length = blank_lines_avail;
                                    fragment_position = i - blank_lines_avail;
                                    blank_lines_avail = 0;
                                  }
                                else
                                  {
                                    // current group doesn't fit at all, so we forget it
                                    blank_lines_avail = 0;
                                  } /*if*/
                              } /*if*/
                          } /*for*/
                        if (blank_lines_avail)
                          {
                            // last screen line is blank, a group ends with it
                            if
                              (
                                    blank_lines_avail >= sub_lines
                                &&
                                    blank_lines_avail < fragment_length
                              )
                              {
                                fragment_position = i - blank_lines_avail;
                              } /*if*/
                          } /*if*/
                        if (fragment_position == -1)
                          {
                            // it was not possible to find free screen line(s) for a subtitle,
                            // usually this means a bug in the code; however we do not overlap
                            fprintf
                              (
                                stderr,
                                "WARN: We could not find a suitable position for an"
                                    " overlapping subtitle\n"
                              );
                            highest_line = SUB_MAX_TEXT + 1;
                            break;
                          }
                        else /* found a place to put it */
                          {
                            int k;
                            for (k = 0; k < sub_lines; ++k)
                              {
                                placeholder[subs_done][fragment_position + k] = sub_first + j;
                              } /*for*/
                          } /*if*/
                      } /*if*/
                  } /*for*/
                for (j = highest_line + 1; j < lines_to_add; ++j)
                  {
                    if (placeholder[subs_done][j] != -1)
                        highest_line = j; /* highest nonblank line within block */
                    else
                        break;
                  } /*for*/
                if (highest_line >= SUB_MAX_TEXT)
                  {
                    // the 'block' has too much lines, so we don't overlap the
                    // subtitles
                    second = (subtitle_elt *)realloc
                      (
                        second,
                        (sub_num + sub_to_add + 1) * sizeof(subtitle_elt)
                      );
                    for (j = 0; j <= sub_to_add; ++j)
                      {
                        int ls;
                        memset(&second[sub_num + j], '\0', sizeof(subtitle_elt));
                        second[sub_num + j].start = first[sub_first + j].start;
                        second[sub_num + j].end = first[sub_first + j].end;
                        second[sub_num + j].lines = first[sub_first + j].lines;
                        second[sub_num + j].alignment = first[sub_first + j].alignment;
                        for (ls = 0; ls < second[sub_num + j].lines; ls++)
                          {
                            second[sub_num + j].text[ls] = strdup(first[sub_first + j].text[ls]);
                          } /*for*/
                      } /*for*/
                    sub_num += sub_to_add + 1;
                    sub_first += sub_to_add;
                    real_block = false;
                    break;
                  } /*if*/
                // we read the placeholder structure and create the new subs.
                second = (subtitle_elt *)realloc(second, (sub_num + 1) * sizeof(subtitle_elt));
                memset(&second[sub_num], '\0', sizeof(subtitle_elt));
                second[sub_num].start = local_start;
                second[sub_num].end = local_end;
                second[sub_num].alignment = H_SUB_ALIGNMENT_CENTER;
                n_max = (lines_to_add < SUB_MAX_TEXT) ? lines_to_add : SUB_MAX_TEXT;
                for (i = 0, j = 0; j < n_max; ++j)
                  {
                    if (placeholder[subs_done][j] != -1)
                      {
                      /* copy all the lines from first[placeholder[subs_done][j]] into
                        i and following positions in second */
                        const int lines = first[placeholder[subs_done][j]].lines;
                        int ls;
                        for (ls = 0; ls < lines; ++ls)
                          {
                            second[sub_num].text[i++] =
                                strdup(first[placeholder[subs_done][j]].text[ls]);
                          } /*for*/
                        j += lines - 1; /* skip over lines just copied */
                      }
                    else
                      {
                      /* no subtitle goes here -- put in blank line */
                        second[sub_num].text[i++] = strdup(" ");
                      } /*if*/
                  } /*for*/
                ++sub_num;
                ++subs_done;
              }
            while (local_end < global_end);
            if (real_block)
                for (i = 0; i < subs_done; ++i)
                    second[start_block_sub + i].lines = highest_line + 1;
            for (i = 0; i < nr_placeholder_entries; ++i)
              {
                free(placeholder[i]);
              } /*for*/
            free(placeholder);
            sub_first += sub_to_add; /* skip over the ones I just added */
          } /*for*/
        for (j = sub_orig - 1; j >= 0; --j)
          {
            for (i = first[j].lines - 1; i >= 0; --i)
              {
                free(first[j].text[i]);
              } /*for*/
          } /*for*/
        free(first);
        return_sub = second;
      }
    else /* not suboverlap_enabled */
      {
        adjust_subs_time(first, 6.0, movie_fps, 1, sub_num, uses_time);/*~6 secs AST*/
        return_sub = first;
      } /*if*/
    if (return_sub == NULL)
        return NULL;
    subt_data = (sub_data *)malloc(sizeof(sub_data));
    subt_data->filename = filename;
    subt_data->sub_uses_time = uses_time;
    subt_data->sub_num = sub_num;
    subt_data->sub_errs = sub_errs;
    subt_data->subtitles = return_sub;
    return subt_data;
  } /*sub_read_file*/

void sub_free(sub_data * subd)
  /* frees all storage allocated for subd. */
  {
    int i;
    if (!subd)
        return;
    if (subd->subtitles)
      {
        for (i = 0; i < subd->subtitles->lines; i++)
            free(subd->subtitles->text[i]);
      } /*if*/
    free(subd->subtitles);
    free((void *)subd->filename);
    free(subd);
  } /*sub_free*/

/*
    Additional unused stuff (remove some time)
*/

void list_sub_file(const sub_data * subd)
  {
    int i, j;
    const subtitle_elt * const subs = subd->subtitles;
    for (j = 0; j < subd->sub_num; j++)
      {
        const subtitle_elt * const egysub = &subs[j];
        fprintf(stdout, "%i line%c (%li-%li)\n",
            egysub->lines,
            1 == egysub->lines ? ' ' : 's',
            egysub->start,
            egysub->end);
        for (i = 0; i < egysub->lines; i++)
          {
            fprintf(stdout, "\t\t%d: %s%s", i, egysub->text[i], i == egysub->lines-1 ? "" : " \n ");
          } /*for*/
        fprintf(stdout, "\n");
      } /*for*/
    fprintf(stdout, "Subtitle format %s time.\n", subd->sub_uses_time ? "uses" : "doesn't use");
    fprintf(stdout, "Read %i subtitles, %i errors.\n", subd->sub_num, subd->sub_errs);
  } /*list_sub_file*/
