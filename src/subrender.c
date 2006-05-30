/* Copyright (C) 2000 - 2003 various authors of the MPLAYER project
 * This module uses various parts of the MPLAYER project (http://www.mplayerhq.hu)
 * With many changes by Sjef van Gool (svangool@hotmail.com) November 2003
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307
 * USA
 */

/* Generic alpha renderers for all YUV modes and RGB depths.
 * Optimized by Nick and Michael
 * Code from Michael Niedermayer (michaelni@gmx.at) is under GPL
 */

#include "config.h"

#include "compat.h"

#include "subconfig.h"

#include "subglobals.h"
#include "subrender.h"
#include "subfont.h"


static const char RCSID[]="$Id: //depot/dvdauthor/src/subrender.c#10 $";


#define NEW_SPLITTING


// Structures needed for the new splitting algorithm.
// osd_text_t contains the single subtitle word.
// osd_text_p is used to mark the lines of subtitles
struct osd_text_t {
    int osd_kerning, //kerning with the previous word
	osd_length,  //horizontal length inside the bbox
	text_length, //number of characters
	*text;       //characters
    struct osd_text_t *prev,
                      *next;
};

struct osd_text_p {
    int  value;
    struct osd_text_t *ott;
    struct osd_text_p *prev,
                      *next;
};
static int sub_unicode=0;
static int sub_pos=100;
/* static int sub_width_p=100; */
static int sub_visibility=1;
static int vo_osd_changed_status = 0;
static mp_osd_obj_t* vo_osd_list=NULL;

int force_load_font;

static inline void vo_draw_alpha_rgb24(int w,int h, unsigned char* src, unsigned char *srca, int srcstride, unsigned char* dstbase,int dststride){
    int y,i;
    for(y=0;y<h;y++){
        register unsigned char *dst = dstbase;
        register int x;
        for(x=0;x<w;x++){
            if(srca[x]){

		dst[0]=(((dst[0]*srca[x]))>>8)+src[x];
		dst[1]=(((dst[1]*srca[x]))>>8)+src[x];
		dst[2]=(((dst[2]*srca[x]))>>8)+src[x];
		/* dst[0]=(src[x]>>6)<<6;
		dst[1]=(src[x]>>6)<<6;
		dst[2]=(src[x]>>6)<<6; */
		for (i=0;i<3;i++)
		{
			if ( dst[i])
			{
				if (dst[i]>=170)
				  dst[i]=255;
				else
				{
					if (dst[i]>=127)
					  dst[i]=127;
					else
					  dst[i]=1;
				}
			}
		}
		/* fprintf(stderr,"%d.",src[x]); */

            }
            dst+=3; // 24bpp
        }
        src+=srcstride;
        srca+=srcstride;
        dstbase+=dststride;
    }
    return;
}
// renders char to a big per-object buffer where alpha and bitmap are separated
static void draw_alpha_buf(mp_osd_obj_t* obj, int x0,int y0, int w,int h, unsigned char* src, unsigned char *srca, int stride)
{
    int dststride = obj->stride;
    int dstskip = obj->stride-w;
    int srcskip = stride-w;
    int i, j;
    unsigned char *b = obj->bitmap_buffer + (y0-obj->bbox.y1)*dststride + (x0-obj->bbox.x1);
    unsigned char *a = obj->alpha_buffer  + (y0-obj->bbox.y1)*dststride + (x0-obj->bbox.x1);
    unsigned char *bs = src;
    unsigned char *as = srca;
	int k=0;

	/* fprintf(stderr,"***w:%d x0:%d bbx1:%d bbx2:%d dstsstride:%d y0:%d h:%d bby1:%d bby2:%d ofs:%d ***\n",w,x0,obj->bbox.x1,obj->bbox.x2,dststride,y0,h,obj->bbox.y1,obj->bbox.y2,(y0-obj->bbox.y1)*dststride + (x0-obj->bbox.x1));*/
    if (x0 < obj->bbox.x1 || x0+w > obj->bbox.x2 || y0 < obj->bbox.y1 || y0+h > obj->bbox.y2)
	  {
	    fprintf(stderr, "WARN: Text out of range: bbox [%d %d %d %d], txt [%d %d %d %d]\n",
		obj->bbox.x1, obj->bbox.x2, obj->bbox.y1, obj->bbox.y2,
		x0, x0+w, y0, y0+h);
		return;
      }

    for (i = 0; i < h; i++)
	  {
	    for (j = 0; j < w; j++, b++, a++, bs++, as++)
		  {
	        if (*b < *bs)
			  *b = *bs;
	        if (*as)
			  {
				if (*a == 0 || *a > *as)
				  *a = *as;
	          }
	      }
	   	k+= dstskip;
		b+= dstskip;
	    a+= dstskip;
	    bs+= srcskip;
	    as+= srcskip;
      }
}

// allocates/enlarges the alpha/bitmap buffer
static void alloc_buf(mp_osd_obj_t* obj)
{
    int len;
	/* fprintf(stderr,"x1:%d x2:%d y1:%d y2:%d\n",obj->bbox.x1,obj->bbox.x2,obj->bbox.y1,obj->bbox.y2); */
	if (obj->bbox.x2 < obj->bbox.x1) obj->bbox.x2 = obj->bbox.x1;
    if (obj->bbox.y2 < obj->bbox.y1) obj->bbox.y2 = obj->bbox.y1;
    obj->stride = ((obj->bbox.x2-obj->bbox.x1)+7)&(~7);
    len = obj->stride*(obj->bbox.y2-obj->bbox.y1);
    if (obj->allocated<len) {
	obj->allocated = len;
	free(obj->bitmap_buffer);
	free(obj->alpha_buffer);
	obj->bitmap_buffer = (unsigned char *)malloc(len);
	obj->alpha_buffer = (unsigned char *)malloc(len);
    }
    memset(obj->bitmap_buffer, sub_bg_color, len);
    memset(obj->alpha_buffer, sub_bg_alpha, len);
}

// vo_draw_text_sub(int dxs,int dys,void (*draw_alpha)(int x0,int y0, int w,int h, unsigned char* src, unsigned char *srca, int stride))

inline static void vo_update_text_sub(mp_osd_obj_t* obj,int dxs,int dys)
{
    unsigned char *t;
    int c,i,j,prev_j,l,x,y,font,prevc,counter;
    int len;
    int k;
    int lastStripPosition;
    int xsize,subs_height;
    int xmin=dxs-sub_left_margin-sub_right_margin,xmax=0;
    int h,lasth;
    int xtblc, utblc;
    obj->flags|=OSDFLAG_CHANGED|OSDFLAG_VISIBLE;

    if(!vo_sub || !vo_font || !sub_visibility){
        obj->flags&=~OSDFLAG_VISIBLE;
        return;
    }
    obj->bbox.y2=obj->y=dys-sub_bottom_margin;
    obj->params.subtitle.lines=0;

    // too long lines divide into a smaller ones
    i=k=lasth=0;
    h=vo_font->height;
    lastStripPosition=-1;
    l=vo_sub->lines;

    {
	struct osd_text_t *osl, *cp_ott, *tmp_ott, *tmp;
	struct osd_text_p *otp_sub = NULL, *otp_sub_tmp=NULL,	// these are used to store the whole sub text osd
            *otp, *tmp_otp, *pmt;	// these are used to manage sub text osd coming from a single sub line
        /* int *char_seq, char_position, xlimit = dxs * sub_width_p / 100, counter; */
        int *char_seq, char_position, xlimit = dxs -sub_right_margin -sub_left_margin, counter;

        while (l) {
	    xsize = -vo_font->charspace;
            l--;
            t=vo_sub->text[i++];
            len=strlen(t)-1;
	    char_position = 0;
	    char_seq = (int *) malloc((len + 1) * sizeof(int));

            prevc = -1;

	    otp = NULL;
	    osl = NULL;
            cp_ott = NULL;
	    x = 1;

	    // reading the subtitle words from vo_sub->text[]
            for (j=0;j<=len;j++){
                if ((c=t[j])>=0x80){
                    if (sub_utf8){
                        if ((c & 0xe0) == 0xc0)    /* 2 bytes U+00080..U+0007FF*/
                            c = (c & 0x1f)<<6 | (t[++j] & 0x3f);
                        else if((c & 0xf0) == 0xe0){ /* 3 bytes U+00800..U+00FFFF*/
                            c = (((c & 0x0f)<<6) | (t[++j] & 0x3f))<<6;
                            c |= (t[++j] & 0x3f);
                        }
                    } else if (sub_unicode)
                        c = (c<<8) + t[++j];
                }
                if (k==MAX_UCS){
                    len=j; // end here
                    fprintf(stderr,"WARN: MAX_UCS exceeded!\n");
                }
                if (!c) c++; // avoid UCS 0
                render_one_glyph(vo_font, c);

		if (c == ' ') {
		    struct osd_text_t *tmp_ott = (struct osd_text_t *) calloc(1, sizeof(struct osd_text_t));

		    if (osl == NULL) {
			cp_ott = tmp_ott;
			osl=cp_ott;
		    } else {
			tmp_ott->prev = cp_ott;
			cp_ott->next = tmp_ott;
			tmp_ott->osd_kerning =
			    vo_font->charspace + vo_font->width[' '];
			cp_ott = tmp_ott;
		    }
		    tmp_ott->osd_length = xsize;
		    tmp_ott->text_length = char_position;
		    tmp_ott->text = (int *) malloc(char_position * sizeof(int));
		    for (counter = 0; counter < char_position; ++counter)
			tmp_ott->text[counter] = char_seq[counter];
		    char_position = 0;
		    xsize = 0;
		    prevc = c;
		} else {
		    int delta_xsize = vo_font->width[c] + vo_font->charspace + kerning(vo_font, prevc, c);

		    if (xsize + delta_xsize <= dxs-sub_right_margin-sub_left_margin) {
			if (!x) x = 1;
			prevc = c;
			char_seq[char_position++] = c;
			xsize += delta_xsize;
			if ((!suboverlap_enabled) && ((font = vo_font->font[c]) >= 0)) {
			    if (vo_font->pic_a[font]->h > h) {
				h = vo_font->pic_a[font]->h;
			    }
			}
		    } else {
			if (x) {
			    fprintf(stderr,"WARN: Subtitle word '%s' too long!\n", t);
			    x = 0;
			}
		    }
		}
            }// for len (all words from subtitle line read)

	    // osl holds an ordered (as they appear in the lines) chain of the subtitle words
	    {
		struct osd_text_t *tmp_ott = (struct osd_text_t *) calloc(1, sizeof(struct osd_text_t));

		if (osl == NULL) {
		    osl = cp_ott = tmp_ott;
		} else {
		    tmp_ott->prev = cp_ott;
		    cp_ott->next = tmp_ott;
		    tmp_ott->osd_kerning =
			vo_font->charspace + vo_font->width[' '];
		    cp_ott = tmp_ott;
		}
		tmp_ott->osd_length = xsize;
		tmp_ott->text_length = char_position;
		tmp_ott->text = (int *) malloc(char_position * sizeof(int));
		for (counter = 0; counter < char_position; ++counter)
		    tmp_ott->text[counter] = char_seq[counter];
		char_position = 0;
		xsize = -vo_font->charspace;
	    }
            if (osl != NULL) {
		int value = 0, exit1 = 0, minimum = 0;

		// otp will contain the chain of the osd subtitle lines coming from the single vo_sub line.
		otp = tmp_otp = (struct osd_text_p *) calloc(1, sizeof(struct osd_text_p));
		tmp_otp->ott = osl;
		for (tmp_ott = tmp_otp->ott; exit1 == 0; ) {
                    while ((tmp_ott != NULL) && (value + tmp_ott->osd_kerning + tmp_ott->osd_length <=xlimit)) {
			value += tmp_ott->osd_kerning + tmp_ott->osd_length;
			tmp_ott = tmp_ott->next;
		    }
                    if (tmp_ott != NULL) {
			struct osd_text_p *tmp = (struct osd_text_p *) calloc(1, sizeof(struct osd_text_p));

			tmp_otp->value = value;
			tmp_otp->next = tmp;
			tmp->prev = tmp_otp;
			tmp_otp = tmp;
			tmp_otp->ott = tmp_ott;
			value = -2 * vo_font->charspace - vo_font->width[' '];
		    } else {
			tmp_otp->value = value;
			exit1 = 1;
		    }
		}

#ifdef NEW_SPLITTING
		// minimum holds the 'sum of the differences in lenght among the lines',
		// a measure of the eveness of the lenghts of the lines
		for (tmp_otp = otp; tmp_otp->next != NULL; tmp_otp = tmp_otp->next) {
		    pmt = tmp_otp->next;
		    while (pmt != NULL) {
			minimum += abs(tmp_otp->value - pmt->value);
			pmt = pmt->next;
		    }
		}

		if (otp->next != NULL) {
		    int mem1, mem2;
		    struct osd_text_p *mem, *hold;

		    exit1 = 0;
		    // until the last word of a line can be moved to the beginning of following line
		    // reducing the 'sum of the differences in lenght among the lines', it is done
		    while (exit1 == 0) {
			hold = NULL;
			exit1 = 1;
			for (tmp_otp = otp; tmp_otp->next != NULL; tmp_otp = tmp_otp->next) {
			    pmt = tmp_otp->next;
			    for (tmp = tmp_otp->ott; tmp->next != pmt->ott; tmp = tmp->next);
			    if (pmt->value + tmp->osd_length + pmt->ott->osd_kerning <= xlimit) {
				mem1 = tmp_otp->value;
				mem2 = pmt->value;
				tmp_otp->value = mem1 - tmp->osd_length - tmp->osd_kerning;
				pmt->value = mem2 + tmp->osd_length + pmt->ott->osd_kerning;

				value = 0;
				for (mem = otp; mem->next != NULL; mem = mem->next) {
				    pmt = mem->next;
				    while (pmt != NULL) {
					value += abs(mem->value - pmt->value);
					pmt = pmt->next;
				    }
				}
				if (value < minimum) {
				    minimum = value;
				    hold = tmp_otp;
				    exit1 = 0;
				}
				tmp_otp->value = mem1;
				tmp_otp->next->value = mem2;
			    }
			}
			// merging
			if (exit1 == 0) {
			    tmp_otp = hold;
			    pmt = tmp_otp->next;
			    for (tmp = tmp_otp->ott; tmp->next != pmt->ott; tmp = tmp->next);
			    mem1 = tmp_otp->value;
			    mem2 = pmt->value;
			    tmp_otp->value = mem1 - tmp->osd_length - tmp->osd_kerning;
			    pmt->value = mem2 + tmp->osd_length + pmt->ott->osd_kerning;
			    pmt->ott = tmp;
			}//~merging
		    }//~while(exit1 == 0)
		}//~if(otp->next!=NULL)
#endif

		// adding otp (containing splitted lines) to otp chain
		if (otp_sub == NULL) {
		    otp_sub = otp;
		    for (otp_sub_tmp = otp_sub; otp_sub_tmp->next != NULL; otp_sub_tmp = otp_sub_tmp->next);
		} else {
		    //updating ott chain
		    tmp = otp_sub->ott;
		    while (tmp->next != NULL) tmp = tmp->next;
		    tmp->next = otp->ott;
		    otp->ott->prev = tmp;
		    //attaching new subtitle line at the end
		    otp_sub_tmp->next = otp;
		    otp->prev = otp_sub_tmp;
		    do
			otp_sub_tmp = otp_sub_tmp->next;
		    while (otp_sub_tmp->next != NULL);
		}
	    }//~ if(osl != NULL)
	} // while
	// write lines into utbl
	xtblc = 0;
	utblc = 0;
	obj->y = dys - sub_bottom_margin;
	obj->params.subtitle.lines = 0;
	for (tmp_otp = otp_sub; tmp_otp != NULL; tmp_otp = tmp_otp->next) {

	    if ((obj->params.subtitle.lines++) >= MAX_UCSLINES)
            {fprintf(stderr,"WARN: max_ucs_lines\n");
            break;}
            if (h+sub_top_margin > obj->y) {	// out of the screen so end parsing
		obj->y -= lasth - vo_font->height;	// correct the y position
		fprintf(stderr,"WARN: Out of screen at Y: %d\n",obj->y);
		obj->params.subtitle.lines=obj->params.subtitle.lines-1;
		break;
	    }
	    xsize = tmp_otp->value;
	    obj->params.subtitle.xtbl[xtblc++] = ((dxs-sub_right_margin-sub_left_margin - xsize) / 2)+sub_left_margin;
	    if (xmin > (((dxs-sub_right_margin-sub_left_margin - xsize) / 2)+sub_left_margin))
		xmin = ((dxs-sub_right_margin-sub_left_margin - xsize) / 2)+sub_left_margin;
            if (xmax < (((dxs-sub_right_margin-sub_left_margin + xsize) / 2)+sub_left_margin))
		xmax = ((dxs-sub_right_margin-sub_left_margin + xsize) / 2)+sub_left_margin;
/* 		fprintf(stderr,"lm %d rm: %d xm:%d xs:%d\n",sub_left_margin,sub_right_margin,xmax,xsize); */
	    tmp = (tmp_otp->next == NULL) ? NULL : tmp_otp->next->ott;
	    for (tmp_ott = tmp_otp->ott; tmp_ott != tmp; tmp_ott = tmp_ott->next) {
		for (counter = 0; counter < tmp_ott->text_length; ++counter) {
		    if (utblc > MAX_UCS) {
			break;
		    }
		    c = tmp_ott->text[counter];
		    render_one_glyph(vo_font, c);
		    obj->params.subtitle.utbl[utblc++] = c;
		    k++;
		}
		obj->params.subtitle.utbl[utblc++] = ' ';
	    }
	    obj->params.subtitle.utbl[utblc - 1] = 0;
            obj->y -= vo_font->height;
	}
	if ( sub_max_lines<obj->params.subtitle.lines)
            sub_max_lines=obj->params.subtitle.lines;
	if ( sub_max_font_height<vo_font->height)
            sub_max_font_height=vo_font->height;
	if ( sub_max_bottom_font_height<vo_font->pic_a[vo_font->font[40]]->h)
            sub_max_bottom_font_height=vo_font->pic_a[vo_font->font[40]]->h;
        if(obj->params.subtitle.lines)
	    obj->y = dys - sub_bottom_margin -((obj->params.subtitle.lines) * vo_font->height); /* + vo_font->pic_a[vo_font->font[40]]->h);*/

	// free memory
	if (otp_sub != NULL) {
	    for (tmp = otp_sub->ott; tmp->next != NULL; free(tmp->prev)) {
		free(tmp->text);
		tmp = tmp->next;
	    }
	    free(tmp->text);
	    free(tmp);

	    for(pmt = otp_sub; pmt->next != NULL; free(pmt->prev)) {
		pmt = pmt->next;
	    }
	    free(pmt);
	} else {
            fprintf(stderr,"WARN: Subtitles requested but not found.\n");
	}

    }

    subs_height=((obj->params.subtitle.lines-1) * vo_font->height)+vo_font->pic_a[vo_font->font[40]]->h;
    /* fprintf(stderr,"^1 bby1:%d bby2:%d h:%d dys:%d oy:%d sa:%d sh:%d f:%d\n",obj->bbox.y1,obj->bbox.y2,h,dys,obj->y,sub_alignment,subs_height,font); */
    if (sub_alignment == 2)
        obj->y = dys * sub_pos / 100 - sub_bottom_margin-subs_height;
    else if (sub_alignment == 1)
        obj->y = (((dys * sub_pos / 100) - sub_bottom_margin - sub_top_margin - subs_height + vo_font->height))/2;
    else
        obj->y = sub_top_margin;
    if (obj->y < sub_top_margin)
        obj->y = sub_top_margin;
    if (obj->y > dys - sub_bottom_margin-vo_font->height)
        obj->y = dys - sub_bottom_margin-vo_font->height;

    obj->bbox.y2 = obj->y + subs_height+3;

    // calculate bbox:
    if (sub_justify) xmin = sub_left_margin;
    obj->bbox.x1=xmin-3;
    obj->bbox.x2=xmax+3+vo_font->spacewidth;

    /* if ( obj->bbox.x2>=dxs-sub_right_margin-20)
       {
       obj->bbox.x2=dxs;
       }

    */
    obj->bbox.y1=obj->y-3;
//    obj->bbox.y2=obj->y+obj->params.subtitle.lines*vo_font->height;
    obj->flags|=OSDFLAG_BBOX;

    alloc_buf(obj);
    y = obj->y;
    /* fprintf(stderr,"^2 bby1:%d bby2:%d h:%d dys:%d oy:%d sa:%d sh:%d\n",obj->bbox.y1,obj->bbox.y2,h,dys,obj->y,sub_alignment,subs_height); */


    switch(vo_sub->alignment) {
    case SUB_ALIGNMENT_HLEFT:
        obj->alignment |= 0x1;
        break;
    case SUB_ALIGNMENT_HCENTER:
        obj->alignment |= 0x0;
        break;
    case SUB_ALIGNMENT_HRIGHT:
    default:
        obj->alignment |= 0x2;
    }
    i=j=prev_j=0;
    if ((l = obj->params.subtitle.lines))
    {
        for(counter = dxs-sub_right_margin-sub_left_margin; i < l; ++i)
	    if (obj->params.subtitle.xtbl[i] < counter) counter = obj->params.subtitle.xtbl[i];
        for (i = 0; i < l; ++i)
        {
            switch (obj->alignment&0x3)
            {
            case 1:
                // left
                if ( sub_justify)
                    x=xmin;
                else
                    x = counter;
                break;
            case 2:
                // right
                x = 2 * obj->params.subtitle.xtbl[i] - counter - ((obj->params.subtitle.xtbl[i] == counter) ? 0 : 1);
                break;
            default:
                //center
                x = obj->params.subtitle.xtbl[i];
            }
            prevc = -1;
            while ((c=obj->params.subtitle.utbl[j++]))
            {
                x += kerning(vo_font,prevc,c);
                if (((font=vo_font->font[c])>=0))
                {
                    /* fprintf(stderr,"^3 vfh:%d vfh+y:%d odys:%d\n",vo_font->pic_a[font]->h,vo_font->pic_a[font]->h+y,obj->dys); */
                    draw_alpha_buf(obj,x,y,
                                   vo_font->width[c],
                                   vo_font->pic_a[font]->h+y<obj->dys-sub_bottom_margin ? vo_font->pic_a[font]->h : obj->dys-sub_bottom_margin-y,
                                   vo_font->pic_b[font]->bmp+vo_font->start[c],
                                   vo_font->pic_a[font]->bmp+vo_font->start[c],
                                   vo_font->pic_a[font]->w);
                }
                x+=vo_font->width[c]+vo_font->charspace;
                prevc = c;
            }
            if ( sub_max_chars<(j-prev_j))
                sub_max_chars=(j-prev_j);
            prev_j=j;
            y+=vo_font->height;
        }
        /* Here you could retreive the buffers*/

    }
}

mp_osd_obj_t* new_osd_obj(int type){
    mp_osd_obj_t* osd=malloc(sizeof(mp_osd_obj_t));
    memset(osd,0,sizeof(mp_osd_obj_t));
    osd->next=vo_osd_list;
    vo_osd_list=osd;
    osd->type=type;
    osd->alpha_buffer = NULL;
    osd->bitmap_buffer = NULL;
    osd->allocated = -1;
    return osd;
}

void free_osd_list(){
    mp_osd_obj_t* obj=vo_osd_list;
    while(obj){
	mp_osd_obj_t* next=obj->next;
	if (obj->alpha_buffer) free(obj->alpha_buffer);
	if (obj->bitmap_buffer) free(obj->bitmap_buffer);
	free(obj);
	obj=next;
    }
    vo_osd_list=NULL;
}

int vo_update_osd(int dxs,int dys){
    mp_osd_obj_t* obj=vo_osd_list;
    int chg=0;

#ifdef HAVE_FREETYPE
    // here is the right place to get screen dimensions
    if (!vo_font || force_load_font) {
	force_load_font = 0;
	load_font_ft(dxs, dys);
    }
#endif

    while(obj){
        if(dxs!=obj->dxs || dys!=obj->dys || obj->flags&OSDFLAG_FORCE_UPDATE) {
            int vis;
            obj->flags=obj->flags|OSDFLAG_VISIBLE;
            vis=obj->flags&OSDFLAG_VISIBLE;
            obj->flags&=~OSDFLAG_BBOX;
            switch(obj->type){
            case OSDTYPE_SUBTITLE:
		if ( vo_sub)
		{
                    obj->dxs=dxs; obj->dys=dys;
                    vo_update_text_sub(obj,dxs,dys);
                    /* obj->dxs=dxs; obj->dys=dys;
                       fprintf(stderr,"x1:%d x2:%d y1:%d y2:%d\n",obj->bbox.x1,obj->bbox.x2,obj->bbox.y1,obj->bbox.y2); */
                    vo_draw_alpha_rgb24( (obj->bbox.x2)-(obj->bbox.x1),(obj->bbox.y2)-(obj->bbox.y1),obj->bitmap_buffer,obj->alpha_buffer,obj->stride,image_buffer+(3*obj->bbox.x1)+(3*(obj->bbox.y1)*movie_width),movie_width*3);
		}
		break;
            }
            // check if visibility changed:
            if(vis != (obj->flags&OSDFLAG_VISIBLE) ) obj->flags|=OSDFLAG_CHANGED;
            // remove the cause of automatic update:

            obj->flags&=~OSDFLAG_FORCE_UPDATE;
        }
        if(obj->flags&OSDFLAG_CHANGED){
            chg|=1<<obj->type;
            /* fprintf(stderr,"DEBUG:OSD chg: %d  V: %s  \n",obj->type,(obj->flags&OSDFLAG_VISIBLE)?"yes":"no"); */
        }
        obj=obj->next;
    }
    return chg;
}

void vo_init_osd(){
    if(vo_osd_list) free_osd_list();
    // temp hack, should be moved to mplayer/mencoder later
    /*new_osd_obj(OSDTYPE_OSD);*/
    new_osd_obj(OSDTYPE_SUBTITLE);
  /*  new_osd_obj(OSDTYPE_PROGBAR);
    new_osd_obj(OSDTYPE_SPU); */
#ifdef HAVE_FREETYPE
    force_load_font = 1;
#endif
}

int vo_osd_changed(int new_value)
{
    mp_osd_obj_t* obj=vo_osd_list;
    int ret = vo_osd_changed_status;
    vo_osd_changed_status = new_value;

    while(obj){
	if(obj->type==new_value) obj->flags|=OSDFLAG_FORCE_UPDATE;
	obj=obj->next;
    }

    return ret;
}

