/*
    Lowest-level interface to FreeType font rendering
*/
#ifndef __SUBFONT_H
#define __SUBFONT_H

#ifdef HAVE_FREETYPE
#ifdef HAVE_FT2BUILD_H
#include <ft2build.h>
#endif
#include FT_FREETYPE_H
#endif

typedef struct /* a bitmap for caching glyph images */
  {
    unsigned char *bmp; /* 8-bit-per-pixel indexed image bitmap accumulated here */
    colorspec pal[256]; /* colour palette */
    int w, h;
#ifdef HAVE_FREETYPE
    int charwidth, charheight;
    int pen, baseline, padding;
    int current_count; /* multiplied by charwidth * charheight = used size of bmp */
    int current_alloc; /* multiplied by charwidth * charheight = allocated size of bmp */
#endif
  } raw_file;

typedef struct
  {
    int spacewidth;
    int charspace; /* extra inter-character spacing, may be negative */
    int height;
    raw_file* pic_b[16]; /* luma for glyph images, 8 bits per pixel */
    short font[65536];
      /* index into faces array, which FT_Face to use for rendering this Unicode character */
    int start[65536];   // short is not enough for unicode fonts
      /* horizontal offset into bmp at which to find image for each Unicode character */
    short width[65536]; /* width in pixels of image for each Unicode character */

#ifdef HAVE_FREETYPE
    int face_cnt; /* how many entries in faces are used (actually always only 1) */
    FT_Face faces[16]; /* I suppose this is to allow getting different ranges of characters from different fonts */
    FT_UInt glyph_index[65536]; /* glyph index indexed by Unicode character code */

    int max_width, max_height;
#endif
  } font_desc_t;

extern font_desc_t* vo_font;

#ifdef HAVE_FREETYPE

int init_freetype();
int done_freetype();

void render_one_glyph(font_desc_t *desc, int c);
int kerning(font_desc_t *desc, int prevc, int c);

void load_font_ft();

#else

static void render_one_glyph(font_desc_t *desc, int c) {}
static int kerning(font_desc_t *desc, int prevc, int c) { return 0; }

#endif

#endif /* !__SUBFONT_H */
