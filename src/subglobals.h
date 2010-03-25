#define SUB_MAX_TEXT 16 /* max nr lines of subtitles allowed */

typedef struct { /* holds subtitle info to be displayed over a particular interval */
    int lines; /* length of text array */
    unsigned long start; /* start time for displaying subtitle */
    unsigned long end; /* end time for displaying subtitle */
    char *text[SUB_MAX_TEXT]; /* array [lines] of char* */
    unsigned char alignment;
    int text_forced;
} subtitle;

typedef struct { /* holds text and related information read from a subtitle file */
    subtitle *subtitles; /* array [sub_num] */ /* succession of subtitles to be displayed */
    const char *filename;
    int sub_uses_time; /* true => start and end are in hundredths of a second; false => they are frame numbers */
    int sub_num;          // number of subtitle structs
    int sub_errs;
} sub_data;

extern char* dvdsub_lang;

#ifdef HAVE_ICONV
extern char *sub_cp; /* code page for interpreting subtitles */
#endif

enum /* horizontal alignment settings */
  {
    H_SUB_ALIGNMENT_LEFT = 1,
    H_SUB_ALIGNMENT_CENTER = 0,
    H_SUB_ALIGNMENT_RIGHT = 2,
    H_SUB_ALIGNMENT_DEFAULT = 4,
  };

enum /* vertical alignment settings */
  {
    V_SUB_ALIGNMENT_TOP = 0,
    V_SUB_ALIGNMENT_CENTER = 1,
    V_SUB_ALIGNMENT_BOTTOM = 2,
  };

enum /* values for subtitle_autoscale */
  {
    AUTOSCALE_NONE = 0, /* no autoscale */
    AUTOSCALE_MOVIE_HEIGHT = 1, /* video height */
    AUTOSCALE_MOVIE_WIDTH = 2, /* video width */
    AUTOSCALE_MOVIE_DIAGONAL = 3, /* diagonal */
  };

extern float sub_delay;
extern float sub_fps;
extern int suboverlap_enabled;
extern float font_factor;
extern char * textsub_font_name;
extern int current_sub;
extern unsigned char * textsub_image_buffer; /* where text subtitles are rendered */
extern float movie_fps;
extern int movie_width;
extern int movie_height;
extern float text_font_scale_factor;
extern int text_forceit;
extern float subtitle_font_radius;
extern float subtitle_font_thickness;
extern int subtitle_autoscale; /* fixme: not user-settable */
extern int h_sub_alignment;
extern int v_sub_alignment;
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
