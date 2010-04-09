#include "config.h"

#include "compat.h"

#include <limits.h>
#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <locale.h>
#include <langinfo.h>

#ifdef HAVE_ICONV

const char *
    default_charset;
static char
    default_charset_buf[32]; /* big enough? */
static iconv_t
    from_locale = ICONV_NULL;

#endif /*HAVE_ICONV*/

void init_locale()
  /* does locale initialization and obtains the default character set. */
  {
#ifdef HAVE_ICONV
    setlocale(LC_ALL, "");
    strncpy(default_charset_buf, nl_langinfo(CODESET), sizeof default_charset_buf - 1);
    default_charset_buf[sizeof default_charset_buf - 1] = 0;
    default_charset = default_charset_buf;
    // fprintf(stderr, "INFO: default codeset is \"%s\"\n", default_charset); /* debug */
#else
    // fprintf(stderr, "INFO: all text will be interpreted as UTF-8\n"); /* debug */
#endif /*HAVE_ICONV*/
  } /*init_locale*/

char * locale_decode
  (
    const char * localestr
  )
  /* allocates and returns a string containing the UTF-8 representation of
    localestr interpreted according to the user's locale settings. */
  /* not actually used anywhere */
  {
    char * result;
#ifdef HAVE_ICONV
    size_t inlen, outlen;
    char * resultnext;
    if (from_locale == ICONV_NULL)
      {
        from_locale = iconv_open("UTF-8", default_charset);
        if (from_locale == ICONV_NULL)
          {
            fprintf(stderr, "ERR:  Cannot convert from charset \"%s\" to UTF-8\n", default_charset);
            exit(1);
          } /*if*/
      } /*if*/
    inlen = strlen(localestr);
    outlen = inlen * 5; /* should be enough? */
    result = malloc(outlen);
    resultnext = result;
    if (iconv(from_locale, (char **)&localestr, &inlen, &resultnext, &outlen) < 0)
      {
        fprintf
          (
            stderr,
            "ERR:  Error %d -- %s decoding string \"%s\"\n",
            errno, strerror(errno), localestr
          );
        exit(1);
      } /*if*/
    assert(outlen != 0); /* there will be room for ... */
    *resultnext++ = 0; /* ... terminating null */
    result = realloc(result, resultnext - result); /* free unneeded memory */
#else
    result = strdup(localestr);
#endif /*HAVE_ICONV*/
    return result;
  } /*locale_decode*/

unsigned int strtounsigned
  (
    const char * s,
    const char * what /* description of what I'm trying to convert, for error message */
  )
  /* parses s as an unsigned decimal integer, returning its value. Aborts the
    program on error. */
  {
    char * s_end;
    unsigned long result;
    errno = 0;
    result = strtoul(s, &s_end, 10);
    if (errno == 0)
      {
        if (*s_end != '\0')
          {
            errno = EINVAL;
          }
        else if (result > UINT_MAX)
          {
            errno = ERANGE;
          }
      } /*if*/
    if (errno != 0)
      {
        fprintf(stderr, "ERR: %d converting %s \"%s\" -- %s\n", errno, what, s, strerror(errno));
        exit(1);
      } /*if*/
    return result;
  } /*strtounsigned*/

#ifndef HAVE_STRNDUP
char * strndup
  (
    const char * s,
    size_t n
  )
  {
    char * result;
    size_t l = strlen(s);
    if (l > n)
      {
        l = n;
      } /*if*/
    result = malloc(l + 1);
    memcpy(result, s, l);
    result[l] = 0;
    return
        result;
  } /*strndup*/
#endif /*HAVE_STRNDUP*/

#if HAVE_ICONV && LOCALIZE_FILENAMES

static iconv_t
    to_locale = ICONV_NULL;

char * localize_filename(const char * pathname)
  /* converts a filename from UTF-8 to localized encoding. */
  {
    char * result;
    size_t inlen, outlen;
    char * resultnext;
    if (to_locale == ICONV_NULL)
      {
        fprintf(stderr, "INFO: Converting filenames to %s\n", default_charset);
        to_locale = iconv_open(default_charset, "UTF-8");
        if (to_locale == ICONV_NULL)
          {
            fprintf(stderr, "ERR:  Cannot convert from UTF-8 to charset \"%s\"\n", default_charset);
            exit(1);
          } /*if*/
      } /*if*/
    inlen = strlen(pathname);
    outlen = inlen * 5; /* should be enough? */
    result = malloc(outlen);
    resultnext = result;
    if (iconv(to_locale, (char **)&pathname, &inlen, &resultnext, &outlen) < 0)
      {
        fprintf
          (
            stderr,
            "ERR:  Error %d -- %s encoding pathname \"%s\"\n",
            errno, strerror(errno), pathname
          );
        exit(1);
      } /*if*/
    assert(outlen != 0); /* there will be room for ... */
    *resultnext++ = 0; /* ... terminating null */
    result = realloc(result, resultnext - result); /* free unneeded memory */
    return result;
  } /*localize_filename*/

#endif

struct vfile varied_open
  (
    const char * fname,
    int mode, /* either O_RDONLY or O_WRONLY, nothing more */
    const char * what /* description of what I'm trying to open, for error message */
  )
  {
    struct vfile vf;
    int fnamelen;
    if (strcmp(fname, "-") == 0)
      {
        vf.ftype = VFTYPE_REDIR;
        vf.h = mode == O_RDONLY ? stdin : stdout;
      }
    else if (fname[0] == '&' && isdigit(fname[1]))
      {
        vf.ftype = VFTYPE_FILE;
        vf.h = fdopen(atoi(fname + 1), mode == O_RDONLY ? "rb" : "wb");
      }
    else if (mode == O_WRONLY && fname[0] == '|')
      {
        vf.ftype = VFTYPE_PIPE;
        vf.h = popen(fname + 1, "w");
      }
    else if (mode == O_RDONLY && fname[0] != '\0' && fname[fnamelen = strlen(fname) - 1] == '|')
      {
        char * const fcopy = strndup(fname, fnamelen);
        vf.ftype = VFTYPE_PIPE;
        vf.h = popen(fcopy, "r");
        free(fcopy);
      }
    else
      {
        vf.ftype = VFTYPE_FILE;
        vf.h = fopen(fname, mode == O_RDONLY ? "rb" : "wb");
      } /*if*/
    if (vf.h == NULL)
      {
        fprintf(stderr, "ERR: %d opening %s \"%s\" -- %s\n", errno, what, fname, strerror(errno));
        exit(1);
      } /*if*/
    vf.mode = mode;
    return vf;
  } /*varied_open*/

void varied_close(struct vfile vf)
  {
    if (vf.mode == O_WRONLY)
      {
      /* check for errors on final write before closing */
        if (fflush(vf.h) != 0)
          {
            fprintf(stderr, "ERR: %d on fflush -- %s\n", errno, strerror(errno));
            exit(1);
          } /*if*/
      } /*if*/
    switch (vf.ftype)
      {
    case VFTYPE_FILE:
        fclose(vf.h);
    break;
    case VFTYPE_PIPE:
        pclose(vf.h);
    break;
    case VFTYPE_REDIR:
    default:
      /* nothing to do */
    break;
      } /*switch*/
  } /*varied_close*/
