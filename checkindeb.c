/*  This file is part of "mirrorer" (TODO: find better title)
 *  Copyright (C) 2003 Bernhard R. Link
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <config.h>

#include <errno.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>
#include <string.h>
#include <malloc.h>
#include <ctype.h>
#include <zlib.h>
#include <db.h>
#include "error.h"
#include "mprintf.h"
#include "md5sum.h"
#include "names.h"
#include "chunks.h"
#include "checkindeb.h"
#include "reference.h"
#include "packages.h"
#include "files.h"
#include "extractcontrol.h"

extern int verbose;

// This file shall include the code to include binaries, i.e.
// create or adopt the chunk of the Packages.gz-file and 
// putting it in the various databases.

/* Add <package> with filename <filekey> and chunk <chunk> (which
 * alreadycontains "Filename:") and characteristica <md5andsize>
 * to the <files> database, add an reference to <referee> in 
 * <references> and overwrite/add it to <pkgs> removing
 * references to oldfilekey that will be fall out of it by this */

retvalue checkindeb_insert(DB *references,const char *referee,
		           DB *pkgs,
		const char *package, const char *chunk,
		const char *filekey, const char *oldfilekey) {

	retvalue result,r;


	/* mark it as needed by this distribution */

	r = references_increment(references,filekey,referee);
	if( RET_WAS_ERROR(r) )
		return r;

	/* Add package to distribution's database */

	// Todo: do this earlier...
	if( oldfilekey != NULL ) {
		result = packages_replace(pkgs,package,chunk);

	} else {
		result = packages_add(pkgs,package,chunk);
	}

	if( RET_WAS_ERROR(result) )
		return result;
		
	/* remove old references to files */
	if( oldfilekey ) {
		r = references_decrement(references,oldfilekey,referee);
		RET_UPDATE(result,r);
	}

	return result;
}

// should superseed the add_package from main.c for inclusion
// of downloaded packages from main.c
//
/*
retvalue add_package(void *data,const char *chunk,const char *package,const char *sourcename,const char *oldfile,const char *basename,const char *filekey,const char *md5andsize,const char *oldchunk) {
	char *newchunk;
	retvalue result,r;
	char *oldfilekey;

	* look for needed files *

	r = files_expect(files,mirrordir,filekey,md5andsize);
	if( ! RET_IS_OK(r) ) {
		printf("Missing file %s\n",filekey);
		return r;
	} 
	
	* calculate the needed and check it in *

	newchunk = chunk_replaceentry(chunk,"Filename",filekey);
	if( !newchunk )
		return RET_ERROR;

	oldfilekey = NULL;
	if( oldchunk ) {
		r = binaries_parse_chunk(oldchunk,NULL,&oldfilekey,NULL,NULL,NULL);
		if( RET_WAS_ERROR(r) ) {
			free(newchunk);
			return r;
		}
	}

	result = insert_package(files,mirrordir,references,referee,pkgs,package,newchunk,filekey,md5andsize,oldfilekey);

	free(newchunk);
	free(oldfilekey);
	return result;
}*/

/* things to do with .deb's checkin by hand: (by comparison with apt-ftparchive)
- extract the control file (that's the hard part -> extractcontrol.c )
- check for Package, Version, Architecture, Maintainer, Description
- apply overwrite if neccesary (section,priority and perhaps maintainer).
- add Size, MD5sum, Filename, Priority, Section
- remove Status (warning if existant?)
- check for Optional-field and reject then..
*/

static inline retvalue getvalue(const char *filename,const char *chunk,const char *field,char **value) {
	retvalue r;

	r = chunk_getvalue(chunk,field,value);
	if( r == RET_NOTHING ) {
		fprintf(stderr,"Cannot find %s-header in control file of %s!\n",field,filename);
		r = RET_ERROR;
	}
	return r;
}

static inline retvalue checkvalue(const char *filename,const char *chunk,const char *field) {
	retvalue r;

	r = chunk_checkfield(chunk,field);
	if( r == RET_NOTHING ) {
		fprintf(stderr,"Cannot find %s-header in control file of %s!\n",field,filename);
		r = RET_ERROR;
	}
	return r;
}

static inline retvalue getvalue_d(const char *defaul,const char *chunk,const char *field,char **value) {
	retvalue r;

	r = chunk_getvalue(chunk,field,value);
	if( r == RET_NOTHING ) {
		*value = strdup(defaul);
		if( *value == NULL )
			r = RET_ERROR_OOM;
	}
	return r;
}

void deb_free(struct debpackage *pkg) {
	if( pkg ) {
		free(pkg->package);free(pkg->version);
		free(pkg->source);free(pkg->arch);
		free(pkg->basename);free(pkg->control);
	}
	free(pkg);
}

retvalue deb_read(struct debpackage **pkg, const char *filename) {
	retvalue r;
	struct debpackage *deb;


	deb = calloc(1,sizeof(struct debpackage));

	r = extractcontrol(&deb->control,filename);
	if( RET_WAS_ERROR(r) ) {
		deb_free(deb);
		return r;
	}

	/* first look for fields that should be there */

	r = getvalue(filename,deb->control,"Package",&deb->package);
	if( RET_WAS_ERROR(r) ) {
		deb_free(deb);
		return r;
	}
	r = checkvalue(filename,deb->control,"Maintainer");
	if( RET_WAS_ERROR(r) ) {
		deb_free(deb);
		return r;
	}
	r = checkvalue(filename,deb->control,"Description");
	if( RET_WAS_ERROR(r) ) {
		deb_free(deb);
		return r;
	}
	r = getvalue(filename,deb->control,"Version",&deb->version);
	if( RET_WAS_ERROR(r) ) {
		deb_free(deb);
		return r;
	}
	r = getvalue(filename,deb->control,"Architecture",&deb->arch);
	if( RET_WAS_ERROR(r) ) {
		deb_free(deb);
		return r;
	}

	/* can be there, otherwise we also know what it is */
	r = getvalue_d(deb->package,deb->control,"Source",&deb->source);
	if( RET_WAS_ERROR(r) ) {
		deb_free(deb);
		return r;
	}

	deb->basename = calc_package_basename(deb->package,deb->version,deb->arch);
	if( deb->basename == NULL ) {
		deb_free(deb);
		return r;
	}

	/* check for priority and section, compare with defaults
	 * and overrides */

	//TODO ... do so ...

	r = checkvalue(filename,deb->control,"Priority");
	if( RET_WAS_ERROR(r) ) {
		deb_free(deb);
		return r;
	}
	r = checkvalue(filename,deb->control,"Section");
	if( RET_WAS_ERROR(r) ) {
		deb_free(deb);
		return r;
	}
	*pkg = deb;

	return RET_OK;
}

retvalue deb_complete(struct debpackage *pkg, const char *filekey, const char *md5andsize) {
	retvalue r;
	const char *size;
	struct fieldtoadd *file;

	size = md5andsize;
	while( !isblank(*size) && *size )
		size++;
	file = addfield_newn("MD5Sum",md5andsize, size-md5andsize,NULL);
	if( !file )
		return RET_ERROR_OOM;
	while( *size && isblank(*size) )
		size++;
	file = addfield_new("Size",size,file);
	if( !file )
		return RET_ERROR_OOM;
	file = addfield_new("Filename",filekey,file);
	if( !file )
		return RET_ERROR_OOM;

	// TODO: add overwriting of other fields here, (before the rest)
	
	r  = chunk_replacefields(&pkg->control,file,"Description");
	addfield_free(file);
	if( RET_WAS_ERROR(r) ) {
		return r;
	}

	return RET_OK;
}
