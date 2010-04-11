
#ifndef __SUBRENDER_H
#define __SUBRENDER_H

/* statistics: */
extern int sub_max_chars;
extern int sub_max_lines;
extern int sub_max_font_height;
extern int sub_max_bottom_font_height;

void vo_init_osd();
void vo_update_osd(const subtitle_elt * vo_sub);
void vo_finish_osd();
#endif
