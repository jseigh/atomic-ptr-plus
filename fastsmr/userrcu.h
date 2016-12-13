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
// userrcu.h -- RCU (Read Copy Update) for preemptive user threads.
//
// version -- 0.0.1 (pre-alpha)
//
// Defined RCU quiesce points (quiescent states)
//
//------------------------------------------------------------------------------


#ifndef USERRCU_H
#define USERRCU_H

#ifdef __cplusplus
extern "C" {
#endif

#include <pthread.h>

//#include <atomix.h>
#include <qcount.h>
#include <fastsmr.h>
#include <rcustats.h>


//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
static inline void pthread_mutex_lockx(pthread_mutex_t *mutex) {
	if (pthread_mutex_lock(mutex) != 0) {
		abort();
	}
}

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
static inline void pthread_mutex_unlockx(pthread_mutex_t *mutex) {
	if (pthread_mutex_unlock(mutex) != 0) {
		abort();
	}
}


//------------------------------------------------------------------------------
// Fifo defer queue
//   tail -> x -> x -> x -> 0
//                     ^head
//------------------------------------------------------------------------------
typedef struct {
	rcu_defer_t	*head;
	rcu_defer_t	*tail;
} fifo_t;

#define FIFO_INITIALIZER {NULL, NULL}
extern void fifo_init(fifo_t *q);
extern void fifo_enqueue(fifo_t *q, rcu_defer_t *item);
extern rcu_defer_t *fifo_dequeue(fifo_t *q);
extern rcu_defer_t *fifo_dequeueall(fifo_t *q);
extern void fifo_requeue(fifo_t *dst, fifo_t *src);


//------------------------------------------------------------------------------
extern pthread_mutex_t	rcu_mutex;
extern pthread_cond_t	rcu_cvar;
extern pthread_key_t	rcu_restart_key;		// TSD key
extern rcu_stats_t		stats;
extern fifo_t			ready_queue;
extern qcount_t			qcobj;					// qcount object
extern sequence_t			current;				// current sequence number
extern int				deferred_work;

//------------------------------------------------------------------------------
extern void rcu_enqueue(rcu_defer_t *, rcu_defer_state_t);
extern void smr_enqueue(rcu_defer_t *);
extern void smr_scan();
extern void rcu_scan();
extern int smr_check();
extern void rcu_shutdown2();
extern int rcu_incoming();

//------------------------------------------------------------------------------
// forrefs callbacks
//------------------------------------------------------------------------------
extern void smr_dropref_cb(rcu_defer_t *);
extern void smr_dropref2_cb(rcu_defer_t *);
extern void smr_chgref_cb(rcu_defer_t *);


//------------------------------------------------------------------------------
extern void rcu_add_node(qhandle_t);



#ifdef __cplusplus
}
#endif

#endif /* USERRCU_H */
