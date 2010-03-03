/*
    Common definitions needed across both authoring and unauthoring tools.
*/
/*
   Copyright (C) 2010 Lawrence D'Oliveiro <ldo@geek-central.gen.nz>.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307
   USA
 */

#ifndef __DVDAUTHOR_COMMON_H_
#define __DVDAUTHOR_COMMON_H_

#ifdef __cplusplus
extern "C" {
#endif

enum
  {
  /* MPEG header ID codes relevant to DVD-Video. Each packet begins with
    a 4-byte code 00 00 01 nn where nn is one of the following */

  /* start codes */
    MPID_PICTURE = 0x00, /* picture header */
    MPID_SEQUENCE = 0xb3, /* sequence header */
    MPID_EXTENSION = 0xb5, /* extension header */
    MPID_SEQUENCE_END = 0xb7, /* sequence end */
    MPID_GOP = 0xb8, /* Group Of Pictures */
    MPID_PROGRAM_END = 0xb9, /* program end (terminates a program stream) */

  /* special headers */
    MPID_PACK = 0xba,
      /* PACK header, contains a more precise clock reference, and indication of overall
        bandwidth requirements. DVD-Video requires each PACK to be 2048 bytes in size. */
    MPID_SYSTEM = 0xbb,
      /* system header, specifies how many audio and video streams there are, the
        bandwidth they need, and whether they are synchronized to the system clock. */

  /* stream ID codes: the two bytes after the first four are the length of the remaining data */
    MPID_PRIVATE1 = 0xbd,
      /* private stream 1, used in DVD-Video for subpictures and additional non-MPEG audio
        formats. The first byte after the packet header+extensions is the substream ID,
        the bits of which are divided up as follows: fffssnnn, where fff is 001 for
        a subpicture stream (with ssnnn giving the stream number, allowing up to 32 subpicture
        streams), or 100 for an audio stream, in which case ss specifies the audio format:
        00 => AC3, 01 => DTS, 10 => PCM, and nnn is the stream number, allowing up to 8
        audio streams. */
    MPID_PAD = 0xbe, /* padding stream */
    MPID_PRIVATE2 = 0xbf,
      /* private stream 2, used in DVD-Video for PCI and DSI packets; the byte after the
        length is 0 for a PCI packet, 1 for a DSI packet. */
    MPID_AUDIO_FIRST = 0xc0,
      /* MPEG audio streams have IDs in range [MPID_AUDIO_FIRST .. MPID_AUDIO_LAST], but note
        DVD-Video only allows 8 audio streams. */
    MPID_AUDIO_LAST = 0xdf,
    MPID_VIDEO_FIRST = 0xe0,
      /* video streams have IDs in range [MPID_VIDEO_FIRST .. MPID_VIDEO_LAST], but note
        DVD-Video only allows one video stream. dvdauthor assumes its ID will be
        MPID_VIDEO_FIRST. */
    MPID_VIDEO_LAST = 0xef,
  };

enum
  { /* subpicture operation codes */
    SPU_FSTA_DSP = 0, /* forced start display, no arguments */
    SPU_STA_DSP = 1, /* start display, no arguments */
    SPU_STP_DSP = 2, /* stop display, no arguments */
    SPU_SET_COLOR = 3, /* four nibble indexes into CLUT for current PGC = 2 bytes of args */
    SPU_SET_CONTR = 4, /* four nibble contrast/alpha values = 2 bytes of args */
    SPU_SET_DAREA = 5, /* set display area, start X/Y, end X/Y = 6 bytes of args */
    SPU_SET_DSPXA = 6,
      /* define pixel data addresses, 2-byte offset to top field data, 2-byte offset to
        bottom field data = 4 bytes of args */
    SPU_CHG_COLCON = 7,
      /* change colour/contrast, 2 bytes param area size (incl itself) + variable nr
        bytes params: one or more LN_CTLI, each immediately followed by 1 to 8 PX_CTLI.
        Each LN_CTLI is 4 bytes, consisting of 4 bits of zero, 12 bits of starting line
        number (must be greater than ending line of previous LN_CTLI, if any), 4 bits
        of number of following PX_CTLI (must be in [1 .. 8]), and 12 bits of ending line
        number (must not be less than starting line number).
        Each PX_CTLI is 6 bytes, consisting of 2 bytes starting col number (must be
        at least 8 greater than previous PX_CTLI, if any), 2 bytes of colour values
        as per SET_COLOR, and 2 bytes of new contrast values as per SET_CONTR. */
    SPU_CMD_END = 255 /* ends one SP_DCSQ */
  };

typedef enum /* attributes of cell */
/* A "cell" is a  grouping of one or more VOBUs, which might have a single VM command attached.
    A "program" is a grouping of one or more cells; the significance is that skipping using
    the next/prev buttons on the DVD player remote is done in units of programs. Also with
    multiple interleaved angles, each angle goes in its own cell(s), but they must be within
    the same program.
    A program can also be marked as a "chapter" (aka "Part Of Title", "PTT"), which means it
    can be directly referenced via an entry in the VTS_PTT_SRPT table, which allows it
    to be linked from outside the current PGC.
    And finally, one or more programs are grouped into a "program chain" (PGC). This can
    have a VM command sequence to be executed at the start of the PGC, and another sequence
    to be executed at the end. It also specifies the actual audio and subpicture stream IDs
    corresponding to the stream attributes described in the IFO header for this menu/title.
    Each menu or title may consist of a single PGC, or a sequence of multiple PGCs.
    There is also a special "first play" PGC (FPC), which if present is automatically entered
    when the disc is inserted into the player. */
  {
    CELL_NEITHER = 0, /* neither of following specified */
    CELL_CHAPTER_PROGRAM = 1, /* cell has chapter attribute (implies program attribute) */
    CELL_PROGRAM = 2, /* cell has program attribute only */
  } cell_chapter_types;

#ifdef __cplusplus
}
#endif

#endif
