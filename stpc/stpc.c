/* 
   Copyright 2013 Joseph W. Seigh

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
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <pthread.h>

#include "stpc.h"

typedef long st_int_t;

typedef struct _stpcNode {
    struct _stpcNode*	next;				// subsequent node
    st_int_t		count;					// reference count
    void            (*freeData)(void *);    // user supplied free data function
    void*			data;
} stpcNode;

typedef union {
#if __SIZEOF_LONG__ == 8
	__int128		ival;
#else
	int64_t			ival;
#endif
	struct {
		stpcNode	*ptr;
		long		sequence;
	};
} sequencedPtr;

typedef struct _stpcProxy {
	sequencedPtr	tail;
    stpcNode*       freeTail;		// referenced node head
    sequencedPtr	freeHead;		// free unreferenced nodes if != head

    unsigned int	maxNodes;		// maximum nodes
	void			*(*allocMem)(size_t);	// allocate memory function (default malloc)
	void            (*freeMem)(void *);    // free memory function (default free)
    //
    unsigned int	numNodes;		// current number of allocated nodes    
    //
    pthread_key_t   statsKey;
	struct _stats_t	stats;
} stpcProxy;



#define REFERENCE 0x2
#define GUARD_BIT 0x1

#define COMPARE(a, b) (a - b)

stats_t * _allocStats(stpcProxy *proxy);
stats_t * stpcGetLocalStats(stpcProxy *proxy);
void _freeStats(void *data);

stpcProxy * stpcNewProxyM(void *(allocMem)(size_t), void (*freeMem)(void *)) {
    // allocate current node

	stpcNode* node = malloc(sizeof(stpcNode));
	memset(node, 0, sizeof(stpcNode));

	node->next = NULL;
	node->count = GUARD_BIT + REFERENCE;		// refcount initially one

	
	// allocate proxy object

	stpcProxy* proxy = allocMem(sizeof(stpcProxy));
	memset(proxy, 0, sizeof(stpcProxy));

	proxy->maxNodes = UINT32_MAX;
    
	proxy->numNodes = 1;
	proxy->allocMem = allocMem;
	proxy->freeMem = freeMem;
        
    pthread_key_create(&proxy->statsKey, &_freeStats);
    
	proxy->tail.ptr = node;
	proxy->tail.sequence = 0;
	proxy->freeTail = node;					// initially empty...
	proxy->freeHead.ptr = node;				// ...
	proxy->freeHead.sequence = 0;

	return proxy;
}

stpcProxy *stpcNewProxy() {
	return stpcNewProxyM(malloc, free);
}

int stpcDeleteProxy(stpcProxy *proxy) {
    stpcNode *node, *next;
    int n = 0;

    node = proxy->freeHead.ptr;
    while (node != NULL) {
        n++;
        next = node->next;
        proxy->freeMem(node);
        node = next;
    }
    
    proxy->freeMem(proxy);
	
	return n;	// # of nodes allocated
}

/*
 * Allocate new node
 * If freeNodes list empty and # nodes < maxNodes, allocate new node
 */
stpcNode* _newNode(stpcProxy* proxy, bool alloc) {
	stpcNode* node = NULL;
	sequencedPtr oldFree, newFree;
	
	oldFree.ival = atomic_load_explicit(&proxy->freeHead.ival, memory_order_acquire);
	while (oldFree.ptr != atomic_load_explicit(&proxy->freeTail, memory_order_relaxed)) {
		newFree.ptr = oldFree.ptr->next;
		newFree.sequence = oldFree.sequence + 1;
        if (atomic_compare_exchange_strong_explicit(&proxy->freeHead.ival, &oldFree.ival, newFree.ival, memory_order_acq_rel, memory_order_acquire)) {
            node = oldFree.ptr;
            stpcGetLocalStats(proxy)->reuse++;
            break;
        }
	}

	if (node == NULL && alloc) {
        int oldNum = atomic_load_explicit(&proxy->numNodes, memory_order_relaxed);
        while (oldNum < proxy->maxNodes && !atomic_compare_exchange_strong_explicit(&proxy->numNodes, &oldNum, oldNum + 1, memory_order_relaxed, memory_order_relaxed));
        if (oldNum >= proxy->maxNodes)
            return NULL;
        node = proxy->allocMem(sizeof(stpcNode));
		if (node == NULL)
			return NULL;
	}

	memset(node, 0, sizeof(stpcNode));

	return node;
}

stpcNode* stpcGetProxyNodeReference(stpcProxy* proxy) {
	sequencedPtr oldTail;
	sequencedPtr newTail;
	
	oldTail.ival = atomic_load_explicit(&proxy->tail.ival, memory_order_relaxed);
	do {
		newTail.sequence = oldTail.sequence + REFERENCE;
		newTail.ptr = oldTail.ptr;
	}
	while (!atomic_compare_exchange_strong_explicit(&proxy->tail.ival, &oldTail.ival, newTail.ival, memory_order_relaxed, memory_order_relaxed));
	
	return oldTail.ptr;
	
}

inline void _dropProxyNodeReference(stpcProxy* proxy, stpcNode* proxyNode, int adjust) {
	stpcNode *node = proxyNode;
	stpcNode *next;
	long rcount = REFERENCE - adjust;
	
	//while (atomic_load_explicit(&node->count, memory_order_relaxed) == rcount || atomic_sub_fetch_explicit(&node->count, rcount, memory_order_release) == 0)
	while (atomic_load_explicit(&node->count, memory_order_relaxed) == rcount || atomic_fetch_sub_explicit(&node->count, rcount, memory_order_release) == rcount)
	{
		atomic_thread_fence(memory_order_release);
		next = node->next;
		atomic_store_explicit(&proxy->freeTail, proxy->freeTail->next, memory_order_release);
		node = next;
        
		// free data queued for deferred deletion
        if (node->freeData != NULL && node->data != NULL) {
            (*node->freeData)(node->data);
            node->data = NULL;
            
            stpcGetLocalStats(proxy)->dataFrees++;
        }
		rcount = REFERENCE;
	}
}

void stpcDropProxyNodeReference(stpcProxy* proxy, stpcNode* proxyNode) {
	_dropProxyNodeReference(proxy, proxyNode, 0);
}

void _queueNode(stpcProxy* proxy, stpcNode* newNode) {
	sequencedPtr oldTail;
	sequencedPtr newTail;
    stpcNode *tailNode, *next;
    bool rc, rc2;
	long attempts = 0;

		
    newNode->count = GUARD_BIT + 2 * REFERENCE;
	
	/*
	 * monkey through the trees queuing trick
	 */
	
	newTail.ptr = newNode;
	newTail.sequence = 0;
	oldTail.ival = atomic_load_explicit(&proxy->tail.ival, memory_order_consume);
	do {
		attempts++;
	}
	while (!atomic_compare_exchange_strong_explicit(&proxy->tail.ival, &oldTail.ival, newTail.ival, memory_order_acq_rel, memory_order_acquire));
    
	atomic_store_explicit(&oldTail.ptr->next, newNode, memory_order_relaxed);
	// update old node's reference count by number of acquired references, clear guard bit, and drop ref acquired from tail pointer
	_dropProxyNodeReference(proxy, oldTail.ptr, (oldTail.sequence - GUARD_BIT));
		    
    stats_t *stats = stpcGetLocalStats(proxy);
    stats->tries++;                         // _addNode invocations
    stats->attempts += attempts;            // tail enqueue attempts
}

void stpcDeferredDelete(stpcProxy *proxy, void (*freeData)(void *), void *data, void (*backoff)(int)) {
    stpcNode *node;
	int n = 0;
    
	while ((node = _newNode(proxy, true)) == NULL) {
		backoff(n++);
	}
    
    node->freeData = freeData;
    node->data = data;
    
    _queueNode(proxy, node);        
}

unsigned int stpcTryDeleteProxyNodes(stpcProxy *proxy, unsigned int count) {
    unsigned int n = 0;
    unsigned int current = atomic_load_explicit(&proxy->numNodes, memory_order_relaxed);
    
    for (n = 0; n < count && current > 1; n++) {
        stpcNode *node = _newNode(proxy, false);
        if (node == NULL)
            break;
        proxy->freeMem(node);
        //current = atomic_sub_fetch_explicit(&proxy->numNodes, 1, memory_order_relaxed);
        current = atomic_fetch_sub_explicit(&proxy->numNodes, 1, memory_order_relaxed) - 1;
    }
    return n;
}

void stpcSetMaxNodes(stpcProxy *proxy, unsigned int maxNodes) {
    if (maxNodes > 1)
        atomic_store_explicit(&proxy->maxNodes, maxNodes, memory_order_relaxed);
}

unsigned int stpcGetMaxNodes(stpcProxy *proxy) {
    return proxy->maxNodes;
}

unsigned int stpcGetNodeCount(stpcProxy *proxy) {
    return atomic_load_explicit(&proxy->numNodes, memory_order_relaxed);
}

stats_t * _allocStats(stpcProxy *proxy) {    
    size_t sz = sizeof(stats_t);
    stats_t *stats = (stats_t *)proxy->allocMem(sz);
    memset(stats, 0, sz);
    stats->proxy = proxy;
    return stats;
}

void _freeStats(void *data) {
    stats_t *stats = (stats_t *)data;
    stpcProxy *proxy = stats->proxy;
    stats_t *pstats = &proxy->stats;
    atomic_fetch_add_explicit(&pstats->attempts, stats->attempts, memory_order_relaxed);
    atomic_fetch_add_explicit(&pstats->dataFrees, stats->dataFrees, memory_order_relaxed);
    atomic_fetch_add_explicit(&pstats->tries, stats->tries, memory_order_relaxed);
    atomic_fetch_add_explicit(&pstats->reuse, stats->reuse, memory_order_relaxed);
    free(data);
}

stats_t * stpcGetLocalStats(stpcProxy *proxy) {
    stats_t *stats = pthread_getspecific(proxy->statsKey);
    if (stats == NULL) {
        stats = _allocStats(proxy);
        pthread_setspecific(proxy->statsKey, stats);        
    }
    return stats;
}

stats_t * stpcGetStats(stpcProxy *proxy) {
    return &proxy->stats;
}


/*-*/
