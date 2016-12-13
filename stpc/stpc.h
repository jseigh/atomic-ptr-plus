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

typedef struct _stpcNode stpcNode;
typedef struct _stpcProxy stpcProxy;

typedef struct _stats_t {
    stpcProxy *proxy;
    long    tries;          // # calls to _addNode
    long    attempts;       // # attempts to queue node onto tail node
    long    reuse;          // # node reuse/recyling
    long    dataFrees;      // # dataFrees
} stats_t;

extern stats_t * stpcGetLocalStats(stpcProxy *proxy);
extern stats_t * stpcGetStats(stpcProxy *proxy);

extern stpcProxy *stpcNewProxy();
extern stpcProxy * stpcNewProxyM(void *(allocMem)(size_t), void (*freeMem)(void *));
extern int stpcDeleteProxy(stpcProxy *proxy);

extern void stpcSetMaxNodes(stpcProxy *proxy, unsigned int maxNodes); // set before initProxy
extern unsigned int stpcGetMaxNodes(stpcProxy *proxy);
extern unsigned int stpcGetNodeCount(stpcProxy *proxy);

extern stpcNode *stpcGetProxyNodeReference(stpcProxy *proxy);
extern void stpcDropProxyNodeReference(stpcProxy* proxy, stpcNode* node);
extern void stpcDeferredDelete(stpcProxy *proxy, void (*freeData)(void *), void *data, void (*backoff)(int));

extern unsigned int stpcTryDeleteProxyNodes(stpcProxy *proxy, unsigned int count);

#endif /* STPDR_H_ */
