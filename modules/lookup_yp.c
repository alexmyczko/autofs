#ident "$Id: lookup_yp.c,v 1.6 2004/11/21 08:38:00 raven Exp $"
/* ----------------------------------------------------------------------- *
 *   
 *  lookup_yp.c - module for Linux automountd to access a YP (NIS)
 *                automount map
 *
 *   Copyright 1997 Transmeta Corporation - All Rights Reserved
 *   Copyright 2001-2003 Ian Kent <raven@themaw.net>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, Inc., 675 Mass Ave, Cambridge MA 02139,
 *   USA; either version 2 of the License, or (at your option) any later
 *   version; incorporated herein by reference.
 *
 * ----------------------------------------------------------------------- */

#include <stdio.h>
#include <malloc.h>
#include <errno.h>
#include <unistd.h>
#include <syslog.h>
#include <time.h>
#include <signal.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <rpc/rpc.h>
#include <rpc/xdr.h>
#include <rpcsvc/yp_prot.h>
#include <rpcsvc/ypclnt.h>

#define MODULE_LOOKUP
#include "automount.h"

#define MAPFMT_DEFAULT "sun"

#define MODPREFIX "lookup(yp): "

struct lookup_context {
	const char *domainname;
	const char *mapname;
	struct parse_mod *parse;
};

struct callback_data {
	int status;
	int map;
	const char *root;
	char direct_base[KEY_MAX_LEN + 1];
	const char *name;
	int name_len;
	unsigned long type;
	const char *mapname;
	time_t age;
	struct lookup_context *context;
};

int lookup_version = AUTOFS_LOOKUP_VERSION;	/* Required by protocol */

int lookup_init(const char *mapfmt, int argc, const char *const *argv, void **context)
{
	struct lookup_context *ctxt;
	int err;

	if (!(*context = ctxt = malloc(sizeof(struct lookup_context)))) {
		crit(MODPREFIX "%m");
		return 1;
	}

	if (argc < 1) {
		crit(MODPREFIX "No map name");
		return 1;
	}
	ctxt->mapname = argv[0];

	/* This should, but doesn't, take a const char ** */
	err = yp_get_default_domain((char **) &ctxt->domainname);
	if (err) {
		crit(MODPREFIX "map %s: %s\n", ctxt->mapname,
		       yperr_string(err));
		return 1;
	}

	if (!mapfmt)
		mapfmt = MAPFMT_DEFAULT;

	cache_init();

	return !(ctxt->parse = open_parse(mapfmt, MODPREFIX, argc - 1, argv + 1));
}

int yp_all_callback(int status, char *ypkey, int ypkeylen,
		    char *val, int vallen, char *ypcb_data)
{
	time_t age = time(NULL);
	char *root = ypcb_data;
	char *key;
	char *mapent;

	if (status != YP_TRUE)
		return status;

	key = alloca(ypkeylen + 1);
	strncpy(key, ypkey, ypkeylen);
	*(key + ypkeylen) = '\0';

	mapent = alloca(vallen + 1);
	strncpy(mapent, val, vallen);
	*(mapent + vallen) = '\0';

	cache_update(root, key, mapent, age);

	return 0;
}

static int read_map(const char *root, struct lookup_context *context)
{
	struct lookup_context *ctxt = (struct lookup_context *) context;
	struct ypall_callback ypcb;
	time_t age = time(NULL);
	int err;

	ypcb.foreach = yp_all_callback;
	ypcb.data = (char *) root;

	err = yp_all((char *) ctxt->domainname, (char *) ctxt->mapname, &ypcb);

	if (err != YPERR_SUCCESS) {
		warn(MODPREFIX "lookup_ghost for %s failed: %s",
		       root, yperr_string(err));
		return 0;
	}

	/* Clean stale entries from the cache */
	cache_clean(root, age);

	return 1;
}

int lookup_ghost(const char *root, int ghost, void *context)
{
	struct lookup_context *ctxt = (struct lookup_context *) context;
	struct mapent_cache *me;
	int status = 1;

	if (!read_map(root, ctxt))
		return LKP_FAIL;

	status = cache_ghost(root, ghost, ctxt->mapname, "yp", ctxt->parse);

	me = cache_lookup_first();
	/* me NULL => empty map */
	if (me == NULL)
		return LKP_FAIL;

	if (*me->key == '/' && *(root + 1) != '-') {
		me = cache_partial_match(root);
		/* me NULL => no entries for this direct mount root or indirect map */
		if (me == NULL)
			return LKP_FAIL | LKP_INDIRECT;
	}

	return status;
}

static int lookup_one(const char *root,
		      const char *key, int key_len,
		      struct lookup_context *ctxt)
{
	char *mapent;
	int mapent_len;
	time_t age = time(NULL);
	int err;

	/*
	 * For reasons unknown, the standard YP definitions doesn't
	 * define input strings as const char *.  However, my
	 * understanding is that they will not be modified by the
	 * library.
	 */
	err = yp_match((char *) ctxt->domainname, (char *) ctxt->mapname,
		       (char *) key, key_len, &mapent, &mapent_len);

	if (err != YPERR_SUCCESS) {
		if (err == YPERR_KEY)
			return CHE_MISSING;

		return -err;
	}

	return cache_update(root, key, mapent, age);
}

static int lookup_wild(const char *root, struct lookup_context *ctxt)
{
	char *mapent;
	int mapent_len;
	time_t age = time(NULL);
	int err;

	mapent = alloca(MAPENT_MAX_LEN + 1);
	if (!mapent)
		return 0;

	err = yp_match((char *) ctxt->domainname,
		       (char *) ctxt->mapname, "*", 1, &mapent, &mapent_len);

	if (err == YPERR_SUCCESS)
		return cache_update(root, "*", mapent, age);
	
	return -err;
}

int lookup_mount(const char *root, const char *name, int name_len, void *context)
{
	struct lookup_context *ctxt = (struct lookup_context *) context;
	char key[KEY_MAX_LEN + 1];
	int key_len;
	char *mapent;
	int mapent_len;
	struct mapent_cache *me;
	int ret;
	time_t now = time(NULL);
	time_t t_last_read;

	debug(MODPREFIX "looking up %s", name);

	if (ap.type == LKP_DIRECT)
		key_len = snprintf(key, KEY_MAX_LEN, "%s/%s", root, name);
	else
		key_len = snprintf(key, KEY_MAX_LEN, "%s", name);

	if (key_len > KEY_MAX_LEN)
		return 1;

	/* check map and if change is detected re-read map */
	ret = lookup_one(root, key, key_len, ctxt);

	debug("ret = %d", ret);

	if (!ret)
		return 1;

	if (ret < 0) {
		warn(MODPREFIX 
		     "lookup for %s failed: %s", name, yperr_string(-ret));
		return 1;
	}

	me = cache_lookup_first();
	t_last_read = me ? now - me->age : ap.exp_runfreq + 1;

	if (ret == CHE_UPDATED) {
		/* Have parent update its map */
		if (t_last_read > ap.exp_runfreq)
			kill(getppid(), SIGHUP);
	} else if (ret == CHE_MISSING) {
		if (!cache_delete(root, key, CHE_RMPATH)) {
			rmdir_path(key);
		}

		/* Maybe update wild card map entry */
		if (!ret && ap.type == LKP_INDIRECT)
			lookup_wild(root, ctxt);

		/* Have parent update its map */
		if (t_last_read > ap.exp_runfreq)
			kill(getppid(), SIGHUP);
	}


	me = cache_lookup(key);
	if (me) {
		mapent = alloca(strlen(me->mapent) + 1);
		mapent_len = sprintf(mapent, "%s", me->mapent);
	} else {
		/* path component, do submount */
		me = cache_partial_match(key);
		if (me) {
			mapent = alloca(strlen(ctxt->mapname) + 20);
			mapent_len =
			    sprintf(mapent, "-fstype=autofs yp:%s", ctxt->mapname);
		}
	}

	if (!me)
		return 1;

	mapent[mapent_len] = '\0';

	debug(MODPREFIX "%s -> %s", key, mapent);

	return ctxt->parse->parse_mount(root, name, name_len, mapent, ctxt->parse->context);
}

int lookup_done(void *context)
{
	struct lookup_context *ctxt = (struct lookup_context *) context;
	int rv = close_parse(ctxt->parse);
	free(ctxt);
	cache_release();
	return rv;
}
