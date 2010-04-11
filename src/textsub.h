extern sub_data *textsub_subdata;

bool textsub_init
  (
    const char *textsub_filename
  );
extern void textsub_dump_file();
extern void textsub_finish();
void textsub_render(const subtitle_elt * sub);
