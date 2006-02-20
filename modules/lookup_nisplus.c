#ident "$Id: lookup_nisplus.c,v 1.7 2006/02/20 01:05:32 raven Exp $"
/*
 * lookup_nisplus.c
 *
 * Module for Linux automountd to access a NIS+ automount map
 */

#include <stdio.h>
#include <malloc.h>
#include <unistd.h>
#include <syslog.h>
#include <sys/param.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/stat.h>
#include <rpc/rpc.h>
#include <rpc/xdr.h>
#include <rpcsvc/nis.h>

#define MODULE_LOOKUP
#include "automount.h"
#include "nsswitch.h"

#define MAPFMT_DEFAULT "sun"

#define MODPREFIX "lookup(nisplus): "

struct lookup_context {
	const char *domainname;
	const char *mapname;
	struct parse_mod *parse;
};

int lookup_version = AUTOFS_LOOKUP_VERSION;	/* Required by protocol */

int lookup_init(const char *mapfmt, int argc, const char *const *argv, void **context)
{
	struct lookup_context *ctxt;
	char buf[MAX_ERR_BUF];

	if (!(*context = ctxt = malloc(sizeof(struct lookup_context)))) {
		char *estr = strerror_r(errno, buf, MAX_ERR_BUF);
		crit(MODPREFIX "%s", estr);
		return 1;
	}

	if (argc < 1) {
		crit(MODPREFIX "No map name");
		return 1;
	}
	ctxt->mapname = argv[0];

	/* 
	 * nis_local_directory () returns a pointer to a static buffer.
	 * We don't need to copy or free it.
	 */
	ctxt->domainname = nis_local_directory();
	if (!ctxt->domainname) {
		error("NIS+ domain not set");
		return 1;
	}

	if (!mapfmt)
		mapfmt = MAPFMT_DEFAULT;

	return !(ctxt->parse = open_parse(mapfmt, MODPREFIX, argc - 1, argv + 1));
}

int lookup_read_map(struct autofs_point *ap, time_t age, void *context)
{
	struct lookup_context *ctxt = (struct lookup_context *) context;
	char tablename[strlen(ctxt->mapname) +
		       strlen(ctxt->domainname) + 20];
	nis_result *result;
	nis_object *this;
	unsigned int current, result_count;
	char *key, *mapent;

	sprintf(tablename, "%s.org_dir.%s", ctxt->mapname, ctxt->domainname);

	/* check that the table exists */
	result = nis_lookup(tablename, FOLLOW_PATH | FOLLOW_LINKS);
	if (result->status != NIS_SUCCESS && result->status != NIS_S_SUCCESS) {
		nis_freeresult(result);
		crit(MODPREFIX "couldn't locat nis+ table %s", ctxt->mapname);
		return NSS_STATUS_NOTFOUND;
	}

	sprintf(tablename, "[],%s.org_dir.%s", ctxt->mapname, ctxt->domainname);

	result = nis_list(tablename, FOLLOW_PATH | FOLLOW_LINKS, NULL, NULL);
	if (result->status != NIS_SUCCESS && result->status != NIS_S_SUCCESS) {
		nis_freeresult(result);
		crit(MODPREFIX "couldn't enumrate nis+ map %s", ctxt->mapname);
		return NSS_STATUS_UNAVAIL;
	}

	current = 0;
	result_count = result->objects.objects_len;

	while (result_count--) {
		this = &result->objects.objects_val[current++];
		key = this->EN_data.en_cols.en_cols_val[0].ec_value.ec_value_val;
		/*
		 * Ignore keys beginning with '+' as plus map
		 * inclusion is only valid in file maps.
		 */
		if (*key == '+')
			continue;
		mapent = this->EN_data.en_cols.en_cols_val[1].ec_value.ec_value_val;
		cache_update(key, mapent, age);
	}

	nis_freeresult(result);

	return NSS_STATUS_SUCCESS;
}

static int lookup_one(const char *root,
		      const char *key, int key_len,
		      struct lookup_context *ctxt)
{
	char tablename[strlen(key) + strlen(ctxt->mapname) +
		       strlen(ctxt->domainname) + 20];
	nis_result *result;
	nis_object *this;
	char *mapent;
	time_t age = time(NULL);
	int ret;

	sprintf(tablename, "[key=%s],%s.org_dir.%s", key, ctxt->mapname,
		ctxt->domainname);

	result = nis_list(tablename, FOLLOW_PATH | FOLLOW_LINKS, NULL, NULL);
	if (result->status != NIS_SUCCESS && result->status != NIS_S_SUCCESS) {
		nis_freeresult(result);
		if (result->status == NIS_NOTFOUND ||
		    result->status == NIS_S_NOTFOUND)
			return CHE_MISSING;

		return -result->status;
	}

	
	this = NIS_RES_OBJECT(result);
	mapent = this->EN_data.en_cols.en_cols_val[1].ec_value.ec_value_val;
	ret = cache_update(key, mapent, age);

	nis_freeresult(result);

	return ret;
}

static int lookup_wild(const char *root, struct lookup_context *ctxt)
{
	char tablename[strlen(ctxt->mapname) +
		       strlen(ctxt->domainname) + 20];
	nis_result *result;
	nis_object *this;
	char *mapent;
	time_t age = time(NULL);
	int ret;

	sprintf(tablename, "[key=*],%s.org_dir.%s", ctxt->mapname,
		ctxt->domainname);

	result = nis_list(tablename, FOLLOW_PATH | FOLLOW_LINKS, NULL, NULL);
	if (result->status != NIS_SUCCESS && result->status != NIS_S_SUCCESS) {
		nis_freeresult(result);
		if (result->status == NIS_NOTFOUND ||
		    result->status == NIS_S_NOTFOUND)
			return CHE_MISSING;

		return -result->status;
	}

	this = NIS_RES_OBJECT(result);
	mapent = this->EN_data.en_cols.en_cols_val[1].ec_value.ec_value_val;
	ret = cache_update("*", mapent, age);

	nis_freeresult(result);

	return ret;
}

static int check_map_indirect(struct autofs_point *ap,
			      char *key, int key_len,
			      struct lookup_context *ctxt)
{
	struct mapent_cache *me, *exists;
	time_t now = time(NULL);
	time_t t_last_read;
	int need_hup = 0;
	int ret = 0;

	/* First check to see if this entry exists in the cache */
	exists = cache_lookup(key);

	/* check map and if change is detected re-read map */
	ret = lookup_one(ap->path, key, key_len, ctxt);
	if (ret == CHE_FAIL)
		return NSS_STATUS_NOTFOUND;

	if (ret < 0) {
		warn(MODPREFIX
		     "lookup for %s failed: %s", key, nis_sperrno(-ret));
		return NSS_STATUS_UNAVAIL;
	}

	me = cache_lookup_first();
	t_last_read = me ? now - me->age : ap->exp_runfreq + 1;

	if (t_last_read > ap->exp_runfreq)
		if ((ret & CHE_UPDATED) ||
		    (exists && (ret & CHE_MISSING)))
			need_hup = 1;

	if (ret == CHE_MISSING) {
		int wild = CHE_MISSING;

		wild = lookup_wild(ap->path, ctxt);
		if (wild == CHE_MISSING)
			cache_delete("*");

		if (cache_delete(key) &&
				wild & (CHE_MISSING | CHE_FAIL))
			rmdir_path(key);
	}

	/* Have parent update its map */
	if (need_hup)
		kill(getppid(), SIGHUP);

	if (ret == CHE_MISSING)
		return NSS_STATUS_NOTFOUND;

	return NSS_STATUS_SUCCESS;
}

int lookup_mount(struct autofs_point *ap, const char *name, int name_len, void *context)
{
	struct lookup_context *ctxt = (struct lookup_context *) context;
	char key[KEY_MAX_LEN + 1];
	int key_len;
	char *mapent;
	int mapent_len;
	struct mapent_cache *me;
	int status;
	int ret = 1;

	debug(MODPREFIX "looking up %s", name);

	key_len = snprintf(key, KEY_MAX_LEN, "%s", name);
	if (key_len > KEY_MAX_LEN)
		return NSS_STATUS_NOTFOUND;

	/*
	 * We can't check the direct mount map as if it's not in
	 * the map cache already we never get a mount lookup, so
	 * we never know about it.
	 */
	if (ap->type == LKP_INDIRECT) {
		status = check_map_indirect(ap, key, key_len, ctxt);
		if (status) {
			debug(MODPREFIX "check indirect map failure");
			return status;
		}
	}

	me = cache_lookup(key);
	if (me) {
		mapent = alloca(strlen(me->mapent) + 1);
		mapent_len = sprintf(mapent, "%s", me->mapent);
		debug(MODPREFIX "%s -> %s", key, mapent);
		ret = ctxt->parse->parse_mount(ap, key, key_len,
					       mapent, ctxt->parse->context);
	}

	if (ret)
		return NSS_STATUS_TRYAGAIN;

	return NSS_STATUS_SUCCESS;
}

int lookup_done(void *context)
{
	struct lookup_context *ctxt = (struct lookup_context *) context;
	int rv = close_parse(ctxt->parse);
	free(ctxt);
	return rv;
}
