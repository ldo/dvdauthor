/*
    Common XML parsing routines
*/

typedef void (*parserfunc)(void);
typedef void (*attrfunc)(const char *);

struct elemdesc { /* defines a valid XML tag */
    const char *elemname; /* element name */
    int parentstate; /* state in which element is expected (initial state is 0) */
    int newstate;
      /* state to push on state stack on entering element.
        Note state stack can only go 10 deep */
    parserfunc start; /* optional action to invoke on entering element */
    parserfunc end; /* optional action to invoke on leaving element */
};

struct elemattr { /* defines a valid attribute for an XML tag */
    const char *elem; /* element name */
    const char *attr; /* attribute name */
    attrfunc f; /* action to invoke with attribute value */
};

int readxml
  (
    const char *xmlfile, /* filename to read */
    const struct elemdesc *elems, /* array terminated by entry with null elemname field */
    const struct elemattr *attrs /* array terminated by entry with null elem field */
  );
  /* opens and reads an XML file according to the given element and attribute definitions. */

bool xml_ison(const char * v, const char * attrname);
  /* interprets v as a value indicating yes/no/on/off, returning true for yes/on or false for no/off. */

extern bool parser_err; /* can be set true by a callback action to abort the parse */
extern bool parser_acceptbody;
  /* whether tag is allowed to be nonempty. Initially false; can be
    set true by a tag-start action */
extern char *parser_body;
  /* tag content, if any, saved here. Note I don't handle sub-tags mixed with content. */
