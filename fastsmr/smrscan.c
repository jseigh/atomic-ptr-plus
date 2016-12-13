/*
Copyright 2005, 2006 Joseph W. Seigh 

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
#include <string.h>
#include <errno.h>
#include <time.h>

#include <userrcu.h>
#include <fastsmr.h>
#include <atomix.h>
#include <rcustats.h>

#define containerof(ptr, type, member) \
	((type *)(((char *)ptr) - (int)&(((type *)0)->member)))


//-----------------------------------------------------------------------------
// SMR node (thread)
//-----------------------------------------------------------------------------
typedef struct smr_node_tt {
	union {
		struct {
			smr_t	hptr[2];		// hazard pointers
		};

		char	cache[128];			// nominal cache size
	};

	//------------------------------
	// -- align to new cache line --
	//------------------------------

	struct smr_node_tt *next;
	struct smr_node_tt *prev;

	qhandle_t		qhandle;		// qcount query handle
	unsigned int	ndx;			// hptr index
	unsigned int	hcount;			// number of hazard pointers

	// debugging info
	pthread_t		tid;			// pthread id for thread

} smr_node_t;


//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------

pthread_key_t	smr_key;
smr_node_t		*smr_node_queue = NULL;	// SMR node list
fifo_t			smr_queue = FIFO_INITIALIZER;	// SMR work queue;
sequence_t		current = 0;			// current sequence number
int				smr_count = 0;			// count of deferred work

smr_t			*hptr = NULL;			// copied hazard pointer list
unsigned int	hsize = 0;				// size of list
unsigned int	hcount = 0;				// count of ptr's in list


//------------------------------------------------------------------------------
// smr_tracecb --
//------------------------------------------------------------------------------
int smr_tracecb(rcu_defer_t *defer) {
	if (defer->state != live) {
		defer->sequence = current;		// reachable
		return 1;
	}

	else
		return 0;
}


//------------------------------------------------------------------------------
// smr_enqueue --
//
//------------------------------------------------------------------------------
void smr_enqueue(rcu_defer_t *work) {
	if (smr_node_queue != NULL) {
		work->state = smr;
		fifo_enqueue(&smr_queue, work);
		smr_count++;
	}

	else {
		rcu_enqueue(work, pass2);
	}

	return;
}


//------------------------------------------------------------------------------
// smr_register -- register thread and initialize local data
//
//	note: quiesce point if node created
//
//------------------------------------------------------------------------------
smr_t * smr_acquire() {
	smr_node_t	*node;
	smr_t		*hptr;


	if ((node = (smr_node_t *)pthread_getspecific(smr_key)) == NULL) {

		// initialize TSD (per thread)
		if ((node = (smr_node_t *)malloc(sizeof(smr_node_t))) == NULL)
			return NULL;

		memset(node, 0, sizeof(smr_node_t));
		node->ndx = 0;
		node->hcount = 2;
		node->hptr[0] = NULL;
		node->hptr[1] = NULL;

		// debugging info
		node->tid = pthread_self();

		pthread_mutex_lockx(&rcu_mutex);

		// add RCU node if RCU thread polling in effect
		if (qcount_self(&(node->qhandle))) {
			qcount_set(qcobj);
			rcu_add_node(node->qhandle);
		}

		if (pthread_setspecific(smr_key, (void *)node) != 0)
			abort();

		// push onto smr node queue
		//

		node->next = smr_node_queue;
		node->prev = NULL;
		if (smr_node_queue != NULL)
			smr_node_queue->prev = node;
		smr_node_queue = node;

		pthread_mutex_unlockx(&rcu_mutex);

	}

	// return next available pair hazard pointer in array

	if (node->ndx < node->hcount) {
		hptr = &(node->hptr[node->ndx]);
		node->ndx += 2;
	}

	else
		abort();

	return hptr;

}


//------------------------------------------------------------------------------
// smr_release -- release thread smr node on thread exit
//
//------------------------------------------------------------------------------
void smr_release(void *tsd) {
	smr_node_t	*node = (smr_node_t *)tsd;
	rcu_defer_t * workqueue;
	rcu_defer_t * work;

	if (node == NULL) {
		abort();
		return;
	}

	pthread_mutex_lockx(&rcu_mutex);

	if (node->next != NULL)
		node->next->prev = node->prev;

	if (node->prev != NULL)
		node->prev->next = node->next;
	else
		smr_node_queue = node->next;


	if (smr_node_queue == NULL) {
		workqueue = fifo_dequeueall(&smr_queue);
		while((work = workqueue) != NULL) {
			workqueue = work->next;			// dequeue
			rcu_enqueue(work, pass2);
			smr_count--;
		}
		//pthread_cond_signal(&rcu_cvar);		// ??
	}

	rcu_delete_node(node->qhandle);

	free(node);

	pthread_mutex_unlockx(&rcu_mutex);
	pthread_cond_broadcast(&rcu_cvar);		// ??

	return;
}


//-----------------------------------------------------------------------------
// smr_alloc --
//-----------------------------------------------------------------------------
smr_t *smr_alloc() {
	smr_node_t	*node;
	smr_t		*hptr;

	if ((node = (smr_node_t *)pthread_getspecific(smr_key)) == NULL)
		return NULL;

	// return next available hazard pointer in array

	if (node->ndx < node->hcount) {
		hptr = &(node->hptr[2*node->ndx]);
		node->ndx += 2;
		// acquire membar
	}

	else
		abort();

	return hptr;
}


//-----------------------------------------------------------------------------
// smr_dealloc --
//-----------------------------------------------------------------------------
void smr_dealloc(smr_t *hptr) {
	smr_node_t	*node;

	if (hptr == NULL)
		return;

	smrnull(hptr);

	if ((node = (smr_node_t *)pthread_getspecific(smr_key)) == NULL)
		return;

	if (node->ndx == 0)
		return;

	// release membar ?
	node->ndx -= 2;

	if (hptr != &(node->hptr[2*node->ndx]))
		abort();

	return;
}


//-----------------------------------------------------------------------------
// smr_check -- verify no hazard pointers are allocated
//
//   returns 0 if ok
//-----------------------------------------------------------------------------
int smr_check() {
	smr_node_t	*node;

	if ((node = (smr_node_t *)pthread_getspecific(smr_key)) == NULL)
		return 0;

	if (node->ndx == 0)
		return 0;
	else
		return 1;
}


//-----------------------------------------------------------------------------
// smr_startup --
//-----------------------------------------------------------------------------
void smr_startup() {
	// initialize TSD key (may fail if previously set)
	pthread_key_create(&smr_key, &smr_release);

	hptr = (smr_t *)malloc(50 * sizeof(smr_t));
	hsize = 50;	
}


//-----------------------------------------------------------------------------
// smr_shutdown --
//-----------------------------------------------------------------------------
void smr_shutdown() {
	utime_t next;
	struct timespec nexttime;

	while (smr_node_queue != NULL) {
		next = getutimeofday();
		next += 10000;					// 10 msec

		nexttime.tv_sec = utime_sec(next);
		nexttime.tv_nsec = utime_nsec(next);
		pthread_cond_timedwait(&rcu_cvar, &rcu_mutex, &nexttime);
	}

}


//-----------------------------------------------------------------------------
// smr_scan -- scan smr hazard pointers
//
//-----------------------------------------------------------------------------
void smr_scan() {
	smr_node_t	*node;
	rcu_defer_t	*work;
	rcu_defer_t	*workqueue;
	int			ndx;
	int			j;

	if (smr_count == 0) {
		stats.smrempty++;
		return;
	}

	current++;				// increment current sequence number

	//
	// copy hazard pointer pairs
	//

	hcount = 0;

	for ( node = smr_node_queue;
		node != NULL;
		node = node->next)
	{
		ndx = node->ndx;
		// acquire membar
		if ((hsize - hcount) < ndx) {
			hsize = (hsize * 2) + ndx;
			hptr = (smr_t *)realloc(hptr, (hsize * sizeof(smr_t)));
			if (hptr == NULL)
				abort();
		}
		for (j = 0; j < ndx; j += 2) {
			hptr[hcount++] = atomic_load(&(node->hptr[j + 0]));
			rmb();		// load/load memory barrier
			hptr[hcount++] = atomic_load(&(node->hptr[j + 1]));
		}
	}

	workqueue = fifo_dequeueall(&smr_queue);

	//
	// mark all reachable nodes
	//
	for (work = workqueue; work != 0; work = work->next) {

		for (j = 0; j < hcount && hptr[j] != work->arg; j++) {}

		// work still referenced by hazard pointers
		if (j < hcount) {

			switch (work->type) {

			case trace:
				work->sequence = current;
				work->forrefs(work->arg, &smr_tracecb); // trace reachable nodes
				break;
		
			case fifo:
				// old sequence # for first one
				work->sequence = current;
				*(work->psequence) = current;
				break;

			default:
				abort();
				break;
			}


		}

		// work not referenced
		else {
			if (work->type == fifo)
				work->sequence = *(work->psequence);
		}

	}

	//
	// dequeue all unreachable nodes
	//
	while((work = workqueue) != NULL) {
		workqueue = work->next;		// dequeue

		if (work->sequence == current)
			fifo_enqueue(&smr_queue, work);		// requeue
		else
			rcu_enqueue(work, pass2);			// dequeue
	}


	// update stats

	if (smr_count != 0)
		stats.smrpartial++;

	else
		stats.smrfull++;
			
	return;
}



//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------


/*-*/
