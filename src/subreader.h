/*
    Reading of subtitle files
*/
#ifndef __SUBREADER_H
#define __SUBREADER_H

// subtitle formats
#define SUB_INVALID   -1
#define SUB_MICRODVD  0 /* see <http://en.wikipedia.org/wiki/MicroDVD> */
#define SUB_SUBRIP    1 /* see <http://wiki.multimedia.cx/index.php?title=SubRip> */
#define SUB_SUBVIEWER 2 /* see <http://en.wikipedia.org/wiki/SubViewer>, sample <http://wiki.videolan.org/SubViewer> */
#define SUB_SAMI      3 /* see <http://en.wikipedia.org/wiki/SAMI>, sample <http://www.titlefactory.com/TitleFactoryDocs/sami_format.htm> */
#define SUB_VPLAYER   4
#define SUB_RT        5
#define SUB_SSA       6 /* spec is at <http://moodub.free.fr/video/ass-specs.doc>, or see <http://www.matroska.org/technical/specs/subtitles/ssa.html> */
#define SUB_PJS       7 /* Phoenix Japanimation Society */
#define SUB_MPSUB     8
#define SUB_AQTITLE   9
#define SUB_SUBVIEWER2 10 /* see <http://en.wikipedia.org/wiki/SubViewer>, sample <http://wiki.videolan.org/SubViewer> */
#define SUB_SUBRIP09 11
#define SUB_JACOSUB  12  /* spec is at <http://unicorn.us.com/jacosub/jscripts.html>. */


// One of the SUB_* constant above

#define MAX_SUBTITLE_FILES 128

sub_data* sub_read_file (const char *filename, float pts);
void subcp_open (void); /* for demux_ogg.c */
void subcp_close (void); /* for demux_ogg.c */
void list_sub_file(const sub_data * subd);
void dump_srt(sub_data* subd, float fps);
void dump_mpsub(sub_data* subd, float fps);
void dump_microdvd(sub_data* subd, float fps);
void dump_jacosub(sub_data* subd, float fps);
void dump_sami(sub_data* subd, float fps);
void sub_free( sub_data * subd );
void find_sub(sub_data* subd,unsigned long key);
/* void step_sub(sub_data *subd, float pts, int movement); */
#endif /* __SUBREADER_H */
