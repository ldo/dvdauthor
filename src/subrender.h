
#ifndef __SUBRENDER_H
#define __SUBRENDER_H

typedef struct mp_osd_bbox_s {
    int x1,y1,x2,y2;
} mp_osd_bbox_t;

#define OSDTYPE_OSD 1
#define OSDTYPE_SUBTITLE 2
#define OSDTYPE_PROGBAR 3
#define OSDTYPE_SPU 4

#define OSDFLAG_VISIBLE 1
#define OSDFLAG_CHANGED 2
#define OSDFLAG_BBOX 4
#define OSDFLAG_OLD_BBOX 8
#define OSDFLAG_FORCE_UPDATE 16

#define MAX_UCS 1600
#define MAX_UCSLINES 16

typedef struct mp_osd_obj_s {
    struct mp_osd_obj_s* next;
    unsigned char type;
    unsigned char alignment; // 2 bits: x;y percentages, 2 bits: x;y relative to parent; 2 bits: alignment left/right/center
    unsigned short flags;
    int x,y;
    int dxs,dys;
    mp_osd_bbox_t bbox; // bounding box
    mp_osd_bbox_t old_bbox; // the renderer will save bbox here
    union {
	struct {
	    void* sub;			// value of vo_sub at last update
	    int utbl[MAX_UCS+1];	// subtitle text
	    int xtbl[MAX_UCSLINES];	// x positions
	    int lines;			// no. of lines
	} subtitle;
	struct {
	    int elems;
	} progbar;
    } params;
    int stride;

    int allocated;
    unsigned char *alpha_buffer;
    unsigned char *bitmap_buffer;
} mp_osd_obj_t;


#include "subreader.h"

void vo_init_osd();
int vo_update_osd(int dxs,int dys);
int vo_osd_changed(int new_value);
#endif
