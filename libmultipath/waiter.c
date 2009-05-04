/*
 * Copyright (c) 2004, 2005 Christophe Varoqui
 * Copyright (c) 2005 Kiyoshi Ueda, NEC
 * Copyright (c) 2005 Benjamin Marzinski, Redhat
 * Copyright (c) 2005 Edward Goggin, EMC
 */
#include <unistd.h>
#include <libdevmapper.h>
#include <sys/mman.h>
#include <pthread.h>
#include <signal.h>

#include "vector.h"
#include "memory.h"
#include "checkers.h"
#include "structs.h"
#include "structs_vec.h"
#include "devmapper.h"
#include "debug.h"
#include "lock.h"
#include "waiter.h"

struct event_thread *alloc_waiter (void)
{

	struct event_thread *wp;

	wp = (struct event_thread *)MALLOC(sizeof(struct event_thread));
	memset(wp, 0, sizeof(struct event_thread));

	return wp;
}

void free_waiter (void *data)
{
	struct event_thread *wp = (struct event_thread *)data;

	if (wp->mpp)
		condlog(3, "%s: waiter not cleared", wp->mapname);
	else
		FREE(wp);
}

void stop_waiter_thread (struct multipath *mpp, struct vectors *vecs)
{
	struct event_thread *wp = (struct event_thread *)mpp->waiter;

	if (!wp) {
		condlog(3, "%s: no waiter thread", mpp->alias);
		return;
	}
	condlog(2, "%s: stop event checker thread", wp->mapname);
	mpp->waiter = NULL;
	wp->mpp = NULL;
	pthread_kill((pthread_t)wp->thread, SIGUSR1);
}

static sigset_t unblock_signals(void)
{
	sigset_t set, old;

	sigemptyset(&set);
	sigaddset(&set, SIGHUP);
	sigaddset(&set, SIGUSR1);
	pthread_sigmask(SIG_UNBLOCK, &set, &old);
	return old;
}

/*
 * returns the reschedule delay
 * negative means *stop*
 */
int waiteventloop (struct event_thread *waiter)
{
	sigset_t set;
	struct dm_task *dmt = NULL;
	int event_nr;
	int r;

	if (!waiter->event_nr)
		waiter->event_nr = dm_geteventnr(waiter->mapname);

	if (!(dmt = dm_task_create(DM_DEVICE_WAITEVENT))) {
		condlog(0, "%s: devmap event #%i dm_task_create error",
				waiter->mapname, waiter->event_nr);
		return 1;
	}

	if (!dm_task_set_name(dmt, waiter->mapname)) {
		condlog(0, "%s: devmap event #%i dm_task_set_name error",
				waiter->mapname, waiter->event_nr);
		dm_task_destroy(dmt);
		return 1;
	}

	if (waiter->event_nr && !dm_task_set_event_nr(dmt,
						      waiter->event_nr)) {
		condlog(0, "%s: devmap event #%i dm_task_set_event_nr error",
				waiter->mapname, waiter->event_nr);
		dm_task_destroy(dmt);
		return 1;
	}

	dm_task_no_open_count(dmt);

	/* accept wait interruption */
	set = unblock_signals();

	/* wait */
	r = dm_task_run(dmt);

	/* wait is over : event or interrupt */
	pthread_sigmask(SIG_SETMASK, &set, NULL);

	dm_task_destroy(dmt);

	if (!r)	/* wait interrupted by signal */
		return -1;

	waiter->event_nr++;

	/*
	 * upon event ...
	 */
	while (1) {
		condlog(3, "%s: devmap event #%i",
				waiter->mapname, waiter->event_nr);

		/*
		 * event might be :
		 *
		 * 1) a table reload, which means our mpp structure is
		 *    obsolete : refresh it through update_multipath()
		 * 2) a path failed by DM : mark as such through
		 *    update_multipath()
		 * 3) map has gone away : stop the thread.
		 * 4) a path reinstate : nothing to do
		 * 5) a switch group : nothing to do
		 */
		pthread_cleanup_push(cleanup_lock, waiter->vecs->lock);
		lock(waiter->vecs->lock);
		r = update_multipath(waiter->vecs, waiter->mapname);
		lock_cleanup_pop(waiter->vecs->lock);

		if (r) {
			condlog(2, "%s: event checker exit",
				waiter->mapname);
			return -1; /* stop the thread */
		}

		event_nr = dm_geteventnr(waiter->mapname);

		if (waiter->event_nr == event_nr)
			return 1; /* upon problem reschedule 1s later */

		waiter->event_nr = event_nr;
	}
	return -1; /* never reach there */
}

void *waitevent (void *et)
{
	int r;
	struct event_thread *waiter;

	mlockall(MCL_CURRENT | MCL_FUTURE);

	waiter = (struct event_thread *)et;
	pthread_cleanup_push(free_waiter, et);

	while (1) {
		r = waiteventloop(waiter);

		if (r < 0)
			break;

		sleep(r);
	}

	pthread_cleanup_pop(1);
	return NULL;
}

int start_waiter_thread (struct multipath *mpp, struct vectors *vecs)
{
	pthread_attr_t attr;
	struct event_thread *wp;
	size_t stacksize;

	if (!mpp)
		return 0;

	if (pthread_attr_init(&attr))
		return 1;

	if (pthread_attr_getstacksize(&attr, &stacksize) != 0)
		stacksize = PTHREAD_STACK_MIN;

	/* Check if the stacksize is large enough */
	if (stacksize < (32 * 1024))
		stacksize = 32 * 1024;

	/* Set stacksize and try to reinitialize attr if failed */
	if (stacksize > PTHREAD_STACK_MIN &&
	    pthread_attr_setstacksize(&attr, stacksize) != 0 &&
	    pthread_attr_init(&attr))
		goto out;

	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	wp = alloc_waiter();

	if (!wp)
		goto out;

	mpp->waiter = (void *)wp;
	strncpy(wp->mapname, mpp->alias, WWID_SIZE);
	wp->vecs = vecs;
	wp->mpp = mpp;

	if (!pthread_create(&wp->thread, &attr, waitevent, wp)) {
		pthread_attr_destroy(&attr);
		condlog(2, "%s: event checker started", wp->mapname);
		return 0;
	}
	condlog(0, "%s: cannot create event checker", wp->mapname);
	free_waiter(wp);
	mpp->waiter = NULL;

out:
	pthread_attr_destroy(&attr);
	condlog(0, "failed to start waiter thread");
	return 1;
}

