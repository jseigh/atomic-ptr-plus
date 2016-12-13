/* 
 * File:   stdatomic.h
 * Author: user0
 *
 * Created on January 29, 2013, 6:47 PM
 */

#ifndef STDATOMIC_H
#define	STDATOMIC_H

#ifdef	__cplusplus
extern "C" {
#endif

#ifdef __ATOMIC_RELAXED
    
#define memory_order_relaxed __ATOMIC_RELAXED
#define memory_order_consume __ATOMIC_CONSUME
#define memory_order_acquire __ATOMIC_ACQUIRE
#define memory_order_release __ATOMIC_RELEASE
#define memory_order_acq_rel __ATOMIC_ACQ_REL
#define memory_order_seq_cst __ATOMIC_SEQ_CST

#define atomic_load_explicit(p, m) __atomic_load_n(p, m)
#define atomic_store_explicit(p, v, m) __atomic_store_n(p, v, m)

#define atomic_add_fetch_explicit(p, v, m) __atomic_add_fetch(p, v, m)
#define atomic_fetch_add_explicit(p, v, m) __atomic_fetch_add(p, v, m)
#define atomic_sub_fetch_explicit(p, v, m) __atomic_sub_fetch(p, v, m)
#define atomic_fetch_sub_explicit(p, v, m) __atomic_fetch_sub(p, v, m)

#define atomic_exchange_explicit(p, v, ms, mf) __atomic_exchange_n(p, v, ms, mf)

#define atomic_compare_exchange_strong_explicit(p, o, n, ms, mf) __atomic_compare_exchange_n(p, o, n, 0, ms, mf)
#define atomic_compare_exchange_weak_explicit(p, o, n, ms, mf) __atomic_compare_exchange_n(p, o, n, 1, ms, mf)
	
#define atomic_thread_fence(m) __atomic_thread_fence(m)
#define atomic_signal_fence(m) __atomic_signal_fence(m)
    
#else

#define atomic_load_explicit(p, m) ({ __typeof__ (*p) _t; __sync_synchronize(); _t = *p;  __sync_synchronize(); _t;})
#define atomic_store_explicit(p, v, m) ({ __sync_synchronize(); *p = v; __sync_synchronize();})

#define atomic_add_fetch_explicit(p, v, m) __sync_add_and_fetch(p, v)
#define atomic_fetch_add_explicit(p, v, m) __sync_fetch_and_add(p, v)
#define atomic_sub_fetch_explicit(p, v, m) __sync_sub_and_fetch(p, v)
#define atomic_fetch_sub_explicit(p, v, m) __sync_fetch_and_sub(p, v)

#define atomic_exchange_explicit(p, v, m) ({ \
        __typeof__ (*p) _o, _t; \
        _t = *p; \
        do { \
            _o = _t; \
        } while((_t = __sync_val_compare_and_swap(p, _o, v)) != _o); \
        _o; \
    })
#define atomic_compare_exchange_weak_explicit atomic_compare_exchange_strong_explicit
#define atomic_compare_exchange_strong_explicit(p, o, n, ms, mf) ({ __typeof__ (*p) _t = *o; *o = __sync_val_compare_and_swap(p, *o, n); (_t == *o); })
    
#endif

//extern __int128 __atomic_load_16(__int128 *src, int memory_order);
//extern int __atomic_compare_exchange_16(__int128 * dest, __int128 * xcmp, __int128 xxchg, int m, int ms, int mf);


#ifdef	__cplusplus
}
#endif

#endif	/* STDATOMIC_H */

