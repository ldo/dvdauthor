#define SUB_MAX_TEXT 16

typedef struct {
    int lines;
    unsigned long start;
    unsigned long end;
    char *text[SUB_MAX_TEXT];
    unsigned char alignment;
} subtitle;

typedef struct {
    subtitle *subtitles;
    char *filename;
    int sub_uses_time;
    int sub_num;          // number of subtitle structs
    int sub_errs;
} sub_data;

extern char* dvdsub_lang;

#ifdef HAVE_ICONV
extern char *sub_cp;
#endif

extern float sub_delay;
extern float sub_fps;
extern int suboverlap_enabled;
extern int sub_utf8;
extern float font_factor;
extern char * filename;
extern char * font_name;
extern int current_sub;
extern unsigned char * image_buffer;
extern float movie_fps;
extern int movie_width;
extern int movie_height;
extern float text_font_scale_factor;
extern float osd_font_scale_factor;
extern float subtitle_font_radius;
extern float subtitle_font_thickness;
extern int subtitle_autoscale;
extern int h_sub_alignment;
extern int sub_alignment;
extern int sub_bg_color; /* subtitles background color */
extern int sub_bg_alpha;
extern subtitle* vo_sub;
extern int sub_justify;
extern int sub_left_margin;
extern int sub_right_margin;
extern int sub_bottom_margin;
extern int sub_top_margin;
extern char * sub_font;
extern int sub_max_chars;
extern int sub_max_lines;
extern int sub_max_font_height;
extern int sub_max_bottom_font_height;
extern int max_sub_size;
