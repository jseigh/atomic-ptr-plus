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
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include <stdio.h>

#include "rcpc.h"

typedef long st_int_t;

typedef struct _rcpcNode {
    struct _rcpcNode*	next;			// subsequent node
    st_int_t        sequence;		// (inclusive)
    st_int_t		prevSequence;   // (exclusive)
    st_int_t		count;			// reference count
    void            (*freeData)(void *);    // user supplied free data function
    void*			data;

    int				qsequence;      // debug
    int				inuse;          // debug
} rcpcNode;

typedef struct _rcpcProxy {
    st_int_t		sequence;
    rcpcNode*         tail;			// referenced node tail
    rcpcNode*         freeTail;		// referenced node head
    rcpcNode*         freeHead;		// free unreferenced nodes if != head

    // set before initProxy
    unsigned int	maxLatency;		// maximum latent node count
    unsigned int    latency;        // max latency permitted for adding a node
    unsigned int	maxNodes;		// maximum nodes
	void			*(*allocMem)(size_t);	// allocate memory function (default malloc)
	void            (*freeMem)(void *);    // free memory function (default free)
    //
    bool            initialized;
    //
    unsigned int	numNodes;		// current number of allocated nodes    
    //
    pthread_key_t   statsKey;
	struct _stats_t	*stats;
} rcpcProxy;



#define REFERENCE 0x2
#define GUARD_BIT 0x1
#define INITIAL_SEQUENCE (GUARD_BIT + 0 * REFERENCE)
#define DEFAULT_LATENCY 1

#define COMPARE(a, b) (a - b)

stats_t * _allocStats(rcpcProxy *proxy);
stats_t * rcpcGetLocalStats(rcpcProxy *proxy);
void _freeStats(void *data);

rcpcProxy * rcpcNewProxyM(void *(allocMem)(size_t), void (*freeMem)(void *)) {
	rcpcProxy* proxy = allocMem(sizeof(rcpcProxy));
	memset(proxy, 0, sizeof(rcpcProxy));
	proxy->sequence = INITIAL_SEQUENCE;

	proxy->maxLatency = 0;
    proxy->latency = DEFAULT_LATENCY;
	proxy->maxNodes = UINT32_MAX;
    
	proxy->numNodes = 0;
	proxy->allocMem = allocMem;
	proxy->freeMem = freeMem;
    
    proxy->initialized = false;

	/*-*/

	return proxy;
}

rcpcProxy *rcpcNewProxy() {
	return rcpcNewProxyM(malloc, free);
}

void rcpcInitProxy(rcpcProxy *proxy) {
    if (proxy->initialized)
        return;
    
    proxy->maxLatency = proxy->latency + 2;
    
    pthread_key_create(&proxy->statsKey, &_freeStats);
    proxy->stats = _allocStats(proxy);
    
    // allocate current node

	rcpcNode* node = malloc(sizeof(rcpcNode));
	memset(node, 0, sizeof(rcpcNode));

	node->next = NULL;
	node->sequence = 0;							// not set
	node->prevSequence = INITIAL_SEQUENCE;
	node->count = GUARD_BIT + REFERENCE;		// refcount initially one

	node->qsequence = 0;
	node->inuse = 1;

	proxy->tail = node;

    // allocate latent nodes
        
	rcpcNode* latentNode;
	for (int j = 0; j < proxy->maxLatency; j++) {
		latentNode = proxy->allocMem(sizeof(rcpcNode));
		memset(latentNode, 0, sizeof(rcpcNode));

		latentNode->next = node;
		latentNode->sequence = 0;
		latentNode->prevSequence = 0;
		latentNode->count = 0;

		latentNode->qsequence = node->qsequence - 1;
		latentNode->inuse = -1;

		node = latentNode;
	}

    // set up free nodes (initially empty)
	proxy->freeTail = latentNode;
	proxy->freeHead = latentNode;

    proxy->initialized = true;
}

void rcpcDeleteProxy(rcpcProxy *proxy) {
    rcpcNode *node, *next;
    int n = 0;

    node = proxy->freeHead;
    while (node != NULL) {
        n++;
        next = node->next;
        proxy->freeMem(node);
        node = next;
    }
    
    if (proxy->stats != NULL)
        proxy->freeMem(proxy->stats);
    proxy->freeMem(proxy);
}

/*
 * Allocate new node
 * If freeNodes list empty and # nodes < maxNodes, allocate new node
 *
 * Proxy reference required to avoid ABA problem
 */
rcpcNode* _newNode(rcpcProxy* proxy, bool alloc) {
	rcpcNode* node = NULL;
	rcpcNode* freeNode = atomic_load_explicit(&proxy->freeHead, memory_order_consume);

	while (freeNode != atomic_load_explicit(&proxy->freeTail, memory_order_consume)) {
        if (atomic_compare_exchange_strong_explicit(&proxy->freeHead, &freeNode, freeNode->next, memory_order_seq_cst, memory_order_consume)) {
            node = freeNode;
            rcpcGetLocalStats(proxy)->reuse++;
            break;
        }
	}

	if (node == NULL && alloc) {
        int oldNum = atomic_load_explicit(&proxy->numNodes, memory_order_relaxed);
        while (oldNum < proxy->maxNodes && !atomic_compare_exchange_strong_explicit(&proxy->numNodes, &oldNum, oldNum + 1, memory_order_relaxed, memory_order_relaxed));
        if (oldNum >= proxy->maxNodes)
            return NULL;
        node = proxy->allocMem(sizeof(rcpcNode));
		if (node == NULL)
			return NULL;
	}

	memset(node, 0, sizeof(rcpcNode));
	node->inuse = 1;

	return node;
}

rcpcNode *rcpcNewNode(rcpcProxy* proxy, bool alloc) {
	rcpcNode *node, *refNode;
	refNode = rcpcGetProxyNodeReference(proxy, NULL);
	node = _newNode(proxy, alloc);
	rcpcDropProxyNodeReference(proxy, refNode);
	return node;
}

void setNodeSequence(rcpcProxy* proxy, rcpcNode* node) {
	if (node->next == NULL)
		return;

	if (node->sequence == 0) { //?
        st_int_t zero = 0;
        st_int_t sequence = atomic_load_explicit(&proxy->sequence, memory_order_relaxed);
        atomic_compare_exchange_strong_explicit(&node->sequence, &zero, sequence, memory_order_acq_rel, memory_order_relaxed);
	}

	atomic_store_explicit(&node->next->prevSequence, atomic_load_explicit(&node->sequence, memory_order_relaxed), memory_order_relaxed);

	// adjust reference count
	st_int_t oldCount, newCount, temp, adjust;

	adjust = (node->sequence - node->prevSequence) - GUARD_BIT;

	oldCount = atomic_load_explicit(&node->count, memory_order_relaxed);
	while ((oldCount & GUARD_BIT) != 0) {
        newCount = oldCount + adjust;
        if (atomic_compare_exchange_strong_explicit(&node->count, &oldCount, newCount, memory_order_acq_rel, memory_order_relaxed))
            break;
	}

	// swing tail pointer to new node if necessary
	if (proxy->tail == node) {
        if (atomic_compare_exchange_strong_explicit(&proxy->tail, &node, node->next, memory_order_relaxed, memory_order_relaxed))
            rcpcDropProxyNodeReference(proxy, node);
	}
}

rcpcNode* rcpcGetProxyNodeReference(rcpcProxy* proxy, int *latency) {
	rcpcNode* node;
	st_int_t oldSequence, newSequence;

	oldSequence = atomic_load_explicit(&proxy->sequence, memory_order_acquire);
	do
	{
            newSequence = oldSequence + REFERENCE;
            node = atomic_load_explicit(&proxy->tail, memory_order_relaxed);
	}
	while (!atomic_compare_exchange_strong_explicit(&proxy->sequence, &oldSequence, newSequence, memory_order_acq_rel, memory_order_acquire));

	int n = 0;
	while (node->next != NULL) {
        //n++;
        setNodeSequence(proxy, node);
        // tail update should complete and be visible before sequence wraps
        if (COMPARE(newSequence, node->sequence) <= 0)
            break;

        node = node->next;
		n++;
	}

	// if node->next is null then newsequence will be <= node->sequence
        
    if (latency != NULL)
        *latency = n;

    rcpcGetLocalStats(proxy)->latency[n]++;
    
	return node;
}

void rcpcDropProxyNodeReference(rcpcProxy* proxy, rcpcNode* proxyNode) {
	rcpcNode* node = proxyNode;
	//while (atomic_sub_fetch_explicit(&node->count, REFERENCE, memory_order_seq_cst) == 0)
	while (atomic_fetch_sub_explicit(&node->count, REFERENCE, memory_order_seq_cst) == REFERENCE)
	{
		atomic_store_explicit(&proxy->freeTail, proxy->freeTail->next, memory_order_release);
		node->inuse = -1;
		node = node->next;
        
		// free data queued for deferred deletion
        if (node->freeData != NULL && node->data != NULL) {
            (*node->freeData)(node->data);
            node->data = NULL;
            
            rcpcGetLocalStats(proxy)->dataFrees++;
        }
	}
}

/*
*
* returns
*     true succeeded
*     false failed
*
*/
bool _addNode(rcpcProxy* proxy, rcpcNode *refNode, int latency, rcpcNode* newNode) {
    rcpcNode *tailNode, *next;
    bool rc;
    
    newNode->sequence = 0;
    newNode->count = GUARD_BIT + 2 * REFERENCE;
    
    long attempts = 0;

    rc = false;
    tailNode = refNode;
    while (latency <= proxy->latency) {
        newNode->qsequence = tailNode->qsequence + 1;
        next = NULL;        // assume NULL
        attempts++;
        rc = atomic_compare_exchange_strong_explicit(&tailNode->next, &next, newNode, memory_order_seq_cst, memory_order_seq_cst);
        setNodeSequence(proxy, tailNode);
        if (rc)
            break;
        tailNode = next;
        latency++;
    }
    
    stats_t *stats = rcpcGetLocalStats(proxy);
    stats->tries++;                         // _addNode invocations
    stats->successful += rc ? 1 : 0;
    stats->attempts += attempts;            // tail enqueue attempts
        
    return rc;
}

bool rcpcAddNode(rcpcProxy *proxy, rcpcNode *node) {
    int latency;
    rcpcNode *refNode = rcpcGetProxyNodeReference(proxy, &latency);
    bool rc = _addNode(proxy, refNode, latency, node);
    rcpcDropProxyNodeReference(proxy, refNode);
    return rc;
}

void rcpcDeferredDelete(rcpcProxy *proxy, void (*freeData)(void *), void *data, void (*backoff)(int)) {
    rcpcNode *refNode, *node;
    int latency;
	int n = 0;
    
	refNode = rcpcGetProxyNodeReference(proxy, &latency);
	while ((node = _newNode(proxy, true)) == NULL) {
		rcpcDropProxyNodeReference(proxy, refNode);
		backoff(n++);
		refNode = rcpcGetProxyNodeReference(proxy, &latency);
	}
    
    node->freeData = freeData;
    node->data = data;
    
    while (!_addNode(proxy, refNode, latency, node)) {        
        rcpcDropProxyNodeReference(proxy, refNode);
        refNode = rcpcGetProxyNodeReference(proxy, &latency);
    }
    rcpcDropProxyNodeReference(proxy, refNode);
}

unsigned int rcpcTryDeleteProxyNodes(rcpcProxy *proxy, unsigned int count) {
    unsigned int n = 0;
    unsigned int current = atomic_load_explicit(&proxy->numNodes, memory_order_relaxed);
    
    for (n = 0; n < count && current > 1; n++) {
        rcpcNode *node = rcpcNewNode(proxy, false);
        if (node == NULL)
            break;
        proxy->freeMem(node);
        //current = atomic_sub_fetch_explicit(&proxy->numNodes, 1, memory_order_relaxed);
        current = atomic_fetch_sub_explicit(&proxy->numNodes, 1, memory_order_relaxed) - 1;
    }
    return n;
}

void rcpcSetLatency(rcpcProxy *proxy, unsigned int latency) {
    if (!proxy->initialized)
        proxy->latency = latency;    
}

void rcpcSetMaxNodes(rcpcProxy *proxy, unsigned int maxNodes) {
    if (maxNodes > 1)
        atomic_store_explicit(&proxy->maxNodes, maxNodes, memory_order_relaxed);
}

unsigned int rcpcGetLatency(rcpcProxy *proxy) {
    return proxy->latency;
}

unsigned int rcpcGetMaxLatency(rcpcProxy *proxy) {
    return proxy->maxLatency;
}

unsigned int rcpcGetMaxNodes(rcpcProxy *proxy) {
    return proxy->maxNodes;
}

unsigned int stpcGetNodeCount(rcpcProxy *proxy) {
    return atomic_load_explicit(&proxy->numNodes, memory_order_relaxed);
}

stats_t * _allocStats(rcpcProxy *proxy) {    
    size_t sz = sizeof(stats_t) + (proxy->maxLatency + 1) * sizeof(*((stats_t *)0)->latency);
    stats_t *stats = (stats_t *)proxy->allocMem(sz);
    memset(stats, 0, sz);
    stats->proxy = proxy;
    stats->latencySize = proxy->maxLatency + 1;
    return stats;
}

void _freeStats(void *data) {
    stats_t *stats = (stats_t *)data;
    rcpcProxy *proxy = stats->proxy;
    stats_t *pstats = proxy->stats;
    atomic_fetch_add_explicit(&pstats->attempts, stats->attempts, memory_order_relaxed);
    atomic_fetch_add_explicit(&pstats->dataFrees, stats->dataFrees, memory_order_relaxed);
    atomic_fetch_add_explicit(&pstats->successful, stats->successful, memory_order_relaxed);
    atomic_fetch_add_explicit(&pstats->tries, stats->tries, memory_order_relaxed);
    atomic_fetch_add_explicit(&pstats->reuse, stats->reuse, memory_order_relaxed);
    for (int j = 0; j <= proxy->maxLatency; j++)
        atomic_fetch_add_explicit(&pstats->latency[j], stats->latency[j], memory_order_relaxed);
    free(data);
}

stats_t * rcpcGetLocalStats(rcpcProxy *proxy) {
    stats_t *stats = pthread_getspecific(proxy->statsKey);
    if (stats == NULL) {
        stats = _allocStats(proxy);
        pthread_setspecific(proxy->statsKey, stats);        
    }
    return stats;
}

stats_t * rcpcGetStats(rcpcProxy *proxy) {
    return proxy->stats;
}


/*-*/
