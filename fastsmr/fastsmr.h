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
// fastsmr.h -- SMR (Safe Memory Recovery) w/o memory barriers     
//
// version -- 0.0.1 (pre-alpha)
//
//
//------------------------------------------------------------------------------


#ifndef FASTSMR_H
#define FASTSMR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <utime.h>


typedef void * smr_t;				// hazard pointer

//-----------------------------------------------------------------------------
// deferred work
//-----------------------------------------------------------------------------
typedef enum {
	live = 0,						// node live
	pass1,							// rcu, pass 1
	smr,							// smr
	pass2,							// rcu, pass 2
} rcu_defer_state_t;

typedef enum {
	fifo = 1,						// deallocate in fifo order
	trace = 2,						// trace reachable nodes
	//ref = 3,						// refcount nodes
} smr_reftype_t;

typedef unsigned int sequence_t;	// sequence

typedef struct rcu_defer_tt {
	void (*func)(void *);			// defer function
	void *arg;						// defer/trace argument

	// for all refs callback function
	void (*forrefs)(void *, int (*)(struct rcu_defer_tt *));
	//
	sequence_t	sequence;			// trace sequence number
	sequence_t	*psequence;			// fifo sequence number

	//--

	smr_reftype_t	type;			//
	rcu_defer_state_t state;		// 
	struct rcu_defer_tt *	next;	//

} rcu_defer_t;

typedef int (*refcb_t)(rcu_defer_t *);

//=============================================================================
// public
//=============================================================================

extern pthread_t rcu_startup();			// rcu startup
extern void rcu_shutdown();				// rcu shutdown

extern smr_t * smr_acquire();			// acquire thread hazard pointer
extern smr_t * smr_alloc();				// allocate hazard pointer
extern void smr_dealloc(smr_t *);		// deallocate hazard pointer

extern int smr_defer(rcu_defer_t *);	// defer work 

extern void rcu_setMinWait(int);		// set polling interval (msecs)
extern int rcu_getMinWait();			// get polling interval (msecs)

#ifdef __cplusplus
}
#endif

#endif /* FASTSMR_H */
