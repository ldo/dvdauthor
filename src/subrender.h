
#ifndef __SUBRENDER_H
#define __SUBRENDER_H

typedef struct mp_osd_bbox_s
  {
    int x1,y1; /* top left */
    int x2,y2; /* bottom right */
  } mp_osd_bbox_t;

/* values for mp_osd_obj_s.type */
/* #define OSDTYPE_OSD 1 */
#define OSDTYPE_SUBTITLE 2 /* only one used by spumux */
/* #define OSDTYPE_PROGBAR 3 */
/* #define OSDTYPE_SPU 4 */

/* masks for mp_osd_obj_s.flags, only the ones used by spumux */
/* only referenced in subrender.c */
#define OSDFLAG_VISIBLE 1 /* doesn't seem to serve any purpose */
#define OSDFLAG_CHANGED 2 /* doesn't seem to serve any purpose */
#define OSDFLAG_BBOX 4 /* doesn't seem to serve any purpose */
/* #define OSDFLAG_OLD_BBOX 8 */
#define OSDFLAG_FORCE_UPDATE 16 /* indicates osd obj needs to be redrawn, but we could do without this flag */

#define MAX_UCS 1600
#define MAX_UCSLINES 16

typedef struct mp_osd_obj_s /* for holding and maintaining a rendered subtitle image */
  {
    struct mp_osd_obj_s* next; /* linked list */
    unsigned char type;
    unsigned char alignment; // 2 bits: x;y percentages, 2 bits: x;y relative to parent; 2 bits: alignment left/right/center
    unsigned short flags;
  /* int x; */
    int y;
    int dxs, dys;
    mp_osd_bbox_t bbox; // bounding box
  /* mp_osd_bbox_t old_bbox; */ // the renderer will save bbox here
    union
      {
        struct /* only one used by spumux */
          {
            void* sub;          // value of vo_sub at last update
            int utbl[MAX_UCS + 1];    // subtitle text
            int xtbl[MAX_UCSLINES]; // x positions
            int lines;          // no. of lines
          } subtitle;
      /* struct
          {
            int elems;
          } progbar; */
      } params;
    int stride; /* bytes per row of both alpha and bitmap buffers */
    int allocated; /* size in bytes of each buffer */
    unsigned char *alpha_buffer; /* alpha channel, one byte per pixel */
    unsigned char *bitmap_buffer; /* one byte per pixel */
  } mp_osd_obj_t;


#include "subreader.h"

void vo_init_osd();
int vo_update_osd(int dxs,int dys);
int vo_osd_changed(int new_value);
void vo_finish_osd();
#endif
