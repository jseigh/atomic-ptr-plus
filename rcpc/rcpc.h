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

#ifndef STPDR_H_
#define STPDR_H_

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

typedef struct _rcpcNode stpcNode;
typedef struct _rcpcProxy rcpcProxy;

typedef struct _stats_t {
    rcpcProxy *proxy;
    long    tries;          // # calls to _addNode
    long    successful;     // # successful calls to _addNode (returned true)
    long    attempts;       // # attempts to queue node onto tail node
    long    reuse;          // # node reuse/recyling
    long    dataFrees;      // # dataFrees
    long    latencySize;    // size of latency[]
    long    latency[];      // getProxyNodeReference latency distribution
} stats_t;

extern stats_t * rcpcGetLocalStats(rcpcProxy *proxy);
extern stats_t * rcpcGetStats(rcpcProxy *proxy);

extern rcpcProxy *rcpcNewProxy();
extern rcpcProxy * rcpcNewProxyM(void *(allocMem)(size_t), void (*freeMem)(void *));
extern void rcpcSetLatency(rcpcProxy *proxy, unsigned int latency);   // set before initProxy
extern void rcpcSetMaxNodes(rcpcProxy *proxy, unsigned int maxNodes); // set before initProxy
extern void rcpcInitProxy(rcpcProxy *proxy);
extern void rcpcDeleteProxy(rcpcProxy *proxy);

extern unsigned int rcpcGetLatency(rcpcProxy *proxy);
extern unsigned int rcpcGetMaxLatency(rcpcProxy *proxy);
extern unsigned int rcpcGetMaxNodes(rcpcProxy *proxy);
extern unsigned int stpcGetNodeCount(rcpcProxy *proxy);

extern stpcNode *rcpcGetProxyNodeReference(rcpcProxy *proxy, int *latency);
extern void rcpcDropProxyNodeReference(rcpcProxy* proxy, stpcNode* node);
extern void rcpcDeferredDelete(rcpcProxy *proxy, void (*freeData)(void *), void *data, void (*backoff)(int));

extern unsigned int rcpcTryDeleteProxyNodes(rcpcProxy *proxy, unsigned int count);

#endif /* STPDR_H_ */
