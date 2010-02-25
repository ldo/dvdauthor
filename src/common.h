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

  /* stream ID codes: the two bytes after the first four are the length of the remaining data */
    MPID_PACK = 0xba, /* PACK header */
    MPID_SYSTEM = 0xbb, /* system header */

    MPID_PRIVATE1 = 0xbd,
      /* private stream 1, used in DVD-Video for subpictures and additional audio formats;
        the first byte after the packet header+extensions is the substream ID */
    MPID_PAD = 0xbe, /* padding stream */
    MPID_PRIVATE2 = 0xbf,
      /* private stream 2, used in DVD-Video for PCI and DSI packets; the byte after the
        length is 0 for a PCI packet, 1 for a DSI packet */
    MPID_AUDIO_FIRST = 0xc0,
      /* audio streams have IDs in range [MPID_AUDIO_FIRST .. MPID_AUDIO_LAST], but note
        DVD-Video only allows 8 audio streams */
    MPID_AUDIO_LAST = 0xdf,
    MPID_VIDEO_FIRST = 0xe0,
      /* video streams have IDs in range [MPID_VIDEO_FIRST .. MPID_VIDEO_LAST], but note
        DVD-Video only allows one video stream */
    MPID_VIDEO_LAST = 0xef,
  };

typedef enum /* attributes of cell */
  {
    CELL_NEITHER = 0, /* neither of following specified */
    CELL_CHAPTER_PROGRAM = 1, /* cell has chapter or chapter+program attribute */
    CELL_PROGRAM = 2, /* cell has program attribute only */
  } cell_chapter_types;

#ifdef __cplusplus
}
#endif

#endif
