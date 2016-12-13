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
//#include <malloc.h>

#include <userrcu.h>
#include <rcustats.h>
#include <fastsmr.h>
#include <atomix.h>


//-----------------------------------------------------------------------------
// RCU node (thread/processor)
//-----------------------------------------------------------------------------
typedef struct rcu_node_tt {


	struct rcu_node_tt *next;
	struct rcu_node_tt *prev;

	uint32_t	last_qcount;	// qcount  at last checkpoint
	qhandle_t	qhandle;		// qcount query handle

	//
	// deferred work FIFO queues
	//

	fifo_t	queue0;				// deferred work queue 0
	fifo_t	queue1;				// deferred work queue 1

	//
	// debugging info
	//
	utime_t	last_time;			// time of last checkpoint
	enum {
		state_none = 0,
		state_explicit,			// explicit eventcount change
		state_norun,			// not running
		state_idle				// no quiescent state detected
	} state;					// last observed quiescent state

} rcu_node_t;


//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
qcount_t	qcobj;					// qcount object

rcu_node_t	*current_node = NULL;


//-----------------------------------------------------------------------------
// rcu_requeue -- transfer work to smr or ready queue
//
//-----------------------------------------------------------------------------
void rcu_requeue(fifo_t *q) {
	rcu_defer_t	*workqueue;
	rcu_defer_t	*work;

	workqueue = fifo_dequeueall(q);

	while ((work = workqueue) != NULL) {
		workqueue = work->next;

		if (work->state == pass1)
			smr_enqueue(work);

		else
			fifo_enqueue(&ready_queue, work);

	}

}


//------------------------------------------------------------------------------
// rcu_enqueue --
//------------------------------------------------------------------------------
void rcu_enqueue(rcu_defer_t *work, rcu_defer_state_t state) {
	rcu_node_t	*node;
	
	work->state = state;
	node = current_node;
	if (node != NULL)
		fifo_enqueue(&(node->queue0), work);

	else if (work->state == pass1)
		smr_enqueue(work);

	else
		fifo_enqueue(&ready_queue, work);


	return;
}


//------------------------------------------------------------------------------
// rcu_add_node --
//
//------------------------------------------------------------------------------
void rcu_add_node(qhandle_t qhandle) {
	rcu_node_t *node;
	int		runstate;

	//
	if ((node = (rcu_node_t *)malloc(sizeof(rcu_node_t))) == NULL)
		abort();

	memset(node, 0, sizeof(rcu_node_t));

	node->qhandle = qhandle;

	// set initial quiesce count and runstate
	node->last_qcount = qcount_get(qcobj, node->qhandle, &runstate);

	fifo_init(&(node->queue0));
	fifo_init(&(node->queue1));

	// debugging info
	node->last_time = 0;

	//
	// link prior to current (can be anywhere)
	//

	if (current_node == NULL) {
		node->next = node;
		node->prev = node;

		current_node = node;
	}

	else {
		node->next = current_node;
		node->prev = current_node->prev;
		current_node->prev = node;
		node->prev->next = node;
	}

	return;

}


//------------------------------------------------------------------------------
// rcu_delete_node --
//------------------------------------------------------------------------------
void rcu_delete_node(qhandle_t qhandle) {
	rcu_node_t	*node;
	
	if (current_node == NULL)
		return;
	//
	// lookup node by qhandle 
	//
	node = current_node;
	do {
		if (node->qhandle == qhandle)	// pthread_equal?
			break;
		node = node->next;
	}
	while (node != current_node);

	if (node->qhandle != qhandle) {
		return;
	}
		

	if (node->next == node) {		// last node
		current_node = NULL;

		// transfer work to smr_queue or ready_queue
		rcu_requeue(&(node->queue0));
		rcu_requeue(&(node->queue1));

		pthread_cond_broadcast(&rcu_cvar);
	}

	else {
		// bump current node forward if necessary
		// note: bumping current_node backwards would revisit
		// previous node without checkpointing intermediate nodes.
		if (current_node == node)
			current_node = node->next;

		//
		// delink from node list
		//
		node->prev->next = node->next;
		node->next->prev = node->prev;

		// transfer work to previous node
		fifo_requeue(&(node->prev->queue0), &(node->queue0));
		fifo_requeue(&(node->prev->queue1), &(node->queue1));
	}

	if (node->queue0.tail != NULL)
		abort();
	if (node->queue1.tail != NULL)
		abort();

	free(node);


}


//-----------------------------------------------------------------------------
// rcu_scan -- check for quiesce points and process ready work
//
//-----------------------------------------------------------------------------
void rcu_scan() {
	utime_t	now;
	rcu_node_t	*node;
	int		qcount;				// working copy of qcount
	int		runstate;

	//
	// poll threads/processors for quiesce points
	//

	qcount_set(qcobj);

	if((node = current_node) == NULL)
		return;

	do {

		now = getutimeofday();

		//----------------------------------------------------------------------
		// check for quiesce point
		//----------------------------------------------------------------------

		qcount = qcount_get(qcobj, node->qhandle, &runstate);

		// thread/processor not running
		if (!runstate) {
			node->state = state_norun;
			stats.norun++;
		}

		// thread/processor quiesced
		else if (qcount != node->last_qcount) {
			node->state  = state_explicit;
			stats.qexplicit++;
		}

		// no quiesce point
		else {
			node->state  = state_idle;
			stats.idle++;

			break;						// no quiesce point, wait a while
		}

		//----------------------------------------------------------------------
		// quiesce point detected
		//----------------------------------------------------------------------

		node->last_qcount = qcount;		// update last seen eventcount
		node->last_time = now;			// time quiesce point was seen
		stats.qpoints++;				// count of quiesce points overall

		//
		// shift work on deferred work queues
		//
		node = node->next;

		rcu_requeue(&(node->queue1));
		fifo_requeue(&(node->queue1), &(node->queue0));

	}
	while (node != current_node);

	current_node = node;

	return;

}


//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
void rcu_shutdown2() {
	pthread_mutex_lockx(&rcu_mutex);
	while (current_node != NULL) {
		rcu_delete_node(current_node->qhandle);
	}
	pthread_mutex_unlockx(&rcu_mutex);
}


//------------------------------------------------------------------------------
// rcu_incoming -- new work
//------------------------------------------------------------------------------
int rcu_incoming() {
	if (current_node)
		return (current_node->queue0.head != NULL);
	else
		return 0;
}


/*-*/
