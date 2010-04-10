/*
    Reading of subtitle files
*/
#ifndef __SUBREADER_H
#define __SUBREADER_H

sub_data* sub_read_file(const char *filename, float pts);
void list_sub_file(const sub_data * subd);
void sub_free(sub_data * subd);
void find_sub(sub_data* subd, unsigned long key);

#endif /* __SUBREADER_H */
