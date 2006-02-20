#ident "$Id: lookup_hesiod.c,v 1.10 2006/02/20 01:05:32 raven Exp $"
/*
 * lookup_hesiod.c
 *
 * Module for Linux automountd to access automount maps in hesiod filsys
 * entries.
 *
 */

#include <sys/types.h>
#include <ctype.h>
#include <limits.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <arpa/nameser.h>
#include <resolv.h>
#include <hesiod.h>

#define MODULE_LOOKUP
#include "automount.h"
#include "nsswitch.h"

#define MAPFMT_DEFAULT "hesiod"

#define MODPREFIX "lookup(hesiod): "
#define HESIOD_LEN 512

struct lookup_context {
	struct parse_mod *parser;
};

int lookup_version = AUTOFS_LOOKUP_VERSION;	/* Required by protocol */

/* This initializes a context (persistent non-global data) for queries to
   this module. */
int lookup_init(const char *mapfmt, int argc, const char *const *argv, void **context)
{
	struct lookup_context *ctxt = NULL;
	char buf[MAX_ERR_BUF];

	/* If we can't build a context, bail. */
	if ((*context = ctxt = (struct lookup_context *)
	     malloc(sizeof(struct lookup_context))) == NULL) {
		char *estr = strerror_r(errno, buf, MAX_ERR_BUF);
		crit(MODPREFIX "malloc: %s", estr);
		return 1;
	}

	/* Initialize the resolver. */
	res_init();

	/* If a map type isn't explicitly given, parse it as hesiod entries. */
	if (!mapfmt)
		mapfmt = MAPFMT_DEFAULT;

	/* Open the parser, if we can. */
	return !(ctxt->parser = open_parse(mapfmt, MODPREFIX, argc - 1, argv + 1));
}

int lookup_read_map(struct autofs_point *ap, time_t age, void *context)
{
	return NSS_STATUS_UNAVAIL;
}

/* Lookup and act on a filesystem name.  In this case, lookup the "filsys"
   record in hesiod.  If it's an AFS or NFS filesyste, parse it out.  If
   it's an ERR filesystem, it's an error message we should log.  Otherwise,
   assume it's something we know how to deal with already (generic). */
int lookup_mount(struct autofs_point *ap, const char *name, int name_len, void *context)
{
	char **hes_result;
	struct lookup_context *ctxt = (struct lookup_context *) context;
	int rv;
	char **record, *best_record = NULL, *p;
	int priority, lowest_priority = INT_MAX;	

	debug(MODPREFIX "looking up root=\"%s\", name=\"%s\"", ap->path, name);

	chdir("/");		/* If this is not here the filesystem stays
				   busy, for some reason... */

	hes_result = hes_resolve(name, "filsys");

	if (!hes_result || !hes_result[0]) {
		warn(MODPREFIX "entry \"%s\" not found in map\n", name);
		return NSS_STATUS_UNAVAIL;
	}

	/* autofs doesn't support falling back to alternate records, so just
	   find the record with the lowest priority and hope it works.
	   -- Aaron Ucko <amu@alum.mit.edu> 2002-03-11 */
	for (record = hes_result; *record; ++record) {
	    p = strrchr(*record, ' ');
	    if ( p && isdigit(p[1]) ) {
		priority = atoi(p+1);
	    } else {
		priority = INT_MAX - 1;
	    }
	    if (priority < lowest_priority) {
		lowest_priority = priority;
		best_record = *record;
	    }
	}

	debug(MODPREFIX "lookup for \"%s\" gave \"%s\"", name, best_record);

	rv = ctxt->parser->parse_mount(ap, name, name_len, best_record,
				       ctxt->parser->context);
	free(hes_result);
	return rv;
}

/* This destroys a context for queries to this module.  It releases the parser
   structure (unloading the module) and frees the memory used by the context. */
int lookup_done(void *context)
{
	struct lookup_context *ctxt = (struct lookup_context *) context;
	int rv = close_parse(ctxt->parser);
	free(ctxt);
	return rv;
}
