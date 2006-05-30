/*
 * Copyright (C) 2002, 2003 Jan Panteltje <panteltje@yahoo.com>
 * With many changes by Scott Smith (trckjunky@users.sourceforge.net)
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

#include "config.h"

#include "compat.h"

#include <assert.h>

#include "subgen.h"

static const char RCSID[]="$Id: //depot/dvdauthor/src/subgen-encode.c#12 $";

static int remainbit,subo;

static void store_init()
{
    memset(sub,0,SUB_BUFFER_MAX);
    remainbit=8;
    subo=0;
}

static int bitmask[9]={0,1,3,7,15,31,63,127,255};
static void store_bits(unsigned int val,int bits)
{
    if( subo>SUB_BUFFER_MAX )
        return;
    while(bits) {
        if( bits>remainbit ) {
            sub[subo++]|=(val>>(bits-remainbit))&bitmask[remainbit];
            bits-=remainbit;
            remainbit=8;
        } else if( bits==remainbit ) {
            sub[subo++]|=val&bitmask[remainbit];
            remainbit=8;
            return;
        } else {
            sub[subo]|=(val&bitmask[bits])<<(remainbit-bits);
            remainbit-=bits;
            return;
        }
    }
}

static void store_2bit(int val)
{
    assert(val>=0 && val<=3);
    store_bits(val,2);
}

static void store_nibble(int val)
{
    assert(val>=0 && val<=15);
    store_bits(val,4);
}


static void store_trinibble(int val)
{
    store_nibble((val>>8)&15);
    store_nibble((val>>4)&15);
    store_nibble(val&15);
}

static void store_align()
{
    if( remainbit!=8 )
        store_bits(0,remainbit);
}

static void store_1(int val)
{
    store_bits(val,8);
}

static void store_2(int val)
{
    store_bits(val,16);
}

static void store_4(unsigned int val)
{
    store_bits(val,32);
}

static void svcd_rotate(stinfo *s)
{
    int cst[4],i,j;

    for( i=0; i<4; i++ )
        cst[i]=0;
    for( i=0; i<s->xd*s->yd; i++ )
        cst[s->fimg[i]]++;
    j=0;
    for( i=1; i<4; i++ )
        if( cst[i]>cst[j] )
            j=i;
    if( j!=0 ) {
        palt p[4];

        for( i=0; i<s->xd*s->yd; i++ )
            s->fimg[i]=(s->fimg[i]-j)&3;
        for( i=0; i<4; i++ )
            p[i]=s->pal[(i+j)&3];
        memcpy(s->pal,p,4*sizeof(palt));
    }
}

int svcd_encode(stinfo *s)
{
    unsigned int x,y,c,l2o;
    palt *epal=s->img.pal;

    svcd_rotate(s);

    store_init();
    subo = 2;
    if (s->sd != -1)
    {
	store_2(0x2e00);
        store_4(s->sd);
    }
    else
    {
	store_2(0x2600);
    } 
 
    if (debug > 2)
	fprintf(stderr,\
                "sd: %d   xd: %d  yd: %d  x0: %d  y0: %d\n", s->sd, s->xd, s->yd, s->x0, s->y0); 

    store_2(s->x0);
    store_2(s->y0);
    store_2(s->xd);
    store_2(s->yd);
    for(c = 0;c<4;c++)
    {
	store_1(calcY(&epal[c]));
	store_1(calcCr(&epal[c]));
	store_1(calcCb(&epal[c]));
	store_1(epal[c].t);
    } 
 
    store_1(0); //?????
 
    l2o = subo;
    subo += 2;
    y = 0;
 odd_row: 
    for(; y<s->yd; y += 2)
    {
	for(x = 0; x<s->xd;x++)
        {
            if ((c = s->fimg[y*s->xd+x]) != 0) store_2bit(c);
            else
            {
                c = 1;
                while (((++x)<s->xd)  &&  (!s->fimg[y*s->xd+x])) c++;
                x--;
                while (c>4)
                {
                    store_nibble(3);
                    c -= 4;
                }
                store_nibble(c-1);
            }
        }
        store_align();
    }
 
    if (!(y&1))
    {
	if (!(subo&1))
        {
            if (debug>3)
                fprintf(stderr,\
			"padded betweed fields with 1 byte to %d\n",subo%4);
            store_1( 0 );
        }
	y = 1;
	sub[l2o] = (subo - l2o - 2) >> 8;
	sub[l2o+1] = (subo - l2o - 2);
	goto odd_row;
    } 
 
    store_1( 0 );// no additional commands
    c = 0;
    while (subo&3)
    {
	store_1( 0 ); c++;
    }
    if (debug>3) fprintf(stderr,"padded with %d byte\n",c);

    sub[0] = subo >> 8;
    sub[1] = subo;
    if (subo == 65536) return -1;
    else return subo;
} /* end function svcd_encode */


int cvd_encode(stinfo *s)
{
    unsigned int x, y, c, d;
    palt *epal=s->pal;
    int ofs, ofs1=0;
 
    store_init();
    subo = 4;
    ofs = 4;

    c = 0;
    for(y = 0; y < s->yd; y += 2)
    {
    odd_row_cvd: 
	for(x = 0; x < s->xd;)
        {
            d = s->fimg[y * s->xd + x];
            c = 1;
            while (((++x) < s->xd)  &&  (s->fimg[y * s->xd + x] == d) ) c++;
            if(x == s->xd)
            {
                store_nibble(0);
                store_nibble(d);
                store_align();
                continue;
            }
	   
            while(c > 3)
            {
                store_nibble(12 + d);
                c -= 3;
            }
   
            store_nibble((c << 2) + d);
        }
    }
 
    if(!(y & 1))
    {
	y = 1;
	ofs1 = subo;
	goto odd_row_cvd;
    }  
 
    sub[2] = subo >> 8;
    sub[3] = subo;

/* setting this to all 0xff then no more subtitles */
    store_1( 0x0c );
    store_1( 0 );
    store_1( 0 );
    store_1( 0 );
 
/* set pallette 0-3 */
    for(c = 0; c < 4; c++)
    {
//#define nco if (subo<65536) sub[subo++]
	store_1( 0x24 + c );
	if(debug > 3)
        {
            fprintf(stderr, "c=%d R=%.2f G=%.2f B=%.2f\n",\
                    c, (double)epal[c].r, (double)epal[c].g, (double)epal[c].b);
        }

	store_1( calcY(&epal[c]) );
	store_1( calcCr(&epal[c]) );
	store_1( calcCb(&epal[c]) );

    } /* end for pallette 0-3 */
 
/* sethighlight  pallette  */
    for(c = 0; c < 4; c++)
    {
	store_1( 0x2c + c );
	if(debug > 3)
        {
            fprintf(stderr, "c=%d R=%.2f G=%.2f B=%.2f\n",\
                    c, (double)epal[c].r, (double)epal[c].g, (double)epal[c].b);
        }

	store_1( calcY(&epal[c]) );
	store_1( calcCr(&epal[c]) );
	store_1( calcCb(&epal[c]) );
    } /* end for pallette 4-7 */

    if(debug > 3)
    {
	fprintf(stderr,\
                "epal[0].t=%d epal[1].t=%d epal[2].t=%d epal[3].t=%d\n",\
                epal[0].t, epal[1].t, epal[2].t, epal[3].t);
    }

/* x0, y0 */
    store_trinibble( 0x17f );
    store_bits(s->x0,10);
    store_bits(s->y0,10);
 
/* xd, yd */
    store_trinibble( 0x1ff );
    store_bits(s->x0+s->xd-1,10);
    store_bits(s->y0+s->yd-1,10);
 
/* 0x37 is pallette.t 0-3 contrast */
    store_2( 0x37ff );
//nco = 0x00;
//nco = 0xff;
    store_nibble( epal[3].t >> 4 );
    store_nibble( epal[2].t >> 4 );
    store_nibble( epal[1].t >> 4 );
    store_nibble( epal[0].t >> 4 );

    if(debug > 3)
    {
	fprintf(stderr, "EPALS nco0(2 3h, 2l)=%02x nco1(2 1h,0l)=%02x\n",\
                sub[subo - 1], sub[subo - 2]);
    }

/* 0x3f is high light pallette.t 4-7 contrast */
    store_2( 0x3fff );
    store_2( 0xfff0 );

/* ofs is 4 ? is offset in bitmap to first field data (interlace) */ 
    store_2( 0x47ff );
    store_2( ofs );
 
/* ofs1 is offset to other field in bitmap (interlace) */
    store_2( 0x4fff );
    store_2( ofs1 );
 
/* unknown!!! (in RX too!) setting all to 0xff keeps the pic on screen! */
    store_1( 0x0c );
    store_1( 0 );
    store_1( 0 );
    store_1( 0 );
 
/* sd, time in display, duration */
    store_1( 0x04 );
    store_bits( s->sd, 24 );
 
// IA3
//0: 02 68 02 24
//1: 08 61 08 1d
//2: 08 6b 08 27
//3: 07 6b 07 27
//4: 05 40 04 fc
//5: 08 81 08 3d
//6: 05 4f 15 40


    sub[0] = subo >> 8;
    sub[1] = subo;
    store_1(4);
    store_1(8);
    store_1(12);
    store_1(16);

    if(subo == 65536) return -1;
    else return subo;
} /* end function cvd_encode */


static void do_rle(int count, int color)
{
    int a;

    /* argument check */
    assert(count>=1 && count<=255);
    assert(color>=0 && color<=3);

    /* make rle code in b */
    a = (count << 2) | color;

    /* a now ranges from 0x4 up, because count is at least 1 */

    if(count >= 64)		// 64 - 255
    {
	/* 64-255, 16 bits, 0 0 0 0  0 0 n n  n n n n  n n c c */
	store_nibble(0);
	store_nibble( (a & 0xf00) >> 8);
	store_nibble( (a & 0xf0) >> 4);
	store_nibble( a & 0xf);
    }
    else if(count >= 16)	// 16 - 63
    {
	/* 16 - 63, 12 bits, 0 0 0 0  n n n n  n n c c */
	store_nibble( 0);
	store_nibble( (a & 0xf0) >> 4);
	store_nibble( a & 0xf);
    }
    else if(count >= 4)		// 4 - 15
    {
	/* 4-15, 8 bits, 0 0 n n  n n c c */
	store_nibble( (a & 0xf0) >> 4);
	store_nibble( a & 0xf);
    }
    else			// 1 - 3
    {
	/* 1-3, 4 bits, n n c c */
	store_nibble( a & 0xf );
    }
} /* end function do_rle */


static void dvd_encode_row(int y,int xd,unsigned char *icptr)
{
    int new_pos, x;
    int osubo=subo;

    new_pos = 0;
    icptr+=y*xd;
    for(x = 0; x < xd-1; x++)
    {
        /* get color from x,y */

        if(icptr[x + 1] != icptr[x])
        {
            int count=x+1-new_pos;
            while(count>255) {
                do_rle(255,icptr[x]);
                count-=255;
            }
            if( count )
                do_rle(count,icptr[x]);
            new_pos = x + 1;
        }
    } /* end for x */

    /*
      One special case,
      encoding a count of zero using the 16-bit format,
      indicates the same pixel value until the end of the line. 
    */
	
    if( xd != new_pos )
    {
        int count=xd-new_pos;

        if( count < 64 )
            do_rle(count,icptr[new_pos]);
        else {
            /* send same colors to end of line */
            store_nibble(0);
            store_nibble(0);
            store_nibble(0);
            store_nibble(icptr[new_pos]);
        }
    }

    /*
      If, at the end of a line, the bit count is not a multiple of 8, four fill bits of 0 are added.
    */

    store_align();

    if( subo-osubo >= 1440/8 ) {
        fprintf(stderr,"ERR: Encoded row takes more than 1440 bits.  Please simplify subtitle.\n");
        exit(1);
    }
}

int dvd_encode(stinfo *s)
{
    int a;
    int xstart, xsize;
    int ystart, ysize;
    int next_command_ptr;
    int offset0, offset1;
    int y;
    unsigned char *icptr;

    xstart = s->x0;
    xsize = s->xd;
    ystart = s->y0;
    ysize = s->yd;


/*
  720 x 576 = 414720 bytes, for a 2 bit bitmap = 103680 bytes, compressing MUST reduce this to less then 65536 bytes,
  or a 2 byte number - overhead 65535 - 65507 = 28 bytes, as max. 65507 is OK).
  This 23 bytes control buffer, 6 bytes end sequence?
*/

    /* encode the .ppm to DVD run length encoded format */
    /* for all bytes in img */

/*
  (X >> 2) is the number of pixels to display, and (X & 0x3)
  is the color of the pixel.
*/

    icptr = s->fimg;  
//icptr = img;// use if call to imgfix() commented out

    store_init();
    subo=0;

    //2 bytes packet size, to be filled in later
    //2 bytes pointer to control area, to be filled in later
    subo += 4;

    /* copy image data to sub */
    offset0=subo;
    for( y=0; y<s->yd; y+=2 )
        dvd_encode_row(y,s->xd,icptr);

    offset1=subo;
    for( y=1; y<s->yd; y+=2 )
        dvd_encode_row(y,s->xd,icptr);

    /* start first command block */
/* 
   control area starts here, with same pointer
   SP_DCSQT
   Sub-Picture Display Control SeQuence Table
   This area contains blocks (SP_DCSQ) of commands to the decoder.
   Each SP_DCSQ begins with a 2 word header

   offset  name            contents
   0       SP_DCSQ_STM     delay to wait before executing these commands.
   The units are 90KHz clock (same as PTM) divided by 1024 - see below.

   2       SP_NXT_DCSQ_SA  offset within the Sub-Picture Unit to the next SP_DCSQ.
   If this is the last SP_DCSQ, it points to itself.

   Converting frames and time to SP_DCSQ_STM values
   The direct method of converting time to delay values is to multiply time in seconds by 90000/1024 and
   truncate the value.
   Rounding up will cause the display to occur one frame late.
*/

    /* set pointer to this command block */
    a = subo;
    sub[2] = a >> 8;
    sub[3] = a;

    store_2(0); // delay to wait before executing this command block

    /* remember position, will set later */
    next_command_ptr = subo;
    subo+=2; // pointer to next command block, 2 bytes, to be filled in from next command block. 

    
    if( s->forced )
        store_1(0); /* command 0, forced start display, 1 byte. */
    else
        store_1(1); /* command 1, start display, 1 byte */

    /* selected palettes for pixel value */

    /* command 3, palette, 3 bytes */
        store_1(3);
    store_nibble(findmasterpal(s,&s->pal[3]));
    store_nibble(findmasterpal(s,&s->pal[2]));
    store_nibble(findmasterpal(s,&s->pal[1]));
    store_nibble(findmasterpal(s,&s->pal[0]));

    /* command 4, alpha blend, contrast / transparency t_palette, 3 bytes  0, 15, 15, 15 */

    store_1(4);
    store_nibble(s->pal[3].t>>4);
    store_nibble(s->pal[2].t>>4);
    store_nibble(s->pal[1].t>>4);
    store_nibble(s->pal[0].t>>4);

    /* command 5, display area, 7 bytes from: startx, xsize, starty, ysize */

    store_1(5);
    store_trinibble(xstart);
    store_trinibble(xstart+xsize-1);
    store_trinibble(ystart);
    store_trinibble(ystart+ysize-1);

    /* command 6, image offsets, 5 bytes */
    store_1(6);

    store_2(offset0);
    store_2(offset1);

    /* command 0xff, end command block, 1 byte, */
    store_1(0xff);

    /* end first command block */


    /* start second command block */

    if( s->sd>=0 ) {
        int duration;

        /* set pointer in previous block to point to this position */
        a = subo;
        sub[next_command_ptr+0] = a >> 8;
        sub[next_command_ptr+1] = a;

        /* update pointer to this command block */
        next_command_ptr = subo;

        /* delay to wait before executing next comand */
        duration = (s->sd+512)/1024;
        while( duration >= 65536 ) {
            store_2(65535);
            duration-=65535;
            store_2(next_command_ptr+5);
            store_1(0xFF);
            next_command_ptr = subo;
        }
            
        store_2(duration);

        /* last block, point to self */
        store_2(next_command_ptr);

        /* stop command (executed after above delay) */
        store_1(0x02);

        /* end command block command */
        store_1(0xff);
    } else {
        a=next_command_ptr-2;
        sub[next_command_ptr+0] = a >> 8;
        sub[next_command_ptr+1] = a;
    }

    /* end second commmand block */


    /* make size even if odd */
    if(subo & 1)
    {
	/* only if odd length, to make it even */
	store_1(0xff);
    }

    /* set subtitle packet size */
    a = subo;
    sub[0] = a >> 8;
    sub[1] = a;

    if( a >= SUB_BUFFER_MAX )
        return -1;
    return a;
} /* end function dvd_encode */
