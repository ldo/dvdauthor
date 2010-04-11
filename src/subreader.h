/*
    Reading of subtitle files
*/
#ifndef __SUBREADER_H
#define __SUBREADER_H

sub_data* sub_read_file(const char *filename, float movie_fps);
void list_sub_file(const sub_data * subd);
void sub_free(sub_data * subd);

#endif /* __SUBREADER_H */
