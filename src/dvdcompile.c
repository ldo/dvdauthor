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

#include "config.h"

#include "compat.h"
#include <assert.h>

#include "dvdauthor.h"
#include "da-internal.h"
#include "dvdvm.h"


struct vm_statement *dvd_vm_parsed_cmd;

#define MAXLABELS 200
#define MAXGOTOS 200

struct dvdlabel {
    char *lname;
    unsigned char *code;
};

static struct dvdlabel labels[MAXLABELS];
static struct dvdlabel gotos[MAXGOTOS];
static int numlabels=0, numgotos=0;

static int negatecompare(int compareop)
{
    return compareop^1^((compareop&4)>>1);
}

static int swapcompare(int compareop)
{
    if( compareop < 4 )
        return compareop;
    else
        return compareop^3;
}

static int compile_usesreg(struct vm_statement *cs,int target)
{
    while(cs) {
        if( cs->op==VM_VAL )
            return cs->i1==target-256;
        if( compile_usesreg(cs->param,target ))
            return 1;
        cs=cs->next;
    }
    return 0;
}

static int nexttarget(int t)
{
    if( !allowallreg ) {
        if( t<13 )
            return 13;
        t++;
        if( t<16 )
            return t;
    }
    fprintf(stderr,"ERR:  Expression is too complicated, ran out of registers\n");
    exit(1);
}

// like nexttarget, but takes a VM_VAL argument
static int nextval(int t)
{
    if( t<-128 )
        return nexttarget(t+256)-256;
    else
        return nexttarget(-1)-256;
}

static unsigned char *compileop(unsigned char *buf,int target,int op,int val)
{
    if( op==VM_VAL && target==val+256 ) return buf;
    write8(buf,val>=0?0x70:0x60,0x00,0x00,target, val>=0?(val>>8):0x00,val,0x00,0x00);
    switch(op) {
    case VM_VAL: buf[0]|=1; break;
    case VM_ADD: buf[0]|=3; break;
    case VM_SUB: buf[0]|=4; break;
    case VM_MUL: buf[0]|=5; break;
    case VM_DIV: buf[0]|=6; break;
    case VM_MOD: buf[0]|=7; break;
    case VM_RND: buf[0]|=8; break;
    case VM_AND: buf[0]|=9; break;
    case VM_OR:  buf[0]|=10; break;
    case VM_XOR: buf[0]|=11; break;
    default: fprintf(stderr,"ERR:  Unknown op in compileop: %d\n",op); exit(1);
    }
    return buf+8;
}

static int issprmval(struct vm_statement *v)
{
    return v->op==VM_VAL && v->i1>=-128 && v->i1<0;
}

static unsigned char *compileexpr(unsigned char *buf,int target,struct vm_statement *cs)
{
    struct vm_statement *v,**vp;
    int isassoc,canusesprm;
    
    if( cs->op==VM_VAL )
        return compileop(buf,target,VM_VAL,cs->i1);

    isassoc=( cs->op==VM_ADD || cs->op==VM_MUL || cs->op==VM_AND || cs->op==VM_OR || cs->op==VM_XOR );
    canusesprm=( cs->op==VM_AND || cs->op==VM_OR || cs->op==VM_XOR );

    // if the target is an operator, move it to the front
    if( isassoc ) {
        for( vp=&cs->param->next; *vp; vp=&(vp[0]->next) )
            if( vp[0]->op==VM_VAL && vp[0]->i1==target-256 ) {
                v=*vp;
                *vp=v->next;
                v->next=cs->param;
                cs->param=v;
                break;
            }
    }

    if( compile_usesreg(cs->param->next,target) ) {
        int t2=nexttarget(target);
        buf=compileexpr(buf,t2,cs);
        write8(buf,0x61,0x00,0x00,target,0x00,t2,0x00,0x00);
        buf+=8;
        if( t2==15 ) {
            write8(buf,0x71,0x00,0x00,0x0f,0x00,0x00,0x00,0x00);
            buf+=8;
        }
        return buf;
    }
        
    if( isassoc && cs->param->op==VM_VAL && cs->param->i1!=target-256 ) {
        // if the first param is a value, then try to move a complex operation farther up or an SPRM access (if SPRM ops are not allowed)
        for( vp=&cs->param->next; *vp; vp=&(vp[0]->next) )
            if( vp[0]->op!=VM_VAL || issprmval(vp[0]) ) {
                v=*vp;
                *vp=v->next;
                v->next=cs->param;
                cs->param=v;
                break;
            }
    }

    // special case -- rnd where the parameter is a reg or value
    if( cs->op == VM_RND && cs->param->op == VM_VAL ) {
        assert(cs->param->next==0);
        return compileop(buf,target,cs->op,cs->param->i1);
    }

    buf=compileexpr(buf,target,cs->param);
    if( cs->op == VM_RND ) {
        assert(cs->param->next==0);
        return compileop(buf,target,cs->op,target-256);
    } else {
        for( v=cs->param->next; v; v=v->next ) {
            if( v->op==VM_VAL && !issprmval(v))
                buf=compileop(buf,target,cs->op,v->i1);
            else {
                int t2=nexttarget(target);
                buf=compileexpr(buf,t2,v);
                buf=compileop(buf,target,cs->op,t2-256);
                if( t2==15 ) {
                    write8(buf,0x71,0x00,0x00,0x0f,0x00,0x00,0x00,0x00);
                    buf+=8;
                }
            }
        }
    }
    return buf;
}

static unsigned char *compilebool(unsigned char *obuf,unsigned char *buf,struct vm_statement *cs,unsigned char *iftrue,unsigned char *iffalse)
{
    switch( cs->op ) {
    case VM_EQ:
    case VM_NE:
    case VM_GTE:
    case VM_GT:
    case VM_LTE:
    case VM_LT:
    {
        int r1,r2,op;

        op=cs->op-VM_EQ+2;
        if( cs->param->op==VM_VAL )
            r1=cs->param->i1;
        else {
            r1=nextval(0);
            buf=compileexpr(buf,r1&15,cs->param);
        }
        if( cs->param->next->op==VM_VAL && (r1<0 || cs->param->next->i1<0) )
            r2=cs->param->next->i1;
        else {
            r2=nextval(r1);
            buf=compileexpr(buf,r2&15,cs->param->next);
        }
        if( r1>=0 ) {
            int t;
            t=r1;
            r1=r2;
            r2=t;
            op=swapcompare(op);
        }
        if( iffalse > iftrue ) {
            unsigned char *t;
            t=iftrue;
            iftrue=iffalse;
            iffalse=t;
            op=negatecompare(op);
        }
        if( r2>=0 )
            write8(buf,0x00,0x81|(op<<4),0x00,r1,r2>>8,r2,0x00,0x00);
        else
            write8(buf,0x00,0x01|(op<<4),0x00,r1,0x00,r2,0x00,0x00);
        buf[7]=(iftrue-obuf)/8+1;
        buf+=8;
        if( iffalse>buf ) {
            write8(buf,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00);
            buf[7]=(iffalse-obuf)/8+1;
            buf+=8;
        }
        break;
    }

    case VM_LOR:
    case VM_LAND:
    {
        int op=cs->op;

        cs=cs->param;
        while(cs->next) {
            unsigned char *n=buf+8;
            while(1) {
                unsigned char *nn=compilebool(obuf,buf,cs,op==VM_LAND?n:iftrue,op==VM_LOR?n:iffalse);
                if( nn==n )
                    break;
                n=nn;
            }
            buf=n;
            cs=cs->next;
        }
        buf=compilebool(obuf,buf,cs,iftrue,iffalse);
        break;
    }

    case VM_NOT:
        return compilebool(obuf,buf,cs->param,iffalse,iftrue);

    default:
        fprintf(stderr,"ERR:  Unknown bool op: %d\n",cs->op);
        exit(1);
    }
    return buf;
}

// NOTE: curgroup is passed separately from curpgc, because in FPC, curpgc==NULL, but curgroup!=NULL
static unsigned char *compilecs(unsigned char *obuf,unsigned char *buf,const struct workset *ws,const struct pgcgroup *curgroup,const struct pgc *curpgc,const struct vm_statement *cs,int ismenu)
{
    int lastif=0;

    while(cs) {
        lastif=0;
        switch(cs->op) {
        case VM_SET:
            switch( cs->i1 ) {
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
                buf=compileexpr(buf,cs->i1,cs->param);
                break;

            case 32+0:
            case 32+1:
            case 32+2:
            case 32+3:
            case 32+4:
            case 32+5:
            case 32+6:
            case 32+7:
            case 32+8:
            case 32+9:
            case 32+10:
            case 32+11:
            case 32+12:
            case 32+13:
            case 32+14:
            case 32+15: // set GPRM
                if( cs->param->op==VM_VAL ) { // we can set counters to system registers
                    int v=cs->param->i1;
                    if( v<0 )
                        write8(buf,0x43,0x00,0x00,v,0x00,0x80|(cs->i1-32),0x00,0x00);
                    else
                        write8(buf,0x53,0x00,v/256,v,0x00,0x80|(cs->i1-32),0x00,0x00);
                    buf+=8;
                } else {
                    int r=nexttarget(0);
                    buf=compileexpr(buf,r,cs->param);
                    write8(buf,0x43,0x00,0x00,r,0x00,0x80|(cs->i1-32),0x00,0x00);
                    buf+=8;
                }
                break;

            case 128+1: // audio
            case 128+2: // subtitle
            case 128+3: // angle
                if( cs->param->op==VM_VAL && !( cs->param->i1>=-128 && cs->param->i1<0 )) {
                    int v=cs->param->i1;
                    write8(buf,v<0?0x41:0x51,0x00,0x00,0x00,0x00,0x00,0x00,0x00);
                    buf[(cs->i1-128)+2]=128|v; // doesn't matter whether this is a register or a value
                    buf+=8;
                } else {
                    int r=nexttarget(0);
                    buf=compileexpr(buf,r,cs->param);
                    write8(buf,0x41,0x00,0x00,0x00,0x00,0x00,0x00,0x00);
                    buf[(cs->i1-128)+2]=128|r;
                    buf+=8;
                }
                break;
                
            case 128+8: // button
                if( cs->param->op==VM_VAL && !( cs->param->i1>=-128 && cs->param->i1<0 )) {
                    int v=cs->param->i1;
                    if( v>0 && (v&1023)!=0 )
                        fprintf(stderr,"WARN: Button value is %d, but it should be a multiple of 1024\nWARN: (button #1=1024, #2=2048, etc)\n",v);
                    if( v<0 )
                        write8(buf,0x46,0x00,0x00,0x00,0x00,v,0x00,0x00);
                    else
                        write8(buf,0x56,0x00,0x00,0x00,v/256,v,0x00,0x00);
                    buf+=8;
                } else {
                    int r=nexttarget(0);
                    buf=compileexpr(buf,r,cs->param);
                    write8(buf,0x46,0x00,0x00,0x00,0x00,r,0x00,0x00);
                    buf+=8;
                }
                break;
                
            default:
                fprintf(stderr,"ERR:  Cannot set SPRM %d\n",cs->i1-128);
                return 0;
            }
            break;

        case VM_IF: {
            unsigned char *iftrue=buf+8,*iffalse=buf+16,*end=buf+16;
            while(1) {
                unsigned char *lp,*ib,*e;

                lp=compilecs(obuf,iftrue,ws,curgroup,curpgc,cs->param->next->param,ismenu);
                if( cs->param->next->next ) {
                    e=compilecs(obuf,lp+8,ws,curgroup,curpgc,cs->param->next->next,ismenu);
                    write8(lp,0x00,0x01,0x00,0x00,0x00,0x00,0x00,(e-obuf)/8+1);
                    lp+=8;
                } else
                    e=lp;
                ib=compilebool(obuf,buf,cs->param,iftrue,iffalse);
                if( !lp ) return 0;
                if( ib==iftrue && lp==iffalse )
                    break;
                iftrue=ib;
                iffalse=lp;
                end=e;
            }
            buf=end;
            lastif=1; // make sure reference statement is generated
            break;
        }

        case VM_LABEL:
        {
            int i;

            for( i=0; i<numlabels; i++ )
                if( !strcasecmp(labels[i].lname,cs->s1) ) {
                    fprintf(stderr,"ERR:  Duplicate label '%s'\n",cs->s1);
                    return 0;
                }
            if( numlabels == MAXLABELS ) {
                fprintf(stderr,"ERR:  Too many labels\n");
                return 0;
            }
            labels[numlabels].lname=cs->s1;
            labels[numlabels].code=buf;
            numlabels++;
            lastif=1; // make sure reference statement is generated
            break;
        }
            
        case VM_GOTO:
            if( numgotos == MAXGOTOS ) {
                fprintf(stderr,"ERR:  Too many gotos\n");
                return 0;
            }
            gotos[numgotos].lname=cs->s1;
            gotos[numgotos].code=buf;
            numgotos++;
            write8(buf,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00);
            buf+=8;
            break;
            
        case VM_BREAK:
            write8(buf,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00);
            buf+=8;
            break;

        case VM_EXIT:
            write8(buf,0x30,0x01,0x00,0x00,0x00,0x00,0x00,0x00);
            buf+=8;
            break;

        case VM_RESUME:
            write8(buf,0x20,0x01,0x00,0x00,0x00,0x00,0x00,0x10);
            buf+=8;
            break;

        case VM_JUMP:
        {
            int i1=cs->i1;
            int i2=cs->i2;

            if( i1==1 && ismenu==2 ) {
                //	VMGM	VMGM	NOPGC	NOCH
                //	VMGM	VMGM	NOPGC	CHXX
                //	VMGM	VMGM	MPGC	NOCH
                //	VMGM	VMGM	MPGC	CHXX
                //	VMGM	VMGM	MEPGC	NOCH
                //	VMGM	VMGM	MEPGC	CHXX
                //	VMGM	VMGM	TPGC	NOCH
                //	VMGM	VMGM	TPGC	CHXX
                i1=0;
            }
            if( ((i2>0 && i2<128) || (i2==0 && i1==1)) && !ismenu ) {
                //	VTS	NONE	MPGC	NOCH
                //	VTS	VMGM	MPGC	NOCH
                //	VTS	TS	MPGC	NOCH
                //	VTS	NONE	MPGC	CHXX
                //	VTS	VMGM	MPGC	CHXX
                //	VTS	TS	MPGC	CHXX
                //	VTS	NONE	MEPGC	NOCH
                //	VTS	VMGM	MEPGC	NOCH
                //	VTS	TS	MEPGC	NOCH
                //	VTS	NONE	MEPGC	CHXX
                //	VTS	VMGM	MEPGC	CHXX
                //	VTS	TS	MEPGC	CHXX
                //	VTS	VMGM	NOPGC	NOCH
                //	VTS	VMGM	NOPGC	CHXX

                fprintf(stderr,"ERR:  Cannot jump to a menu from a title, use 'call' instead\n");
                return 0;
            }
            if( i2>0 && i2<128 && cs->i3 && ismenu ) {
                //	VMGM	NONE	MPGC	CHXX
                //	VMGM	TS	MPGC	CHXX
                //	VMGM	NONE	MEPGC	CHXX
                //	VMGM	TS	MEPGC	CHXX
                //	VTSM	NONE	MPGC	CHXX
                //	VTSM	VMGM	MPGC	CHXX
                //	VTSM	TS	MPGC	CHXX
                //	VTSM	NONE	MEPGC	CHXX
                //	VTSM	VMGM	MEPGC	CHXX
                //	VTSM	TS	MEPGC	CHXX
                fprintf(stderr,"ERR:  Cannot specify chapter when jumping to another menu\n");
                return 0;
            }
            if( i1 && !i2 ) {
                //	VTSM	VMGM	NOPGC	CHXX
                //	VTS	TS	NOPGC	CHXX
                //	VTSM	TS	NOPGC	CHXX
                //	VMGM	TS	NOPGC	CHXX
                //	VTS	TS	NOPGC	NOCH
                //	VTSM	TS	NOPGC	NOCH
                //	VMGM	TS	NOPGC	NOCH
                fprintf(stderr,"ERR:  Cannot omit menu/title if specifying vmgm/titleset\n");
                return 0;
            }
            if( !i1 && !i2 && !cs->i3 ) {
                //	VTS	NONE	NOPGC	NOCH
                //	VTSM	NONE	NOPGC	NOCH
                //	VMGM	NONE	NOPGC	NOCH
                fprintf(stderr,"ERR:  Nop jump statement\n");
                return 0;
            }
            if( i2==121 && (i1>=2 || (i1==0 && ismenu!=2)) ) {
                fprintf(stderr,"ERR:  VMGM must be specified with FPC\n");
                return 0;
            }



            // *** ACTUAL COMPILING
            if( i1>=2 && i2>=120 && i2<128 ) {
                //	VTSM	TS	MEPGC	NOCH
                //	VMGM	TS	MEPGC	NOCH
                if( i2==120 )
                    i2=123;
                write8(buf,0x30,0x06,0x00,0x01,i1-1,0x80+(i2-120),0x00,0x00); buf+=8; // JumpSS VTSM vts 1 menu
            } else if( i1>=2 ||
                       ( i1==1 && i2>=128 ) ||
                       ( ismenu==2 && i2>=128 && cs->i3 )) {
                //	VMGM	TS	TPGC	CHXX
                //	VTSM	TS	MPGC	NOCH
                //	VMGM	TS	MPGC	NOCH
                //	VTS	TS	TPGC	NOCH
                //	VTSM	TS	TPGC	NOCH
                //	VMGM	TS	TPGC	NOCH
                //	VTS	TS	TPGC	CHXX
                //	VTSM	TS	TPGC	CHXX
                //	VTS	VMGM	TPGC	NOCH
                //	VTSM	VMGM	TPGC	NOCH
                //	VTS	VMGM	TPGC	CHXX
                //	VTSM	VMGM	TPGC	CHXX
                //	VMGM	NONE	TPGC	CHXX
                if( jumppad ) {
                    if( !i1 )
                        i1=1;
                    write8(buf,0x71,0x00,0x00,0x0F,i2,i1,0x00,0x00); buf+=8;
                    write8(buf,0x71,0x00,0x00,0x0E,0x00,cs->i3,0x00,0x00); buf+=8;
                    write8(buf,0x30,ismenu?0x06:0x08,0x00,0x00,0x00,0x42,0x00,0x00); buf+=8;
                } else {
                    fprintf(stderr,"ERR:  That form of jumping is not allowed\n");
                    return 0;
                }
            } else if( i1==1 || i2==121 ) {
                //	VTSM	VMGM	NOPGC	NOCH
                //	VTSM	VMGM	MPGC	NOCH
                //	VTSM	VMGM	MEPGC	NOCH
                // cannot error check jumps to the vmgm menu
                if( !i2 || i2==120 )
                    i2=122;
                if( i2<120 )
                    write8(buf,0x30,0x06,0x00,i2,0x00,0xC0,0x00,0x00); // JumpSS VMGM pgcn
                else
                    write8(buf,0x30,0x06,0x00,0x00,0x00,i2==121?0:(0x40+i2-120),0x00,0x00); // JumpSS FP or JumpSS VMGM menu
                buf+=8;
            } else if( !i1 && !i2 && cs->i3 ) {
                int numc;
                char *des;

                //	VTS	NONE	NOPGC	CHXX
                //	VTSM	NONE	NOPGC	CHXX
                //	VMGM	NONE	NOPGC	CHXX
                if( curpgc==0 ) {
                    fprintf(stderr,"ERR:  Cannot jump to a chapter from a FPC\n");
                    return 0;
                }
                if( cs->i3<65536 && ismenu ) {
                    fprintf(stderr,"ERR:  Menus do not have chapters\n");
                    return 0;
                }
                switch(cs->i3>>16) {
                case 0: numc=curpgc->numchapters; des="chapter"; break;
                case 1: numc=curpgc->numprograms; des="program"; break;
                case 2: numc=curpgc->numcells; des="cell"; break;
                default: numc=0; des="<err>"; break;
                }
                if( (cs->i3&65535)>numc ) {
                    fprintf(stderr,"ERR:  Cannot jump to %s %d, only %d exist\n",des,cs->i3&65535,numc);
                    return 0;
                }
                write8(buf,0x20,0x05+(cs->i3>>16),0x00,0x00,0x00,0x00,0x00,cs->i3); // LinkPTTN pttn, LinkPGCN pgn, or LinkCN cn
                buf+=8;
            } else if( i2<128 ) {
                //	VTSM	NONE	MPGC	NOCH
                //	VMGM	NONE	MPGC	NOCH
                //	VTSM	NONE	MEPGC	NOCH
                //	VMGM	NONE	MEPGC	NOCH
                if( !curgroup ) {
                    fprintf(stderr,"ERR:  Cannot jump to menu; none exist\n");
                    return 0;
                } else if( i2>=120 && i2<128 ) {
                    int i;
                    
                    for( i=0; i<curgroup->numpgcs; i++ )
                        if( curgroup->pgcs[i]->entries&(1<<(i2-120)) ) {
                            i2=i+1;
                            break;
                        }
                    if( i2>=120 ) {
                        fprintf(stderr,"ERR:  Cannot find PGC with entry %s\n",entries[i2-120]);
                        return 0;
                    }
                } else {
                    if( i2>curgroup->numpgcs ) {
                        fprintf(stderr,"ERR:  Cannot jump to menu PGC #%d, only %d exist\n",i2,curgroup->numpgcs);
                        return 0;
                    }
                }
                if( ismenu==2 ) {
                    // In case we are jumping from a FP to VMGM, we need to use a JumpSS
                    // instruction
                    write8(buf,0x30,0x06,0x00,i2&127,0x00,0xc0,0x00,0x00); // JumpSS VMGM pgcn
                } else
                    write8(buf,0x20,0x04,0x00,0x00,0x00,0x00,0x00,i2&127); // LinkPGCN pgcn
                buf+=8;
            } else {
                //	VMGM	NONE	TPGC	NOCH
                //	VTS	NONE	TPGC	NOCH
                //	VTSM	NONE	TPGC	NOCH
                //	VTS	NONE	TPGC	CHXX
                //	VTSM	NONE	TPGC	CHXX
                if( ismenu<2 ) {
                    if( i2-128>ws->titles->numpgcs ) {
                        fprintf(stderr,"ERR:  Cannot jump to title #%d, only %d exist\n",i2-128,ws->titles->numpgcs);
                        return 0;
                    }
                    if( cs->i3 && cs->i3>ws->titles->pgcs[i2-128-1]->numchapters ) {
                        fprintf(stderr,"ERR:  Cannot jump to chapter %d of title %d, only %d exist\n",cs->i3,i2-128,ws->titles->pgcs[i2-128-1]->numchapters);
                        return 0;
                    }
                }
                write8(buf,0x30,ismenu==2?0x02:(cs->i3?0x05:0x03),0x00,cs->i3,0x00,i2-128,0x00,0x00);
                buf+=8;
            }
            break;
        }

        case VM_CALL:
        {
            int i2=cs->i2;
            int i4=cs->i4;

            // CALL's from <post> MUST have a resume cell
            if( !i4 )
                i4=1;
            if( ismenu ) {
                //	VTSM	NONE	NOPGC	NOCH
                //	VMGM	NONE	NOPGC	NOCH
                //	VTSM	VMGM	NOPGC	NOCH
                //	VMGM	VMGM	NOPGC	NOCH
                //	VTSM	TS	NOPGC	NOCH
                //	VMGM	TS	NOPGC	NOCH
                //	VTSM	NONE	NOPGC	CHXX
                //	VMGM	NONE	NOPGC	CHXX
                //	VTSM	VMGM	NOPGC	CHXX
                //	VMGM	VMGM	NOPGC	CHXX
                //	VTSM	TS	NOPGC	CHXX
                //	VMGM	TS	NOPGC	CHXX
                //	VTSM	NONE	MPGC	NOCH
                //	VMGM	NONE	MPGC	NOCH
                //	VTSM	VMGM	MPGC	NOCH
                //	VMGM	VMGM	MPGC	NOCH
                //	VTSM	TS	MPGC	NOCH
                //	VMGM	TS	MPGC	NOCH
                //	VTSM	NONE	MPGC	CHXX
                //	VMGM	NONE	MPGC	CHXX
                //	VTSM	VMGM	MPGC	CHXX
                //	VMGM	VMGM	MPGC	CHXX
                //	VTSM	TS	MPGC	CHXX
                //	VMGM	TS	MPGC	CHXX
                //	VTSM	NONE	MEPGC	NOCH
                //	VMGM	NONE	MEPGC	NOCH
                //	VTSM	VMGM	MEPGC	NOCH
                //	VMGM	VMGM	MEPGC	NOCH
                //	VTSM	TS	MEPGC	NOCH
                //	VMGM	TS	MEPGC	NOCH
                //	VTSM	NONE	MEPGC	CHXX
                //	VMGM	NONE	MEPGC	CHXX
                //	VTSM	VMGM	MEPGC	CHXX
                //	VMGM	VMGM	MEPGC	CHXX
                //	VTSM	TS	MEPGC	CHXX
                //	VMGM	TS	MEPGC	CHXX
                //	VTSM	NONE	TPGC	NOCH
                //	VMGM	NONE	TPGC	NOCH
                //	VTSM	VMGM	TPGC	NOCH
                //	VMGM	VMGM	TPGC	NOCH
                //	VTSM	TS	TPGC	NOCH
                //	VMGM	TS	TPGC	NOCH
                //	VTSM	NONE	TPGC	CHXX
                //	VMGM	NONE	TPGC	CHXX
                //	VTSM	VMGM	TPGC	CHXX
                //	VMGM	VMGM	TPGC	CHXX
                //	VTSM	TS	TPGC	CHXX
                //	VMGM	TS	TPGC	CHXX
                fprintf(stderr,"ERR:  Cannot 'call' a menu from another menu, use 'jump' instead\n");
                return 0;
            }
            if( i2==0 || i2>=128 ) {
                //	VTS	NONE	NOPGC	NOCH
                //	VTS	VMGM	NOPGC	NOCH
                //	VTS	TS	NOPGC	NOCH
                //	VTS	NONE	NOPGC	CHXX
                //	VTS	VMGM	NOPGC	CHXX
                //	VTS	TS	NOPGC	CHXX
                //	VTS	NONE	TPGC	NOCH
                //	VTS	VMGM	TPGC	NOCH
                //	VTS	TS	TPGC	NOCH
                //	VTS	NONE	TPGC	CHXX
                //	VTS	VMGM	TPGC	CHXX
                //	VTS	TS	TPGC	CHXX

                fprintf(stderr,"ERR:  Cannot 'call' another title, use 'jump' instead\n");
                return 0;
            }
            if( cs->i3!=0 ) {
                //	VTS	NONE	MPGC	CHXX
                //	VTS	VMGM	MPGC	CHXX
                //	VTS	TS	MPGC	CHXX
                //	VTS	NONE	MEPGC	CHXX
                //	VTS	VMGM	MEPGC	CHXX
                //	VTS	TS	MEPGC	CHXX
                fprintf(stderr,"ERR:  Cannot 'call' a chatper within a menu\n");
                return 0;
            }
            if( i2==121 && (cs->i1>=2 || (cs->i1==0 && ismenu!=2)) ) {
                fprintf(stderr,"ERR:  VMGM must be specified with FPC\n");
                return 0;
            }



            if( cs->i1>=2 ) {
                //	VTS	TS	MPGC	NOCH
                //	VTS	TS	MEPGC	NOCH
                if( jumppad ) {
                    write8(buf,0x71,0x00,0x00,0x0F,i2,cs->i1,0x00,0x00); buf+=8;
                    write8(buf,0x71,0x00,0x00,0x0E,0x00,cs->i3,0x00,0x00); buf+=8;
                    write8(buf,0x30,0x08,0x00,0x00,i4,0x42,0x00,0x00); buf+=8;
                } else {
                    fprintf(stderr,"ERR:  Cannot call to a menu in another titleset\n");
                    return 0;
                }
            } else if( cs->i1==0 && i2<120 ) {
                //	VTS	NONE	MPGC	NOCH
                if( jumppad ) {
                    write8(buf,0x71,0x00,0x00,0x0F,i2,0x00,0x00,0x00); buf+=8;
                    write8(buf,0x30,0x08,0x00,0x00,i4,0x87,0x00,0x00); buf+=8;
                } else {
                    fprintf(stderr,"ERR:  Cannot call to a specific menu PGC, only an entry\n");
                    return 0;
                }
            } else if( cs->i1==1 ) {
                //	VTS	VMGM	MPGC	NOCH
                //	VTS	VMGM	MEPGC	NOCH
                // we cannot provide error checking when jumping to a VMGM
                if( i2==120 )
                    i2=122;
                if( i2<120 )
                    write8(buf,0x30,0x08,0x00,i2,i4,0xC0,0x00,0x00);
                else
                    write8(buf,0x30,0x08,0x00,0x00,i4,i2==121?0:(0x40+i2-120),0x00,0x00);
                buf+=8;
            } else {
                int i,j;

                //	VTS	NONE	MEPGC	NOCH
                if( i2==120 )
                    i2=127;
                    
                for( j=0; j<ws->menus->numgroups; j++ ) {
                    struct pgcgroup *pg=ws->menus->groups[j].pg;
                    for( i=0; i<pg->numpgcs; i++ )
                        if( pg->pgcs[i]->entries&(1<<(i2-120)) )
                            goto foundpgc;
                }
                fprintf(stderr,"ERR:  Cannot find PGC with entry %s\n",entries[i2-120]);
                return 0;
            foundpgc:
                write8(buf,0x30,0x08,0x00,0x00,i4,0x80+i2-120,0x00,0x00);
                buf+=8;
            }
            break;
        }

        case VM_NOP:
            break;

        default:
            fprintf(stderr,"ERR:  Unsupported VM opcode %d\n",cs->op);
            return 0;
        }
        cs=cs->next;
    }
    if( lastif ) {
        write8(buf,0,0,0,0,0,0,0,0);
        buf+=8;
    }
    return buf;
}

static unsigned int extractif(unsigned char *b)
{
    switch(b[0]>>4) {
    case 0:
    case 1:
    case 2:
        return ((b[1]>>4)<<24)|
            (b[3]<<16)|
            (b[4]<<8)|
            b[5];

    default:
        fprintf(stderr,"ERR:  Unhandled extractif scenario (%x), file bug\n",b[0]);
        exit(1);
    }
}

static unsigned int negateif(unsigned int ifs)
{
    return (ifs&0x8ffffff)|(negatecompare((ifs>>24)&7)<<24);
}

static void applyif(unsigned char *b,unsigned int ifs)
{
    switch(b[0]>>4) {
    case 0:
    case 1:
    case 2:
        b[5]=ifs;
        b[4]=ifs>>8;
        b[3]=ifs>>16;
        b[1]|=(ifs>>24)<<4;
        break;

    case 3:
    case 4:
    case 5:
        b[7]=ifs;
        b[6]=ifs>>16;
        b[1]|=(ifs>>24)<<4;
        break;        

    case 6:
    case 7:
        b[7]=ifs;
        b[6]=ifs>>8;
        b[2]=ifs>>16;
        b[1]=(ifs>>24)<<4;
        break;

    default:
        fprintf(stderr,"ERR:  Unhandled applyif scenario (%x), file bug\n",b[0]);
        exit(1);
    }
}

static int ifcombinable(unsigned char b0,unsigned char b1,unsigned char b8)
{
    int iftype=-1;

    switch(b0>>4) {
    case 0:
    case 1:
    case 2:
    case 6:
    case 7:
        iftype=b1>>7; break;
    case 3:
    case 4:
    case 5:
        iftype=0; break;
    default:
        return 0;
    }
    switch(b8>>4) {
    case 0:
    case 1:
    case 2:
    case 6:
    case 7:
        return 1;
    case 3:
    case 4:
    case 5:
        return iftype==0;
    default:
        return 0;
    }
}

static int countreferences(unsigned char *obuf,unsigned char *buf,unsigned char *end,int linenum)
{
    unsigned char *b;
    int numref=0;

    for( b=buf; b<end; b+=8 )
        if( b[0]==0 && (b[1]&15)==1 && b[7]==linenum )
            numref++;
    return numref;
}

static void deleteinstruction(unsigned char *obuf,unsigned char *buf,unsigned char **end,unsigned char *b)
{
    unsigned char *b2;
    int linenum=(b-obuf)/8+1;

    for( b2=buf; b2<*end; b2+=8 )
        if( b2[0]==0 && (b2[1]&15)==1 && b2[7]>linenum )
            b2[7]--;
    memmove(b,b+8,*end-(b+8));
    *end-=8;
    memset(*end,0,8); // clean up tracks (so pgc structure is not polluted)
}

void vm_optimize(unsigned char *obuf,unsigned char *buf,unsigned char **end)
{
    unsigned char *b;

 again:
    for( b=buf; b<*end; b+=8 ) {
        int curline=(b-obuf)/8+1;
        // if
        // 1. this is a jump over one statement
        // 2. we can combine the statement with the if
        // 3. there are no references to the statement
        // then
        // combine statement with if, negate if, and replace statement with nop
        if( b[0]==0 && (b[1]&0x70)!=0 && (b[1]&15)==1 && b[7]==curline+2 && // step 1
            ifcombinable(b[0],b[1],b[8]) && // step 2
            countreferences(obuf,buf,*end,curline+1)==0 ) // step 3
        {
            unsigned int ifs=negateif(extractif(b));
            memcpy(b,b+8,8); // move statement
            memset(b+8,0,8); // replace with nop
            applyif(b,ifs);
            goto again;
        }
        // if
        // 1. this is a NOP instruction
        // 2. there are more instructions after this OR there are no references here
        // then
        // delete instruction, fix goto labels
        if( b[0]==0 && b[1]==0 && b[2]==0 && b[3]==0 && 
            b[4]==0 && b[5]==0 && b[6]==0 && b[7]==0 &&
            (b+8!=*end || countreferences(obuf,buf,*end,curline)==0) ) {
            deleteinstruction(obuf,buf,end,b);
            goto again;
        }
        // if
        // 1. the prev instruction is an UNCONDITIONAL jump/goto
        // 2. there are no references to the statement
        // then
        // delete instruction, fix goto labels
        if( b>buf &&
            (b[-8]>>4)<=3 && (b[-7]&0x70)==0 && (b[-7]&15)!=0 &&
            countreferences(obuf,buf,*end,curline)==0 ) {
            deleteinstruction(obuf,buf,end,b);
            goto again;
        }
        // if
        // 1. this instruction sets subtitle/angle/audio
        // 2. the next instruction sets subtitle/angle/audio
        // 3. they both set them the same way (i.e. immediate/indirect)
        // 4. there are no references to the second instruction
        // then
        // combine
        if( b+8!=*end &&
            (b[0]&0xEF)==0x41 && b[1]==0 && // step 1
            b[0]==b[8] && b[1]==b[9] && // step 2 & 3
            countreferences(obuf,buf,*end,curline+1)==0 ) {
            if( b[8+3] ) b[3]=b[8+3];
            if( b[8+4] ) b[4]=b[8+4];
            if( b[8+5] ) b[5]=b[8+5];
            deleteinstruction(obuf,buf,end,b+8);
            goto again;
        }
        // if
        // 1. this instruction sets the button directly
        // 2. the next instruction is a link command (not NOP, not PGCN)
        // 3. there are no references to the second instruction
        // then
        // combine
        if( b+8!=*end &&
            b[0]==0x56 && b[1]==0x00 &&
            b[8]==0x20 && ((b[8+1]&0xf)==5 || 
                           (b[8+1]&0xf)==6 ||
                           (b[8+1]&0xf)==7 ||
                           ((b[8+1]&0xf)==1 && (b[8+7]&0x1f)!=0)) &&
            countreferences(obuf,buf,*end,curline+1)==0 ) {
            if( b[8+6]==0 )
                b[8+6]=b[4];
            deleteinstruction(obuf,buf,end,b);
            goto again;
        }
        // if
        // 1. this instruction sets a GPRM/SPRM register
        // 2. the next instruction is a link command (not NOP)
        // 3. there are no references to the second instruction
        // then
        // combine
        if( b+8!=*end &&
            ((b[0]&0xE0)==0x40 || (b[0]&0xE0)==0x60) && (b[1]&0x7f)==0x00 &&
            b[8]==0x20 && ((b[8+1]&0x7f)==4 || 
                           (b[8+1]&0x7f)==5 ||
                           (b[8+1]&0x7f)==6 ||
                           (b[8+1]&0x7f)==7 ||
                           ((b[8+1]&0x7f)==1 && (b[8+7]&0x1f)!=0)) &&
            countreferences(obuf,buf,*end,curline+1)==0 ) {
            b[1]=b[8+1];
            b[6]=b[8+6];
            b[7]=b[8+7];
            deleteinstruction(obuf,buf,end,b+8);
            goto again;
        }
    }
}

unsigned char *vm_compile(unsigned char *obuf,unsigned char *buf,const struct workset *ws,const struct pgcgroup *curgroup,const struct pgc *curpgc,const struct vm_statement *cs,int ismenu)
{
    unsigned char *end;
    int i, j;

    numlabels=0;
    numgotos=0;

    end=compilecs(obuf,buf,ws,curgroup,curpgc,cs,ismenu);
    if( !end ) return end;

    // fix goto references
    for( i=0; i<numgotos; i++ ) {
        for( j=0; j<numlabels; j++ )
            if( !strcasecmp(gotos[i].lname,labels[j].lname) )
                break;
        if( j==numlabels ) {
            fprintf(stderr,"ERR:  Cannot find label %s\n",gotos[i].lname);
            return 0;
        }
        gotos[i].code[7]=(labels[j].code-obuf)/8+1;
    }

    vm_optimize(obuf,buf,&end);

    return end;
}

void dvdvmerror(char *s)
{
    extern char *dvdvmtext;
    fprintf(stderr,"ERR:  Parse error '%s' on token '%s'\n",s,dvdvmtext);
}

struct vm_statement *vm_parse(const char *b)
{
    if( b ) {
        char *cmd=strdup(b);
        dvdvm_buffer_state buf=dvdvm_scan_string(cmd);
        dvd_vm_parsed_cmd=0;
        if( dvdvmparse() ) {
            fprintf(stderr,"ERR:  Parser failed on code '%s'.\n",b);
            exit(1);
        }
        if( !dvd_vm_parsed_cmd ) {
            fprintf(stderr,"ERR:  Nothing parsed from '%s'\n",b);
            exit(1);
        }
        dvdvm_delete_buffer(buf);
        free(cmd);
        return dvd_vm_parsed_cmd;
    } else {
        // pieces of code in dvdauthor rely on a non-null vm_statement
        // meaning something significant.  so if we parse an empty string,
        // we should return SOMETHING not null.
        // also, since the xml parser strips whitespace, we treat a
        // NULL string as an empty string
        struct vm_statement *v=statement_new();
        v->op=VM_NOP;
        return v;
    }
}
