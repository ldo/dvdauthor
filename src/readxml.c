/*
    Common XML parsing routines
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307
 * USA
 */

#include "config.h"

#include "compat.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>

#include <libxml/xmlreader.h>

#include "readxml.h"

bool
    parser_err = false,
    parser_acceptbody = false;
char
    *parser_body = 0;

static int xml_varied_read(void *context, char *buffer, int len)
  {
    return fread(buffer,1,len,((struct vfile *)context)->h);
  } /*xml_varied_read*/

static int xml_varied_close(void *context)
  {
    varied_close(*((struct vfile *)context));
    return 0;
  } /*xml_varied_close*/

int readxml
  (
    const char *xmlfile, /* filename to read */
    const struct elemdesc *elems, /* array terminated by entry with null elemname field */
    const struct elemattr *attrs /* array terminated by entry with null elem field */
  )
  /* opens and reads an XML file according to the given element and attribute definitions. */
  {
    enum
      {
        maxdepth = 10, /* should be enough */
      };
    int curstate = 0, statehistory[maxdepth];
    xmlTextReaderPtr f;
    struct vfile fd;

    fd = varied_open(xmlfile, O_RDONLY, "XML file");
    f = xmlReaderForIO(xml_varied_read, xml_varied_close, &fd, xmlfile, NULL, 0);
    if (!f)
      {
        fprintf(stderr, "ERR:  Unable to open XML file %s\n", xmlfile);
        return 1;
      } /*if*/
    while (true)
      {
        int r = xmlTextReaderRead(f);
        if (!r)
          {
            fprintf(stderr, "ERR:  Read premature EOF\n");
            return 1;
          } /*if*/
        if (r != 1)
          {
            fprintf(stderr, "ERR:  Error in parsing XML\n");
            return 1;
          } /*if*/
        switch (xmlTextReaderNodeType(f))
          {
        case XML_READER_TYPE_SIGNIFICANT_WHITESPACE:
        case XML_READER_TYPE_WHITESPACE:
        case XML_READER_TYPE_COMMENT:
          /* ignore */
        break;
        case XML_READER_TYPE_ELEMENT:
          {
            const char * const elemname = (const char *)xmlTextReaderName(f);
            int tagindex;
            assert(!parser_body);
            for (tagindex = 0; elems[tagindex].elemname; tagindex++)
                if
                  (
                        curstate == elems[tagindex].parentstate
                    &&
                        !strcmp(elemname, elems[tagindex].elemname)
                  )
                  {
                    // reading the attributes causes these values to change
                    // so if you want to use them later, save them now
                    const bool empty = xmlTextReaderIsEmptyElement(f);
                    const int depth = xmlTextReaderDepth(f);
                    if (depth >= maxdepth)
                      {
                        fprintf
                          (
                            stderr,
                            "ERR:  max XML parsing depth of %d exceeded\n",
                            maxdepth - 1
                          );
                        exit(1);
                      } /*if*/
                    if (elems[tagindex].start)
                      {
                        elems[tagindex].start();
                        if (parser_err)
                            return 1;
                      } /*if*/
                    while (xmlTextReaderMoveToNextAttribute(f))
                      {
                        const char * const nm = (const char *)xmlTextReaderName(f);
                        const char * const v = (const char *)xmlTextReaderValue(f);
                        int attrindex;
                        for (attrindex = 0; attrs[attrindex].elem; attrindex++)
                            if
                              (
                                    !strcmp(attrs[attrindex].elem, elems[tagindex].elemname)
                                &&
                                    !strcmp(attrs[attrindex].attr, nm)
                              )
                              {
                                attrs[attrindex].f(v);
                                if (parser_err)
                                    return 1;
                                break;
                              } /*if*/
                        if (!attrs[attrindex].elem)
                          {
                            bool gotattr = false;
                            fprintf
                              (
                                stderr,
                                "ERR:  Cannot match attribute '%s' in tag '%s'."
                                    "  Valid attributes are:\n",
                                nm,
                                elems[tagindex].elemname
                              );
                            for (attrindex = 0; attrs[attrindex].elem; attrindex++)
                                if (!strcmp(attrs[attrindex].elem, elems[tagindex].elemname))
                                  {
                                    fprintf(stderr, "ERR:      %s\n", attrs[attrindex]. attr);
                                    gotattr = true;
                                  } /*if*/
                            if (!gotattr)
                              {
                                fprintf(stderr, "ERR:      (none)\n");
                              } /*if*/
                            return 1;
                          } /*if*/
                        xmlFree((xmlChar *)nm);
                        xmlFree((xmlChar *)v);
                      } /*while*/
                    if (empty)
                      {
                      /* tag ends immediately */
                        if (elems[tagindex].end)
                          {
                            elems[tagindex].end();
                            if (parser_err)
                                return 1;
                          } /*if*/
                      }
                    else
                      {
                        statehistory[depth] = tagindex;
                        curstate = elems[tagindex].newstate;
                      } /*if*/
                    break;
                  } /*if; for*/
            if (!elems[tagindex].elemname)
              {
                fprintf(stderr, "ERR:  Cannot match start tag '%s'.  Valid tags are:\n", elemname);
                for (tagindex = 0; elems[tagindex].elemname; tagindex++)
                    if (curstate == elems[tagindex].parentstate)
                        fprintf(stderr, "ERR:      %s\n", elems[tagindex].elemname);
                return 1;
              } /*if*/
            xmlFree((xmlChar *)elemname);
          }
        break;
        case XML_READER_TYPE_END_ELEMENT:
          {
            const int tagindex = statehistory[xmlTextReaderDepth(f)];
            if (elems[tagindex].end)
              {
                elems[tagindex].end();
                if (parser_err)
                    return 1;
              } /*if*/
            curstate = elems[tagindex].parentstate;
          /* Note I don't handle sub-tags mixed with content! */
            free(parser_body);
            parser_body = 0;
            parser_acceptbody = false;
            if (!curstate)
                goto done_parsing;
          }
        break;
        case XML_READER_TYPE_TEXT:
        case XML_READER_TYPE_CDATA:
          {
            const char * const v = (const char *)xmlTextReaderValue(f);
            if (!parser_body)
              {
                // stupid buggy libxml2 2.5.4 that ships with RedHat 9.0!
                // we must manually check if this is just whitespace
                int i;
                for (i = 0; v[i]; i++)
                    if
                      (
                            v[i] != '\r'
                        &&
                            v[i] != '\n'
                        &&
                            v[i] != ' '
                        &&
                            v[i] != '\t'
                      )
                        goto has_nonws_body;
                xmlFree((xmlChar *)v);
                break;
              } /*if*/
has_nonws_body:
            if (!parser_acceptbody)
              {
                fprintf(stderr, "ERR:  text not allowed here\n");
                return 1;
              } /*if*/
            if (!parser_body)
                parser_body = strdup(v); /* first lot of tag content */
            else
              {
              /* append to previous tag content */
                parser_body = realloc(parser_body, strlen(parser_body) + strlen(v) + 1);
                strcat(parser_body, v);
              } /*if*/
            xmlFree((xmlChar *)v);
          }
        break;
      /* give better messages for some other node types: */
        case XML_READER_TYPE_ENTITY_REFERENCE:
            fprintf(stderr, "ERR:  Invalid XML entity reference\n");
            exit(1);
        break;
        case XML_READER_TYPE_PROCESSING_INSTRUCTION:
            fprintf(stderr, "ERR:  Invalid XML processing instruction\n");
            exit(1);
        break;
        case XML_READER_TYPE_DOCUMENT_TYPE:
            fprintf(stderr, "ERR:  Invalid XML document type\n");
            exit(1);
        break;
        default:
            fprintf(stderr, "ERR:  Unknown XML node type %d\n", xmlTextReaderNodeType(f));
            exit(1);
          } /*switch*/
      } /*while*/
 done_parsing:
    xmlFreeTextReader(f);
    return 0;
  } /*readxml*/

bool xml_ison(const char * s, const char * attrname)
  /* interprets v as a value indicating yes/no/on/off, returning true for yes/on or false for no/off. */
  {
    if (!strcmp(s, "1") || !strcasecmp(s, "on") || !strcasecmp(s, "yes"))
        return 1;
    if (!strcmp(s, "0") || !strcasecmp(s, "off") || !strcasecmp(s, "no"))
        return 0;
   fprintf(stderr, "ERR:  Cannot parse boolean value \"%s\" for attribute \"%s\"\n", s, attrname);
 /* parser_err = true; */
    exit(1);
  } /*xml_ison*/
