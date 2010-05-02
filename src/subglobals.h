#define SUB_MAX_TEXT 16 /* max nr lines of subtitles allowed */

typedef struct { /* holds subtitle info to be displayed over a particular interval */
    int lines; /* length of text array */
    unsigned long start; /* start time for displaying subtitle */
    unsigned long end; /* end time for displaying subtitle */
    char *text[SUB_MAX_TEXT]; /* array [lines] of char* */
    unsigned char alignment;
} subtitle_elt;

typedef struct { /* holds text and related information read from a subtitle file */
    subtitle_elt *subtitles; /* array [sub_num] */ /* succession of subtitles to be displayed */
    const char *filename;
    bool sub_uses_time; /* true => start and end are in hundredths of a second; false => they are frame numbers */
    int sub_num;          // number of subtitle structs
    int sub_errs;
} sub_data;

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

/* parameters for subreader */
extern float sub_delay; /* not used anywhere */
extern float sub_fps;
extern int suboverlap_enabled;
#ifdef HAVE_ICONV
extern char *subtitle_charset; /* code page for interpreting subtitles */
#endif

/* parameters for subfont */
extern float font_factor;
extern float text_font_scale_factor;
extern char * sub_font;
extern int subtitle_autoscale; /* fixme: not user-settable */
extern float subtitle_font_thickness;
extern colorspec subtitle_fill_color, subtitle_outline_color, subtitle_shadow_color;
extern int subtitle_shadow_dx, subtitle_shadow_dy;

/* parameters for subrender */
extern float movie_fps;
extern int movie_width;
extern int movie_height;
extern int h_sub_alignment;
extern int v_sub_alignment;
extern int sub_justify;
extern int sub_left_margin;
extern int sub_right_margin;
extern int sub_bottom_margin;
extern int sub_top_margin;
/* maintained by subrender: */
extern unsigned char * textsub_image_buffer; /* where text subtitles are rendered */
extern size_t textsub_image_buffer_size; /* size of buffer */

/* parameters for subgen-image */
extern bool text_forceit;
/* maintained by subgen-image: */
extern sub_data *textsub_subdata;

/* kept in subgen.c, but usable elsewhere: */
extern int default_video_format;
extern bool widescreen;
