/*
Copyright 2002-2005 Joseph W. Seigh 

Permission to use, copy, modify and distribute this software
and its documentation for any purpose and without fee is
hereby granted, provided that the above copyright notice
appear in all copies, that both the copyright notice and this
permission notice appear in supporting documentation.  I make
no representations about the suitability of this software for
any purpose. It is provided "as is" without express or implied
warranty.

---

//------------------------------------------------------------------------------
// utime.h -- miscellaneous 64 bit time (in microseconds) routines
//
// version -- 0.0.0 (pre-alpha)
//
//
//
//------------------------------------------------------------------------------

*/
#ifndef _UTIME_H
#define _UTIME_H

#include <time.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MICROSEC 1000000

#define utime_sec(t)  ((int)((t)/MICROSEC))
#define utime_usec(t) ((int)((t)%MICROSEC))
#define utime_nsec(t) ((int)(((t)%MICROSEC)*1000))
#define time_utime(t) ((utime_t)((t)*MICROSEC))
#define mtime_utime(t) ((utime_t)((t)*1000))
#define utime_mtime(t) ((int)((t)/1000))
#define utime_float(t) ((float)utime_sec(t) + (float)utime_usec(t)/1000000.0)

#define timeval_utime(p) ((((utime_t)(p).tv_sec * MICROSEC) + (utime_t)(p).tv_usec))
#define timespec_utime(p) ((((utime_t)(p).tv_sec * MICROSEC) + (utime_t)((p).tv_nsec)/1000))
#define timestruc_utime(p) ((((utime_t)(p).tv_sec * MICROSEC) + (utime_t)((p).tv_nsec)/1000))
	
typedef unsigned long long  utime_t;        // time_t in usecs (microseconds)

//-----------------------------------------------------------------------------
// getutimeofday -- get current time in microseconds
//-----------------------------------------------------------------------------
static inline utime_t getutimeofday() {
	struct	timeval x;
	gettimeofday(&x, NULL);
	return timeval_utime(x);
}

#ifdef __cplusplus
}
#endif

#endif // _UTIME_H
