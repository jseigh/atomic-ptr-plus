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
// atomix.h -- basic atomic primatives and memory barriers
//
// version -- 0.0.1 (pre-alpha)
//
//
//
//
//
//------------------------------------------------------------------------------

#ifndef __ATOMIX_H
#define __ATOMIX_H

#ifdef __cplusplus
extern "C" {
#endif




//------------------------------------------------------------------------------
// atomic_cas32 --
// returns:
//   rc = 0, fail - xcmp updated
//   rc = 1, success - dest updated
//------------------------------------------------------------------------------
__attribute__((always_inline))
static __inline__ int _atomic_cas32(uint32_t *dest, uint32_t *xcmp, uint32_t *xxchg)
{
	int rc;

	__asm__ __volatile__ (
		"lock cmpxchg  %3, %1        ;\n"
		"jz  	1f                   ;\n"

		"mov	%%eax, %2            ;\n"	// comparand
"1:		 mov	$0, %0               ;\n"
		"setz 	%b0                  ;\n"
		: "=&a"	(rc)
		: "m" (*dest), "m" (*xcmp), "r"	(*xxchg), "0" (*xcmp)
		: "cc", "memory"
		);

	return rc;
}


//------------------------------------------------------------------------------
// atomic_cas64 --
//
// returns:
//   rc = 0, fail - xcmp updated
//   rc = 1, success - dest updated
//------------------------------------------------------------------------------
__attribute__((always_inline))
static __inline__ int _atomic_cas64(uint64_t * dest, uint64_t * xcmp, uint64_t * xxchg) {
	int rc;

	__asm__ __volatile__ (
		"mov	%3, %%esi            ;\n"	// exchange
		"mov	0(%%esi), %%ebx      ;\n"	// exchange low
		"mov	4(%%esi), %%ecx      ;\n"	// exchange high

		"mov	%2, %%esi            ;\n"	// comparand
		"mov	0(%%esi), %%eax      ;\n"	// comparand low
		"mov	4(%%esi), %%edx      ;\n"	// comparand high
		
		"mov	%1, %%esi            ;\n"	// destination
		"lock cmpxchg8b (%%esi)      ;\n"
		"jz  	1f                   ;\n"
		"mov	%2, %%esi            ;\n"	// comparand
		"mov	%%eax, 0(%%esi)      ;\n"	// comparand low
		"mov	%%edx, 4(%%esi)      ;\n"	// comparand high
"1:	     mov	$0, %0               ;\n"	
		"setz	%b0                  ;\n"	// rc = 
		: "=&a" (rc)
		: "m" (dest), "m" (xcmp), "m" (xxchg)
		: "cc", "memory", "edx", "ebx", "ecx", "esi"
		);

	return rc;
}

//------------------------------------------------------------------------------
// atomic_cas64 --
//
// returns:
//   rc = 0, fail - xcmp updated
//   rc = 1, success - dest updated
//------------------------------------------------------------------------------
#define __atomic_compare_exchange_16(p, o, n, m, ms, mf) __atomic_compare_exchange_16x(p, o, n)
__attribute__((always_inline))
static __inline__ int __atomic_compare_exchange_16x(__int128 * dest, __int128 * xcmp, __int128 xxchg) {
	int rc;

	__asm__ __volatile__ (
		"lea	%3, %%rsi            ;\n"	// address of exchange
		"mov	0(%%rsi), %%rbx      ;\n"	// exchange low
		"mov	4(%%rsi), %%rcx      ;\n"	// exchange high

		"mov	%2, %%rsi            ;\n"	// comparand
		"mov	0(%%rsi), %%rax      ;\n"	// comparand low
		"mov	4(%%rsi), %%rdx      ;\n"	// comparand high
		
		"mov	%1, %%rsi            ;\n"	// destination
		"lock cmpxchg16b (%%rsi)     ;\n"
		"jz  	1f                   ;\n"
		"mov	%2, %%rsi            ;\n"	// comparand
		"mov	%%rax, 0(%%rsi)      ;\n"	// comparand low
		"mov	%%rdx, 4(%%rsi)      ;\n"	// comparand high
"1:	     mov	$0, %0               ;\n"	
		"setz	%b0                  ;\n"	// rc = 
		: "=&a" (rc)
		: "m" (dest), "m" (xcmp), "m" (xxchg)
		: "cc", "memory", "rdx", "rbx", "rcx", "rsi"
		);

	return rc;
}



#ifdef __cplusplus
}
#endif

#endif // __ATOMIX_H
/*-*/
