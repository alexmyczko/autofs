#ident "$Id: master.c,v 1.1 2006/03/21 04:28:53 raven Exp $"
/* ----------------------------------------------------------------------- *
 *   
 *  master.c - master map utility routines.
 *
 *   Copyright 2006 Ian Kent <raven@themaw.net>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, Inc., 675 Mass Ave, Cambridge MA 02139,
 *   USA; either version 2 of the License, or (at your option) any later
 *   version.
 *   
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 * ----------------------------------------------------------------------- */

#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <limits.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>
#include "automount.h"

char default_master_map[] = DEFAULT_MASTER_MAP;

static unsigned int default_timeout = DEFAULT_TIMEOUT;
static unsigned int default_ghost_mode = DEFAULT_GHOST_MODE;

/* The root of the map entry tree */
struct master *master;

/* Attribute to create detached thread */
extern pthread_attr_t detach_attr;

extern struct startup_cond sc;

pthread_mutex_t master_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t instance_mutex = PTHREAD_MUTEX_INITIALIZER;

void master_set_default_timeout(void)
{
	char *timeout;

	timeout = getenv("DEFAULT_TIMEOUT");
	if (timeout) {
		if (isdigit(*timeout))
			default_timeout = atoi(timeout);
	}
}

void master_set_default_ghost_mode(void)
{
	char *ghost;

	ghost = getenv("DEFAULT_BROWSE_MODE");
	if (ghost) {
		if (isdigit(*ghost)) {
			if (!atoi(ghost))
				default_ghost_mode = 0;
		} else if (strcasecmp(ghost, "no")) {
			default_ghost_mode = 0;
		}
	}
}

int master_readmap_cond_init(struct readmap_cond *rc)
{
	int status;


	if (rc->busy)
		return 0;

	status = pthread_mutex_init(&rc->mutex, NULL);
	if (status) {
		error("failed to init readmap condition mutex");
		return 0;
	}

	status = pthread_cond_init(&rc->cond, NULL);
	if (status) {
		error("failed to init readmap condition");
		status = pthread_mutex_destroy(&rc->mutex);
		if (status)
			fatal(status);
		return 0;
	}

	return 1;
}

void master_readmap_cond_destroy(struct readmap_cond *rc)
{
	int status;

	status = pthread_cond_destroy(&rc->cond);
	if (status) {
		debug("failed to destroy readmap condition mutex");
		fatal(status);
	}

	status = pthread_mutex_destroy(&rc->mutex);
	if (status) {
		debug("failed to destroy readmap condition");
		fatal(status);
	}

	return;
}

int master_add_autofs_point(struct master_mapent *entry,
		time_t timeout, unsigned logopt, unsigned ghost, int submount) 
{
	struct autofs_point *ap;
	int status;

	ap = malloc(sizeof(struct autofs_point));
	if (!ap)
		return 0;

	memset(ap, 0, sizeof(struct autofs_point));

	ap->state = ST_INIT;

	ap->state_pipe[0] = -1;
	ap->state_pipe[1] = -1;

	ap->mc = cache_init(ap);
	if (!ap->mc) {
		free(ap);
		return 0;
	}

	ap->path = strdup(entry->path);
	if (!ap->path) {
		free(ap->mc);
		free(ap);
		return 0;
	}

	ap->entry = entry;
	ap->exp_timeout = timeout;
	ap->exp_runfreq = (timeout + CHECK_RATIO - 1) / CHECK_RATIO;
	if (ghost)
		ap->ghost = ghost;
	else
		ap->ghost = default_ghost_mode;

	if (ap->path[1] == '-')
		ap->type = LKP_DIRECT;
	else
		ap->type = LKP_INDIRECT;

	ap->dir_created = 0;
	ap->logopt = logopt;

	ap->parent = NULL;
	ap->submount = submount;
	INIT_LIST_HEAD(&ap->mounts);
	INIT_LIST_HEAD(&ap->submounts);

	status = pthread_mutex_init(&ap->state_mutex, NULL);
	if (status) {
		free(ap->path);
		free(ap->mc);
		free(ap);
		return 0;
	}

	status = pthread_mutex_init(&ap->mounts_mutex, NULL);
	if (status) {
		status = pthread_mutex_destroy(&ap->state_mutex);
		if (status)
			fatal(status);
		free(ap->path);
		free(ap->mc);
		free(ap);
		return 0;
	}

	entry->ap = ap;

	debug("add %s", ap->path);

	return 1;
}

void master_free_autofs_point(struct autofs_point *ap)
{
	int status;

	if (!ap)
		return;

	debug("free %s", ap->path);

	if (ap->submount) {
		pthread_mutex_lock(&ap->parent->mounts_mutex);
		if (!list_empty(&ap->mounts))
			list_del(&ap->mounts);
		pthread_mutex_unlock(&ap->parent->mounts_mutex);
	}
	cache_release(ap);
	free(ap->path);
	status = pthread_mutex_destroy(&ap->state_mutex);
	if (status)
		fatal(status);
	status = pthread_mutex_destroy(&ap->mounts_mutex);
	if (status)
		fatal(status);

	free(ap);
}

struct map_source *
master_add_map_source(struct master_mapent *entry,
		      char *type, char *format, time_t age,
		      int argc, const char **argv)
{
	struct map_source *source;
	char *ntype, *nformat;
	const char **tmpargv, *name = NULL;
	int status;

	source = malloc(sizeof(struct map_source));
	if (!source)
		return NULL;
	memset(source, 0, sizeof(struct map_source));

	if (type) {
		ntype = strdup(type);
		if (!ntype) {
			master_free_map_source(source);
			return NULL;
		}
		source->type = ntype;
	}

	if (format) {
		nformat = strdup(format);
		if (!nformat) {
			master_free_map_source(source);
			return NULL;
		}
		source->format = nformat;
	}

	source->age = age;

	tmpargv = copy_argv(argc, argv);
	if (!tmpargv) {
		master_free_map_source(source);
		return NULL;
	}
	source->argc = argc;
	source->argv = tmpargv;

	/* Can be NULL for "hosts" map */
	if (argv)
		name = argv[0];

	status = pthread_mutex_lock(&master_mutex);
	if (status)
		fatal(status);

	if (!entry->maps) {
		entry->maps = source;
		entry->first = source;
	} else {
		struct map_source *last, *next;

		/* Typically there only a few map sources */

		if (master_find_map_source(entry, type, format, argc, tmpargv)) {
			warn("ignoring duplicate map source %s", name);
			master_free_map_source(source);
			return NULL;
		}

		last = NULL;
		next = entry->maps;
		while (next) {
			last = next;
			next = next->next;
		}
		if (last)
			last->next = source;
		else
			entry->maps = source;
	}

	status = pthread_mutex_unlock(&master_mutex);
	if (status)
		fatal(status);

	if (source->argv)
		debug("add %s %s %s",
			source->type, source->format, source->argv[0]);
	else
		debug("add %s %s",
			source->type, source->format);

	return source;
}

static int compare_source_type_and_format(struct map_source *map, const char *type, const char *format)
{
	int res = 0;

	if (type) {
		if (!map->type)
			goto done;

		if (strcmp(map->type, type))
			goto done;
	} else if (map->type)
		goto done;

	if (format) {
		if (!map->format)
			goto done;

		if (strcmp(map->format, format))
			goto done;
	} else if (map->format)
		goto done;

	res = 1;
done:
	return res;
}

struct map_source *master_find_map_source(struct master_mapent *entry,
				const char *type, const char *format,
				int argc, const char **argv)
{
	struct map_source *map;
	struct map_source *source = NULL;
	int status, res;

	status = pthread_mutex_lock(&master_mutex);
	if (status)
		fatal(status);

	map = entry->first;
	while (map) {
		res = compare_source_type_and_format(map, type, format);
		if (!res)
			goto next;

		res = compare_argv(map->argc, map->argv, argc, argv);
		if (!res)
			goto next;

		source = map;
		break;
next:
		map = map->next;
	}

	status = pthread_mutex_unlock(&master_mutex);
	if (status)
		fatal(status);

	return source;
}

void master_free_map_source(struct map_source *source)
{
	int status;

	if (source->argv[0])
		debug("free %s %s %s",
			source->type, source->format, source->argv[0]);
	else
		debug("free %s %s",
			source->type, source->format);

	if (source->type)
		free(source->type);
	if (source->format)
		free(source->format);
	if (source->lookup)
		close_lookup(source->lookup);
	if (source->argv)
		free_argv(source->argc, source->argv);
	if (source->instance) {
		struct map_source *instance, *next;

		status = pthread_mutex_lock(&instance_mutex);
		if (status)
			fatal(status);

		debug("free source instances");

		instance = source->instance;
		while (instance) {
			next = instance->next;
			master_free_map_source(instance);
			instance = next;
		}

		status = pthread_mutex_unlock(&instance_mutex);
		if (status)
			fatal(status);
	}

	free(source);

	return;
}

struct map_source *master_find_source_instance(struct map_source *source, const char *type, const char *format, int argc, const char **argv)
{
	struct map_source *map;
	struct map_source *instance = NULL;;
	int status, res;

	status = pthread_mutex_lock(&instance_mutex);
	if (status)
		fatal(status);

	map = source->instance;
	while (map) {
		res = compare_source_type_and_format(map, type, format);
		if (!res)
			goto next;

		if (!argv) {
			instance = map;
			break;
		}

		res = compare_argv(map->argc, map->argv, argc, argv);
		if (!res)
			goto next;

		instance = map;
		break;
next:
		map = map->next;
	}

	status = pthread_mutex_unlock(&instance_mutex);
	if (status)
		fatal(status);

	return instance;
}

struct map_source *
master_add_source_instance(struct map_source *source, const char *type, const char *format, time_t age)
{
	struct map_source *instance;
	struct map_source *new;
	char *ntype, *nformat;
	const char **tmpargv, *name;
	int status;

	if (!type)
		return NULL;

	instance = master_find_source_instance(source,
			type, format, source->argc, source->argv);
	if (instance)
		return instance;

	new = malloc(sizeof(struct map_source));
	if (!new)
		return NULL;
	memset(new, 0, sizeof(struct map_source));

	ntype = strdup(type);
	if (!ntype) {
		master_free_map_source(new);
		return NULL;
	}
	new->type = ntype;

	if (format) {
		nformat = strdup(format);
		if (!nformat) {
			master_free_map_source(new);
			return NULL;
		}
		new->format = nformat;
	}

	new->age = age;

	tmpargv = copy_argv(source->argc, source->argv);
	if (!tmpargv) {
		master_free_map_source(new);
		return NULL;
	}
	new->argc = source->argc;
	new->argv = tmpargv;

	name = new->argv[0];

	status = pthread_mutex_lock(&instance_mutex);
	if (status)
		fatal(status);

	if (!source->instance)
		source->instance = new;
	else {
		/*
		 * We know there's no other instance of this
		 * type so just add to head of list
		 */
		new->next = source->instance;
		source->instance = new;
	}

	status = pthread_mutex_unlock(&instance_mutex);
	if (status)
		fatal(status);

	debug("add source instance %s %s %s",
			new->type, new->format, new->argv[0]);

	return new;
}

struct master_mapent *master_find_mapent(struct master *master, const char *path)
{
	struct list_head *head, *p;
	int status;

	status = pthread_mutex_lock(&master_mutex);
	if (status)
		fatal(status);

	head = &master->mounts;
	list_for_each(p, head) {
		struct master_mapent *entry;

		entry = list_entry(p, struct master_mapent, list);

		if (!strcmp(entry->path, path)) {
			status = pthread_mutex_unlock(&master_mutex);
			if (status)
				fatal(status);
			return entry;
		}
	}

	status = pthread_mutex_unlock(&master_mutex);
	if (status)
		fatal(status);

	return NULL;
}

struct master_mapent *master_new_mapent(const char *path, time_t age)
{
	struct master_mapent *entry;
	char *tmp;

	entry = malloc(sizeof(struct master_mapent));
	if (!entry)
		return NULL;

	memset(entry, 0, sizeof(struct master_mapent));

	tmp = strdup(path);
	if (!tmp) {
		free(entry);
		return NULL;
	}
	entry->path = tmp;

	entry->thid = 0;
	entry->age = age;
	entry->first = NULL;
	entry->maps = NULL;
	entry->ap = NULL;

	INIT_LIST_HEAD(&entry->list);

	debug("new %s", path);

	return entry;
}

void master_add_mapent(struct master *master, struct master_mapent *entry)
{
	int status;

	status = pthread_mutex_lock(&master_mutex);
	if (status)
		fatal(status);

	list_add_tail(&entry->list, &master->mounts);

	status = pthread_mutex_unlock(&master_mutex);
	if (status)
		fatal(status);

	debug("add %s", entry->path);

	return;
}

void master_free_mapent(struct master_mapent *entry)
{
	int status;

	debug("free %s", entry->path);

	status = pthread_mutex_lock(&master_mutex);
	if (status)
		fatal(status);

	if (!list_empty(&entry->list))
		list_del(&entry->list);

	if (entry->path)
		free(entry->path);

	if (entry->maps) {
		struct map_source *m, *n;

		m = entry->maps;
		while (m) {
			n = m->next;
			master_free_map_source(m);
			m = n;
		}
		entry->maps = NULL;
	}

	status = pthread_mutex_unlock(&master_mutex);
	if (status)
		fatal(status);

	master_free_autofs_point(entry->ap);

	free(entry);

	return;
}

struct master *master_new(const char *name)
{
	struct master *master;
	char *tmp;

	master = malloc(sizeof(struct master_mapent));
	if (!master)
		return NULL;

	if (!name)
		tmp = strdup(default_master_map);
	else
		tmp = strdup(name);

	if (!tmp) {
		free(tmp);
		return NULL;
	}
	master->name = tmp;

	master->default_ghost = default_ghost_mode;
	master->default_logging = DEFAULT_LOGGING;
	master->default_timeout = DEFAULT_TIMEOUT;

	INIT_LIST_HEAD(&master->mounts);

	return master;
}

int master_read_master(struct master *master, time_t age, int readall)
{
	int status;

	if (!lookup_nss_read_master(master, age)) {
		error("can't read master map %s", master->name);
		return 0;
	}

	master_mount_mounts(master, age, readall);

	status = pthread_mutex_unlock(&master_mutex);
	if (status)
		fatal(status);

	if (list_empty(&master->mounts)) {
		error("no mounts in table");
		status = pthread_mutex_unlock(&master_mutex);
		if (status)
			fatal(status);
		return 0;
	}

	status = pthread_mutex_unlock(&master_mutex);
	if (status)
		fatal(status);

	return 1;
}

static void notify_submounts(struct autofs_point *ap, enum states state)
{
	struct list_head *head;
	struct list_head *p;
	struct autofs_point *this;
	int status;

	status = pthread_mutex_lock(&ap->mounts_mutex);
	if (status)
		fatal(status);

	head = &ap->submounts;
	p = head->next;
	while (p != head) {
		unsigned int empty;

		this = list_entry(p, struct autofs_point, mounts);
		p = p->next;

		empty = list_empty(&this->submounts);

		pthread_mutex_unlock(&ap->mounts_mutex);

		status = pthread_mutex_lock(&this->state_mutex);
		if (status)
			fatal(status);

		if (!empty)
			notify_submounts(this, state);

		nextstate(this->state_pipe[1], state);

		status = pthread_mutex_unlock(&this->state_mutex);
		if (status)
			fatal(status);

		pthread_mutex_lock(&ap->mounts_mutex);
	}

	status = pthread_mutex_unlock(&ap->mounts_mutex);
	if (status)
		fatal(status);

	return;
}

void master_notify_state_change(struct master *master, int sig)
{
	struct master_mapent *entry;
	struct autofs_point *ap;
	struct list_head *p;
	enum states next = ST_INVAL;
	int state_pipe;
	int status;

	status = pthread_mutex_lock(&master_mutex);
	if (status)
		fatal(status);

	list_for_each(p, &master->mounts) {
		entry = list_entry(p, struct master_mapent, list);

		ap = entry->ap;

		if (ap->state == ST_INVAL)
			return;

		status = pthread_mutex_lock(&ap->state_mutex);

		state_pipe = ap->state_pipe[1];

		switch (sig) {
		case SIGTERM:
			if (ap->state != ST_SHUTDOWN) {
				next = ST_SHUTDOWN_PENDING;
				notify_submounts(ap, next);
				nextstate(state_pipe, next);
			}
			break;

		case SIGUSR2:
			if (ap->state != ST_SHUTDOWN) {
				next = ST_SHUTDOWN_FORCE;
				notify_submounts(ap, next);
				nextstate(state_pipe, next);
			}
			break;

		case SIGUSR1:
			assert(ap->state == ST_READY);
			next = ST_PRUNE;
			notify_submounts(ap, next);
			nextstate(state_pipe, next);
			break;
		}

		debug("sig %d switching %s from %d to %d",
			sig, ap->path, ap->state, next);

		status = pthread_mutex_unlock(&ap->state_mutex);
		if (status)
			fatal(status);
	}

	status = pthread_mutex_unlock(&master_mutex);
	if (status)
		fatal(status);

	return;
}

static void master_do_mount(struct master_mapent *entry)
{
	struct autofs_point *ap;
	pthread_t thid;
	int status;

	debug("mounting %s", entry->path);

	status = pthread_mutex_lock(&sc.mutex);
	if (status)
		fatal(status);

	sc.done = 0;
	sc.status = 0;

	ap = entry->ap;
	if (pthread_create(&thid, &detach_attr, handle_mounts, ap)) {
		crit("failed to create mount handler thread for %s",
				entry->path);
		status = pthread_mutex_unlock(&sc.mutex);
		if (status)
			fatal(status);
	}
	entry->thid = thid;

	while (!sc.done) {
		status = pthread_cond_wait(&sc.cond, &sc.mutex);
		if (status)
			fatal(status);
	}

	if (sc.status) {
		error("failed to startup mount %s", entry->path);
		status = pthread_mutex_unlock(&sc.mutex);
		if (status)
			fatal(status);
	}

	status = pthread_mutex_unlock(&sc.mutex);
	if (status)
		fatal(status);

	return;
}

static void shutdown_entry(struct master_mapent *entry)
{
	int status, state_pipe;
	struct autofs_point *ap;
	struct stat st;
	int ret;

	ap = entry->ap;

	debug("shutting down %s", entry->path);

	status = pthread_mutex_lock(&ap->state_mutex);
	if (status)
		fatal(status);

	state_pipe = ap->state_pipe[1];

	ret = fstat(state_pipe, &st);
	if (ret == -1)
		goto next;

	notify_submounts(ap, ST_SHUTDOWN_PENDING);
	nextstate(state_pipe, ST_SHUTDOWN_PENDING);
next:
	status = pthread_mutex_unlock(&ap->state_mutex);
	if (status)
		fatal(status);

	return;
}

static void check_update_map_sources(struct master_mapent *entry, time_t age, int readall)
{
	struct map_source *source, *last;
	int status, state_pipe, map_stale = 0;
	struct autofs_point *ap;
	struct stat st;
	int ret;

	if (readall)
		map_stale = 1;

	ap = entry->ap;

	source = entry->maps;
	last = NULL;
	while (source) {
		/* Map source has gone away */
		if (source->age < age) {
			master_free_map_source(source);
			map_stale = 1;
		} else if (source->type) {
			if (!strcmp(source->type, "null")) {
				entry->ap->mc = cache_init(entry->ap);
				entry->first = source->next;
				map_stale = 1;
			}
		}
		source = source->next;
	}

	/* The map sources have changed */
	if (map_stale) {

		status = pthread_mutex_lock(&ap->state_mutex);
		if (status)
			fatal(status);

		state_pipe = entry->ap->state_pipe[1];

		ret = fstat(state_pipe, &st);
		if (ret != -1)
			nextstate(state_pipe, ST_READMAP);

		status = pthread_mutex_unlock(&ap->state_mutex);
		if (status)
			fatal(status);

	}

	return;
}

int master_mount_mounts(struct master *master, time_t age, int readall)
{
	struct list_head *p;
	int status;

	status = pthread_mutex_lock(&master_mutex);
	if (status)
		fatal(status);

	list_for_each(p, &master->mounts) {
		struct master_mapent *this;
		struct autofs_point *ap;
		struct stat st;
		int state_pipe, save_errno;
		int ret;

		this = list_entry(p, struct master_mapent, list);

		ap = this->ap;

		/* A master map entry has gone away */
		if (this->age < age) {
			shutdown_entry(this);
			continue;
		}

		check_update_map_sources(this, age, readall);

		status = pthread_mutex_lock(&ap->state_mutex);
		if (status)
			fatal(status);

		state_pipe = this->ap->state_pipe[1];

		/* No pipe so mount is needed */
		ret = fstat(state_pipe, &st);
		save_errno = errno;

		status = pthread_mutex_unlock(&ap->state_mutex);
		if (status)
			fatal(status);

		if (ret == -1 && save_errno == EBADF)
			master_do_mount(this);
	}

	status = pthread_mutex_unlock(&master_mutex);
	if (status)
		fatal(status);

	return 1;
}

int master_list_empty(struct master *master)
{
	int status;
	int res = 0;

	status = pthread_mutex_lock(&master_mutex);
	if (status)
		fatal(status);

	if (list_empty(&master->mounts))
		res = 1;

	status = pthread_mutex_unlock(&master_mutex);
	if (status)
		fatal(status);

	return res;
}

int master_kill(struct master *master, unsigned int mode)
{
	if (!list_empty(&master->mounts))
		return 0;

	if (master->name)
		free(master->name);

	free(master);

	return 1;
}

