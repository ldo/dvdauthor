/*
    Common XML parsing routines
*/

typedef void (*parserfunc)(void);
typedef void (*attrfunc)(char *);

struct elemdesc { /* defines a valid XML tag */
    char *elemname; /* element name */
    int parentstate; /* state in which element is expected (initial state is 0) */
    int newstate; /* state to push on state stack on entering element */
    parserfunc start; /* action to invoke on entering element */
    parserfunc end; /* action to invoke on leaving element */
};

struct elemattr { /* defines a valid attribute for an XML tag */
    char *elem; /* element name */
    char *attr; /* attribute name */
    attrfunc f; /* action to invoke with attribute value */
};

int readxml(const char *xmlfile,struct elemdesc *elems,struct elemattr *attrs);
  /* opens and reads an XML file according to the given element and attribute definitions. */
int xml_ison(const char *v);
  /* interprets v as a value indicating yes/no/on/off, returning 1 for yes/on or 0 for no/off. */
char *utf8tolocal(const char *in);

extern int parser_err;
extern int parser_acceptbody; /* whether tag is allowed to be nonempty (initially false) */
extern char *parser_body;

