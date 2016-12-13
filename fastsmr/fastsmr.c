/*
Copyright 2004-2006 Joseph W. Seigh 

Permission to use, copy, modify and distribute this software
and its documentation for any purpose and without fee is
hereby granted, provided that the above copyright notice
appear in all copies, that both the copyright notice and this
permission notice appear in supporting documentation.  I make
no representations about the suitability of this software for
any purpose. It is provided "as is" without express or implied
warranty.

---
*/

//------------------------------------------------------------------------------
// userrcu.c -- RCU (Read Copy Update) for preemptive user threads.
//
// version -- 0.0.1 (pre-alpha)
//
//
//------------------------------------------------------------------------------

#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sched.h>
#include <errno.h>
#include <time.h>

#include <userrcu.h>
#include <rcustats.h>
#include <fastsmr.h>
#include <atomix.h>

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
pthread_mutex_t rcu_poll_mutex = PTHREAD_MUTEX_INITIALIZER;

pthread_mutex_t rcu_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t	rcu_cvar = PTHREAD_COND_INITIALIZER;

rcu_stats_t	stats;					// rcu statistics

fifo_t		ready_queue = FIFO_INITIALIZER;	// ready work
utime_t		rcu_minWait = 50000;	// minimum time to wait	(usec)


//=============================================================


pthread_t	rcu_poll_id;			// rcu polling thread
int			deferred_work = 0;

qcount_t	qcobj;					// qcount object

int			rcu_stop = 0;			// shutdown flag 0|1


//=============================================================



void *rcu_poll(void *z);			// forward declare


//------------------------------------------------------------------------------
// rcu_startpoll -- start polling thread
//
//------------------------------------------------------------------------------
pthread_t rcu_startpoll() {
	pthread_attr_t	attr;
	int		rc;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
	pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM);

	rc = pthread_create(&rcu_poll_id, &attr, &rcu_poll, NULL);

	return rcu_poll_id;
}


//------------------------------------------------------------------------------
// rcu_startup -- start rcu
//
//------------------------------------------------------------------------------
pthread_t rcu_startup() {

	qcount_init(&qcobj);
	smr_startup();
	memset(&stats, 0, sizeof(stats));
	return rcu_startpoll();
}


//------------------------------------------------------------------------------
// rcu_shutdown -- shutdown rcu
//
//------------------------------------------------------------------------------
void rcu_shutdown() {

	if (smr_check() != 0) {
		abort();
	}

	pthread_mutex_lockx(&rcu_mutex);
	rcu_stop = 1;
	pthread_cond_broadcast(&rcu_cvar);
	pthread_mutex_unlockx(&rcu_mutex);

	pthread_join(rcu_poll_id, NULL);

	// deallocate RCU nodes if necessary
	rcu_shutdown2();

	// cleanup here
	qcount_destroy(&qcobj);

	return;
}


//-----------------------------------------------------------------------------
// process_work --
//-----------------------------------------------------------------------------
void process_work() {
	rcu_defer_t	*work;
	rcu_defer_t	*workqueue;
	int			workcount;			// count of work performed

	while ((workqueue = fifo_dequeueall(&ready_queue)) != NULL) {

		pthread_mutex_unlockx(&rcu_mutex);

		workcount = 0;
		while ((work = workqueue) != NULL) {
			workcount++;
			workqueue = work->next;		// dequeue
			work->func(work->arg);
		}

		pthread_mutex_lockx(&rcu_mutex);
		stats.undefers += workcount;
		deferred_work -= workcount;
	}

	return;
}


//-----------------------------------------------------------------------------
// rcu_xxxx --
//-----------------------------------------------------------------------------
int rcu_xxxx() {

	// poll threads for quiesce points
	//
	rcu_scan();

	// check for any work on smr_queue not
	// in any thread's hazard ptr
	//
	smr_scan();

	// process ready deferred work
	//
	if (ready_queue.tail != NULL) {
		process_work();
		return 1;
	}

	else
		return 0;


}


//-----------------------------------------------------------------------------
// rcu_poll	-- deferred work polling routine
//             invoked by pthread_create from rcu_init
//
//-----------------------------------------------------------------------------
void *rcu_poll(void *z) {
	utime_t	now, next;
	struct	timespec	nexttime;


	pthread_mutex_lockx(&rcu_mutex);

	for (;;) {

		if (rcu_xxxx())
			;

		else if (deferred_work > 0) {
			//
			// no quiesce point, wait a while
			//

			now = getutimeofday();
			next = now + rcu_minWait;
			nexttime.tv_sec = utime_sec(next);
			nexttime.tv_nsec = utime_nsec(next);
			pthread_cond_timedwait(&rcu_cvar, &rcu_mutex, &nexttime);

			stats.qwaits++;
			stats.qtime += (getutimeofday() - now);

		} // if  (deferred_work > 0)

		else if (rcu_stop != 0) {
			break;
		}

		//
		// wait for work
		//
		else {
			now = getutimeofday();
			pthread_cond_wait(&rcu_cvar, &rcu_mutex);

			stats.wwaits++;
			stats.wtime += (getutimeofday() - now);
		}


	} // for (;;)

printf("polling thread shutting down...\n");

	if (ready_queue.tail != NULL || deferred_work != 0)
		abort();

	pthread_mutex_unlockx(&rcu_mutex);

printf("polling thread returning...\n");

	return NULL;
}


//------------------------------------------------------------------------------
// smr_defer -- 
//
//------------------------------------------------------------------------------
int smr_defer(rcu_defer_t *work) {
	int			n;

	pthread_mutex_lockx(&rcu_mutex);
	stats.defers++;

	work->sequence = current - 1;

	rcu_enqueue(work, pass1);

	if ((n = deferred_work++) == 0)
		stats.defersigs++;


	pthread_mutex_unlockx(&rcu_mutex);

	if (n == 0)			 // polling thread waiting for work
		pthread_cond_signal(&rcu_cvar);

	return 0;
}


//------------------------------------------------------------------------------
// rcu_check --
//------------------------------------------------------------------------------
void rcu_check() {
	if (deferred_work > 0 && pthread_mutex_trylock(&rcu_mutex) == 0) {
		rcu_xxxx();
		pthread_mutex_unlockx(&rcu_mutex);
	}
}


//------------------------------------------------------------------------------
// rcu_signal --
//------------------------------------------------------------------------------
void rcu_signal() {
	pthread_cond_broadcast(&rcu_cvar);
}


//------------------------------------------------------------------------------
// (set|get)MinWait in milliseconds
//------------------------------------------------------------------------------
void rcu_setMinWait(int val) {
	rcu_minWait = mtime_utime(val);
}

int rcu_getMinWait() {
	return utime_mtime(rcu_minWait);
}


//------------------------------------------------------------------------------
// copyStats --
//------------------------------------------------------------------------------
void copyStats(rcu_stats_t * target) {
	pthread_mutex_lockx(&rcu_mutex);
	memcpy(target, &stats, sizeof(stats));
	target->deferred_work = deferred_work;
	pthread_mutex_unlockx(&rcu_mutex);
}


/*-*/
