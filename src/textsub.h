typedef struct {
	unsigned long start;
	unsigned long end;
	int valid;
} textsub_subtitle_type;

extern sub_data * textsub_init(char *textsub_filename, float textsub_movie_fps, float textsub_movie_width, float textsub_movie_height);
extern void textsub_dump_file();
extern textsub_subtitle_type *textsub_find_sub(unsigned long text_sub_pts);
extern void textsub_statistics();
extern void textsub_finish();
void textsub_render(subtitle* sub);
extern subtitle *textsub_subs;
extern sub_data *textsub_subdata;
