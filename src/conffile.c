/*
    Accessing user configuration settings
*/

/*
 * Copyright (C) 2010 Lawrence D'Oliveiro <ldo@geek-central.gen.nz>.
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
#include <errno.h>
#include "conffile.h"

/*
    Implementation of relevant parts of the XDG Base Directory specification
    <http://standards.freedesktop.org/basedir-spec/latest/>.
*/

static char * xdg_make_home_relative
  (
    const char * path
  )
  /* prepends the value of $HOME onto path (assumed not to begin with a slash), or
    NULL on error (out of memory, $HOME not defined or not absolute). Caller must dispose
    of the result pointer. */
  {
    const char * const home = getenv("HOME");
    char * result = 0;
    size_t result_len;
    do /*once*/
      {
        if (home == 0 || home[0] != '/')
          {
            errno = ENOENT;
            break;
          } /*if*/
      /* assert strlen(home) > 0 */
        result_len = strlen(home) + 1 + strlen(path) + 1; /* worst case */
        result = malloc(result_len);
        if (result == 0)
            break;
        strncpy(result, home, result_len);
        if (result[strlen(result) - 1] != '/')
          {
            strconcat(result, result_len, "/");
          } /*if*/
        strconcat(result, result_len, path);
      }
    while (false);
    return
        result;
  } /*xdg_make_home_relative*/

static char * xdg_get_config_home(void)
  /* returns the directory for holding user-specific config files, or NULL on
    error. Caller must dispose of the result pointer. */
  {
    char * result;
    void * to_dispose = 0;
    result = getenv("XDG_CONFIG_HOME");
    if (result == 0)
      {
        to_dispose = xdg_make_home_relative(".config");
        if (to_dispose == 0)
          {
            return
                result;
          }
        result = to_dispose;
      } /*if*/
    if (result != 0 && to_dispose == 0)
      {
        result = strdup(result);
      } /*if*/
    return
        result;
  } /*xdg_get_config_home*/

static char * xdg_config_search_path(void)
  /* returns a string containing the colon-separated list of config directories to search
    (apart from the user area). Caller must dispose of the result pointer. */
  {
    char * result = getenv("XDG_CONFIG_DIRS");
    if (result == 0)
      {
        result = "/etc";
          /* note spec actually says default should be /etc/xdg, but /etc is the
            conventional location for system config files. */
      } /*if*/
    return strdup(result);
  } /*xdg_config_search_path*/

typedef int (*xdg_path_component_action)
  (
    const unsigned char * path, /* storage belongs to me, make a copy if you want to keep it */
    size_t path_len, /* length of path string */
    void * arg /* meaning is up to you */
  );
  /* return nonzero to abort the scan */

static int xdg_for_each_path_component
  (
    const unsigned char * path,
    size_t path_len,
    xdg_path_component_action action,
    void * actionarg,
    bool forwards /* false to do in reverse */
  )
  /* splits the string path with len path_len at any colon separators, calling
    action for each component found, in forward or reverse order as specified.
    Returns nonzero on error, or if action returned nonzero. */
  {
    int status;
    const unsigned char * const path_end = path + path_len;
    const unsigned char * path_prev;
    const unsigned char * path_next;
    if (forwards)
      {
        path_prev = path;
        for (;;)
          {
            path_next = path_prev;
            for (;;)
              {
                if (path_next == path_end)
                    break;
                if (*path_next == ':')
                    break;
                ++path_next;
              } /*for*/
            status = action(path_prev, path_next - path_prev, actionarg);
            if (status != 0)
                break;
            if (path_next == path_end)
                break;
            path_prev = path_next + 1;
          } /*for*/
      }
    else /* backwards */
      {
        path_next = path_end;
        for (;;)
          {
            path_prev = path_next;
            for (;;)
              {
                if (path_prev == path)
                    break;
                --path_prev;
                if (*path_prev == ':')
                  {
                    ++path_prev;
                    break;
                  } /*if*/
              } /*for*/
            status = action(path_prev, path_next - path_prev, actionarg);
            if (status != 0)
                break;
            if (path_prev == path)
                break;
            path_next = path_prev - 1;
          } /*for*/
      } /*if*/
    return status;
  } /*xdg_for_each_path_component*/

typedef int (*xdg_item_path_action)
  (
    const char * path, /* a complete expanded pathname */
    void * arg /* meaning is up to you */
  );
  /* return nonzero to abort the scan */

typedef struct
  {
    const char * itempath;
    xdg_item_path_action action;
    void * actionarg;
  } xdg_for_each_config_found_context;

static int xdg_for_each_config_found_try_component
  (
    const unsigned char * dirpath,
    size_t dirpath_len,
    xdg_for_each_config_found_context * context
  )
  /* generates the full item path, and passes it to the caller's action
    if it is accessible. */
  {
    char * thispath;
    const size_t thispath_maxlen = dirpath_len + 1 + strlen(context->itempath) + 1;
    struct stat statinfo;
    int status = 0;
    do /*once*/
      {
        thispath = malloc(thispath_maxlen);
        if (thispath == 0)
          {
            status = -1;
            break;
          } /*if*/
        memcpy(thispath, dirpath, dirpath_len);
        thispath[dirpath_len] = 0;
        if (dirpath_len != 0 && dirpath[dirpath_len - 1] != '/')
          {
            strconcat(thispath, thispath_maxlen, "/");
          } /*if*/
        strconcat(thispath, thispath_maxlen, context->itempath);
        if (stat(thispath, &statinfo) == 0)
          {
            status = context->action(thispath, context->actionarg);
          }
        else
          {
            errno = 0; /* ignore stat result */
          } /*if*/
      }
    while (false);
    free(thispath);
    return
        status;
  } /*xdg_for_each_config_found_try_component*/;

static int xdg_for_each_config_found
  (
    const char * itempath, /* relative path of item to look for in each directory */
    xdg_item_path_action action,
    void * actionarg,
    bool forwards /* false to do in reverse */
  )
  {
    xdg_for_each_config_found_context context;
    int status = 0;
    const char * const home_path = xdg_get_config_home();
    const char * const search_path = xdg_config_search_path();
    context.itempath = itempath;
    context.action = action;
    context.actionarg = actionarg;
    do /*once*/
      {
        if (home_path != 0 && forwards)
          {
            status = xdg_for_each_config_found_try_component
              (
                (const unsigned char *)home_path,
                strlen(home_path),
                &context
              );
            if (status != 0)
                break;
          } /*if*/
        status = xdg_for_each_path_component
          (
            /*path =*/ (const unsigned char *)search_path,
            /*path_len =*/ strlen(search_path),
            /*action =*/ (xdg_path_component_action)xdg_for_each_config_found_try_component,
            /*actionarg =*/ (void *)&context,
            /*forwards =*/ forwards
          );
        if (status != 0)
            break;
        if (home_path != 0 && !forwards)
          {
            status = xdg_for_each_config_found_try_component
              (
                (const unsigned char *)home_path,
                strlen(home_path),
                &context
              );
            if (status != 0)
                break;
          } /*if*/
      }
    while (false);
    if (home_path != 0)
        free((void *)home_path);
    free((void *)search_path);
    return
        status;
  } /*xdg_for_each_config_found*/

typedef struct
  {
    char * result;
  } xdg_find_first_config_path_context;

static int xdg_find_first_config_path_save_item
  (
    const char * itempath,
    xdg_find_first_config_path_context * context
  )
  {
    context->result = strdup(itempath);
    return
        context->result != 0;
  } /*xdg_find_first_config_path_save_item*/;

static char * xdg_find_first_config_path
  (
    const char * itempath
  )
  /* searches for itempath in all the config directory locations in order of decreasing
    priority, returning the expansion where it is first found, or NULL if not found.
    Caller must dispose of the result pointer. */
  {
    xdg_find_first_config_path_context context;
    context.result = 0;
    errno = 0;
    (void)xdg_for_each_config_found
      (
        /*itempath =*/ itempath,
        /*action =*/ (xdg_item_path_action)xdg_find_first_config_path_save_item,
        /*actionarg =*/ (void *)&context,
        /*forwards =*/ true
      );
    if (context.result == 0 && errno == 0)
      {
        errno = ENOENT;
      } /*if*/
    return
        context.result;
  } /*xdg_find_first_config_path*/

/*
    User-visible stuff
*/

char * get_outputdir(void)
  /* allocates and returns a string containing the user-specified output
    directory path, or NULL if not specified. */
 {
#if 0 /* don't do this any more */
    char * outputdir = getenv("OUTPUTDIR");
    if (outputdir != 0)
      {
        outputdir = strdup(outputdir);
      } /*if*/
    return outputdir;
#else
    return 0;
#endif
 } /*get_outputdir*/

int get_video_format(void)
  /* returns one of VF_NTSC or VF_PAL indicating the default video format
    if specified, else VF_NONE if not. */
  /* This implements the Video Format Preference proposal
    <http://create.freedesktop.org/wiki/Video_Format_Pref>. */
  {
    int result = VF_NONE;
    char * format;
    char * conffilename = 0;
    FILE * conffile = 0;
    char * eol;
    char line[40]; /* should be plenty */
    do /*once*/
      {
        format = getenv("VIDEO_FORMAT");
        if (format != 0)
            break;
        conffilename = xdg_find_first_config_path("video_format");
        if (conffilename == 0)
            break;
        conffile = fopen(conffilename, "r");
        if (!conffile)
            break;
        format = fgets(line, sizeof line, conffile);
        eol = strchr(format, '\n');
        if (eol)
          {
            *eol = 0;
          } /*if*/
        break;
      }
    while (false);
    if (conffile)
      {
        fclose(conffile);
      } /*if*/
    free(conffilename);
    if (format != 0)
      {
        if (!strcasecmp(format, "NTSC"))
          {
            result = VF_NTSC;
          }
        else if (!strcasecmp(format, "PAL"))
          {
            result = VF_PAL;
          } /*if*/
      } /*if*/
    return result;
  } /*get_video_format*/
