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

#include "compat.h" /* needed for bool */
#include "dvdvm.h"


#define YYERROR_VERBOSE

%}


// we have one shift/reduce conflict: if/else

%expect 1

%token <int_val> NUM_TOK G_TOK S_TOK
%token <str_val> ID_TOK

%token ANGLE_TOK
%token AUDIO_TOK
%token BREAK_TOK
%token BUTTON_TOK
%token CALL_TOK
%token CELL_TOK
%token CHAPTER_TOK
%token CLOSEBRACE_TOK
%token CLOSEPAREN_TOK
%token COUNTER_TOK
%token ELSE_TOK
%token ENTRY_TOK
%token EXIT_TOK
%token FPC_TOK
%token GOTO_TOK
%token IF_TOK
%token JUMP_TOK
%token MENU_TOK
%token NEXT_TOK
%token OPENBRACE_TOK
%token OPENPAREN_TOK
%token PGC_TOK
%token PREV_TOK
%token PROGRAM_TOK
%token PTT_TOK
%token REGION_TOK
%token RESUME_TOK
%token RND_TOK
%token ROOT_TOK
%token SET_TOK
%token SUBTITLE_TOK
%token TAIL_TOK
%token TITLE_TOK
%token TITLESET_TOK
%token TOP_TOK
%token UP_TOK
%token VMGM_TOK


%left _OR_TOK XOR_TOK LOR_TOK BOR_TOK
%left _AND_TOK LAND_TOK BAND_TOK
%right NOT_TOK
%nonassoc EQ_TOK NE_TOK
%nonassoc GE_TOK GT_TOK LE_TOK LT_TOK
%left ADD_TOK SUB_TOK
%left MUL_TOK DIV_TOK MOD_TOK

%token ADDSET_TOK SUBSET_TOK MULSET_TOK DIVSET_TOK MODSET_TOK ANDSET_TOK ORSET_TOK XORSET_TOK

%token SEMICOLON_TOK
%token COLON_TOK
%token ERROR_TOK

%union {
    unsigned int int_val;
    char *str_val;
    struct vm_statement *statement;
}

%type <statement> finalparse statements statement callstatement jumpstatement setstatement ifstatement ifelsestatement expression boolexpr
%type <int_val> jtsl jtml jcl resumel reg regornum regorcounter

%%

finalparse: statements {
    dvd_vm_parsed_cmd=$$;
}
;

statements: statement {
    $$=$1;
}
| statement statements {
    $$=$1;
    $$->next=$2;
}
;

statement: jumpstatement {
    $$=$1;
}
| callstatement {
    $$=$1;
}
| EXIT_TOK SEMICOLON_TOK {
    $$=statement_new();
    $$->op=VM_EXIT;
}
| RESUME_TOK SEMICOLON_TOK {
    $$=statement_new();
    $$->op=VM_LINK;
    $$->i1=16;
}
| GOTO_TOK ID_TOK SEMICOLON_TOK {
    $$=statement_new();
    $$->op=VM_GOTO;
    $$->s1=$2;
}
| ID_TOK COLON_TOK {
    $$=statement_new();
    $$->op=VM_LABEL;
    $$->s1=$1;
}
| BREAK_TOK SEMICOLON_TOK {
    $$=statement_new();
    $$->op=VM_BREAK;
}
| setstatement {
    $$=$1;
}
| OPENBRACE_TOK statements CLOSEBRACE_TOK {
    $$=$2;
}
| ifelsestatement {
    $$=$1;
}
;

jtsl: TITLESET_TOK NUM_TOK {
    if ($2 < 0 || $2 > 99)
      {
        yyerror("titleset number out of range");
      } /*if*/
    $$=($2)+1;
}
| VMGM_TOK {
    $$=1;
}
| {
    $$=0;
}
;

jtml: MENU_TOK NUM_TOK {
    if ($2 < 1 || $2 > 99)
      {
        yyerror("menu number out of range");
      } /*if*/
    $$=$2;
}
| MENU_TOK {
    $$=120; // default entry
}
| MENU_TOK ENTRY_TOK TITLE_TOK {
    $$=122;
}
| MENU_TOK ENTRY_TOK ROOT_TOK {
    $$=123;
}
| MENU_TOK ENTRY_TOK SUBTITLE_TOK {
    $$=124;
}
| MENU_TOK ENTRY_TOK AUDIO_TOK {
    $$=125;
}
| MENU_TOK ENTRY_TOK ANGLE_TOK {
    $$=126;
}
| MENU_TOK ENTRY_TOK PTT_TOK {
    $$=127;
}
| FPC_TOK {
    $$=121;
}
| TITLE_TOK NUM_TOK {
    if ($2 < 1 || $2 > 99)
      {
        yyerror("title number out of range");
      } /*if*/
    $$=($2)|128;
}
| {
    $$=0;
}
;

jcl: CHAPTER_TOK NUM_TOK {
    if ($2 < 1 || $2 > 65535)
      {
        yyerror("chapter number out of range");
      } /*if*/
    $$=$2;
}
| {
    $$=0;
}
;

jumpstatement: JUMP_TOK jtsl jtml jcl SEMICOLON_TOK {
    $$=statement_new();
    $$->op=VM_JUMP;
  /* values already range-checked: */
    $$->i1=$2;
    $$->i2=$3;
    $$->i3=$4;
}
| JUMP_TOK CELL_TOK NUM_TOK SEMICOLON_TOK {
    if ($3 < 1 || $3 > 65535)
      {
        yyerror("cell number out of range");
      } /*if*/
    $$=statement_new();
    $$->op=VM_JUMP;
    $$->i3=2*65536+$3;
}
| JUMP_TOK PROGRAM_TOK NUM_TOK SEMICOLON_TOK {
    if ($3 < 1 || $3 > 65535)
      {
        yyerror("program number out of range");
      } /*if*/
    $$=statement_new();
    $$->op=VM_JUMP;
    $$->i3=65536+$3;
}
| JUMP_TOK PGC_TOK NUM_TOK SEMICOLON_TOK {
    if ($3 < 1 || $3 > 128)
      {
        yyerror("pgc number out of range");
      } /*if*/
    $$=statement_new();
    $$->op=VM_JUMP;
    $$->i2=$3;
}
| JUMP_TOK CELL_TOK TOP_TOK SEMICOLON_TOK {
    $$=statement_new();
    $$->op=VM_LINK;
    $$->i1=1;
}
| JUMP_TOK NEXT_TOK CELL_TOK SEMICOLON_TOK {
    $$=statement_new();
    $$->op=VM_LINK;
    $$->i1=2;
}
| JUMP_TOK PREV_TOK CELL_TOK SEMICOLON_TOK {
    $$=statement_new();
    $$->op=VM_LINK;
    $$->i1=3;
}
| JUMP_TOK PROGRAM_TOK TOP_TOK SEMICOLON_TOK {
    $$=statement_new();
    $$->op=VM_LINK;
    $$->i1=5;
}
| JUMP_TOK NEXT_TOK PROGRAM_TOK SEMICOLON_TOK {
    $$=statement_new();
    $$->op=VM_LINK;
    $$->i1=6;
}
| JUMP_TOK PREV_TOK PROGRAM_TOK SEMICOLON_TOK {
    $$=statement_new();
    $$->op=VM_LINK;
    $$->i1=7;
}
| JUMP_TOK PGC_TOK TOP_TOK SEMICOLON_TOK {
    $$=statement_new();
    $$->op=VM_LINK;
    $$->i1=9;
}
| JUMP_TOK NEXT_TOK PGC_TOK SEMICOLON_TOK {
    $$=statement_new();
    $$->op=VM_LINK;
    $$->i1=10;
}
| JUMP_TOK PREV_TOK PGC_TOK SEMICOLON_TOK {
    $$=statement_new();
    $$->op=VM_LINK;
    $$->i1=11;
}
| JUMP_TOK UP_TOK PGC_TOK SEMICOLON_TOK {
    $$=statement_new();
    $$->op=VM_LINK;
    $$->i1=12;
}
| JUMP_TOK PGC_TOK TAIL_TOK SEMICOLON_TOK {
    $$=statement_new();
    $$->op=VM_LINK;
    $$->i1=13;
}
;

resumel: RESUME_TOK NUM_TOK {
    if ($2 < 1 || $2 > 65535)
      {
        yyerror("resume cell number out of range");
      } /*if*/
    $$=$2;
}
| {
    $$=0;
}
;

callstatement: CALL_TOK jtsl jtml jcl resumel SEMICOLON_TOK {
    $$=statement_new();
    $$->op=VM_CALL;
  /* values already range-checked: */
    $$->i1=$2;
    $$->i2=$3;
    $$->i3=$4;
    $$->i4=$5;
}
;

reg: G_TOK {
    $$=$1;
}
| S_TOK {
    $$=$1+0x80;
}
| AUDIO_TOK {
    $$=0x81;
}
| SUBTITLE_TOK {
    $$=0x82;
}
| ANGLE_TOK {
    $$=0x83;
}
| BUTTON_TOK {
    $$=0x88;
}
| REGION_TOK {
    $$=0x80+20;
}
;

regornum: reg {
    $$=$1-256;
}
| NUM_TOK {
    $$=$1;
}
;

expression: OPENPAREN_TOK expression CLOSEPAREN_TOK {
    $$=$2;
}
| regornum {
    $$=statement_new();
    $$->op=VM_VAL;
    $$->i1=$1;
}
| expression ADD_TOK expression {
    $$=statement_expression($1,VM_ADD,$3);
}
| expression SUB_TOK expression {
    $$=statement_expression($1,VM_SUB,$3);
}
| expression MUL_TOK expression {
    $$=statement_expression($1,VM_MUL,$3);
}
| expression DIV_TOK expression {
    $$=statement_expression($1,VM_DIV,$3);
}
| expression MOD_TOK expression {
    $$=statement_expression($1,VM_MOD,$3);
}
| expression BAND_TOK expression {
    $$=statement_expression($1,VM_AND,$3);
}
| expression BOR_TOK expression {
    $$=statement_expression($1,VM_OR, $3);
}
| expression _AND_TOK expression {
    $$=statement_expression($1,VM_AND,$3);
}
| expression _OR_TOK expression {
    $$=statement_expression($1,VM_OR, $3);
}
| expression XOR_TOK expression {
    $$=statement_expression($1,VM_XOR,$3);
}
| RND_TOK OPENPAREN_TOK expression CLOSEPAREN_TOK {
    $$=statement_new();
    $$->op=VM_RND;
    $$->param=$3;
}
;

boolexpr: OPENPAREN_TOK boolexpr CLOSEPAREN_TOK {
    $$=$2;
}
| expression EQ_TOK expression {
    $$=statement_expression($1,VM_EQ,$3);
}
| expression NE_TOK expression {
    $$=statement_expression($1,VM_NE,$3);
}
| expression GE_TOK expression {
    $$=statement_expression($1,VM_GTE,$3);
}
| expression GT_TOK expression {
    $$=statement_expression($1,VM_GT,$3);
}
| expression LE_TOK expression {
    $$=statement_expression($1,VM_LTE,$3);
}
| expression LT_TOK expression {
    $$=statement_expression($1,VM_LT,$3);
}
| boolexpr LOR_TOK boolexpr {
    $$=statement_expression($1,VM_LOR,$3);
}
| boolexpr LAND_TOK boolexpr {
    $$=statement_expression($1,VM_LAND,$3);
}
| boolexpr _OR_TOK boolexpr {
    $$=statement_expression($1,VM_LOR,$3);
}
| boolexpr _AND_TOK boolexpr {
    $$=statement_expression($1,VM_LAND,$3);
}
| NOT_TOK boolexpr {
    $$=statement_new();
    $$->op=VM_NOT;
    $$->param=$2;
}
;

regorcounter: reg {
    $$=$1;
}
| COUNTER_TOK G_TOK {
    $$=$2+0x20;
}
;

setstatement: regorcounter SET_TOK expression SEMICOLON_TOK {
    $$=statement_new();
    $$->op=VM_SET;
    $$->i1=$1;
    $$->param=$3;
}
| reg ADDSET_TOK expression SEMICOLON_TOK {
    $$=statement_setop($1,VM_ADD,$3);
}
| reg SUBSET_TOK expression SEMICOLON_TOK {
    $$=statement_setop($1,VM_SUB,$3);
}
| reg MULSET_TOK expression SEMICOLON_TOK {
    $$=statement_setop($1,VM_MUL,$3);
}
| reg DIVSET_TOK expression SEMICOLON_TOK {
    $$=statement_setop($1,VM_DIV,$3);
}
| reg MODSET_TOK expression SEMICOLON_TOK {
    $$=statement_setop($1,VM_MOD,$3);
}
| reg ANDSET_TOK expression SEMICOLON_TOK {
    $$=statement_setop($1,VM_AND,$3);
}
| reg ORSET_TOK expression SEMICOLON_TOK {
    $$=statement_setop($1,VM_OR,$3);
}
| reg XORSET_TOK expression SEMICOLON_TOK {
    $$=statement_setop($1,VM_XOR,$3);
}
;

ifstatement: IF_TOK OPENPAREN_TOK boolexpr CLOSEPAREN_TOK statement {
    $$=statement_new();
    $$->op=VM_IF;
    $$->param=$3;
    $3->next=statement_new();
    $3->next->op=VM_IF;
    $3->next->param=$5;
}
;

ifelsestatement: ifstatement {
    $$=$1;
}
| ifstatement ELSE_TOK statement {
    $$=$1;
    $$->param->next->next=$3;
}
;
