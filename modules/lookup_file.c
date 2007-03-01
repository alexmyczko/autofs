/* ----------------------------------------------------------------------- *
 *   
 *  lookup_file.c - module for Linux automount to query a flat file map
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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <ctype.h>
#include <signal.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>

#define MODULE_LOOKUP
#include "automount.h"
#include "nsswitch.h"

#define MAPFMT_DEFAULT "sun"

#define MODPREFIX "lookup(file): "

#define MAX_INCLUDE_DEPTH	16

typedef enum {
	st_begin, st_compare, st_star, st_badent, st_entspc, st_getent
} LOOKUP_STATE;

typedef enum { got_nothing, got_star, got_real, got_plus } FOUND_STATE;
typedef enum { esc_none, esc_char, esc_val, esc_all } ESCAPES;


struct lookup_context {
	const char *mapname;
	time_t mtime;
	struct parse_mod *parse;
};

int lookup_version = AUTOFS_LOOKUP_VERSION;	/* Required by protocol */

int lookup_init(const char *mapfmt, int argc, const char *const *argv, void **context)
{
	struct lookup_context *ctxt;
	char buf[MAX_ERR_BUF];
	struct stat st;

	*context = NULL;

	ctxt = malloc(sizeof(struct lookup_context));
	if (!ctxt) {
		char *estr = strerror_r(errno, buf, MAX_ERR_BUF);
		crit(LOGOPT_ANY, MODPREFIX "malloc: %s", estr);
		return 1;
	}

	if (argc < 1) {
		free(ctxt);
		crit(LOGOPT_ANY, MODPREFIX "No map name");
		return 1;
	}

	ctxt->mapname = argv[0];

	if (ctxt->mapname[0] != '/') {
		free(ctxt);
		msg(MODPREFIX "file map %s is not an absolute pathname",
		    argv[0]);
		return 1;
	}

	if (access(ctxt->mapname, R_OK)) {
		free(ctxt);
		msg(MODPREFIX "file map %s missing or not readable",
		    argv[0]);
		return 1;
	}

	if (stat(ctxt->mapname, &st)) {
		free(ctxt);
		crit(LOGOPT_ANY, MODPREFIX "file map %s, could not stat",
		     argv[0]);
		return 1;
	}
		
	ctxt->mtime = st.st_mtime;

	if (!mapfmt)
		mapfmt = MAPFMT_DEFAULT;

	ctxt->parse = open_parse(mapfmt, MODPREFIX, argc - 1, argv + 1);
	if (!ctxt->parse) {
		free(ctxt);
		crit(LOGOPT_ANY, MODPREFIX "failed to open parse context");
		return 1;
	}
	*context = ctxt;

	return 0;
}

static int read_one(FILE *f, char *key, unsigned int *k_len, char *mapent, unsigned int *m_len)
{
	char *kptr, *p;
	int mapent_len, key_len;
	int ch, nch;
	LOOKUP_STATE state;
	FOUND_STATE getting, gotten;
	ESCAPES escape;

	kptr = key;
	p = NULL;
	mapent_len = key_len = 0;
	state = st_begin;
	memset(key, 0, KEY_MAX_LEN + 1);
	memset(mapent, 0, MAPENT_MAX_LEN + 1);
	getting = gotten = got_nothing;
	escape = esc_none;

	/* This is ugly.  We can't just remove \ escape sequences in the value
	   portion of an entry, because the parsing routine may need it. */

	while ((ch = getc(f)) != EOF) {
		switch (escape) {
		case esc_none:
			if (ch == '\\') {
				/* Handle continuation lines */
				if ((nch = getc(f)) == '\n')
					continue;
				ungetc(nch, f);
				escape = esc_char;
			}
			if (ch == '"')
				escape = esc_all;
			break;

		case esc_char:
			escape = esc_val;
			break;

		case esc_val:
			escape = esc_none;
			break;

		case esc_all:
			if (ch == '"')
				escape = esc_none;
			break;
		}

		switch (state) {
		case st_begin:
			if (!escape) {
				if (isspace(ch));
				else if (ch == '#')
					state = st_badent;
				else if (ch == '*') {
					state = st_star;
					*(kptr++) = ch;
					key_len++;
				} else {
					if (ch == '+')
						gotten = got_plus;
					state = st_compare;
					*(kptr++) = ch;
					key_len++;
				}
			} else if (escape == esc_all) {
				state = st_compare;
				*(kptr++) = ch;
				key_len++;
			} else if (escape == esc_char);
			else
				state = st_badent;
			break;

		case st_compare:
			if (ch == '\n') {
				state = st_begin;
				if (gotten == got_plus)
					goto got_it;
				else if (escape == esc_all) {
					warn(LOGOPT_ANY, MODPREFIX
					    "unmatched \" in map key %s", key);
					goto next;
				} else if (escape != esc_val)
					goto got_it;
			} else if (isspace(ch) && !escape) {
				getting = got_real;
				state = st_entspc;
				if (gotten == got_plus)
					goto got_it;
			} else if (escape == esc_char);
			else {
				if (key_len == KEY_MAX_LEN) {
					state = st_badent;
					gotten = got_nothing;
					warn(LOGOPT_ANY,
					      MODPREFIX "map key \"%s...\" "
					      "is too long.  The maximum key "
					      "length is %d", key,
					      KEY_MAX_LEN);
				} else {
					if (escape == esc_val) {
						*(kptr++) = '\\';
						key_len++;
					}
					*(kptr++) = ch;
					key_len++;
				}
			}
			break;

		case st_star:
			if (ch == '\n')
				state = st_begin;
			else if (isspace(ch) && gotten < got_star && !escape) {
				getting = got_star;
				state = st_entspc;
			} else if (escape != esc_char)
				state = st_badent;
			break;

		case st_badent:
			if (ch == '\n') {
				nch = getc(f);
				if (nch != EOF && isblank(nch)) {
					ungetc(nch, f);
					break;
				}
				ungetc(nch, f);
				state = st_begin;
				if (gotten == got_real || gotten == getting)
					goto got_it;
				warn(LOGOPT_ANY, MODPREFIX 
				      "bad map entry \"%s...\" for key "
				      "\"%s\"", mapent, key);
				goto next;
			} else if (!isblank(ch))
				gotten = got_nothing;
			break;

		case st_entspc:
			if (ch == '\n')
				state = st_begin;
			else if (!isspace(ch) || escape) {
				if (escape) {
					if (escape == esc_char)
						break;
					if (ch <= 32) {
						getting = got_nothing;
						state = st_badent;
						break;
					}
					p = mapent;
					if (escape == esc_val) {
						*(p++) = '\\';
						mapent_len++;
					}
					*(p++) = ch;
					mapent_len++;
				} else {
					p = mapent;
					*(p++) = ch;
					mapent_len = 1;
				}
				state = st_getent;
				gotten = getting;
			}
			break;

		case st_getent:
			if (ch == '\n') {
				if (escape == esc_all) {
					state = st_begin;
					warn(LOGOPT_ANY, MODPREFIX
					     "unmatched \" in %s for key %s",
					     mapent, key);
					goto next;
				}
				nch = getc(f);
				if (nch != EOF && isblank(nch)) {
					ungetc(nch, f);
					state = st_badent;
					break;
				}
				ungetc(nch, f);
				state = st_begin;
				if (gotten == got_real || gotten == getting)
					goto got_it;
			} else if (mapent_len < MAPENT_MAX_LEN) {
				mapent_len++;
				*(p++) = ch;
				nch = getc(f);
				if (nch == EOF &&
				   (gotten == got_real || gotten == getting))
				   	goto got_it;
				ungetc(nch, f);
			} else {
				warn(LOGOPT_ANY,
				      MODPREFIX "map entry \"%s...\" for key "
				      "\"%s\" is too long.  The maximum entry"
				      " size is %d", mapent, key,
				      MAPENT_MAX_LEN);
				state = st_badent;
			}
			break;
		}
		continue;

	      got_it:
		if (gotten == got_nothing)
			goto next;

		*k_len = key_len;
		*m_len = mapent_len;

		return 1;

	      next:
		kptr = key;
		p = NULL;
		mapent_len = key_len = 0;
		memset(key, 0, KEY_MAX_LEN + 1);
		memset(mapent, 0, MAPENT_MAX_LEN + 1);
		getting = gotten = got_nothing;
		escape = esc_none;
	}

	return 0;
}

static int check_master_self_include(struct master *master, struct lookup_context *ctxt)
{
	char *m_path, *m_base, *i_path, *i_base;

	/*
	 * If we are including a file map then check the
	 * full path of the map.
	 */
	if (*master->name == '/') {
		if (!strcmp(master->name, ctxt->mapname))
			return 1;
		else
			return 0;
	}

	/* Otherwise only check the map name itself. */

	i_path = strdup(ctxt->mapname);
	if (!i_path)
		return 0;
	i_base = basename(i_path);

	m_path = strdup(master->name);
	if (!m_path) {
		free(i_path);
		return 0;
	}
	m_base = basename(m_path);

	if (!strcmp(m_base, i_base)) {
		free(i_path);
		free(m_path);
		return  1;
	}
	free(i_path);
	free(m_path);

	return 0;
}

int lookup_read_master(struct master *master, time_t age, void *context)
{
	struct lookup_context *ctxt = (struct lookup_context *) context;
	unsigned int timeout = master->default_timeout;
	unsigned int logging = master->default_logging;
	char *buffer;
	int blen;
	char *path;
	char *ent;
	struct stat st;
	FILE *f;
	int fd, cl_flags;
	unsigned int path_len, ent_len;
	int entry, cur_state;

	if (master->recurse)
		return NSS_STATUS_UNAVAIL;

	if (master->depth > MAX_INCLUDE_DEPTH) {
		error(LOGOPT_ANY,
		      MODPREFIX
		      "maximum include depth exceeded %s", master->name);
		return NSS_STATUS_UNAVAIL;
	}

	path = alloca(KEY_MAX_LEN + 1);
	if (!path) {
		error(LOGOPT_ANY,
		      MODPREFIX "could not malloc storage for path");
		return NSS_STATUS_UNAVAIL;
	}

	ent = alloca(MAPENT_MAX_LEN + 1);
	if (!ent) {
		error(LOGOPT_ANY,
		      MODPREFIX "could not malloc storage for mapent");
		return NSS_STATUS_UNAVAIL;
	}

	f = fopen(ctxt->mapname, "r");
	if (!f) {
		error(LOGOPT_ANY,
		      MODPREFIX "could not open master map file %s",
		      ctxt->mapname);
		return NSS_STATUS_UNAVAIL;
	}

	fd = fileno(f);

	if ((cl_flags = fcntl(fd, F_GETFD, 0)) != -1) {
		cl_flags |= FD_CLOEXEC;
		fcntl(fd, F_SETFD, cl_flags);
	}

	while(1) {
		entry = read_one(f, path, &path_len, ent, &ent_len);
		if (!entry) {
			if (feof(f))
				break;
			continue;
		}

		debug(LOGOPT_NONE, MODPREFIX "read entry %s", path);

		/*
		 * If key starts with '+' it has to be an
		 * included map.
		 */
		if (*path == '+') {
			char *save_name;
			unsigned int inc;
			int status;

			save_name = master->name;
			master->name = path + 1;

			inc = check_master_self_include(master, ctxt);
			if (inc) 
				master->recurse = 1;;
			master->depth++;
			status = lookup_nss_read_master(master, age);
			if (!status)
				warn(LOGOPT_ANY,
				     MODPREFIX
				     "failed to read included master map %s",
				     master->name);
			master->depth--;
			master->recurse = 0;

			master->name = save_name;
		} else {
			blen = path_len + 1 + ent_len + 1;
			buffer = malloc(blen);
			if (!buffer) {
				error(LOGOPT_ANY,
				      MODPREFIX "could not malloc parse buffer");
				return NSS_STATUS_UNAVAIL;
			}
			memset(buffer, 0, blen);

			strcpy(buffer, path);
			strcat(buffer, " ");
			strcat(buffer, ent);

			pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cur_state);
			master_parse_entry(buffer, timeout, logging, age);
			free(buffer);
			pthread_setcancelstate(cur_state, NULL);
		}

		if (feof(f))
			break;
	}

	if (fstat(fd, &st)) {
		crit(LOGOPT_ANY, MODPREFIX "file map %s, could not stat",
		       ctxt->mapname);
		return NSS_STATUS_UNAVAIL;
	}
	ctxt->mtime = st.st_mtime;

	fclose(f);

	return NSS_STATUS_SUCCESS;
}

static int check_self_include(const char *key, struct lookup_context *ctxt)
{
	char *m_key, *m_base, *i_key, *i_base;

	/*
	 * If we are including a file map then check the
	 * full path of the map.
	 */
	if (*(key + 1) == '/') {
		if (!strcmp(key + 1, ctxt->mapname))
			return 1;
		else
			return 0;
	}

	i_key = strdup(key + 1);
	if (!i_key)
		return 0;
	i_base = basename(i_key);

	m_key = strdup(ctxt->mapname);
	if (!m_key) {
		free(i_key);
		return 0;
	}
	m_base = basename(m_key);

	if (!strcmp(m_base, i_base)) {
		free(i_key);
		free(m_key);
		return 1;
	}
	free(i_key);
	free(m_key);

	return 0;
}

static struct autofs_point *
prepare_plus_include(struct autofs_point *ap, time_t age, char *key, unsigned int inc)
{
	struct master *master;
	struct master_mapent *entry;
	struct map_source *current;
	struct map_source *source;
	struct autofs_point *iap;
	char *type, *map, *fmt;
	const char *argv[2];
	int ret, argc;
	unsigned int timeout = ap->exp_timeout;
	unsigned int logopt = ap->logopt;
	unsigned int ghost = ap->ghost;
	char *buf, *tmp;

	current = ap->entry->current;
	ap->entry->current = NULL;
	master_source_current_signal(ap->entry);

	master = ap->entry->master;

	entry = master_new_mapent(master, ap->path, ap->entry->age);
	if (!entry) {
		error(ap->logopt, MODPREFIX "malloc failed for entry");
		return NULL;
	}

	ret = master_add_autofs_point(entry, timeout, logopt, ghost, 0);
	if (!ret) {
		master_free_mapent(entry);
		error(ap->logopt,
		      MODPREFIX "failed to add autofs_point to entry");
		return NULL;
	}
	iap = entry->ap;
	iap->kpipefd = ap->kpipefd;
	set_mnt_logging(iap);

	/*
	 * TODO:
	 * Initially just consider the passed in key to be a simple map
	 * name (and possible source) and use the global map options in
	 * the given autofs_point. ie. global options override.
	 *
	 * Later we might want to parse this and fill in the autofs_point
	 * options fields.
	 */
	/* skip plus */
	buf = strdup(key + 1);
	if (!buf) {
		master_free_mapent(entry);
		error(ap->logopt, MODPREFIX "failed to strdup key");
		return NULL;
	}

	type = fmt = NULL;

	/* Look for space terminator - ignore local options */
	map = buf;
	for (tmp = buf; *tmp; tmp++) {
		if (*tmp == ' ') {
			*tmp = '\0';
			break;
		} else if (*tmp == ',') {
			type = buf;
			*tmp++ = '\0';
			fmt = tmp;
		} else if (*tmp == ':') {
			if (!fmt)
				type = buf;
			*tmp++ = '\0';
			map = tmp;
		}
		if (*tmp == '\\')
			tmp++;
	}

	argc = 1;
	argv[0] = map;
	argv[1] = NULL;

	source = master_add_map_source(entry, type, fmt, age, argc, argv);
	if (!source) {
		master_free_mapent(entry);
		free(buf);
		error(ap->logopt, "failed to creat map_source");
		return NULL;
	}
	source->mc = current->mc;
	source->depth = current->depth + 1;
	if (inc)
		source->recurse = 1;

	free(buf);

	return iap;
}

int lookup_read_map(struct autofs_point *ap, time_t age, void *context)
{
	struct lookup_context *ctxt = (struct lookup_context *) context;
	struct map_source *source;
	struct mapent_cache *mc;
	char *key;
	char *mapent;
	struct stat st;
	FILE *f;
	int fd, cl_flags;
	unsigned int k_len, m_len;
	int entry;

	source = ap->entry->current;
	ap->entry->current = NULL;
	master_source_current_signal(ap->entry);

	mc = source->mc;

	if (source->recurse)
		return NSS_STATUS_UNAVAIL;

	if (source->depth > MAX_INCLUDE_DEPTH) {
		error(ap->logopt,
		      "maximum include depth exceeded %s", ctxt->mapname);
		return NSS_STATUS_UNAVAIL;
	}

	key = alloca(KEY_MAX_LEN + 1);
	if (!key) {
		error(ap->logopt,
		      MODPREFIX "could not malloc storage for key");
		return NSS_STATUS_UNAVAIL;
	}

	mapent = alloca(MAPENT_MAX_LEN + 1);
	if (!mapent) {
		error(ap->logopt,
		      MODPREFIX "could not malloc storage for mapent");
		return NSS_STATUS_UNAVAIL;
	}

	f = fopen(ctxt->mapname, "r");
	if (!f) {
		error(ap->logopt,
		      MODPREFIX "could not open map file %s", ctxt->mapname);
		return NSS_STATUS_UNAVAIL;
	}

	fd = fileno(f);

	if ((cl_flags = fcntl(fd, F_GETFD, 0)) != -1) {
		cl_flags |= FD_CLOEXEC;
		fcntl(fd, F_SETFD, cl_flags);
	}

	while(1) {
		entry = read_one(f, key, &k_len, mapent, &m_len);
		if (!entry) {
			if (feof(f))
				break;
			continue;
		}
			
		/*
		 * If key starts with '+' it has to be an
		 * included map.
		 */
		if (*key == '+') {
			struct autofs_point *iap;
			unsigned int inc;
			int status;

			debug(ap->logopt, "read included map %s", key);

			inc = check_self_include(key, ctxt);

			master_source_current_wait(ap->entry);
			ap->entry->current = source;

			iap = prepare_plus_include(ap, age, key, inc);
			if (!iap) {
				debug(ap->logopt,
				      "failed to select included map %s", key);
				continue;
			}

			/* Gim'ee some o' that 16k stack baby !! */
			status = lookup_nss_read_map(iap, age);
			if (!status)
				warn(ap->logopt,
				     "failed to read included map %s", key);

			master_free_mapent_sources(iap->entry, 0);
			master_free_mapent(iap->entry);
		} else {
			char *s_key; 

			s_key = sanitize_path(key, k_len, ap->type, ap->logopt);
			if (!s_key)
				continue;

			cache_writelock(mc);
			cache_update(mc, s_key, mapent, age);
			cache_unlock(mc);

			free(s_key);
		}

		if (feof(f))
			break;
	}

	if (fstat(fd, &st)) {
		crit(ap->logopt,
		     MODPREFIX "file map %s, could not stat",
		     ctxt->mapname);
		return NSS_STATUS_UNAVAIL;
	}
	ctxt->mtime = st.st_mtime;
	source->age = age;

	fclose(f);

	return NSS_STATUS_SUCCESS;
}

static int lookup_one(struct autofs_point *ap,
		      const char *key, int key_len,
		      struct lookup_context *ctxt)
{
	struct map_source *source;
	struct mapent_cache *mc;
	char mkey[KEY_MAX_LEN + 1];
	char mapent[MAPENT_MAX_LEN + 1];
	time_t age = time(NULL);
	FILE *f;
	int fd, cl_flags;
	unsigned int k_len, m_len;
	int entry, ret;

	source = ap->entry->current;
	ap->entry->current = NULL;
	master_source_current_signal(ap->entry);

	mc = source->mc;

	f = fopen(ctxt->mapname, "r");
	if (!f) {
		error(ap->logopt,
		      MODPREFIX "could not open map file %s", ctxt->mapname);
		return CHE_FAIL;
	}

	fd = fileno(f);

	if ((cl_flags = fcntl(fd, F_GETFD, 0)) != -1) {
		cl_flags |= FD_CLOEXEC;
		fcntl(fd, F_SETFD, cl_flags);
	}

	while(1) {
		entry = read_one(f, mkey, &k_len, mapent, &m_len);
		if (entry) {
			/*
			 * If key starts with '+' it has to be an
			 * included map.
			 */
			if (*mkey == '+') {
				struct autofs_point *iap;
				unsigned int inc;
				int status;

				debug(ap->logopt,
				      MODPREFIX "lookup included map %s", mkey);

				inc = check_self_include(mkey, ctxt);

				master_source_current_wait(ap->entry);
				ap->entry->current = source;

				iap = prepare_plus_include(ap, age, mkey, inc);
				if (!iap) {
					debug(ap->logopt,
					      MODPREFIX
					      "failed to select included map %s",
					      key);
					continue;
				}

				/* Gim'ee some o' that 16k stack baby !! */
				status = lookup_nss_mount(iap, key, key_len);

				master_free_mapent_sources(iap->entry, 0);
				master_free_mapent(iap->entry);

				if (status) {
					fclose(f);
					return CHE_COMPLETED;
				}
			} else {
				char *s_key; 
				int eq;

				s_key = sanitize_path(mkey, k_len, ap->type, ap->logopt);
				if (!s_key)
					continue;

				if (key_len != strlen(s_key)) {
					free(s_key);
					continue;
				}

				eq = strncmp(s_key, key, key_len);
				if (eq != 0) {
					free(s_key);
					continue;
				}

				free(s_key);

				cache_writelock(mc);
				ret = cache_update(mc, key, mapent, age);
				cache_unlock(mc);

				fclose(f);

				return ret;
			}
		}

		if (feof(f))
			break;
	}

	fclose(f);

	return CHE_MISSING;
}

static int lookup_wild(struct autofs_point *ap, struct lookup_context *ctxt)
{
	struct map_source *source;
	struct mapent_cache *mc;
	char mkey[KEY_MAX_LEN + 1];
	char mapent[MAPENT_MAX_LEN + 1];
	time_t age = time(NULL);
	FILE *f;
	int fd, cl_flags;
	unsigned int k_len, m_len;
	int entry, ret;

	source = ap->entry->current;
	ap->entry->current = NULL;
	master_source_current_signal(ap->entry);

	mc = source->mc;

	f = fopen(ctxt->mapname, "r");
	if (!f) {
		error(ap->logopt,
		      MODPREFIX "could not open map file %s", ctxt->mapname);
		return CHE_FAIL;
	}

	fd = fileno(f);

	if ((cl_flags = fcntl(fd, F_GETFD, 0)) != -1) {
		cl_flags |= FD_CLOEXEC;
		fcntl(fd, F_SETFD, cl_flags);
	}

	while(1) {
		entry = read_one(f, mkey, &k_len, mapent, &m_len);
		if (entry) {
			int eq;

			eq = (*mkey == '*' && k_len == 1);
			if (eq == 0)
				continue;

			cache_writelock(mc);
			ret = cache_update(mc, "*", mapent, age);
			cache_unlock(mc);

			fclose(f);

			return ret;
		}

		if (feof(f))
			break;
	}

	fclose(f);

	return CHE_MISSING;
}

static int check_map_indirect(struct autofs_point *ap,
			      char *key, int key_len,
			      struct lookup_context *ctxt)
{
	struct map_source *source;
	struct mapent_cache *mc;
	struct mapent *exists;
	int need_map = 0;
	int ret = CHE_OK;

	source = ap->entry->current;
	ap->entry->current = NULL;
	master_source_current_signal(ap->entry);

	mc = source->mc;

	cache_readlock(mc);
	exists = cache_lookup_distinct(mc, key);
	if (exists && exists->mc != mc)
		exists = NULL;
	cache_unlock(mc);

	master_source_current_wait(ap->entry);
	ap->entry->current = source;

	ret = lookup_one(ap, key, key_len, ctxt);
	if (ret == CHE_COMPLETED)
		return NSS_STATUS_COMPLETED;

	if (ret == CHE_FAIL)
		return NSS_STATUS_NOTFOUND;

	if ((ret & CHE_UPDATED) ||
	    (exists && (ret & CHE_MISSING)))
		need_map = 1;

	if (ret == CHE_MISSING) {
		int wild = CHE_MISSING;

		master_source_current_wait(ap->entry);
		ap->entry->current = source;

		wild = lookup_wild(ap, ctxt);
		if (wild == CHE_COMPLETED || CHE_UPDATED || CHE_OK)
			return NSS_STATUS_SUCCESS;
/*
		if (wild == CHE_FAIL)
			return NSS_STATUS_NOTFOUND;
*/
		pthread_cleanup_push(cache_lock_cleanup, mc);
		cache_writelock(mc);
		if (wild == CHE_MISSING)
			cache_delete(mc, "*");

		if (cache_delete(mc, key) && wild & (CHE_MISSING | CHE_FAIL))
			rmdir_path(ap, key, ap->dev);
		pthread_cleanup_pop(1);
	}

	/* Have parent update its map ? */
	/* TODO: update specific map */
	if (ap->ghost && need_map) {
		int status;

		source->stale = 1;

		status = pthread_mutex_lock(&ap->state_mutex);
		if (status)
			fatal(status);

		nextstate(ap->state_pipe[1], ST_READMAP);

		status = pthread_mutex_unlock(&ap->state_mutex);
		if (status)
			fatal(status);
	}

	if (ret == CHE_MISSING)
		return NSS_STATUS_NOTFOUND;

	return NSS_STATUS_SUCCESS;
}

int lookup_mount(struct autofs_point *ap, const char *name, int name_len, void *context)
{
	struct lookup_context *ctxt = (struct lookup_context *) context;
	struct map_source *source;
	struct mapent_cache *mc;
	struct mapent *me;
	char key[KEY_MAX_LEN + 1];
	int key_len;
	char *mapent = NULL;
	int mapent_len;
	int status = 0;
	int ret = 1;

	source = ap->entry->current;
	ap->entry->current = NULL;
	master_source_current_signal(ap->entry);

	mc = source->mc;

	if (source->recurse)
		return NSS_STATUS_UNAVAIL;

	if (source->depth > MAX_INCLUDE_DEPTH) {
		error(ap->logopt,
		      MODPREFIX
		      "maximum include depth exceeded %s", ctxt->mapname);
		return NSS_STATUS_SUCCESS;
	}

	debug(ap->logopt, MODPREFIX "looking up %s", name);

	key_len = snprintf(key, KEY_MAX_LEN, "%s", name);
	if (key_len > KEY_MAX_LEN)
		return NSS_STATUS_NOTFOUND;

	/* Check if we recorded a mount fail for this key */
	cache_readlock(mc);
	me = cache_lookup_distinct(mc, key);
	if (me && me->status >= time(NULL)) {
		cache_unlock(mc);
		return NSS_STATUS_NOTFOUND;
	}
	cache_unlock(mc);

	/*
	 * We can't check the direct mount map as if it's not in
	 * the map cache already we never get a mount lookup, so
	 * we never know about it.
	 */
	if (ap->type == LKP_INDIRECT && *key != '/') {
		char *lkp_key;

		cache_readlock(mc);
		me = cache_lookup_distinct(mc, key);
		if (me && me->multi)
			lkp_key = strdup(me->multi->key);
		else
			lkp_key = strdup(key);
		cache_unlock(mc);

		if (!lkp_key)
			return NSS_STATUS_UNKNOWN;

		master_source_current_wait(ap->entry);
		ap->entry->current = source;

		status = check_map_indirect(ap, lkp_key, strlen(lkp_key), ctxt);
		free(lkp_key);
		if (status) {
			if (status == NSS_STATUS_COMPLETED)
				return NSS_STATUS_SUCCESS;

			debug(ap->logopt,
			      MODPREFIX "check indirect map lookup failed");

			return NSS_STATUS_NOTFOUND;
		}
	}

	cache_readlock(mc);
	me = cache_lookup(mc, key);
	if (me && me->mapent && *me->mapent) {
		pthread_cleanup_push(cache_lock_cleanup, mc);
		mapent_len = strlen(me->mapent);
		mapent = alloca(mapent_len + 1);
		strcpy(mapent, me->mapent);
		pthread_cleanup_pop(0);
	}
	cache_unlock(mc);

	if (mapent) {
		master_source_current_wait(ap->entry);
		ap->entry->current = source;

		debug(ap->logopt, MODPREFIX "%s -> %s", key, mapent);
		ret = ctxt->parse->parse_mount(ap, key, key_len,
					mapent, ctxt->parse->context);
		if (ret) {
			time_t now = time(NULL);
			int rv = CHE_OK;

			cache_writelock(mc);
			me = cache_lookup_distinct(mc, key);
			if (!me)
				rv = cache_update(mc, key, NULL, now);
			if (rv != CHE_FAIL) {
				me = cache_lookup_distinct(mc, key);
				me->status = now + NEGATIVE_TIMEOUT;
			}
			cache_unlock(mc);
		}
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
