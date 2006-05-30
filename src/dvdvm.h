#include <stdlib.h>
#include <string.h>

#ifdef YY_END_OF_BUFFER_CHAR
#define dvdvm_buffer_state YY_BUFFER_STATE
#else
typedef void *dvdvm_buffer_state;
#endif

extern void dvdvmerror(char *s);
extern int dvdvmlex(void);
extern dvdvm_buffer_state dvdvm_scan_string(const char *s);
extern int dvdvmparse(void);

struct vm_statement {
    int op;
    int i1,i2,i3,i4;
    char *s1,*s2,*s3,*s4;
    struct vm_statement *param;
    struct vm_statement *next;
};

extern struct vm_statement *dvd_vm_parsed_cmd;

enum { VM_NOP=0,
       VM_JUMP,
       VM_CALL,
       VM_EXIT,
       VM_RESUME,

       VM_SET,
       VM_IF,
       VM_ADD,
       VM_SUB,
       VM_MUL,

       VM_DIV,
       VM_MOD,
       VM_AND,
       VM_OR,
       VM_XOR,

       VM_VAL,
       VM_EQ, // EQ .. LT are all in a specific order
       VM_NE,
       VM_GTE,
       VM_GT,

       VM_LTE,
       VM_LT,
       VM_LAND,
       VM_LOR,
       VM_NOT,
       
       VM_MAX_OPCODE
};

static inline struct vm_statement *statement_new()
{
    struct vm_statement *s=malloc(sizeof(struct vm_statement));
    memset(s,0,sizeof(struct vm_statement));
    return s;
}

static inline struct vm_statement *statement_expression(struct vm_statement *v1,int op,struct vm_statement *v2)
{
    struct vm_statement *v;

    if( v1->op==op ) {
        v=v1->param;
        while(v->next) v=v->next;
        v->next=v2;
        return v1;
    } else {
        v=statement_new();
        v->op=op;
        v->param=v1;
        v1->next=v2;
        return v;
    }
}

static inline struct vm_statement *statement_setop(int reg,int op,struct vm_statement *vp)
{
    struct vm_statement *v,*v2;

    v=statement_new();
    v->op=VM_SET;
    v->i1=reg;
    v2=statement_new();
    v2->op=VM_VAL;
    v2->i1=reg-256;
    v->param=statement_expression(v2,op,vp);
    return v;
}

static inline void statement_free(struct vm_statement *s)
{
    if( s->s1 ) free(s->s1);
    if( s->s2 ) free(s->s2);
    if( s->s3 ) free(s->s3);
    if( s->s4 ) free(s->s4);
    if( s->param ) statement_free(s->param);
    if( s->next ) statement_free(s->next);
    free(s);
}
