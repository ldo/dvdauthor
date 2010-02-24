/*
    Decompile VM instructions to human-readable form
*/

#ifndef DVDUNCOMPILE_H_INCLUDED
#define DVDUNCOMPILE_H_INCLUDED

/* Ogle - A video player
 * Copyright (C) 2000, 2001 Martin Norbäck, Håkan Hjort
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, 
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <libxml/tree.h>
#include <dvdread/ifo_types.h> // Only for vm_cmd_t 

void vm_add_mnemonics
  (
    xmlNodePtr node, /* the node to append the disassembly to */
    const char *base, /* prepended to every output line for indentation purposes */
    int ncmd, /* nr of commands */
    const vm_cmd_t *commands /* array */
  );
  /* disassembles the specified command sequence as content for the specified XML tag. */

#endif /* DVDUNCOMPILE_H_INCLUDED */
