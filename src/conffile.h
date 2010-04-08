/*
    Accessing user configuration settings
*/

char * get_outputdir(void);
  /* allocates and returns a string containing the user-specified output
    directory path, or NULL if not specified. */

int get_video_format(void);
  /* returns one of VF_NTSC or VF_PAL indicating the default video format
    if specified, else VF_NONE if not. */
