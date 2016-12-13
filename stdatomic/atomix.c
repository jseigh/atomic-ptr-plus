#include <stdint.h>

#if __SIZEOF_LONG__ == 8	
__int128 __atomic_load_16(__int128 *src, int memory_order) {
	return *src;
}


//------------------------------------------------------------------------------
// __atomic_compare_exchange_16 --
//
// returns:
//   rc = 0, fail - xcmp updated
//   rc = 1, success - dest updated
//------------------------------------------------------------------------------
int __atomic_compare_exchange_16(__int128 * dest, __int128 * xcmp, __int128 xxchg, int m, int ms, int mf) {
	int rc;

	__asm__ __volatile__ (
		"lea	%3, %%rsi            ;\n"	// address of exchange
		"mov	0(%%rsi), %%rbx      ;\n"	// exchange low
		"mov	8(%%rsi), %%rcx      ;\n"	// exchange high

		"mov	%2, %%rsi            ;\n"	// comparand
		"mov	0(%%rsi), %%rax      ;\n"	// comparand low
		"mov	8(%%rsi), %%rdx      ;\n"	// comparand high
		
		"mov	%1, %%rsi            ;\n"	// destination
		"lock cmpxchg16b (%%rsi)     ;\n"
		"jz  	1f                   ;\n"
		"mov	%2, %%rsi            ;\n"	// comparand
		"mov	%%rax, 0(%%rsi)      ;\n"	// comparand low
		"mov	%%rdx, 8(%%rsi)      ;\n"	// comparand high
"1:	     mov	$0, %0               ;\n"	
		"setz	%b0                  ;\n"	// rc = 
		: "=&a" (rc)
		: "m" (dest), "m" (xcmp), "m" (xxchg)
		: "cc", "memory", "rdx", "rbx", "rcx", "rsi"
		);

	return rc;
}
#endif

