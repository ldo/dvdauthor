/*
    Code generation for the VM language
*/
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

#include "compat.h"
#include <assert.h>

#include "dvdauthor.h"
#include "da-internal.h"
#include "dvdvm.h"


struct vm_statement *dvd_vm_parsed_cmd;

/* arbitrary implementation limits--should be adequate, given
  the restrictions on the length of instruction sequences */
#define MAXLABELS 200
#define MAXGOTOS 200

struct dvdlabel {
    char *lname; /* label name */
    unsigned char *code;
      /* pointer into buf where label is defined or where goto instruction needs fixup */
};

static struct dvdlabel labels[MAXLABELS];
static struct dvdlabel gotos[MAXGOTOS];
static int numlabels=0, numgotos=0;

static int negatecompare(int compareop)
  /* returns the comparison with the opposite result. Assumes the op isn't BC ("&"). */
  {
    return compareop ^ 1 ^ ((compareop & 4) >> 1);
      /* EQ <=> NE, GE <=> LT, GT <=> LE */
  } /*negatecompare*/

static int swapcompare(int compareop)
  /* returns the equivalent comparison with the operands swapped. */
  {
    if (compareop < 4) /* BC, EQ, NE unchanged */
        return compareop;
    else
        return compareop ^ 3; /* GE <=> LT, GT <=> LE */
  } /*swapcompare*/

static bool compile_usesreg(const struct vm_statement *cs, int target)
  /* does cs reference the specified register. */
  {
    while (cs)
      {
        if (cs->op == VM_VAL)
            return cs->i1 == target - 256;
        if (compile_usesreg(cs->param, target))
            return true;
        cs = cs->next;
      } /*while*/
    return false;
  } /*compile_usesreg*/

static int nexttarget(int t)
  /* returns the next register after t in the range I have reserved. Will fail
    if it's all used, or if I haven't got a reserved range. */
  {
    if (!allowallreg)
      {
        if (t < 13)
            return 13;
        t++;
        if (t < 16)
            return t;
      } /*if*/
    fprintf(stderr,"ERR:  Expression is too complicated, ran out of registers\n");
    exit(1);
  } /*nexttarget*/

// like nexttarget, but takes a VM_VAL argument
static int nextval(int t)
  {
    if (t < -128)
        return nexttarget(t + 256) - 256; /* it's a register, return next available register */
    else
        return nexttarget(-1) - 256; /* not a register, return first available register */
  } /*nextval*/

static unsigned char *compileop(unsigned char *buf, int target, int op, int val)
  /* compiles a command to set the target GPRM to the result of the specified operation
    on it and the specified value. */
  {
    if (op == VM_VAL && target == val + 256)
        return buf; /* setting register to its same value => noop */
    write8(buf, val >= 0 ? 0x70 : 0x60, 0x00, 0x00, target, val >= 0 ? (val >> 8) : 0x00, val, 0x00, 0x00);
      /* target op= val (op to be filled in below) */
    switch(op)
      {
    case VM_VAL: /* simple assignment */
        buf[0] |= 1;
    break;
    case VM_ADD:
        buf[0] |= 3;
    break;
    case VM_SUB:
        buf[0] |= 4;
    break;
    case VM_MUL:
        buf[0] |= 5;
    break;
    case VM_DIV:
        buf[0] |= 6;
    break;
    case VM_MOD:
        buf[0] |= 7;
    break;
    case VM_RND:
        buf[0] |= 8;
    break;
    case VM_AND:
        buf[0] |= 9;
    break;
    case VM_OR:
        buf[0] |= 10;
    break;
    case VM_XOR:
        buf[0] |= 11;
    break;
    default:
        fprintf(stderr, "ERR:  Unknown op in compileop: %d\n", op);
        exit(1);
      } /*switch*/
    return buf + 8;
  } /*compileop*/

static int issprmval(const struct vm_statement *v)
  /* is operand v a reference to an SPRM value. */
  {
    return v->op == VM_VAL && v->i1 >= -128 && v->i1 < 0;
  } /*issprmval*/

static unsigned char *compileexpr(unsigned char *buf, int target, struct vm_statement *cs)
  /* generates code to put the the value of an expression cs into GPRM target.
    Returns pointer to after generated code. */
  {
    struct vm_statement *v, **vp;
    bool isassoc, canusesprm;
    if (cs->op == VM_VAL) /* simple value reference */
        return compileop(buf, target, VM_VAL, cs->i1); /* assign value to target */

    isassoc = /* associative operator--just so happens these are also commutative */
            cs->op == VM_ADD
        ||
            cs->op == VM_MUL
        ||
            cs->op == VM_AND
        ||
            cs->op == VM_OR
        ||
            cs->op == VM_XOR;
    canusesprm = cs->op == VM_AND || cs->op == VM_OR || cs->op == VM_XOR;
      /* operations where the source may be an SPRM (also VM_VAL, but that was already dealt with) */

    if (isassoc)
      {
      /* if one of the source operands is the destination register, move it to the front */
        for (vp = &cs->param->next; *vp; vp = &(vp[0]->next))
            if (vp[0]->op == VM_VAL && vp[0]->i1 == target - 256)
              {
                v = *vp;
                *vp = v->next; /* take out from its place in chain */
                v->next = cs->param;
                cs->param = v; /* and put the VM_VAL op on front of chain */
                break;
              } /*if*/
      } /*if*/

    if (compile_usesreg(cs->param->next, target))
      {
      /* cannot evaluate cs->param directly into target, because target is used
        in evaluation */
        const int t2 = nexttarget(target); /* need another register */
        buf = compileexpr(buf, t2, cs); /* evaluate expr into t2 */
        write8(buf, 0x61, 0x00, 0x00, target, 0x00, t2, 0x00, 0x00);
          /* and then move value of t2 to target */
        buf += 8;
        if (t2 == 15)
          /* just zapped a reserved register whose value might be misinterpreted elsewhere */
          {
            write8(buf, 0x71, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00);
              /* g15 = 0 */
            buf += 8;
          } /*if*/
        return buf;
      } /*if*/

    if (isassoc && cs->param->op == VM_VAL && cs->param->i1 != target - 256)
      {
        // if the first param is a value, then try to move a complex operation farther up or an SPRM access (if SPRM ops are not allowed)
        for (vp = &cs->param->next; *vp; vp = &(vp[0]->next))
            if (vp[0]->op != VM_VAL || canusesprm < issprmval(vp[0]))
              {
                v = *vp;
                *vp = v->next; /* take out from its place in chain */
                v->next = cs->param;
                cs->param = v; /* and put the SPRM/non-VM_VAL op on front of chain */
                break;
              } /*if*/
      } /*if*/

    // special case -- rnd where the parameter is a reg or value
    if (cs->op == VM_RND && cs->param->op == VM_VAL)
      {
        assert(cs->param->next == 0); /* only one operand */
        return compileop(buf, target, cs->op, cs->param->i1);
      } /*if*/

    buf = compileexpr(buf, target, cs->param); /* use target for first/only operand */
    if (cs->op == VM_RND)
      {
        assert(cs->param->next == 0); /* only one operand */
        return compileop(buf, target, cs->op, target - 256);
          /* operand from target, result into target */
      }
    else /* all other operators take two operands */
      {
        for (v = cs->param->next; v; v = v->next) /* process chain of operations */
          {
            if (v->op == VM_VAL && canusesprm >= issprmval(v))
                buf = compileop(buf, target, cs->op, v->i1);
                  /* can simply put function value straight into target */
            else
              {
                const int t2 = nexttarget(target);
                buf = compileexpr(buf, t2, v); /* put value of v into t2 */
                buf = compileop(buf, target, cs->op, t2 - 256);
                  /* then value of that and target operated on by op into target */
                if (t2 == 15)
                  {
                  /* just zapped a reserved register whose value might be misinterpreted elsewhere */
                    write8(buf, 0x71, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00);
                      /* g15 = 0 */
                    buf += 8;
                  } /*if*/
              } /*if*/
          } /*for*/
      } /*if*/
    return buf;
  } /*compileexpr*/

static unsigned char *compilebool
  (
    const unsigned char *obuf, /* start of buffer, for calculating instruction numbers */
    unsigned char *buf, /* where to insert compiled instructions */
    struct vm_statement *cs, /* expression to compile */
    const unsigned char *iftrue, /* branch target for true */
    const unsigned char *iffalse /* branch target for false */
 )
  /* compiles an expression that returns a true/false result, branching to iftrue or
    iffalse depending. Returns pointer to after generated code. */
  {
    switch (cs->op)
      {
    case VM_EQ:
    case VM_NE:
    case VM_GTE:
    case VM_GT:
    case VM_LTE:
    case VM_LT:
      { /* the two operands are cs->param and cs->param->next */
        int r1, r2, op;
        op = cs->op - VM_EQ + 2;
          /* convert to comparison encoding that can be inserted directly into instruction */
        if (cs->param->op == VM_VAL)
            r1 = cs->param->i1; /* value already here */
        else /* cs->param is something more complex */
          {
            r1 = nextval(0); /* temporary place to put it */
            buf = compileexpr(buf, r1 & 15, cs->param); /* put it there */
          } /*if*/
      /* at this point, r1 is literal/register containing first operand */
        if (cs->param->next->op == VM_VAL && (r1 < 0 || cs->param->next->i1 < 0))
          /* if one operand is a register and the other is a simple literal or register,
            I can combine them directly */
            r2 = cs->param->next->i1;
        else /* not so simple */
          {
            r2 = nextval(r1);
            buf = compileexpr(buf, r2 & 15, cs->param->next);
          } /*if*/
      /* at this point, r2 is literal/register containing second operand */
        if (r1 >= 0)
          {
          /* literal value--swap with r2 */
            const int t = r1;
            r1 = r2;
            r2 = t;
            op = swapcompare(op);
          } /*if*/
        if (iffalse > iftrue)
          {
          /* make false branch the one earlier in buffer, in the hope I can fall through to it */
          /* really only worth doing this if iffalse - buf == 8 */
            const unsigned char * const t = iftrue;
            iftrue = iffalse;
            iffalse = t;
            op = negatecompare(op);
          } /*if*/
        if (r2 >= 0)
            write8(buf, 0x00, 0x81 | (op << 4), 0x00, r1, r2 >> 8, r2,0x00, 0x00);
              /* if r1 op r2 then goto true branch */
        else /* r1 and r2 both registers */
            write8(buf, 0x00, 0x01 | (op << 4), 0x00, r1, 0x00, r2, 0x00, 0x00);
              /* if r1 op r2 then goto true branch */
        buf[7] = (iftrue - obuf) / 8 + 1; /* branch target instr nr */
        buf += 8;
        if (iffalse > buf)
          {
          /* can't fallthrough for false branch */
            write8(buf, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
              /* goto false branch */
            buf[7] = (iffalse - obuf) / 8 + 1; /* branch target */
            buf += 8;
          } /*if*/
      } /*case*/
    break;

    case VM_LOR:
    case VM_LAND:
      {
        const int op = cs->op;
        cs = cs->param;
        while (cs->next) /* process chain of operands, doing short-cut evaluation */
          {
          /* compile this operand, branching to next or to final true/false destination
            as appropriate */
            unsigned char * n = buf + 8;
              /* where to continue chain--assume it'll compile to one instruction to begin with */
            while (true) /* should loop no more than twice */
              {
                unsigned char * const nn = compilebool
                  (
                    /*obuf =*/ obuf,
                    /*buf =*/ buf,
                    /*cs =*/ cs,
                    /*iftrue =*/
                        op == VM_LAND ?
                            n /* continue AND-chain as long as conditions are true */
                        :
                            iftrue, /* no need to continue OR-chain on true */
                    /*iffalse =*/
                        op == VM_LOR ?
                            n /* continue OR-chain as long as conditions are false */
                        :
                            iffalse /* no need to continue AND-chain on false */
                  );
                if (nn == n)
                    break;
              /* too many instructions generated to fit */
                n = nn; /* try again leaving this much room */
              } /*while*/
            buf = n;
            cs = cs->next;
          } /*while*/
        buf = compilebool(obuf, buf, cs, iftrue, iffalse);
      }
    break;

    case VM_NOT:
        return compilebool(obuf, buf, cs->param, iffalse, iftrue);
          /* re-enter myself with the operands swapped */

    default:
        fprintf(stderr, "ERR:  Unknown bool op: %d\n", cs->op);
        exit(1);
      } /*switch*/
    return buf;
  } /*compilebool*/

// NOTE: curgroup is passed separately from curpgc, because in FPC, curpgc==NULL, but curgroup!=NULL
static unsigned char *compilecs
  (
    const unsigned char *obuf,
    unsigned char *buf,
    const struct workset *ws,
    const struct pgcgroup *curgroup,
    const struct pgc *curpgc,
    const struct vm_statement *cs,
    vtypes ismenu /* needed to decide what kinds of jumps/calls are allowed */
  )
  /* compiles a parse tree into naive VM instructions: no optimization of conditionals,
    and no fixup of gotos; these tasks are left to caller. */
  {
    bool lastif = false;
    while (cs)
      {
        if (cs->op != VM_NOP)
            lastif = false; /* no need for dummy target for last branch, I'll be providing a real one */
          /* actually check for VM_NOP is unnecessary so long as no construct will
            generate one */
        switch (cs->op)
          {
        case VM_SET: /* cs->i1 is destination, cs->param is source */
            switch (cs->i1)
              {
            case 0:
            case 1:
            case 2:
            case 3:
            case 4:
            case 5:
            case 6:
            case 7:
            case 8:
            case 9:
            case 10:
            case 11:
            case 12:
            case 13:
            case 14:
            case 15: // set GPRM
                buf = compileexpr(buf, cs->i1, cs->param);
            break;

            case 32 + 0:
            case 32 + 1:
            case 32 + 2:
            case 32 + 3:
            case 32 + 4:
            case 32 + 5:
            case 32 + 6:
            case 32 + 7:
            case 32 + 8:
            case 32 + 9:
            case 32 + 10:
            case 32 + 11:
            case 32 + 12:
            case 32 + 13:
            case 32 + 14:
            case 32 + 15: // set GPRM, counter mode
                if (cs->param->op == VM_VAL)
                  { // we can set counters to system registers
                    const int v = cs->param->i1; /* reg/literal value to set */
                    if (v < 0)
                        write8(buf, 0x43, 0x00, 0x00, v,0x00, 0x80 | (cs->i1 - 32), 0x00, 0x00);
                          /* SetGPRMMD indirect */
                    else
                        write8(buf, 0x53, 0x00, v / 256, v, 0x00, 0x80 | (cs->i1 - 32), 0x00, 0x00);
                          /* SetGPRMMD direct */
                    buf += 8;
                  }
                else /* not so simple */
                  {
                    const int r = nexttarget(0); /* temporary place to put value */
                    buf = compileexpr(buf, r, cs->param); /* put it there */
                    write8(buf, 0x43, 0x00, 0x00, r, 0x00, 0x80 | (cs->i1 - 32), 0x00, 0x00);
                      /* SetGPRMMD indirect to r */
                    buf += 8;
                  } /*if*/
            break;

            case 128 + 1: // audio
            case 128 + 2: // subtitle
            case 128 + 3: // angle
                if (cs->param->op == VM_VAL && !(cs->param->i1 >= -128 && cs->param->i1 < 0))
                  {
                    const int v = cs->param->i1;
                    write8(buf, v < 0 ? 0x41 : 0x51, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
                      /* SetSTN indirect/direct */
                    buf[(cs->i1 - 128) + 2] = 128 | v; // doesn't matter whether this is a register or a value
                      /* put new value/regnr into audio/subpicture/angle field as appropriate */
                    buf += 8;
                  }
                else /* complex expression */
                  {
                    const int r = nexttarget(0);
                    buf = compileexpr(buf, r, cs->param);
                    write8(buf, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
                      /* SetSTN indirect */
                    buf[(cs->i1 - 128) + 2] = 128 | r;
                      /* put regnr containing new value into audio/subpicture/angle field as appropriate */
                    buf += 8;
                  } /*if*/
            break;

            case 128 + 8: // button
                if (cs->param->op == VM_VAL && !(cs->param->i1 >= -128 && cs->param->i1 < 0))
                  {
                    const int v = cs->param->i1;
                    if (v > 0 && (v & 1023) != 0)
                        fprintf
                          (
                            stderr,
                            "WARN: Button value is %d, but it should be a multiple of 1024\n"
                              "WARN: (button #1=1024, #2=2048, etc)\n",
                            v
                          );
                    if (v < 0)
                        write8(buf, 0x46, 0x00, 0x00, 0x00, 0x00, v, 0x00, 0x00);
                          /* SetHL_BTNN indirect */
                    else
                        write8(buf, 0x56, 0x00, 0x00, 0x00, v / 256, v, 0x00, 0x00);
                          /* SetHL_BTNN direct */
                    buf += 8;
                  }
                else /* complex expression */
                  {
                    const int r = nexttarget(0);
                    buf = compileexpr(buf, r, cs->param);
                    write8(buf, 0x46, 0x00, 0x00, 0x00, 0x00, r, 0x00, 0x00);
                      /* SetHL_BTNN indirect */
                    buf += 8;
                  } /*if*/
                break;

            default:
                fprintf(stderr, "ERR:  Cannot set SPRM %d\n", cs->i1 - 128);
                return 0;
              } /*switch*/
            break;

        case VM_IF: /* if-statement */
          {
            unsigned char * iftrue = buf + 8; /* initially try putting true branch here */
            const unsigned char * iffalse = buf + 16; /* initially try putting false branch here */
            unsigned char * end = buf + 16; /* initially assuming code will end here */
            while (true) /* should loop no more than twice */
              {
                unsigned char *lp, *ib, *e;
                lp = compilecs(obuf, iftrue, ws, curgroup, curpgc, cs->param->next->param, ismenu);
                  /* the if-true part */
                if (cs->param->next->next)
                  {
                  /* there's an else-part */
                    e = compilecs(obuf, lp + 8, ws, curgroup, curpgc, cs->param->next->next, ismenu);
                      /* compile the else-part, leaving room for following instr */
                    write8(lp, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, (e - obuf ) / 8 + 1);
                      /* insert a goto at the end of the if-true part to branch over the else-part */
                    lp += 8; /* include in true branch */
                  }
                else
                    e = lp; /* code ends with true branch */
                ib = compilebool(obuf, buf, cs->param, iftrue, iffalse);
                  /* put condition test at start */
                if (!lp)
                    return 0;
              /* at this point, ib is just after the condition test, lp is just after
                the true branch, and e is just after the end of all the code */
                if (ib == iftrue && lp == iffalse)
                    break; /* all fitted nicely */
              /* didn't leave enough room for pieces next to each other, try again */
                iftrue = ib; /* enough room for condition code */
                iffalse = lp; /* enough room for true branch */
                end = e;
              } /*while*/
            buf = end;
            lastif = true; // make sure reference statement is generated
          }
        break;

        case VM_LABEL:
          {
            int i;
            for (i = 0; i < numlabels; i++)
                if (!strcasecmp(labels[i].lname, cs->s1))
                  {
                    fprintf(stderr, "ERR:  Duplicate label '%s'\n", cs->s1);
                    return 0;
                  } /*if; for*/
            if (numlabels == MAXLABELS)
              {
                fprintf(stderr, "ERR:  Too many labels\n");
                return 0;
              } /*if*/
            labels[numlabels].lname = cs->s1;
            labels[numlabels].code = buf; /* where label points to */
            numlabels++;
            lastif = true; // make sure reference statement is generated
          }
        break;

        case VM_GOTO:
            if (numgotos == MAXGOTOS)
              {
                fprintf(stderr, "ERR:  Too many gotos\n");
                return 0;
              } /*if*/
            gotos[numgotos].lname = cs->s1;
            gotos[numgotos].code = buf; /* point to instruction so it can be fixed up later */
            numgotos++;
            write8(buf, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
            buf += 8;
        break;

        case VM_BREAK:
            write8(buf, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
            buf += 8;
        break;

        case VM_EXIT:
            write8(buf, 0x30, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
            buf += 8;
        break;

        case VM_LINK:
            write8(buf, 0x20, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, cs->i1);
            buf += 8;
        break;

        case VM_JUMP:
          {
            int i1 = cs->i1; /* if nonzero, 1 for VMGM, or titleset nr + 1 */
            int i2 = cs->i2; /* menu number or menu entry ID + 120 or title number + 128 */
          /* cs->i3 is PGC number if nonzero and less than 65536; or chapter number + 65536;
            or program number + 131072; or cell number + 196608 */

          /* check for various disallowed combinations */
            if (i1 == 1 && ismenu == VTYPE_VMGM)
              {
                //  VMGM    VMGM    NOPGC   NOCH
                //  VMGM    VMGM    NOPGC   CHXX
                //  VMGM    VMGM    MPGC    NOCH
                //  VMGM    VMGM    MPGC    CHXX
                //  VMGM    VMGM    MEPGC   NOCH
                //  VMGM    VMGM    MEPGC   CHXX
                //  VMGM    VMGM    TPGC    NOCH
                //  VMGM    VMGM    TPGC    CHXX
                i1 = 0; /* no need to explicitly specify VMGM */
              } /*if*/
            if
              (
                    (
                        i2 > 0 && i2 < 128 /* jump to non-entry menu */
                    ||
                        i2 == 0 && i1 == 1 /* jump to VMGM */
                    )
                &&
                    ismenu == VTYPE_VTS
              )
              {
                //  VTS     NONE    MPGC    NOCH
                //  VTS     VMGM    MPGC    NOCH
                //  VTS     TS      MPGC    NOCH
                //  VTS     NONE    MPGC    CHXX
                //  VTS     VMGM    MPGC    CHXX
                //  VTS     TS      MPGC    CHXX
                //  VTS     NONE    MEPGC   NOCH
                //  VTS     VMGM    MEPGC   NOCH
                //  VTS     TS      MEPGC   NOCH
                //  VTS     NONE    MEPGC   CHXX
                //  VTS     VMGM    MEPGC   CHXX
                //  VTS     TS      MEPGC   CHXX
                //  VTS     VMGM    NOPGC   NOCH
                //  VTS     VMGM    NOPGC   CHXX

                fprintf(stderr, "ERR:  Cannot jump to a menu from a title, use 'call' instead\n");
                return 0;
              } /*if*/
            if
              (
                    i2 > 0
                &&
                    i2 < 128 /* jump to non-entry menu */
                &&
                    (cs->i3 & 65535) /* PGC/chapter/cell/program specified */
                &&
                    ismenu != VTYPE_VTS
              )
              {
                //  VMGM    NONE    MPGC    CHXX
                //  VMGM    TS      MPGC    CHXX
                //  VMGM    NONE    MEPGC   CHXX
                //  VMGM    TS      MEPGC   CHXX
                //  VTSM    NONE    MPGC    CHXX
                //  VTSM    VMGM    MPGC    CHXX
                //  VTSM    TS      MPGC    CHXX
                //  VTSM    NONE    MEPGC   CHXX
                //  VTSM    VMGM    MEPGC   CHXX
                //  VTSM    TS      MEPGC   CHXX
                fprintf(stderr, "ERR:  Cannot specify chapter when jumping to another menu\n");
                return 0;
              } /*if*/
            if (i1 /*VMGM/titleset*/ && !i2 /*no PGC*/)
              {
                //  VTSM    VMGM    NOPGC   CHXX
                //  VTS     TS      NOPGC   CHXX
                //  VTSM    TS      NOPGC   CHXX
                //  VMGM    TS      NOPGC   CHXX
                //  VTS     TS      NOPGC   NOCH
                //  VTSM    TS      NOPGC   NOCH
                //  VMGM    TS      NOPGC   NOCH
                fprintf(stderr, "ERR:  Cannot omit menu/title if specifying vmgm/titleset\n");
                return 0;
              } /*if*/
            if
              (
                    !i1 /*same VMGM/titleset*/
                &&
                    !i2 /*same PGC*/
                &&
                    !(cs->i3 & 65535) /*no PGC/chapter/cell/program*/
              )
              {
                //  VTS     NONE    NOPGC   NOCH
                //  VTSM    NONE    NOPGC   NOCH
                //  VMGM    NONE    NOPGC   NOCH
                fprintf(stderr, "ERR:  Nop jump statement\n");
                return 0;
              } /*if*/
            if
              (
                    i2 == 121 /*jump to FPC*/
                &&
                    (
                        i1 >= 2 /*titleset*/
                    ||
                        (i1 == 0 /*current VMGM/titleset*/ && ismenu != VTYPE_VMGM)
                    )
              )
              {
                fprintf(stderr, "ERR:  VMGM must be specified with FPC\n");
                return 0;
              } /*if*/

            // *** ACTUAL COMPILING
            if
              (
                    i1 >= 2 /*titleset*/
                &&
                    i2 >= 120
                &&
                    i2 < 128 /*entry PGC*/
              )
              {
                //  VTSM    TS      MEPGC   NOCH
                //  VMGM    TS      MEPGC   NOCH
                if (i2 == 120) /* "default" entry means "root" */
                    i2 = 123;
                write8(buf, 0x30, 0x06, 0x00, 0x01, i1 - 1, 0x80 + (i2 - 120), 0x00, 0x00); buf += 8; // JumpSS VTSM vts 1 menu
              }
            else if
              (
                  i1 >= 2 /*jump to titleset*/
              ||
                  i1 == 1 /*jump to VMGM*/ && i2 >= 128 /*title*/
              ||
                  ismenu == VTYPE_VMGM && i2 >= 128 /*title*/ && (cs->i3 & 65535) != 0 /*chapter/program/cell*/
              )
              {
                //  VMGM    TS      TPGC    CHXX
                //  VTSM    TS      MPGC    NOCH
                //  VMGM    TS      MPGC    NOCH
                //  VTS     TS      TPGC    NOCH
                //  VTSM    TS      TPGC    NOCH
                //  VMGM    TS      TPGC    NOCH
                //  VTS     TS      TPGC    CHXX
                //  VTSM    TS      TPGC    CHXX
                //  VTS     VMGM    TPGC    NOCH
                //  VTSM    VMGM    TPGC    NOCH
                //  VTS     VMGM    TPGC    CHXX
                //  VTSM    VMGM    TPGC    CHXX
                //  VMGM    NONE    TPGC    CHXX
                if (jumppad)
                  {
                    if (!i1)
                        i1 = 1; /* make VMGM explicit */
                    write8(buf, 0x71, 0x00, 0x00, 0x0F, i2, i1, 0x00, 0x00);
                      /* g15 = i2 << 8 | i1 */
                    buf += 8;
                    write8(buf, 0x71, 0x00, 0x00, 0x0E, 0x00, cs->i3, 0x00, 0x00);
                      /* g14 = cs->i3 */
                    buf += 8;
                    write8(buf, 0x30, ismenu != VTYPE_VTS ? 0x06 : 0x08, 0x00, 0x00, 0x00, 0x42, 0x00, 0x00);
                      /* JumpSS/CallSS VMGM menu entry title */
                    buf += 8;
                  }
                else
                  {
                  /* JumpTT allows directly jumping from VMGM to any titleset, but that
                    requires global title numbers, not titleset-local title numbers,
                    and I only work these out later when putting together the final VMGM,
                    which is why I can't handle it here without the jumppad */
                    fprintf(stderr, "ERR:  That form of jumping is not allowed\n");
                    return 0;
                  } /*if*/
              }
            else  if (i1 == 1 /*jump to VMGM*/ || i2 == 121 /*jump to FPC*/)
              {
                //  VTSM    VMGM    NOPGC   NOCH
                //  VTSM    VMGM    MPGC    NOCH
                //  VTSM    VMGM    MEPGC   NOCH
                // cannot error check jumps to the vmgm menu
                if (!i2 || i2 == 120)
                    i2 = 122; /* must be title menu */
                if (i2 < 120)
                    write8(buf, 0x30, 0x06, 0x00, i2, 0x00, 0xC0, 0x00, 0x00); // JumpSS VMGM pgcn
                else
                    write8(buf, 0x30, 0x06, 0x00, 0x00, 0x00, i2 == 121 ? 0 : (0x40 + i2 - 120), 0x00, 0x00); // JumpSS FP or JumpSS VMGM menu
                buf += 8;
              }
            else if (!i1 && !i2 && (cs->i3 & 65535))
              {
                int numc;
                const char *des;

                //  VTS     NONE    NOPGC   CHXX
                //  VTSM    NONE    NOPGC   CHXX
                //  VMGM    NONE    NOPGC   CHXX
                if (curpgc == 0)
                  {
                    fprintf(stderr, "ERR:  Cannot jump to a chapter from FPC\n");
                    return 0;
                  } /*if*/
                if (cs->i3 >> 16 == 1 && ismenu != VTYPE_VTS)
                  {
                    fprintf(stderr, "ERR:  Menus do not have chapters\n");
                    return 0;
                  } /*if*/
                switch (cs->i3 >> 16)
                  {
                case 0:
                    numc = curgroup->numpgcs;
                    des = "pgc";
                break;
                case 1:
                    numc = curpgc->numchapters;
                    des = "chapter";
                break;
                case 2:
                    numc = curpgc->numprograms;
                    des = "program";
                break;
                case 3:
                    numc = curpgc->numcells;
                    des = "cell";
                break;
                default: /* shouldn't occur! */
                    numc = 0;
                    des = "<err>";
                break;
                  } /*switch*/
                if ((cs->i3 & 65535) > numc)
                  {
                    fprintf(stderr, "ERR:  Cannot jump to %s %d, only %d exist\n", des, cs->i3 & 65535, numc);
                    return 0;
                  } /*if*/
                write8(buf, 0x20, 0x04 + (cs->i3 >> 16), 0x00, 0x00, 0x00, 0x00, 0x00, cs->i3); // LinkPGCN pgcn, LinkPTTN pttn, LinkPGCN pgn, or LinkCN cn
                buf += 8;
              }
            else if (i2 < 128) /* menu */
              {
                //  VTSM    NONE    MPGC    NOCH
                //  VMGM    NONE    MPGC    NOCH
                //  VTSM    NONE    MEPGC   NOCH
                //  VMGM    NONE    MEPGC   NOCH
                if (!curgroup)
                  {
                    fprintf(stderr,"ERR:  Cannot jump to menu; none exist\n");
                    return 0;
                  }
                else if (i2 >= 120 && i2 < 128) /* menu entry */
                  {
                    int i;
                    for (i = 0; i < curgroup->numpgcs; i++)
                        if (curgroup->pgcs[i]->entries & (1 << (i2 - 120)))
                          {
                            i2 = i + 1;
                            break;
                          } /*if; for*/
                    if (i2 >= 120)
                      {
                        fprintf(stderr, "ERR:  Cannot find PGC with entry %s\n", entries[i2 - 120]);
                        return 0;
                      } /*if*/
                  }
                else /* non-entry menu */
                  {
                    if (i2 > curgroup->numpgcs)
                      {
                        fprintf(stderr, "ERR:  Cannot jump to menu PGC #%d, only %d exist\n", i2, curgroup->numpgcs);
                        return 0;
                      } /*if*/
                  } /*if*/
                if (ismenu == VTYPE_VMGM)
                  {
                    // In case we are jumping from FP to VMGM, we need to use a JumpSS
                    // instruction
                    write8(buf, 0x30, 0x06, 0x00, i2 & 127, 0x00, 0xc0, 0x00, 0x00); // JumpSS VMGM pgcn
                  }
                else
                    write8(buf, 0x20, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, i2 & 127); // LinkPGCN pgcn
                buf += 8;
              }
            else
              {
                //  VMGM    NONE    TPGC    NOCH
                //  VTS     NONE    TPGC    NOCH
                //  VTSM    NONE    TPGC    NOCH
                //  VTS     NONE    TPGC    CHXX
                //  VTSM    NONE    TPGC    CHXX
                if (ismenu < VTYPE_VMGM) /* VTS or VTSM */
                  {
                    if (i2 - 128 > ws->titles->numpgcs) /* title nr */
                      {
                        fprintf
                          (
                            stderr,
                            "ERR:  Cannot jump to title #%d, only %d exist\n",
                            i2 - 128,
                            ws->titles->numpgcs
                          );
                        return 0;
                      } /*if*/
                    if ((cs->i3 & 65535) != 0 && (cs->i3 & 65535) > ws->titles->pgcs[i2 - 128 - 1]->numchapters)
                      {
                        fprintf
                          (
                            stderr,
                            "ERR:  Cannot jump to chapter %d of title %d, only %d exist\n",
                            cs->i3 & 65535,
                            i2 - 128,
                            ws->titles->pgcs[i2 - 128 - 1]->numchapters
                          );
                        return 0;
                      } /*if*/
                  } /*if*/
                write8
                  (
                    buf,
                    0x30,
                    ismenu == VTYPE_VMGM ?
                        0x02 /*JumpTT*/
                    : (cs->i3 & 65535) != 0 ?
                        0x05 /*JumpVTS_PTT*/
                    :
                        0x03 /*JumpVTS_TT*/,
                    0x00,
                    cs->i3, /* chapter if present */
                    0x00,
                    i2 - 128, /* title nr */
                    0x00,
                    0x00
                  );
                buf += 8;
              } /*if*/
          }
        break;

        case VM_CALL:
          {
          /* cs->i1 if nonzero is 1 for VMGM, or titleset nr + 1 */
            int i2 = cs->i2; /* menu number or menu entry ID + 120 or title number + 128 */
          /* cs->i3 is chapter number if specified, else zero */
            int i4 = cs->i4; /* resume cell if specified, else zero */

            // CALL's from <post> MUST have a resume cell
            if (!i4)
                i4 = 1; /* resume from start if not specified */
            if (ismenu != VTYPE_VTS)
              {
                //  VTSM    NONE    NOPGC   NOCH
                //  VMGM    NONE    NOPGC   NOCH
                //  VTSM    VMGM    NOPGC   NOCH
                //  VMGM    VMGM    NOPGC   NOCH
                //  VTSM    TS      NOPGC   NOCH
                //  VMGM    TS      NOPGC   NOCH
                //  VTSM    NONE    NOPGC   CHXX
                //  VMGM    NONE    NOPGC   CHXX
                //  VTSM    VMGM    NOPGC   CHXX
                //  VMGM    VMGM    NOPGC   CHXX
                //  VTSM    TS      NOPGC   CHXX
                //  VMGM    TS      NOPGC   CHXX
                //  VTSM    NONE    MPGC    NOCH
                //  VMGM    NONE    MPGC    NOCH
                //  VTSM    VMGM    MPGC    NOCH
                //  VMGM    VMGM    MPGC    NOCH
                //  VTSM    TS      MPGC    NOCH
                //  VMGM    TS      MPGC    NOCH
                //  VTSM    NONE    MPGC    CHXX
                //  VMGM    NONE    MPGC    CHXX
                //  VTSM    VMGM    MPGC    CHXX
                //  VMGM    VMGM    MPGC    CHXX
                //  VTSM    TS      MPGC    CHXX
                //  VMGM    TS      MPGC    CHXX
                //  VTSM    NONE    MEPGC   NOCH
                //  VMGM    NONE    MEPGC   NOCH
                //  VTSM    VMGM    MEPGC   NOCH
                //  VMGM    VMGM    MEPGC   NOCH
                //  VTSM    TS      MEPGC   NOCH
                //  VMGM    TS      MEPGC   NOCH
                //  VTSM    NONE    MEPGC   CHXX
                //  VMGM    NONE    MEPGC   CHXX
                //  VTSM    VMGM    MEPGC   CHXX
                //  VMGM    VMGM    MEPGC   CHXX
                //  VTSM    TS      MEPGC   CHXX
                //  VMGM    TS      MEPGC   CHXX
                //  VTSM    NONE    TPGC    NOCH
                //  VMGM    NONE    TPGC    NOCH
                //  VTSM    VMGM    TPGC    NOCH
                //  VMGM    VMGM    TPGC    NOCH
                //  VTSM    TS      TPGC    NOCH
                //  VMGM    TS      TPGC    NOCH
                //  VTSM    NONE    TPGC    CHXX
                //  VMGM    NONE    TPGC    CHXX
                //  VTSM    VMGM    TPGC    CHXX
                //  VMGM    VMGM    TPGC    CHXX
                //  VTSM    TS      TPGC    CHXX
                //  VMGM    TS      TPGC    CHXX
                fprintf(stderr, "ERR:  Cannot 'call' a menu from another menu, use 'jump' instead\n");
                return 0;
              } /*if*/
            if (i2 == 0 || i2 >= 128) /* title nr or no menu/title */
              {
                //  VTS NONE    NOPGC   NOCH
                //  VTS VMGM    NOPGC   NOCH
                //  VTS TS      NOPGC   NOCH
                //  VTS NONE    NOPGC   CHXX
                //  VTS VMGM    NOPGC   CHXX
                //  VTS TS      NOPGC   CHXX
                //  VTS NONE    TPGC    NOCH
                //  VTS VMGM    TPGC    NOCH
                //  VTS TS      TPGC    NOCH
                //  VTS NONE    TPGC    CHXX
                //  VTS VMGM    TPGC    CHXX
                //  VTS TS      TPGC    CHXX

                fprintf(stderr, "ERR:  Cannot 'call' another title, use 'jump' instead\n");
                return 0;
              } /*if*/
            if (cs->i3 != 0) /* chapter/cell/program */
              {
                //  VTS NONE    MPGC    CHXX
                //  VTS VMGM    MPGC    CHXX
                //  VTS TS      MPGC    CHXX
                //  VTS NONE    MEPGC   CHXX
                //  VTS VMGM    MEPGC   CHXX
                //  VTS TS      MEPGC   CHXX
                fprintf(stderr, "ERR:  Cannot 'call' a chapter within a menu\n");
                return 0;
              } /*if*/
            if
              (
                    i2 == 121 /*FPC*/
                &&
                    (
                        cs->i1 >= 2 /*titleset*/
                    ||
                        cs->i1 == 0 /*no VMGM/titleset*/ && ismenu != VTYPE_VMGM
                    )
              )
              {
                fprintf(stderr, "ERR:  VMGM must be specified with FPC\n");
                return 0;
              } /*if*/

            if (cs->i1 >= 2) /*titleset*/
              {
                //  VTS TS  MPGC    NOCH
                //  VTS TS  MEPGC   NOCH
                if (jumppad)
                  {
                    write8(buf, 0x71, 0x00, 0x00, 0x0F, i2, cs->i1, 0x00, 0x00); buf += 8;
                    write8(buf, 0x71, 0x00, 0x00, 0x0E, 0x00, cs->i3, 0x00, 0x00); buf += 8;
                    write8(buf, 0x30, 0x08, 0x00, 0x00, i4, 0x42, 0x00, 0x00); buf += 8;
                  }
                else
                  {
                    fprintf(stderr, "ERR:  Cannot call to a menu in another titleset\n");
                    return 0;
                  } /*if*/
              }
            else if (cs->i1 == 0 && i2 < 120) /* non-entry menu in current VMGM/titleset */
              {
                //  VTS NONE    MPGC    NOCH
                if (jumppad)
                  {
                    write8(buf, 0x71, 0x00, 0x00, 0x0F, i2, 0x00, 0x00, 0x00); buf += 8;
                    write8(buf, 0x30, 0x08, 0x00, 0x00, i4, 0x87, 0x00, 0x00); buf += 8;
                  }
                else
                  {
                    fprintf(stderr, "ERR:  Cannot call to a specific menu PGC, only an entry\n");
                    return 0;
                  } /*if*/
              }
            else if (cs->i1 == 1) /* jump to VMGM */
              {
                //  VTS VMGM    MPGC    NOCH
                //  VTS VMGM    MEPGC   NOCH
                // we cannot provide error checking when jumping to a VMGM
                if (i2 == 120) /* "default" entry means "title" */
                    i2 = 122;
                if (i2 < 120)
                    write8(buf, 0x30, 0x08, 0x00, i2, i4, 0xC0, 0x00, 0x00);
                else
                    write8(buf, 0x30, 0x08, 0x00, 0x00, i4, i2 == 121 ? 0 : (0x40 + i2 - 120), 0x00, 0x00);
                buf += 8;
              }
            else
              {
                int i, j;
                //  VTS NONE    MEPGC   NOCH
                if (i2 == 120)
                    i2 = 127; /* "default" means chapter menu? */
                for (j = 0; j < ws->menus->numgroups; j++)
                  {
                    const struct pgcgroup * const pg = ws->menus->groups[j].pg;
                    for (i = 0; i < pg->numpgcs; i++)
                        if (pg->pgcs[i]->entries & (1 << (i2 - 120)))
                            goto foundpgc;
                  } /*for*/
                fprintf(stderr, "ERR:  Cannot find PGC with entry %s\n", entries[i2 - 120]);
                return 0;
            foundpgc:
                write8(buf, 0x30, 0x08, 0x00, 0x00, i4 /*rsm cell*/, 0x80 + i2 - 120 /*VTSM menu*/, 0x00, 0x00);
                  /* CallSS VTSM menu, rsm_cell */
                buf += 8;
              } /*if*/
          }
        break;

        case VM_NOP:
          /* nothing to do */
        break;

        default:
            fprintf(stderr,"ERR:  Unsupported VM opcode %d\n",cs->op);
            return 0;
          } /*switch*/
        cs = cs->next;
      } /*while*/
    if (lastif)
      {
      /* need target for last branch */
        write8(buf, 0, 0, 0, 0, 0, 0, 0, 0); /* NOP */
        buf += 8;
      } /*if*/
    return buf;
  } /*compilecs*/

static unsigned int extractif(const unsigned char *b)
  /* extracts the common bits from a conditional instruction into a format
    that can be reapplied to another instruction. */
  {
    switch (b[0] >> 4)
      {
    case 0:
    case 1:
    case 2:
        return
                (b[1] >> 4) << 24 /* the comparison op and direct-operand-2 flag */
            |
                b[3] << 16 /* operand 1 for the comparison */
            |
                b[4] << 8 /* operand 2, high byte */
            |
                b[5]; /* operand 2, low byte */

    default:
        fprintf(stderr, "ERR:  Unhandled extractif scenario (%x), file bug\n", b[0]);
        exit(1);
      } /*switch*/
  } /*extractif*/

static unsigned int negateif(unsigned int ifs)
  /* negates the comparison op part of a value returned from extractif. */
  {
    return
            ifs & 0x8ffffff /* remove comparison op, leave direct flag and operands */
        |
            negatecompare((ifs >> 24) & 7) << 24; /* replace with opposite comparison */
  } /*negateif*/

static void applyif(unsigned char *b,unsigned int ifs)
  /* inserts the bits extracted by extractif into a new instruction. */
  {
    switch (b[0] >> 4)
      {
    case 0:
    case 1:
    case 2:
        b[5] = ifs;
        b[4] = ifs >> 8;
        b[3] = ifs >> 16;
        b[1] |= (ifs >> 24) << 4;
    break;

    case 3:
    case 4:
    case 5:
        b[7] = ifs; /* assume ifs >> 8 & 255 is zero! */
        b[6] = ifs >> 16;
        b[1] |= (ifs >> 24) << 4;
    break;

    case 6:
    case 7:
        b[7] = ifs;
        b[6] = ifs >> 8;
        b[2] = ifs >> 16;
        b[1] = (ifs >> 24) << 4;
    break;

    default:
        fprintf(stderr,"ERR:  Unhandled applyif scenario (%x), file bug\n", b[0]);
        exit(1);
      } /*switch*/
  } /*applyif*/

static bool ifcombinable(unsigned char b0 /* actually caller always passes 0 */, unsigned char b1, unsigned char b8)
  /* can the instruction whose first two bytes are b0 and b1 have its condition
    combined with the one whose first byte is b8. */
  {
    int iftype = -1;
    switch (b0 >> 4)
      {
    case 0:
    case 1:
    case 2:
    case 6:
    case 7:
        iftype = b1 >> 7; /* 1 if 2nd operand is immediate, 0 if it's a register */
    break;
    case 3:
    case 4:
    case 5:
        iftype = 0; /* 2nd operand always register */
    break;
    default:
        return false;
      } /*switch*/
    switch (b8 >> 4)
      {
    case 0:
    case 1:
    case 2:
    case 6:
    case 7:
        return true; /* can take both immediate and register 2nd operands */
    case 3:
    case 4:
    case 5:
        return iftype == 0; /* can only take register 2nd operand */
    default:
        return false;
      } /*switch*/
  } /*ifcombinable*/

static bool isreferenced(const unsigned char *buf, const unsigned char *end, int linenum)
  /* checks if there are any branches with destination linenum. */
  {
    const unsigned char *b;
    bool referenced;
    for (b = buf;;)
      {
        if (b == end)
          {
            referenced = false;
            break;
          } /*if*/
        if (b[0] == 0 && (b[1] & 15) == 1 && b[7] == linenum)
          /* check for goto -- fixme: should also check for SetTmpPML if I ever implement that */
          {
            referenced = true;
            break;
          } /*if*/
        b += 8;
      } /*for*/
    return
        referenced;
  } /*isreferenced*/

static void deleteinstruction
  (
    const unsigned char *obuf, /* start of instruction buffer */
    unsigned char *buf, /* start of area where branch targets need adjusting */
    unsigned char **end, /* pointer to next free part of buffer, to be updated */
    unsigned char *b /* instruction to be deleted from buffer */
  )
  /* deletes an instruction from the buffer, and moves up the following ones,
    adjusting branches over the deleted instruction as appropriate. */
  {
    unsigned char *b2;
    const int linenum = (b - obuf) / 8 + 1;
    for (b2 = buf; b2 < *end; b2 += 8) /* adjust branches to following instructions */
        if (b2[0] == 0 && (b2[1] & 15) == 1 && b2[7] > linenum)
            b2[7]--;
    memmove(b, b + 8, *end - (b + 8));
    *end -= 8;
    memset(*end, 0, 8); // clean up tracks (so pgc structure is not polluted)
  } /*deleteinstruction*/

static void dumpcode
  (
    const char * descr,
    const unsigned char *obuf, /* start of buffer for computing instruction numbers for branches */
    unsigned char *buf, /* where to insert new compiled code */
    const unsigned char *end /* points to after last instruction generated */
  )
  /* dumps out compiled code for debugging. */
  {
#ifdef VM_DEBUG
    const unsigned int nrlines = (end - buf) / 8;
    unsigned int i, j;
    fprintf(stderr, "* %s:\n", descr);
    for (i = 0; i < nrlines; ++i)
      {
        fprintf(stderr, " %3d:", i + (buf - obuf) + 1);
        for (j = 0; j < 8; ++j)
          {
            fprintf(stderr, " %02X", buf[i * 8 + j]);
          } /*for*/
        fputs("\n", stderr);
      } /*for*/
#endif
  } /*dumpcode*/

void vm_optimize(const unsigned char *obuf, unsigned char *buf, unsigned char **end)
  /* does various peephole optimizations on the part of obuf from buf to *end.
    *end will be updated if unnecessary instructions are removed. */
  {
    unsigned char *b;
 again:
    for (b = buf; b < *end; b += 8)
      {
        const int curline = (b - obuf) / 8 + 1;
        // if
        // 1. this is a jump over one statement
        // 2. we can combine the statement with the if
        // 3. there are no references to the statement
        // then
        // combine statement with if, negate if, and replace statement with nop
        if
          (
                b[0] == 0
            &&
                (b[1] & 0x70) != 0 /* conditional */
            &&
                (b[1] & 15) == 1 /* cmd = goto */
            &&
                b[7] == curline + 2 // step 1
            &&
                (b[9] & 0x70) == 0 /* second instr not conditional */
            &&
                (
                    (b[8] & 15) == 0 /* not a set */
                ||
                    (b[9] & 15) == 0 /* not a link */
                ) /* not set-and-link in one */
            &&
                ifcombinable(b[0], b[1], b[8]) // step 2
            &&
                !isreferenced(buf, *end, curline + 1) // step 3
          )
          {
            const unsigned int ifs = negateif(extractif(b));
            memcpy(b, b + 8, 8); // move statement
            memset(b + 8, 0, 8); // replace with nop
            applyif(b, ifs);
            dumpcode("vm_optimize: jump over one => inverse conditional", obuf, buf, *end);
            goto again;
          } /*if*/
        // 1. this is a NOP instruction
        // 2. there are more instructions after this OR there are no references here
        // then
        // delete instruction, fix goto labels
        if
          (
                b[0] == 0
            &&
                b[1] == 0
            &&
                b[2] == 0
            &&
                b[3] == 0
            &&
                b[4] == 0
            &&
                b[5] == 0
            &&
                b[6] == 0
            &&
                b[7] == 0 /* it's a NOP */
            &&
                (
                    b + 8 != *end /* more instructions after this */
                ||
                    !isreferenced(buf, *end, curline) /* no references here */
                )
          )
          {
            deleteinstruction(obuf, buf, end, b);
            dumpcode("vm_optimize: remove nop", obuf, buf, *end);
            goto again;
          } /*if*/
        // if
        // 1. the prev instruction is an UNCONDITIONAL jump/goto
        // 2. there are no references to the statement
        // then
        // delete instruction, fix goto labels
        if
          (
                b > buf
            &&
                (b[-8] >> 4) <= 3
            &&
                (b[-7] & 0x70) == 0
            &&
                (b[-7] & 15) != 0 /* previous was unconditional transfer */
            &&
                !isreferenced(buf, *end, curline) /* no references here */
           /* fixme: should also remove in the case where jump was to this instruction */
          )
          {
          /* remove dead code */
            deleteinstruction(obuf, buf, end, b);
            dumpcode("vm_optimize: remove dead code", obuf, buf, *end);
            goto again;
          } /*if*/
        // if
        // 1. this instruction sets subtitle/angle/audio
        // 2. the next instruction sets subtitle/angle/audio
        // 3. they both set them the same way (i.e. immediate/indirect)
        // 4. there are no references to the second instruction
        // then
        // combine
        if
          (
                b + 8 != *end
            &&
                (b[0] & 0xEF) == 0x41 /* SetSTN */
            &&
                b[1] == 0 // step 1
            &&
                b[0] == b[8]
            &&
                b[1] == b[9] // step 2 & 3
            &&
                !isreferenced(buf, *end, curline + 1)
          )
          {
            if (b[8 + 3])
                b[3] = b[8 + 3];
            if (b[8 + 4])
                b[4] = b[8 + 4];
            if (b[8 + 5])
                b[5] = b[8 + 5];
            deleteinstruction(obuf, buf, end, b + 8);
            dumpcode("vm_optimize: merge setting of subtitle/angle/audio", obuf, buf, *end);
            goto again;
          } /*if*/
        // if
        // 1. this instruction sets the button directly
        // 2. the next instruction is a link command (not NOP, not PGCN)
        // 3. there are no references to the second instruction
        // then
        // combine
        if
          (
                b + 8 != *end
            &&
                b[0] == 0x56
            &&
                b[1] == 0x00
            &&
                b[8] == 0x20
            &&
                (
                    (b[8 + 1] & 0xf) == 5
                ||
                    (b[8 + 1] & 0xf) == 6
                ||
                    (b[8 + 1] & 0xf) == 7
                ||
                        (b[8 + 1] & 0xf) == 1
                    &&
                        (b[8 + 7] & 0x1f) != 0
                )
            &&
                !isreferenced(buf, *end, curline + 1)
          )
          {
            if (b[8 + 6] == 0)
                b[8 + 6] = b[4];
            deleteinstruction(obuf, buf, end, b);
            dumpcode("vm_optimize: merge set button and link", obuf, buf, *end);
            goto again;
          } /*if*/
        // if
        // 1. this instruction sets a GPRM/SPRM register
        // 2. the next instruction is a link command (not NOP)
        // 3. there are no references to the second instruction
        // then
        // combine
        if
          (
                b + 8 != *end
            &&
                ((b[0] & 0xE0) == 0x40 || (b[0] & 0xE0) == 0x60)
            &&
                (b[1] & 0x7f) == 0x00
            &&
                b[8] == 0x20
            &&
                (
                    (b[8 + 1] & 0x7f) == 4
                ||
                    (b[8 + 1] & 0x7f) == 5
                ||
                    (b[8 + 1] & 0x7f) == 6
                ||
                    (b[8 + 1] & 0x7f) == 7
                ||
                        (b[8 + 1] & 0x7f) == 1
                    &&
                        (b[8 + 7] & 0x1f) != 0
                )
            &&
                !isreferenced(buf, *end, curline + 1)
          )
          {
            b[1] = b[8 + 1];
            b[6] = b[8 + 6];
            b[7] = b[8 + 7];
            deleteinstruction(obuf, buf, end, b + 8);
            dumpcode("vm_optimize: merge set register and link", obuf, buf, *end);
            goto again;
          } /*if*/
      } /*for*/
  } /*vm_optimize*/

unsigned char *vm_compile
  (
    const unsigned char *obuf, /* start of buffer for computing instruction numbers for branches */
    unsigned char *buf, /* where to insert new compiled code */
    const struct workset *ws,
    const struct pgcgroup *curgroup,
    const struct pgc *curpgc,
    const struct vm_statement *cs,
    vtypes ismenu
  )
  /* compiles the parse tree cs into actual VM instructions with optimization,
    and fixes up all the gotos. */
  {
    unsigned char *end;
    int i, j;
    numlabels = 0;
    numgotos = 0;
    end = compilecs(obuf, buf, ws, curgroup, curpgc, cs, ismenu);
    if (!end) /* error */
        return end;
    // fix goto references
    for (i = 0; i < numgotos; i++)
      {
        for (j = 0; j < numlabels; j++)
            if (!strcasecmp(gotos[i].lname,labels[j].lname))
                break;
        if (j == numlabels)
          {
            fprintf(stderr, "ERR:  Cannot find label %s\n", gotos[i].lname);
            return 0;
          } /*if*/
        gotos[i].code[7] = (labels[j].code - obuf) / 8 + 1;
      } /*for*/
    dumpcode("vm_compile: before vm_optimize", obuf, buf, end);
    vm_optimize(obuf, buf, &end);
    dumpcode("vm_compile: after vm_optimize", obuf, buf, end);
    return end;
  } /*vm_compile*/

void dvdvmerror(const char *s)
  /* reports a parse error. */
  {
    extern char *dvdvmtext;
    fprintf(stderr, "ERR:  Parse error '%s' on token '%s'\n", s, dvdvmtext);
    exit(1);
  } /*dvdvmerror*/

struct vm_statement *vm_parse(const char *b)
  /* parses a VM source string and returns the constructed parse tree. */
  {
    if (b)
      {
        const char * const cmd = strdup(b);
        dvdvm_buffer_state buf = dvdvm_scan_string(cmd);
        dvd_vm_parsed_cmd = 0;
        if (dvdvmparse())
          {
            fprintf(stderr, "ERR:  Parser failed on code '%s'.\n", b);
            exit(1);
          } /*if*/
        if (!dvd_vm_parsed_cmd)
          {
            fprintf(stderr, "ERR:  Nothing parsed from '%s'\n", b);
            exit(1);
          } /*if*/
        dvdvm_delete_buffer(buf);
        free((void *)cmd);
        return dvd_vm_parsed_cmd;
      }
    else
      {
        // pieces of code in dvdauthor rely on a non-null vm_statement
        // meaning something significant.  so if we parse an empty string,
        // we should return SOMETHING not null.
        // also, since the xml parser strips whitespace, we treat a
        // NULL string as an empty string
        struct vm_statement * const v = statement_new();
        v->op = VM_NOP;
        return v;
      } /*if*/
  } /*vm_parse*/
