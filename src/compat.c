#include "config.h"

#include "compat.h"

#include <ctype.h>
#include <fcntl.h>
#include <errno.h>

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
