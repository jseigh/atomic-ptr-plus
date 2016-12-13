/*
   Copyright 2002-2013 Joseph W. Seigh 

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 */

//------------------------------------------------------------------------------
// atomic_ptr -- C++ atomic lock-free reference counted smart pointer
//
// version -- 0.0.x (pre-alpha)
//
//
// memory visibility:
//   atomic_ptr has memory release consistency for store into atomic_ptr
// types and dependent load consistency for loads from atomic_ptr types.
// There are no memory visibility guarantees for local_ptr except where
// local_ptr types are loaded from or stored into atomic_ptr types.
// Dereferencing an atomic_ptr has dependent load consistency.
// 
// The current implementation has two dependent load barriers when
// dereferencing an atomic_ptr type, one for the atomic_ptr_ref
// type and one for the referenced object itself.  This may be somewhat
// redundant but I'm leaving it that way for now until dependent load
// "memory barrier" semantics become better understood.
//
// Dropping a reference requires a release memory barrier if the
// resulting reference counts are non zero to prevent late stores
// into recycled objects.  If the resulting reference counts are zero
// an acquire memory barrier is required to prevent subsequent stores
// into the object before the count was dropped to zero.  The current
// implementation (atomic_ptr_ref::adjust) uses full synchronization
// handle both cases.  This may change to handle platforms where
// interlocked instructions don't have memory barriers built in.
// atomic_ptr_ref::adjust also handles adding references which do
// not require memory barriers.  A separate method without memory
// barriers may be created in the future.
//
// notes:
//   Dereferencing an atomic_ptr generates a local_ptr temp to guarantee
// that the object will not be deleted for the duration of the dereference.
// For operator ->, this will be transparent.  For operator * it will require
// a double ** to work.  This is a C++ quirk.
//
//   The recycling pool interface is experimental and may be subject
// to change.
//
//------------------------------------------------------------------------------

#ifndef _ATOMIC_PTR_H
#define _ATOMIC_PTR_H

#include <stdlib.h>
#include <stdatomic.h>

// Membar defines for atomic_ptr load w/ memory_order_acquire semantics
#define MEMBAR0 memory_order_acquire
#define MEMBAR1 memory_order_relaxed
// Membar defines for atomic_ptr load w/ memory_order_consume semantics
//#define MEMBAR0 memory_order_consume
//#define MEMBAR1 memory_order_consime

template<typename T> class atomic_ptr;
template<typename T> class local_ptr;
template<typename T> class atomic_ptr_ref;

// double word sized integer type to get around illogical c11 atomics restriction
#if __SIZEOF_LONG__ == 8
#define ival __int128
#else
#define ival int64_t
#endif


struct refcount {
	long	ecount;	// ephemeral count
	long	rcount;	// reference count
};

//
// Differential reference to allow race free access to reference count
//
template<typename T> struct differentialReference {
	long	ecount; // ephemeral count
	atomic_ptr_ref<T> *ptr;
};

typedef void (*pool_put_t)(void *);


//=============================================================================
// atomic_ptr_ref -- non intrusive reference count
//
//
//=============================================================================
template<typename T> class atomic_ptr_ref {
	friend class atomic_ptr<T>;
	friend class local_ptr<T>;

	private:
		refcount	count;				// reference counts
		T *			ptr;				// ptr to actual object
		pool_put_t	pool;

	public:
		atomic_ptr_ref<T> * next;


		atomic_ptr_ref(T * p = nullptr) {
			count.ecount = 0;
			count.rcount = 1;
			ptr = p;
			pool = nullptr;
			next = nullptr;
		};


		~atomic_ptr_ref() {
			delete ptr;
		}

	private:

		//----------------------------------------------------------------------
		// adjust -- adjust refcounts
		//
		// For dropping references, a release membar is
		// required if reference counts do not go to zero to
		// prevent late stores into deleted storage. An
		// acquire membar is required if the reference counts
		// go to zero to prevent early stores into a live object.
		//
		// Adding references does not require membars.
		//----------------------------------------------------------------------
		int adjust_mb(long xephemeralCount, long xreferenceCount) {
			refcount oldval, newval;

			oldval.ecount = count.ecount;
			oldval.rcount = count.rcount;
			do {
				newval.ecount = oldval.ecount + xephemeralCount;
				newval.rcount = oldval.rcount + xreferenceCount;
			}
			while (!atomic_compare_exchange_strong_explicit((ival*)&count, (ival*)&oldval, (ival)newval, memory_order_acq_rel, memory_order_relaxed));

			return (newval.ecount == 0 && newval.rcount == 0) ? 0 : 1;
		}

		//----------------------------------------------------------------------
		// adjust refcount w/o membar
		//----------------------------------------------------------------------
		int adjust(long xephemeralCount, long xreferenceCount) {
			refcount oldval, newval;

			oldval.ecount = count.ecount;
			oldval.rcount = count.rcount;
			do {
				newval.ecount = oldval.ecount + xephemeralCount;
				newval.rcount = oldval.rcount + xreferenceCount;
			}
			while (!atomic_compare_exchange_strong_explicit((ival*)&count, (ival*)&oldval, (ival)newval, memory_order_relaxed, memory_order_relaxed));

			return (newval.ecount == 0 && newval.rcount == 0) ? 0 : 1;
		}

}; // class atomic_ptr_ref


//=============================================================================
// local_ptr
//
//
//=============================================================================
template<typename T> class local_ptr {
	friend class atomic_ptr<T>;
	public:

		local_ptr(T * obj = nullptr) {
			if (obj != nullptr) {
				refptr = new atomic_ptr_ref<T>(obj);
				refptr->count.ecount = 1;
				refptr->count.rcount = 0;
			}
			else
				refptr = nullptr;
		}

		local_ptr(const local_ptr<T> & src) {
			if ((refptr = src.refptr) != 0)
				refptr->adjust(+1, 0);
		}

		local_ptr(atomic_ptr<T> & src) {
			refptr = src.getrefptr();
		}

		// recycled ref object
		local_ptr(atomic_ptr_ref<T>  * src) {
			refptr = src;
			if (refptr != nullptr) {
				refptr->count.ecount = 1;
				refptr->count.rcount = 0;
			}
			// pool unchanged
		}

		~local_ptr() {
			if (refptr != nullptr && refptr->adjust_mb(-1, 0) == 0) {
				if (refptr->pool == nullptr)
					delete refptr;
				else
					refptr->pool(refptr);		// recyle to pool
			}
		}
		
		local_ptr<T> & operator = (T * obj) {
			local_ptr<T> temp(obj);
			swap(temp);	// non-atomic
			return *this;
		}

		local_ptr<T> & operator = (local_ptr<T> & src) {
			local_ptr<T> temp(src);
			swap(temp);	// non-atomic
			return *this;
		}

		local_ptr<T> & operator = (atomic_ptr<T> & src) {
			local_ptr<T> temp(src);
			swap(temp);	// non-atomic
			return *this;
		}


		T * get() {
			return (refptr != nullptr) ? atomic_load_explicit(&refptr->ptr, MEMBAR1) : (T *)nullptr;
		}

		T * operator -> () { return get(); }
		T & operator * () { return *get(); }
		//operator T* () { return  get(); }

		bool operator == (T * rhd) { return (rhd == get() ); }
		bool operator != (T * rhd) { return (rhd != get() ); }

		// refptr == rhd.refptr  iff  refptr->ptr == rhd.refptr->ptr
		bool operator == (local_ptr<T> & rhd) { return (refptr == rhd.refptr);}
		bool operator != (local_ptr<T> & rhd) { return (refptr != rhd.refptr);}
		bool operator == (atomic_ptr<T> & rhd) { return (refptr == rhd.ref.ptr);}
		bool operator != (atomic_ptr<T> & rhd) { return (refptr != rhd.ref.ptr);}

		//-----------------------------------------------------------------
		// set/get recycle pool methods
		//-----------------------------------------------------------------

		void recycle(atomic_ptr_ref<T> * src) {
			local_ptr<T> temp(src);
			swap(temp);	// non-atomic
			//return *this;
		}

		void setPool(pool_put_t pool) {
			refptr->pool = pool;	// set pool
		}

		pool_put_t getPool() {
			return refptr->pool;
		}


	private:
		void * operator new (size_t) {}		// auto only

		atomic_ptr_ref<T> * refptr;

		inline void swap(local_ptr & other) { // non-atomic swap
			atomic_ptr_ref<T> * temp;
			temp = refptr;
			refptr = other.refptr;
			other.refptr = temp;
		}


}; // class local_ptr


template<typename T> inline bool operator == (int lhd, local_ptr<T> & rhd)
	{ return ((T *)lhd == rhd); }

template<typename T> inline bool operator != (int lhd, local_ptr<T> & rhd)
	{ return ((T *)lhd != rhd); }

template<typename T> inline bool operator == (T * lhd, local_ptr<T> & rhd)
	{ return (rhd == lhd); }

template<typename T> inline bool operator != (T * lhd, local_ptr<T> & rhd)
	{ return (rhd != lhd); }


//=============================================================================
// atomic_ptr
//
//
//=============================================================================
template<typename T> class atomic_ptr {
	friend class local_ptr<T>;

	protected:
		differentialReference<T>  ref;

	public:

		atomic_ptr(T * obj = nullptr) {
			ref.ecount = 0;
			if (obj != nullptr) {
				ref.ptr = new atomic_ptr_ref<T>(obj);
			}
			else
				ref.ptr = nullptr;
		}

		atomic_ptr(local_ptr<T> & src) {	// copy constructor
			ref.ecount = 0;
			if ((ref.ptr = src.refptr) != nullptr)
				ref.ptr->adjust(0, +1);
		}

		atomic_ptr(atomic_ptr<T> & src) {  // copy constructor
			ref.ecount = 0;
			ref.ptr = src.getrefptr();	// atomic 

			// adjust link count
			if (ref.ptr != nullptr)
				ref.ptr->adjust(-1, +1);	// atomic
		}

		// recycled ref objects
		atomic_ptr(atomic_ptr_ref<T> * src) { // copy constructor
			if (src != nullptr) {
				src->count.ecount = 0;
				src->count.rcount = 1;
			}
			ref.ecount = 0;
			ref.ptr = src;	// atomic 
		}


		~atomic_ptr() {					// destructor
			atomic_thread_fence(memory_order_release);
			if (ref.ptr != nullptr && ref.ptr->adjust_mb(ref.ecount, -1) == 0) {
				atomic_thread_fence(memory_order_acquire);
				if (ref.ptr->pool == nullptr)
					delete ref.ptr;
				else
					ref.ptr->pool(ref.ptr);
			}
		}

		atomic_ptr & operator = (T * obj) {
			atomic_ptr<T> temp(obj);
			swap(temp);					// atomic
			return *this;
		}

		atomic_ptr & operator = (local_ptr<T> & src) {
			atomic_ptr<T> temp(src);
			swap(temp);					// atomic
			return *this;
		}

		atomic_ptr & operator = (atomic_ptr<T> & src) {
			atomic_ptr<T> temp(src);
			swap(temp);					// atomic
			return *this;
		}

		
		//-----------------------------------------------------------------
		// generate local temp ptr to guarantee validity of ptr
		// during lifetime of expression. temp is dtor'd after
		// expression has finished execution.
		//-----------------------------------------------------------------
		inline local_ptr<T> operator -> () { return local_ptr<T>(*this); }
		inline local_ptr<T> operator * () { return local_ptr<T>(*this); }

		bool operator == (T * rhd) {
			if (rhd == nullptr)
				return (ref.ptr == nullptr);
			else
				return (local_ptr<T>(*this) == rhd);
		}

		bool operator != (T * rhd) {
			if (rhd == nullptr)
				return (ref.ptr != nullptr);
			else
				return (local_ptr<T>(*this) != rhd);
		}

		bool operator == (local_ptr<T> & rhd) {return (local_ptr<T>(*this) == rhd); }
		bool operator != (local_ptr<T> & rhd) {return (local_ptr<T>(*this) != rhd); }
		bool operator == (atomic_ptr<T> & rhd) {return (local_ptr<T>(*this) == local_ptr<T>(rhd)); }
		bool operator != (atomic_ptr<T> & rhd) {return (local_ptr<T>(*this) != local_ptr<T>(rhd)); }

		bool cas(local_ptr<T> cmp, atomic_ptr<T> xchg) {
			differentialReference<T> temp;
			bool rc = false;

			temp.ecount = ref.ecount;
			temp.ptr = cmp.refptr;

			do {
				if (atomic_compare_exchange_strong_explicit(&ref, &temp, xchg.ref, memory_order_acq_rel, memory_order_relaxed)) {
					xchg.ref = temp;
					rc = true;
					break;
				}

			}
			while (cmp.refptr == temp.ptr);

			return rc;
		}

		//-----------------------------------------------------------------
		// recycle pool methods
		//-----------------------------------------------------------------

		void recyle(atomic_ptr_ref<T> * src) {
			atomic_ptr<T> temp(src);
			swap(temp);	// atomic
			//return *this;
		}

	//protected:
		// atomic
		void swap(atomic_ptr<T> & obj) {	// obj is local & non-shared
			/*
			differentialReference<T> temp;

			temp.ecount = ref.ecount;
			temp.ptr = ref.ptr;

			while(!atomic_compare_exchange_strong_explicit(&ref, &temp, obj.ref, memory_order_release, memory_order_relaxed));

			obj.ref.ecount = temp.ecount;
			obj.ref.ptr = temp.ptr;
			*/
			obj.ref = atomic_exchange_explicit(&ref, obj.ref, memory_order_release, memory_order_relaxed);
		}

	private:

		// atomic
		atomic_ptr_ref<T> * getrefptr() {
			differentialReference<T> oldval, newval;

			oldval.ecount = ref.ecount;
			oldval.ptr = ref.ptr;

			do {
				newval.ecount = oldval.ecount + 1;
				newval.ptr = oldval.ptr;
			}
			while (!atomic_compare_exchange_strong_explicit(&ref, &oldval, newval, memory_order_relaxed, memory_order_relaxed));

			return atomic_load_explicit(&oldval.ptr, MEMBAR0);
		}

}; // class atomic_ptr

template<typename T> inline bool operator == (int lhd, atomic_ptr<T> & rhd)
	{ return ((T *)lhd == rhd); }

template<typename T> inline bool operator != (int lhd, atomic_ptr<T> & rhd)
	{ return ((T *)lhd != rhd); }

template<typename T> inline bool operator == (T * lhd, atomic_ptr<T> & rhd)
	{ return (rhd == lhd); }

template<typename T> inline bool operator != (T * lhd, atomic_ptr<T> & rhd)
	{ return (rhd != lhd); }

#endif // _ATOMIC_PTR_H


/*-*/
