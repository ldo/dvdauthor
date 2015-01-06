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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA.
 */

#include "config.h"
#include "compat.h" /* needed for bool */
#include "dvdvm.h"
#include "dvdvmy.h"


%}

%option noyywrap
%option never-interactive
%option nounput

blank           [ \t\n]
ws              {blank}+
letter          [a-zA-Z_]
digit           [0-9]
identifier      {letter}({letter}|{digit})*
hex_num         0x[0-9a-fA-F]+
int_num         {digit}+
lang_code       \"[a-z]{2}\"

%%

"/*" {
        bool seen_star = false;
        for (;;)
          {
            const int c = input();
            if (c == EOF)
              {
                fprintf(stderr, "EOF in comment");
                return ERROR_TOK;
              } /*if*/
            if (c == '/' && seen_star)
                break;
            seen_star = c == '*';
          } /*for*/
    }

and     { return _AND_TOK; }
angle       { return ANGLE_TOK; }
audio       { return AUDIO_TOK; }
break           { return BREAK_TOK; }
button      { return BUTTON_TOK; }
call            { return CALL_TOK; }
cell        { return CELL_TOK; }
chapter     { return CHAPTER_TOK; }
counter         { return COUNTER_TOK; }
else        { return ELSE_TOK; }
entry       { return ENTRY_TOK; }
eq      { return EQ_TOK; }
exit        { return EXIT_TOK; }
fpc     { return FPC_TOK; }
ge      { return GE_TOK; }
goto            { return GOTO_TOK; }
gt      { return GT_TOK; }
if      { return IF_TOK; }
jump        { return JUMP_TOK; }
le      { return LE_TOK; }
lt      { return LT_TOK; }
menu        { return MENU_TOK; }
ne      { return NE_TOK; }
next            { return NEXT_TOK; }
not     { return NOT_TOK; }
or      { return _OR_TOK; }
pgc     { return PGC_TOK; }
prev            { return PREV_TOK; }
program     { return PROGRAM_TOK; }
ptt     { return PTT_TOK; }
random          { return RND_TOK; }
region          { return REGION_TOK; }
resume      { return RESUME_TOK; }
root        { return ROOT_TOK; }
subtitle    { return SUBTITLE_TOK; }
tail            { return TAIL_TOK; }
title       { return TITLE_TOK; }
titleset    { return TITLESET_TOK; }
top             { return TOP_TOK; }
up             { return UP_TOK; }
vmgm        { return VMGM_TOK; }
xor     { return XOR_TOK; }


{ws}        ;

"="     { return SET_TOK; }

"+"     { return ADD_TOK; }
"-"     { return SUB_TOK; }
"*"     { return MUL_TOK; }
"/"     { return DIV_TOK; }
"%"     { return MOD_TOK; }
"&"     { return BAND_TOK; }
"|"     { return BOR_TOK;  }
"^"     { return XOR_TOK; }

"+="        { return ADDSET_TOK; }
"-="        { return SUBSET_TOK; }
"*="        { return MULSET_TOK; }
"/="        { return DIVSET_TOK; }
"%="        { return MODSET_TOK; }
"&="        { return ANDSET_TOK; }
"|="        { return ORSET_TOK;  }
"^="        { return XORSET_TOK; }

"=="        { return EQ_TOK; }
"!="        { return NE_TOK; }
">="        { return GE_TOK; }
">"     { return GT_TOK; }
"<="        { return LE_TOK; }
"<"     { return LT_TOK; }

"&&"        { return LAND_TOK; }
"||"        { return LOR_TOK; }
"!"     { return NOT_TOK; }

";"             { return SEMICOLON_TOK; }
":"             { return COLON_TOK; }
"{"     { return OPENBRACE_TOK; }
"}"     { return CLOSEBRACE_TOK; }
"("     { return OPENPAREN_TOK; }
")"     { return CLOSEPAREN_TOK; }

{hex_num}       { sscanf((char *)yytext,"0x%x",&dvdvmlval.int_val);
          if( dvdvmlval.int_val<0 || dvdvmlval.int_val>=65536 ) {
            fprintf(stderr,"ERR:  Integers must be between 0 and 65535, inclusive (%s)\n",yytext);
            return ERROR_TOK;
          }
                  return NUM_TOK; }


{int_num}   { sscanf((char *)yytext,"%u",&dvdvmlval.int_val);
          if( dvdvmlval.int_val<0 || dvdvmlval.int_val>=65536 ) {
            fprintf(stderr,"ERR:  Integers must be between 0 and 65535, inclusive (%s)\n",yytext);
            return ERROR_TOK;
          }
                  return NUM_TOK; }

{int_num}k  { sscanf((char *)yytext,"%u",&dvdvmlval.int_val);
          if( dvdvmlval.int_val<0 || dvdvmlval.int_val>=64 ) {
            fprintf(stderr,"ERR:  Integers must be between 0 and 65535, inclusive (%s)\n",yytext);
            return ERROR_TOK;
          }
                  dvdvmlval.int_val*=1024;
                  return NUM_TOK; }

{lang_code}     { dvdvmlval.int_val = ((unsigned char *)yytext)[1] * 256 + ((unsigned char *)yytext)[2];
                  return NUM_TOK; }


g{int_num}  { sscanf((char *)yytext+1,"%u",&dvdvmlval.int_val);
                  if( allowallreg ) {
                      if( dvdvmlval.int_val<0 || dvdvmlval.int_val>=16 ) {
                          fprintf(stderr,"ERR:  Can only access g0-g15 (%s)\n",yytext);
                          return ERROR_TOK;
                      }
                  } else {
                      if( dvdvmlval.int_val<0 || dvdvmlval.int_val>=13 ) {
                          fprintf(stderr,"ERR:  Can only access g0-g12 (%s)\n",yytext);
                          return ERROR_TOK;
                      }
                  }
          return G_TOK; }

s{int_num}  { sscanf((char *)yytext+1,"%u",&dvdvmlval.int_val);
          if( dvdvmlval.int_val<0 || dvdvmlval.int_val>23 ) {
            fprintf(stderr,"ERR:  Can only access s0-s23 (%s)\n",yytext);
            return ERROR_TOK;
          }
          return S_TOK; }

{identifier}    { dvdvmlval.str_val = strdup((char *)yytext);
                  return ID_TOK; }


.       { return ERROR_TOK; }

%%
