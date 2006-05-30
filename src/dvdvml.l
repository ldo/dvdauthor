%{

/*
 * Copyright (C) 2002 Scott Smith (trckjunky@users.sourceforge.net)
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

#include "dvdvm.h"
#include "dvdvmy.h"

static const char RCSID[]="$Id: //depot/dvdauthor/src/dvdvml.l#2 $";

%}

%option noyywrap
%option never-interactive
%option nounput

blank           [ \t\n]
ws              {blank}+ 
letter          [a-zA-Z_]
digit           [0-9]
identifier      {letter}({letter}|{digit})* 
int_num         {digit}+ 

%%

and		{ return _AND_TOK; }
angle		{ return ANGLE_TOK; }
audio		{ return AUDIO_TOK; }
button		{ return BUTTON_TOK; }
call            { return CALL_TOK; }
cell		{ return CELL_TOK; }
chapter		{ return CHAPTER_TOK; }
else		{ return ELSE_TOK; }
entry		{ return ENTRY_TOK; }
eq		{ return EQ_TOK; }
exit		{ return EXIT_TOK; }
fpc		{ return FPC_TOK; }
ge		{ return GE_TOK; }
gt		{ return GT_TOK; }
if		{ return IF_TOK; }
jump		{ return JUMP_TOK; }
le		{ return LE_TOK; }
lt		{ return LT_TOK; }
menu		{ return MENU_TOK; }
ne		{ return NE_TOK; }
not		{ return NOT_TOK; }
or		{ return _OR_TOK; }
program		{ return PROGRAM_TOK; }
ptt		{ return PTT_TOK; }
resume		{ return RESUME_TOK; }
root		{ return ROOT_TOK; }
subtitle	{ return SUBTITLE_TOK; }
title		{ return TITLE_TOK; }
titleset	{ return TITLESET_TOK; }
vmgm		{ return VMGM_TOK; }
xor		{ return XOR_TOK; }


{ws}		;

"="		{ return SET_TOK; }

"+"		{ return ADD_TOK; }
"-"		{ return SUB_TOK; }
"*"		{ return MUL_TOK; }
"/"		{ return DIV_TOK; }
"%"		{ return MOD_TOK; }
"&"		{ return BAND_TOK; }
"|"		{ return BOR_TOK;  }
"^"		{ return XOR_TOK; }

"+="		{ return ADDSET_TOK; }
"-="		{ return SUBSET_TOK; }
"*="		{ return MULSET_TOK; }
"/="		{ return DIVSET_TOK; }
"%="		{ return MODSET_TOK; }
"&="		{ return ANDSET_TOK; }
"|="		{ return ORSET_TOK;  }
"^="		{ return XORSET_TOK; }

"=="		{ return EQ_TOK; }
"!="		{ return NE_TOK; }
">="		{ return GE_TOK; }
">"		{ return GT_TOK; }
"<="		{ return LE_TOK; }
"<"		{ return LT_TOK; }

"&&"		{ return LAND_TOK; }
"||"		{ return LOR_TOK; }
"!"		{ return NOT_TOK; }

";"             { return SEMICOLON_TOK; }
"{"		{ return OPENBRACE_TOK; }
"}"		{ return CLOSEBRACE_TOK; }
"("		{ return OPENPAREN_TOK; }
")"		{ return CLOSEPAREN_TOK; }

{int_num}	{ sscanf((char *)yytext,"%d",&dvdvmlval.int_val);
                  return NUM_TOK; }

g{int_num}	{ sscanf((char *)yytext+1,"%d",&dvdvmlval.int_val);
		  if( dvdvmlval.int_val<0 || dvdvmlval.int_val>=13 ) {
			fprintf(stderr,"ERR:  Can only access g0-g12 (%s)\n",yytext);
			return ERROR_TOK;
		  }
		  return G_TOK; }

s{int_num}	{ sscanf((char *)yytext+1,"%d",&dvdvmlval.int_val);
		  if( dvdvmlval.int_val<0 || dvdvmlval.int_val>23 ) {
			fprintf(stderr,"ERR:  Can only access s0-s23 (%s)\n",yytext);
			return ERROR_TOK;
		  }		
		  return S_TOK; }

{identifier}    { dvdvmlval.str_val = strdup((char *)yytext);
                  return ID_TOK; }


.		{ return ERROR_TOK; }

%%
