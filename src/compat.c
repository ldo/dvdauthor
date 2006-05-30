#include "config.h"

#include "compat.h"

static const char RCSID[]="$Id: //depot/dvdauthor/src/compat.c#2 $";

#ifndef HAVE_STRSEP
/* Match STRING against the filename pattern PATTERN, returning zero if
   it matches, nonzero if not.  */
char *strsep(char **stringp, const char *delim)
{
    char *res;
    
    if(!stringp || !*stringp || !**stringp)
        return (char*)0;
    
    res = *stringp;
    while(**stringp && !strchr(delim,**stringp))
        (*stringp)++;
    
    if(**stringp) {
        **stringp = '\0';
        (*stringp)++;
    }
    
    return res;
}
#endif  /* HAVE_STRSEP */
