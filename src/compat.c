#include "config.h"

#include "compat.h"

#include <ctype.h>
#include <fcntl.h>

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

/* Mode is either O_RDONLY or O_WRONLY, nothing more */
struct vfile varied_open(const char *fname,int mode)
{
    struct vfile vf;

    if( !strcmp(fname,"-") ) {
        vf.ftype=2;
        if( mode==O_RDONLY )
            vf.h=stdin;
        else
            vf.h=stdout;
        return vf;
    } else if( fname[0]=='&' && isdigit(fname[1])) {
        vf.ftype=0;
        vf.h=fdopen(atoi(fname+1),(mode==O_RDONLY)?"rb":"wb");
        return vf;
    } else if( mode==O_RDONLY ) {
        int l=strlen(fname);
        if( l>0 && fname[l-1]=='|' ) {
            char *fcopy=strdup(fname);
            fcopy[l-1]=0;
            vf.ftype=1;
            vf.h=popen(fcopy,"r");
            free(fcopy);
            return vf;
        }
    } else if( fname[0]=='|' ) {
        vf.ftype=1;
        vf.h=popen(fname+1,"w");
        return vf;
    }
    vf.ftype=0;
    vf.h=fopen(fname,(mode==O_RDONLY)?"rb":"wb");
    return vf;
}

void varied_close(struct vfile vf)
{
    switch( vf.ftype ) {
    case 0: fclose(vf.h); break;
    case 1: pclose(vf.h); break;
    case 2: break;
    }
}
