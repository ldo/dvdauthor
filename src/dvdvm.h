#include <stdlib.h>
#include <string.h>

#ifdef YY_END_OF_BUFFER_CHAR
#define dvdvm_buffer_state YY_BUFFER_STATE
#else
typedef void *dvdvm_buffer_state;
#endif

extern bool allowallreg;

extern void dvdvmerror(const char *s);
extern int dvdvmlex(void);
extern dvdvm_buffer_state dvdvm_scan_string(const char *s);
extern void dvdvm_delete_buffer(dvdvm_buffer_state b);
extern int dvdvmparse(void);

struct vm_statement /* for building parse tree */
  {
    int op;
  /* meanings of following fields depend on op */
    int i1, i2, i3, i4;
    char *s1, *s2, *s3, *s4; /* s1 is label for gotos and label defs; s2, s3, s4 not used */
    struct vm_statement *param;
    struct vm_statement *next; /* sequence of operations */
  };

extern struct vm_statement *dvd_vm_parsed_cmd;

enum /* values for vm_statement.op */
  {
    VM_NOP=0,
    VM_JUMP,
    VM_CALL,
    VM_EXIT,
    VM_LINK,

    VM_SET,
    VM_IF,
      /* two of these are used to represent an if-statement; the outer one puts the
         condition in the vm_statement.param field, while its vm_statement.next
         points to the second one, where vm_statement.param holds the true branch.
         If there is an else-part, this is then attached as the vm_statement.next
         of that. */
    VM_ADD,
    VM_SUB,
    VM_MUL,

    VM_DIV,
    VM_MOD,
    VM_AND,
    VM_OR,
    VM_XOR,

    VM_VAL, /* i1 is either register number - 256 or literal value */
    VM_EQ, // EQ .. LT are all in a specific order
    VM_NE,
    VM_GTE,
    VM_GT,

    VM_LTE,
    VM_LT,
    VM_LAND,
    VM_LOR,
    VM_NOT,

    VM_RND,
    VM_GOTO,
    VM_LABEL,
    VM_BREAK,

    VM_MAX_OPCODE
};

/* Utility routines used during parse */

static inline struct vm_statement *statement_new()
  /* allocates and initializes a new vm_statement structure. */
  {
    struct vm_statement * const s = malloc(sizeof(struct vm_statement));
    memset(s, 0, sizeof(struct vm_statement));
    return s;
  } /*statement_new*/

static inline struct vm_statement *statement_expression
  (
    struct vm_statement *v1,
    int op,
    struct vm_statement *v2
  )
  /* returns a statement that combines v1 and v2 according to op. */
  {
    struct vm_statement *v;
    if (v1->op == op)
      {
      /* same op => append v2 onto end of v1 and return that */
        v = v1->param;
        while (v->next)
            v = v->next;
        v->next = v2;
        return v1;
      }
    else
      {
      /* construct a new statement */
        v = statement_new();
        v->op = op;
        v->param = v1;
        v1->next = v2;
        return v;
      } /*if*/
  } /*statement_expression*/

static inline struct vm_statement *statement_setop
  (
    int reg,
    int op,
    struct vm_statement *vp
  )
  /* returns a statement that sets reg to the combination of its value and that of
    vp according to op. */
  {
  /* Generate a naive "reg = reg op vp" representation, leave it to the peephole
    optimizer to get it down to "reg op= vp" form later */
    struct vm_statement *v, *v2;
    v = statement_new();
    v->op = VM_SET;
    v->i1 = reg;
    v2 = statement_new();
    v2->op = VM_VAL;
    v2->i1 = reg - 256;
    v->param = statement_expression(v2, op, vp);
    return v;
  } /*statement_setop*/
