/*
    Decompile VM instructions to human-readable form
*/
/* Ogle - A video player
 * Copyright (C) 2000, 2001 Martin Norbäck, Håkan Hjort
 *
 * Heavily modified (C) 2005 Scott Smith
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, 
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"
#include "compat.h"

#include <stdio.h>
#include <ctype.h>
#include <inttypes.h>

#include "dvduncompile.h"


static xmlNodePtr
    output; /* formatted output appended here */
static const char
    *outputbase=0;
      /* prepended to every line for indentation purposes relative to caller's output */
static int
    indent = 0, /* current internal indentation level */
    curline; /* for labelling branch targets */
static bool
    isnewline = false; /* am I starting a new line */

typedef struct /* for holding command instruction currently being decoded */
{
    uint8_t bits[8]; /* the command instruction */
    uint8_t examined[8]; /* for tracking leftover bits */
} cmd_t;


static const char *cmp_op_table[] = {
    NULL, "and", "==", "!=", "ge", "gt", "le", "lt"
};
static const char *set_op_table[] = {
    NULL, "=", "swap", "+", "-", "*", "/", "%", "random", "and", "or", "xor"
};

static const char * const link_table[] = {
    "NOP",         "jump cell top",    "jump next cell",    "jump prev cell",
    NULL,          "jump program top", "jump next program", "jump prev program",
    NULL,          "jump pgc top",     "jump next pgc",     "jump prev pgc",
    "jump up pgc", "jump pgc tail",    NULL,                NULL,
    "resume"
};

static const char * const system_reg_table[] = {
    "s0",  // "Menu Description Language Code",
    "audio",
    "subtitle",
    "angle",
    "s4",  // "Title Track Number",
    "s5",  // "VTS Title Track Number",
    "s6",  // "VTS PGC Number",
    "s7",  // "PTT Number for One_Sequential_PGC_Title",
    "button",
    "s9",  // "Navigation Timer",
    "s10", // "Title PGC Number for Navigation Timer",
    "s11", // "Audio Mixing Mode for Karaoke",
    "s12", // "Country Code for Parental Management",
    "s13", // "Parental Level",
    "s14", // "Player Configurations for Video",
    "s15", // "Player Configurations for Audio",
    "s16", // "Initial Language Code for Audio",
    "s17", // "Initial Language Code Extension for Audio",
    "s18", // "Initial Language Code for Sub-picture",
    "s19", // "Initial Language Code Extension for Sub-picture",
    "region",
    "s21", // "Reserved 21",
    "s22", // "Reserved 22",
    "s23", // "Reserved 23"
};

#if 0
static const char * const system_reg_abbr_table[] = {
    NULL,
    "ASTN",
    "SPSTN",
    "AGLN",
    "TTN",
    "VTS_TTN",
    "TT_PGCN",
    "PTTN",
    "HL_BTNN",
    "NVTMR",
    "NV_PGCN",
    NULL,
    "CC_PLT",
    "PLT",
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
};
#endif

static const char * const entries[16] = /* menu entry types */
{
    "UNKNOWN0",  "UNKNOWN1",  "title",     "root", // XXX: is 1 == fpc?
    "subtitle",  "audio",     "angle",     "ptt",
    "UNKNOWN8",  "UNKNOWN9",  "UNKNOWN10", "UNKNOWN11",
    "UNKNOWN12", "UNKNOWN13", "UNKNOWN14", "UNKNOWN15"
};

static void node_printf(const char *format,...)
  /* appends formatted content to output. */
{
    static char bigbuf[1024];
    va_list ap;

    if( isnewline ) { /* starting a new line, prepend appropriate indentation */
        int i;

        xmlAddChildList(output,xmlNewText((const xmlChar *)outputbase));
        for( i=0; i<indent; i++ )
            xmlAddChildList(output,xmlNewText((const xmlChar *)"  "));
        isnewline = false;
    }

    va_start(ap,format);
    vsnprintf(bigbuf,sizeof(bigbuf),format,ap);
    va_end(ap);
    bigbuf[sizeof(bigbuf)-1]=0; /* just in case */
    xmlAddChildList(output,xmlNewText((const xmlChar *)bigbuf));
}

static void node_commentf(const char *format,...)
/* attaches an XML comment to the output. */
{
    static char bigbuf[1024];
    va_list ap;

    va_start(ap,format);
    vsnprintf(bigbuf,sizeof(bigbuf),format,ap);
    va_end(ap);
    bigbuf[sizeof(bigbuf)-1]=0;
    node_printf(" ");
    xmlAddChildList(output,xmlNewComment((const xmlChar *)bigbuf));
    node_printf(" ");
}

static void node_newline(void)
/* starts a new line in the output. */
{
    isnewline = true;
}

static void node_indent(void)
/* increases the indentation level. */
{
    indent++;
}

static void node_closebrace(void)
/* outputs a closing brace on a line by itself and decreases the indentation level */
{
    node_newline();
    indent--;
    node_printf("}");
    node_newline();
}

static uint32_t bits(cmd_t *cmd, int byte, int bit, int count)
/* extracts count bits from cmd->bits at offset given by byte and bit. */
  {
    uint32_t val = 0;
    int bit_mask;
    while (count--)
      {
        if (bit > 7) /* finished a byte */
          {
            bit = 0;
            byte++;
          } /*if*/
        bit_mask = 0x01 << (7 - bit); /* bits are numbered from most significant end of byte */
        val <<= 1;
        if ((cmd->bits[byte]) & bit_mask) /* extract next bit */
            val |= 1;
        cmd->examined[byte] |= bit_mask; /* mark bit as used */
        bit++;
      } /*while*/
    return val;
  } /*bits*/

static void print_system_reg(uint16_t reg)
  /* prints the name of an SPRM to output. */
  {
    if (reg < sizeof(system_reg_table) / sizeof(char *))
        node_printf(system_reg_table[reg]);
    else
      {
        node_printf("sXXX");
        fprintf(stderr, "WARN: Unknown system register %d\n", reg);
      } /*if*/
  } /*print_system_reg*/

static void print_reg(uint8_t reg)
  /* prints the name of a GPRM or SPRM to output. */
  {
    if (reg & 0x80)
        print_system_reg(reg & 0x7f);
    else if (reg < 16)
        node_printf("g%" PRIu8 "", reg);
    else
      {
        node_printf("gXXX");
        fprintf(stderr, "WARN: Unknown general register %d\n", reg);
      }
  } /*print_reg*/

static void print_cmp_op(uint8_t op)
  /* prints a comparison operator to output. */
  {
    if (op < sizeof(cmp_op_table) / sizeof(char *) && cmp_op_table[op] != NULL)
        node_printf(" %s ", cmp_op_table[op]);
    else
        fprintf(stderr, "WARN: Unknown compare op %d\n",op);
  } /*print_cmp_op*/

static void print_set_op(uint8_t op)
  /* prints a set-operator to output. */
  {
    if (op < sizeof(set_op_table) / sizeof(char *) && set_op_table[op] != NULL)
        node_printf(" %s ", set_op_table[op]);
    else
        fprintf(stderr, "WARN: Unknown set op %d\n", op);
  } /*print_set_op*/

static void print_reg_or_data(cmd_t *cmd, bool immediate, int immedbyte, int regbyte, bool scln)
  /* prints a 16-bit operand, which might be a literal value or a register reference. */
  {
    if (immediate)
      {
        const int i = bits(cmd, immedbyte, 0, 16); /* immediate value */
        node_printf("%d", i);
        if (scln)
            node_printf(";");
        if (isprint(i & 0xff) && isprint((i >> 8) & 0xff)) /* looks printable */
            node_commentf(" \"%c%c\" ", (char)((i >> 8) & 0xff), (char)(i & 0xff));
              /* include a comment showing ASCII value */
      }
    else
      {
        print_reg(bits(cmd, regbyte, 0, 8));
        if (scln)
            node_printf(";");
      } /*if*/
  } /*print_reg_or_data*/

static void print_reg_or_data_2(cmd_t *cmd, bool immediate, int byte)
  /* prints an operand for a SetSTN instruction, which might be a 7-bit literal
    value or a GPRM reference. */
  {
    if (immediate)
        node_printf("0x%x;", bits(cmd, byte, 1, 7));
    else
        node_printf("g%" PRIu8 ";", bits(cmd, byte, 4, 4));
  } /*print_reg_or_data_2*/

static void print_if(cmd_t *cmd, int regbyte1, int regbit1, bool op2immed, int immedbyte2, int regbyte2)
  /* common code for printing the start of an if-statement, if an instruction is conditinal. */
  {
    const uint8_t cmpop = bits(cmd, 1, 1, 3);
    if (cmpop) /* 0 => unconditional */
      {
        node_printf("if (");
        if (cmpop == 1)  // and gets special treatment
            node_printf("(");
        print_reg(bits(cmd, regbyte1, regbit1, 8 - regbit1));
        print_cmp_op(cmpop);
        print_reg_or_data(cmd, op2immed, immedbyte2, regbyte2, false);
        if (cmpop == 1)  // and gets special treatment
            node_printf(") != 0");
        node_printf(") {");
        node_indent();
      } /*if*/
  } /*print_if*/

static void print_if_version_1(cmd_t *cmd)
  {
    print_if
      (
        /*cmd =*/ cmd,
        /*regbyte1 =*/ 3,
        /*regbit1 =*/ 0,
        /*op2immed =*/ bits(cmd, 1, 0, 1),
        /*immedbyte2 =*/ 4,
        /*regbyte2 =*/ 5
      );
  } /*print_if_version_1*/

static void print_if_version_2(cmd_t *cmd)
  {
    print_if
      (
        /*cmd =*/ cmd,
        /*regbyte1 =*/ 6,
        /*regbit1 =*/ 0,
        /*op2immed =*/ false,
        /*immedbyte2 =*/ 6,
        /*regbyte2 =*/ 7
      );
  } /*print_if_version_2*/

static void print_if_version_3(cmd_t *cmd)
  {
    print_if
      (
        /*cmd =*/ cmd,
        /*regbyte1 =*/ 2,
        /*regbit1 =*/ 0,
        /*op2immed =*/ bits(cmd, 1, 0, 1),
        /*immedbyte2 =*/ 6,
        /*regbyte2 =*/ 7
      );
  } /*print_if_version_3*/

static void print_if_version_4(cmd_t *cmd)
  {
    print_if
      (
        /*cmd =*/ cmd,
        /*regbyte1 =*/ 1,
        /*regbit1 =*/ 4,
        /*op2immed =*/ bits(cmd, 1, 0, 1),
        /*immedbyte2 =*/ 4,
        /*regbyte2 =*/ 5
      );
  } /*print_if_version_4*/

static void print_if_version_5(cmd_t *cmd)
  {
    print_if
      (
        /*cmd =*/ cmd,
        /*regbyte1 =*/ 4,
        /*regbit1 =*/ 0,
        /*op2immed =*/ false,
        /*immedbyte2 =*/ 4,
        /*regbyte2 =*/ 5
      );
  } /*print_if_version_5*/

static void print_if_close_v12345(cmd_t *cmd)
  /* prints common closing sequence for all the above forms of if-statement. */
  {
    const uint8_t cmpop = bits(cmd, 1, 1, 3);
    if (cmpop) /* 0 => unconditional */
        node_closebrace();
  } /*print_if_close_v12345*/

static void print_special_instruction(cmd_t *cmd)
  {
    const uint8_t op = bits(cmd, 1, 4, 4);
    node_newline();
    switch(op)
      {
    case 0: // NOP
        // fprintf(stderr, "Nop");
    break;
    case 1: // Goto line
        node_printf("goto l%" PRIu8 ";", bits(cmd, 7, 0, 8));
    break;
    case 2: // Break
        node_printf("break;");
    break;
    case 3: // Parental level
        if (bits(cmd, 7, 0, 8) == curline + 1)
          { /* branch to following instr => no branch */
            node_printf("SetTmpPML(%" PRIu8 ");", bits(cmd, 6, 4, 4));
          }
        else
          {
            node_printf("if( SetTmpPML(%" PRIu8 ") ) {", bits(cmd, 6, 4, 4));
            node_indent();
            node_newline();
            node_printf("goto l%" PRIu8 ";", bits(cmd, 7, 0, 8));
            node_closebrace();
          } /*if*/
    break;
    default:
        fprintf(stderr, "WARN: Unknown special instruction (%i)", bits(cmd, 1, 4, 4));
    break;
      } /*switch*/
  } /*print_special_instruction*/

static void button_set(int button)
  /* prints an instruction to set the currently-highlighted button. */
  {
    print_system_reg(8);
    if (!(button & 1023))
        node_printf(" = %dk;", button >> 10);
    else
        node_printf(" = %d;", button);
    node_commentf(" button no %d ", button >> 10);
  } /*button_set*/

static void button_maybe_set(int button)
  /* prints an instruction to set the currently-highlighted button, if nonzero. */
  {
    if (button)
      {
        button_set(button * 1024);
        node_newline();
      } /*if*/
  } /*button_maybe_set*/

static void print_linksub_instruction(cmd_t *cmd)
  {
    const int linkop = bits(cmd, 7, 3, 5);
    const int button = bits(cmd, 6, 0, 6);
    if (linkop < sizeof(link_table) / sizeof(char *) && link_table[linkop] != NULL)
      {
        node_newline();
        button_maybe_set(button);
        if (linkop != 0) /* not LinkNoLink */
            node_printf("%s;", link_table[linkop]);
      }
    else
        fprintf(stderr, "WARN: Unknown linksub instruction (%i)", linkop);
  } /*print_linksub_instruction*/

static void print_link_instruction(cmd_t *cmd, bool optional)
  {
    const uint8_t op = bits(cmd, 1, 4, 4);
    node_newline();
    switch (op)
      {
    case 0:
        if (!optional)
            fprintf(stderr, "WARN: NOP (link)!");
    break;
    case 1:
        print_linksub_instruction(cmd);
    break;
    case 4:
        node_printf("jump pgc %" PRIu16 ";", bits(cmd, 6, 1, 15));
    break;
    case 5:
        button_maybe_set(bits(cmd, 6, 0, 6));
        node_printf("jump chapter %" PRIu16 ";", bits(cmd, 6, 6, 10));
    break;
    case 6:
        button_maybe_set(bits(cmd, 6, 0, 6));
        node_printf("jump program %" PRIu8 ";", bits(cmd, 7, 1, 7));
    break;
    case 7:
        button_maybe_set(bits(cmd, 6, 0, 6));
        node_printf("jump cell %" PRIu8 ";", bits(cmd, 7, 0, 8));
    break;
    default:
        fprintf(stderr, "WARN: Unknown link instruction");
    break;
      } /*switch*/
  } /*print_link_instruction*/

static void print_jump_instruction(cmd_t *cmd)
  {
    node_newline();
    switch (bits(cmd, 1, 4, 4))
      {
    case 1:
        node_printf("exit;");
    break;
    case 2: // JumpTT  -- perhaps jump vmgm title?
        node_printf("jump title %" PRIu8 ";", bits(cmd, 5, 1, 7));
    break;
    case 3: // JumpVTS_TT
        node_printf("jump title %" PRIu8 ";", bits(cmd, 5, 1, 7));
    break;
    case 5:
        node_printf("jump title %" PRIu8 " chapter %" PRIu16 ";",
                bits(cmd, 5, 1, 7), bits(cmd, 2, 6, 10));
    break;
    case 6:
        switch (bits(cmd, 5, 0, 2))
          {
        case 0:
            node_printf("jump fpc;");
        break;
        case 1:
            node_printf("jump vmgm menu entry %s;", entries[bits(cmd, 5, 4, 4)]);
        break;
        case 2:
            if (bits(cmd, 3, 0, 8) != 1)
              {
                fprintf
                  (
                    stderr,
                    "WARN: Title not 1 as expected:  jump titleset %" PRIu8
                        " menu entry %s; /* title=%" PRIu8 " */", 
                    bits(cmd, 4, 0, 8),
                    entries[bits(cmd, 5, 4, 4)],
                    bits(cmd, 3, 0, 8)
                  );
              } /*if*/
            node_printf
              (
                "jump titleset %" PRIu8 " menu entry %s;", 
                bits(cmd, 4, 0, 8),
                entries[bits(cmd, 5, 4, 4)]
              );
            node_commentf(" title=%" PRIu8 " ", bits(cmd, 3, 0, 8));
        break;
        case 3:
            node_printf("jump vmgm menu %" PRIu8 ";", bits(cmd, 2, 1, 15));
        break;
          } /*switch*/
    break;
    case 8:
        switch (bits(cmd, 5, 0, 2))
          {
        case 0:
            node_printf("call fpc");
        break;
        case 1:
            node_printf("call vmgm menu entry %s", entries[bits(cmd, 5, 4, 4)]);
        break;
        case 2: // VTSM menu
            node_printf("call menu entry %s", entries[bits(cmd, 5, 4, 4)]);
        break;
        case 3:
            node_printf("call vmgm menu %" PRIu8, bits(cmd, 2, 1, 15));
        break;
          } /*switch*/
        if (bits(cmd, 4, 0, 8))
            node_printf(" resume %" PRIu8, bits(cmd, 4, 0, 8));
        node_printf(";");
    break;
    default:
        fprintf(stderr, "WARNING: Unknown Jump/Call instruction");
    break;
      } /*switch*/
  } /*print_jump_instruction*/

static void print_system_set(cmd_t *cmd)
  {
    int i;
    node_newline();
    switch (bits(cmd, 0, 4, 4))
      {
    case 1: // Set system reg 1 &| 2 &| 3 (Audio, Subp. Angle)
        for (i = 1; i <= 3; i++)
          {
            if (bits(cmd, 2 + i, 0, 1))
              {
                print_system_reg(i);
                node_printf(" = ");
                print_reg_or_data_2(cmd, bits(cmd, 0, 3, 1), 2 + i);
                node_newline();
              } /*if*/
          } /*for*/
    break;
    case 2: // Set system reg 9 & 10 (Navigation timer, Title PGC number)
        print_system_reg(9);
        node_printf(" = ");
        print_reg_or_data(cmd, bits(cmd, 0, 3, 1), 2, 3, true);
        node_printf(" ");
        print_system_reg(10);
        node_printf(" = %" PRIu8 ";", bits(cmd, 5, 0, 8)); // ??
    break;
    case 3: // Mode: Counter / Register + Set
        if (bits(cmd, 5, 0, 1))
            node_printf("counter ");
        else
            node_printf("register ");
        print_reg(bits(cmd, 5, 4, 4));
        node_printf(" = ");
        print_reg_or_data(cmd, bits(cmd, 0, 3, 1), 2, 3, true);
    break;
    case 6: // Set system reg 8 (Highlighted button)
        if (bits(cmd, 0, 3, 1))
          {
          /* literal value */
            button_set(bits(cmd, 4, 0, 16));
          }
        else
          {
          /* set from a GPRM */
            print_system_reg(8);
            node_printf(" = g%" PRIu8 ";", bits(cmd, 5, 4, 4));
          } /*if*/
    break;
    default:
        fprintf(stderr, "WARN: Unknown system set instruction (%i)", bits(cmd, 0, 4, 4));
    break;
      } /*switch*/
  } /*print_system_set*/

static void print_set(cmd_t *cmd, int dstregbyte, int dstregbit, int dstregwidth, int srcimmedbyte, int srcregbyte)
  {
    const uint8_t set_op = bits(cmd, 0, 4, 4);
    node_newline();
    if (set_op == 2) // swap
      {
        node_printf("swap(");
        print_reg(bits(cmd, dstregbyte, dstregbit, dstregwidth));
        node_printf(", ");
        print_reg_or_data(cmd, bits(cmd, 0, 3, 1), srcimmedbyte, srcregbyte, false);
        node_printf(");");
      }
    else if (set_op)
      {
        print_reg(bits(cmd, dstregbyte, dstregbit, dstregwidth));
        node_printf(" = ");
        if (set_op == 8)
          { // random
            node_printf("random(");
            print_reg_or_data(cmd, bits(cmd, 0, 3, 1), srcimmedbyte, srcregbyte, false);
            node_printf(");");
          }
        else
          {
            if (set_op != 1) /* not mov */
              {
                print_reg(bits(cmd, dstregbyte, dstregbit, dstregwidth));
                print_set_op(set_op);
              } /*if*/
            print_reg_or_data(cmd, bits(cmd, 0, 3, 1), srcimmedbyte, srcregbyte, true);
          } /*if*/
      }
    else
      {
        // fprintf(stderr, "NOP");
      } /*if*/
  } /*print_set*/

static void print_set_version_1(cmd_t *cmd) {
    print_set(cmd,3,0,8,4,5);
}

static void print_set_version_2(cmd_t *cmd) {
    print_set(cmd,1,4,4,2,3);
}

static void print_set_version_3(cmd_t *cmd) {
    print_set(cmd,1,4,4,2,2);
}

static void print_command(cmd_t *cmd)
  /* does the actual disassembly of a single command instruction. */
  {
    node_newline();
    switch (bits(cmd, 0, 0, 3)) /* instruction class */
      {
    case 0: // Special instructions
        print_if_version_1(cmd);
        print_special_instruction(cmd);
        print_if_close_v12345(cmd);
    break;
    case 1: // Jump/Call or Link instructions
        if (bits(cmd, 0, 3, 1))
          {
            print_if_version_2(cmd);
            print_jump_instruction(cmd);
            print_if_close_v12345(cmd);
          }
        else
          {
            print_if_version_1(cmd);
            print_link_instruction(cmd, false); // must be present
            print_if_close_v12345(cmd);
          } /*if*/
    break;
    case 2: // Set System Parameters instructions
        print_if_version_2(cmd);
        print_system_set(cmd);
        print_if_close_v12345(cmd);
        print_link_instruction(cmd, true); // either 'if' or 'link'
    break;
    case 3: // Set General Parameters instructions
        print_if_version_3(cmd);
        print_set_version_1(cmd);
        print_if_close_v12345(cmd);
        print_link_instruction(cmd, true); // either 'if' or 'link'
    break;
    case 4: // Set, Compare -> LinkSub instructions
        print_set_version_2(cmd);
        node_newline();
        print_if_version_4(cmd);
        print_linksub_instruction(cmd);
        print_if_close_v12345(cmd);
    break;
    case 5: // Compare -> (Set and LinkSub) instructions
        if (bits(cmd, 0, 3, 1))
            print_if_version_5(cmd);
        else
            print_if_version_1(cmd);
        print_set_version_3(cmd);
        print_linksub_instruction(cmd);
        print_if_close_v12345(cmd);
    break;
    case 6: // Compare -> Set, always LinkSub instructions
        if (bits(cmd, 0, 3, 1))
            print_if_version_5(cmd);
        else
            print_if_version_1(cmd);
        print_set_version_3(cmd);
        print_if_close_v12345(cmd);
        print_linksub_instruction(cmd);
    break;
    default:
        fprintf(stderr, "WARN: Unknown instruction type (%i)",  bits(cmd, 0, 0, 3));
    break;
    }
  } /*print_command*/

static void vm_add_mnemonic(const vm_cmd_t *command)
  /* disassembles a single command. */
 {
    int i;
    bool extra_bits;
    cmd_t cmd;
    for (i = 0; i < 8; i++)
      {
        cmd.bits[i] = command->bytes[i];
        cmd.examined[i] = 0;
      } /*for*/
    print_command(&cmd);
    // Check if there still are bits set that were not examined
    extra_bits = false;
    for (i = 0; i < 8; i++)
        if (cmd.bits[i] & ~ cmd.examined[i])
          {
            extra_bits = true;
            break;
          } /*if; for*/
    if (extra_bits)
      {
        fprintf(stderr, "WARN: unknown cmd bits:");
        for (i = 0; i < 8; i++)
            fprintf(stderr, " %02x", cmd.bits[i] & ~ cmd.examined[i]);
        fprintf(stderr, "\n");
      } /*if*/
  } /*vm_add_mnemonic*/

void vm_add_mnemonics
  (
    xmlNodePtr node, /* the node to append the disassembly to */
    const char *base, /* prepended to every output line for indentation purposes */
    int ncmd, /* nr of commands */
    const vm_cmd_t *commands /* array */
  )
  /* disassembles the specified command sequence as content for the specified XML tag. */
  {
    int i, j;
    output = node; /* for easy reference by other routines */
    outputbase = base;
    indent = 1;
    isnewline = true;
    for (i = 0; i < ncmd; i++)
      {
        curline = i + 1;
        for (j = 0; j < ncmd; j++)
            if /* some instruction has a branch target here */
              (
                    commands[j].bytes[0] == 0 /* special instruction */
                &&
                    (
                        (commands[j].bytes[1] & 15) == 1 /* goto */
                    ||
                        (commands[j].bytes[1] & 15) == 3 /* parental control */
                    )
                &&
                    commands[j].bytes[7] == curline /* target of branch is here */
              )
              {
              /* output label definition for target of branch */
                indent--;
                node_newline();
                node_printf("l%d:",curline);
                indent++;
                break;
              } /* if; for */
        vm_add_mnemonic(commands + i);
      } /*for*/
    node_newline();
    indent = 0;
    node_printf("");
  } /*vm_add_mnemonics*/
