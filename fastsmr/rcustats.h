/*
Copyright 2005 Joseph W. Seigh 

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
// usersmr.h -- SMR (Safe Memory Recovery) w/o memory barriers     
//
// version -- 0.0.1 (pre-alpha)
//
//
//------------------------------------------------------------------------------


#ifndef RCUSTATS_H
#define RCUSTATS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <utime.h>

//-----------------------------------------------------------------------------
// stats
//-----------------------------------------------------------------------------
typedef struct {
	int		qpoints;	// quiesce points
	int		qexplicit;	// explicit quiesce points seen
	int		norun;      // not running, suspended, etc..
	int		qwaits;		// waits for quiesce points
	int		wwaits;		// waits for work
	utime_t qtime;		// accumlated quiesce point waiting time
	utime_t wtime;		// accumlated wait for work time
	//
	int		qwakeups;	// quiesce point wait wakeups 
	//
	int		defers;		// number of defers
	int		undefers;	// number of undefers (continues)
	int		defersigs;	// defer instant quiesce wakeups
	int		idle;		// number of idle (non quiesced)

	//
	int		smrempty;	// smr queue empty
	int		smrfull;	// smr queue fully processed
	int		smrpartial;	// smr queue partial processed

	// debugging info
	int		deferred_work;	// copy of current deferred work count;
} rcu_stats_t;


//=============================================================================
// public
//=============================================================================

extern void copyStats(rcu_stats_t *);


// experimental functions
extern void rcu_check();				// check for work
extern void rcu_signal();				// check for work

#ifdef __cplusplus
}
#endif

#endif /* RCUSTATS_H */
