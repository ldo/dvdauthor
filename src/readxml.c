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

#include <libxml/xmlreader.h>

#include "readxml.h"

#ifdef HAVE_LANGINFO_CODESET
#include <langinfo.h>
#include <locale.h>
#endif

static const char RCSID[]="$Id: //depot/dvdauthor/src/readxml.c#17 $";

int parser_err=0, parser_acceptbody=0;
char *parser_body=0;

int readxml(const char *xmlfile,struct elemdesc *elems,struct elemattr *attrs)
{
    int curstate=0,statehistory[10];
    xmlTextReaderPtr f;

    if( xmlfile[0]=='&' && isdigit(xmlfile[1]) )
        f=xmlReaderForFd(atoi(xmlfile+1),xmlfile,NULL,0);
    else
        f=xmlNewTextReaderFilename(xmlfile);
    if(!f) {
        fprintf(stderr,"ERR:  Unable to open XML file %s\n",xmlfile);
        return 1;
    }

    while(1) {
        int r=xmlTextReaderRead(f);
        int i;

        if( !r ) {
            fprintf(stderr,"ERR:  Read premature EOF\n");
            return 1;
        }
        if( r!=1 ) {
            fprintf(stderr,"ERR:  Error in parsing XML\n");
            return 1;
        }
        switch(xmlTextReaderNodeType(f)) {
        case XML_READER_TYPE_SIGNIFICANT_WHITESPACE:
        case XML_READER_TYPE_WHITESPACE:
        case XML_READER_TYPE_COMMENT:
            break;

        case XML_READER_TYPE_ELEMENT:
            assert(!parser_body);
            for( i=0; elems[i].elemname; i++ )
                if( curstate==elems[i].parentstate &&
                    !strcmp(xmlTextReaderName(f),elems[i].elemname) ) {
                    // reading the attributes causes these values to change
                    // so if you want to use them later, save them now
                    int empty=xmlTextReaderIsEmptyElement(f),
                        depth=xmlTextReaderDepth(f);
                    if( elems[i].start ) {
                        elems[i].start();
                        if( parser_err )
                            return 1;
                    }
                    while(xmlTextReaderMoveToNextAttribute(f)) {
                        char *nm=xmlTextReaderName(f),*v=xmlTextReaderValue(f);
                        int j;

                        for( j=0; attrs[j].elem; j++ )
                            if( !strcmp(attrs[j].elem,elems[i].elemname) &&
                                !strcmp(attrs[j].attr,nm )) {
                                attrs[j].f(v);
                                if( parser_err )
                                    return 1;
                                break;
                            }
                        if( !attrs[j].elem ) {
                            fprintf(stderr,"ERR:  Cannot match attribute '%s' in tag '%s'.  Valid attributes are:\n",nm,elems[i].elemname);
                            for( j=0; attrs[j].elem; j++ )
                                if( !strcmp(attrs[j].elem,elems[i].elemname) )
                                    fprintf(stderr,"ERR:      %s\n",attrs[j].attr);
                            return 1;
                        }
                    }
                    if( empty ) {
                        if( elems[i].end ) {
                            elems[i].end();
                            if( parser_err )
                                return 1;
                        }
                    } else {
                        statehistory[depth]=i;
                        curstate=elems[i].newstate;
                    }
                    break;
                }
            if( !elems[i].elemname ) {
                fprintf(stderr,"ERR:  Cannot match start tag '%s'.  Valid tags are:\n",xmlTextReaderName(f));
                for( i=0; elems[i].elemname; i++ )
                    if( curstate==elems[i].parentstate )
                        fprintf(stderr,"ERR:      %s\n",elems[i].elemname);
                return 1;
            }
            break;

        case XML_READER_TYPE_END_ELEMENT:
            i=statehistory[xmlTextReaderDepth(f)];
            if( elems[i].end ) {
                elems[i].end();
                if( parser_err )
                    return 1;
            }
            curstate=elems[i].parentstate;
            parser_body=0;
            parser_acceptbody=0;
            if( !curstate )
                goto done_parsing;
            break;

        case XML_READER_TYPE_TEXT: {
            char *v=xmlTextReaderValue(f);

            if( !parser_body ) {
                // stupid buggy libxml2 2.5.4 that ships with RedHat 9.0!
                // we must manually check if this is just whitespace
                for( i=0; v[i]; i++ )
                    if( v[i]!='\r' &&
                        v[i]!='\n' &&
                        v[i]!=' '  &&
                        v[i]!='\t' )
                        goto has_nonws_body;
                break;
            }
        has_nonws_body:
            if( !parser_acceptbody ) {
                fprintf(stderr,"ERR:  text not allowed here\n");
                return 1;
            }

            if( !parser_body )
                parser_body=strdup(v);
            else {
                parser_body=realloc(parser_body,strlen(parser_body)+strlen(v)+1);
                strcat(parser_body,v);
            }
            break;
        }

        default:
            fprintf(stderr,"ERR:  Unknown XML node type %d\n",xmlTextReaderNodeType(f));
            return 1;
        }
    }
 done_parsing:

    return 0;
}

int xml_ison(const char *s)
{
    if( !strcmp(s,"1") || !strcasecmp(s,"on") || !strcasecmp(s,"yes") )
        return 1;
    if( !strcmp(s,"0") || !strcasecmp(s,"off") || !strcasecmp(s,"no") )
        return 0;
    return -1;
}

#if defined(HAVE_ICONV) && defined(HAVE_LANGINFO_CODESET)

static iconv_t get_conv()
{
    static iconv_t ic=(iconv_t)-1;

    if( ic==((iconv_t)-1) ) {
        char *enc;

        errno=0;
        enc=setlocale(LC_ALL,"");
        if( enc ) {
            fprintf(stderr,"INFO: Locale=%s\n",enc);
            if( !setlocale(LC_NUMERIC,"C") ) {
                fprintf(stderr,"ERR:  Cannot set numeric locale to C!\n");
                exit(1);
            }
        } else
            fprintf(stderr,"WARN: Error reading locale (%m), assuming C\n");
        enc=nl_langinfo(CODESET);
        fprintf(stderr,"INFO: Converting filenames to %s\n",enc);
        ic=iconv_open(enc,"UTF-8");
        if( ic==((iconv_t)-1) ) {
            fprintf(stderr,"ERR:  Cannot generate charset conversion from UTF8 to %s\n",enc);
            exit(1);
        }
    }
    return ic;
}

char *utf8tolocal(const char *in)
{
    iconv_t c=get_conv();
    size_t inlen=strlen(in);
    size_t outlen=inlen*5;
    char *r=malloc(outlen+1);
    char *out=r;
    size_t v;

    v=iconv(c,ICONV_CAST &in,&inlen,&out,&outlen);
    if(v==-1) {
        fprintf(stderr,"ERR:  Cannot convert UTF8 string '%s': %s\n",in,strerror(errno));
        exit(1);
    }
    *out=0;

    r=realloc(r,strlen(r)+1); // reduce memory requirements
    
    return r;
}
#else
char *utf8tolocal(const char *in)
{
    return strdup(in);
}
#endif
