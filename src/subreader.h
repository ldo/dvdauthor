#ifndef __SUBREADER_H
#define __SUBREADER_H

// subtitle formats
#define SUB_INVALID   -1
#define SUB_MICRODVD  0
#define SUB_SUBRIP    1
#define SUB_SUBVIEWER 2
#define SUB_SAMI      3
#define SUB_VPLAYER   4
#define SUB_RT        5
#define SUB_SSA       6
#define SUB_DUNNOWHAT 7		// FIXME what format is it ?
#define SUB_MPSUB     8
#define SUB_AQTITLE   9
#define SUB_SUBVIEWER2 10
#define SUB_SUBRIP09 11
#define SUB_JACOSUB  12

// One of the SUB_* constant above

#define MAX_SUBTITLE_FILES 128

#define SUB_ALIGNMENT_HLEFT	1
#define SUB_ALIGNMENT_HCENTER	0
#define SUB_ALIGNMENT_HRIGHT	2
#define SUB_ALIGNMENT_DEFAULT 4

sub_data* sub_read_file (char *filename, float pts);
subtitle* subcp_recode1 (subtitle *sub);
void subcp_open (void); /* for demux_ogg.c */
void subcp_close (void); /* for demux_ogg.c */
char ** sub_filenames(char *path, char *fname);
void list_sub_file(sub_data* subd);
void dump_srt(sub_data* subd, float fps);
void dump_mpsub(sub_data* subd, float fps);
void dump_microdvd(sub_data* subd, float fps);
void dump_jacosub(sub_data* subd, float fps);
void dump_sami(sub_data* subd, float fps);
void sub_free( sub_data * subd );
void find_sub(sub_data* subd,unsigned long key);
/* void step_sub(sub_data *subd, float pts, int movement); */
#endif /* __SUBREADER_H */
