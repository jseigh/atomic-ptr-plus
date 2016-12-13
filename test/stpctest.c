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

/*
 * Sample program using stpc
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <printf.h>
#include <getopt.h>
#include <sys/time.h>
#define __USE_POSIX199309
#include <time.h>
#include <sched.h>
#define __USE_GNU
#define __USE_XOPEN2K
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>


#include <assert.h>

#include <stpc.h>


#define WAITSIZE 5

typedef struct _data_t {
    struct _data_t *next;
    long val;
} data_t;

static bool run = true;
static data_t *current = NULL;
static data_t *freedata = NULL;
static long dataseq = 1;

static pthread_key_t parmKey;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cvar = PTHREAD_COND_INITIALIZER;
#ifdef PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP
static pthread_rwlock_t rwlock = PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP;
#else
static pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;
#endif
static sem_t sem;

typedef struct _testparm {
	pthread_t  tid;
	int         id;
	stpcProxy     *proxy;
	long        count;
	int         verbose;

	int			reads;
	int			stale_reads;
	int			invalid_reads;
	
	long		waittime;
	long		totalwaits;
	long        waits[WAITSIZE];
} testparm;

testparm *starttest(int num, void *(*test)(void *), stpcProxy *proxy, long count, int verbose) {
	pthread_attr_t attr;
    pthread_attr_init(&attr);

    testparm *parms = (testparm *)malloc(num * sizeof(testparm));
    memset(parms, 0, num * sizeof(testparm));
	
    for (int j = 0; j < num; j++) {
        testparm *parm = &parms[j];
        parm->id = j;
        parm->proxy = proxy;
        parm->count = count;
        parm->verbose = verbose;
        pthread_create(&parm->tid, &attr, test, parm);
    }
	
	return parms;
}

void endtest(int num, testparm *parms, testparm* result) {
    void    *retval;

    for (int j = 0; j < num; j++) {
        testparm *parm = &parms[j];
        pthread_join(parm->tid, &retval);
		
		result->reads += parm->reads;
		result->stale_reads += parm->stale_reads;
		result->invalid_reads += parm->invalid_reads;
		
        result->waittime += parm->waittime;
		result->totalwaits += parm->totalwaits;
		for (int n = 0; n < WAITSIZE; n++) {
            result->waits[n] += parm->waits[n];
        }

    }	
}
    

uint64_t gettimemillisec() {
    struct timeval t;
    gettimeofday(&t, NULL);
    return (uint64_t)(t.tv_sec * 1000 + (t.tv_usec/1000));
}

void prtProxy(stpcProxy* proxy) {
    stats_t *stats = stpcGetStats(proxy);
    unsigned long alloc = stpcGetNodeCount(proxy);
    printf("proxy: maxnodes=%u allocated=%lu reused=%lu allocated+reused=%lu\n",
            stpcGetMaxNodes(proxy),
            alloc,
            stats->reuse,
            alloc + stats->reuse
            );
    
    if (stats->tries == 0)
        return;
    
    printf("tries (calls to _addNode) = %ld\n", stats->tries);
    printf("attempts to queue node    = %ld\n", stats->attempts);
    double attemptRate = (double)stats->attempts / (double)stats->tries;
    printf("attempts/try              = %5.3f\n", attemptRate);
    printf("frees of data             = %ld\n", stats->dataFrees);
}

void prtWaits(testparm *result) {
    double avgwait = result->totalwaits > 0 ? (double)result->waittime/(double)result->totalwaits : 0.0;
    printf("waits = %ld, waittime=%ld msec, average wait = %4.2f msec\n", result->totalwaits, result->waittime, avgwait);
    printf("wait distribution =");
    char *sep = "";
    for (int n = 0; n < WAITSIZE; n++) {
        int ms = 0;
        switch (n) {
            case 0: ms = 0; break;
            case 1: ms = 10; break;
            case 2: ms = 20; break;
            case 3: ms = 40; break;
            default: ms = 80; break;
        }

        printf("%s %ld [%d msec]", sep, result->waits[n], ms);
        sep = ",";
    }
    printf("\n");
}

void exponentialBackoff(int waits) {
    struct timespec waittime;
    int ms = 0;
    switch (waits) {
        case 0: ms = 0; break;
        case 1: ms = 10; break;
        case 2: ms = 20; break;
        case 3: ms = 40; break;
        default: ms = 80; break;
    }
    
    if (ms == 0)
        sched_yield();
    else {
        waittime.tv_sec = 0;
        waittime.tv_nsec = ms * 1000000;
        nanosleep(&waittime, NULL);
    }

	testparm *parm = (testparm *)pthread_getspecific(parmKey);
	parm->totalwaits++;
	parm->waittime += ms;
	if (waits < WAITSIZE)
		parm->waits[waits]++;
	else
		parm->waits[WAITSIZE - 1]++;

}

void push(data_t *item) {
	item->val = 0;
	item->next = freedata;
	freedata = item;
}

data_t *pop() {
	data_t *item = freedata;
	if (item != NULL)
		freedata = item->next;
	return item;
}

void push_lf(data_t *item) {
	item->val = 0;
    data_t *old = atomic_load_explicit(&freedata, memory_order_relaxed);
    do {
        item->next = old;
    }
    while (!atomic_compare_exchange_weak_explicit(&freedata, &old, item, memory_order_release, memory_order_relaxed));
}
        
data_t *pop_lf() {
	data_t *old = atomic_load_explicit(&freedata, memory_order_consume);
	data_t *item;
	do {
		item = old;
	}
	while (item != NULL && !atomic_compare_exchange_weak_explicit(&freedata, &old, old->next, memory_order_consume, memory_order_consume));
	
	assert (item == old);
	
	return item;
}

void testFree0(void *data) {
}

void testFree1(void *data) {
	push_lf((data_t *)data);
}
        
void testFree3(void *data) {
	push_lf((data_t *)data);
	sem_post(&sem);
}

void testFree4(void *data) {
	pthread_mutex_lock(&mutex);
	bool wasEmpty = (freedata == NULL);
	push((data_t *)data);
	if (wasEmpty)
		pthread_cond_broadcast(&cvar);
	pthread_mutex_unlock(&mutex);
}

void *testWrite0(void *arg) {
    testparm *parm = (testparm *) arg;
	pthread_setspecific(parmKey, parm);

    for (int j = 0; j < parm->count; j++) {
        stpcDeferredDelete(parm->proxy, &testFree0, NULL, exponentialBackoff);
    }
    
    return NULL;
}

void *testWrite1(void *arg) {
	sleep(5);	// no writes, let reader threads run for a bit.
}
        
void *testWrite2(void *arg) {
    testparm *parm = (testparm *) arg;
	pthread_setspecific(parmKey, parm);
    stpcNode *refNode, *node;
    
    data_t *item;
    
    for (int j = 0; j < parm->count; j++) {
        for (;;)
        {
            refNode = stpcGetProxyNodeReference(parm->proxy);
            item = pop_lf();
            stpcDropProxyNodeReference(parm->proxy, refNode);
			if (item != NULL)
				break;
            sched_yield();
        }

        // swap current with item
        item->val = atomic_fetch_add_explicit(&dataseq, 1, memory_order_relaxed) + 1;
        item = atomic_exchange_explicit(&current, item, memory_order_acquire);
        atomic_store_explicit(&item->val, -(item->val), memory_order_relaxed);  // mark stale
        
        stpcDeferredDelete(parm->proxy, &testFree1, item, exponentialBackoff);
    }
    
    return NULL;
}

void *testWrite3(void *arg) {
    testparm *parm = (testparm *) arg;
	pthread_setspecific(parmKey, parm);
    
    data_t *item;
    
    for (int j = 0; j < parm->count; j++) {
		pthread_mutex_lock(&mutex);
        for (;;)
        {
			item = pop_lf();
			if (item != NULL)
				break;
			pthread_mutex_unlock(&mutex);
			sched_yield();
			pthread_mutex_lock(&mutex);
        }

        // swap current with item
		item->val = ++dataseq;
		data_t *tmp = item;
		item = atomic_load_explicit(&current, memory_order_consume);
		atomic_store_explicit(&current, tmp, memory_order_release);
        atomic_store_explicit(&item->val, -(item->val), memory_order_relaxed);  // mark stale
		pthread_mutex_unlock(&mutex);
		
        stpcDeferredDelete(parm->proxy, &testFree1, item, exponentialBackoff);
    }
    
    return NULL;
}

void *testWrite4(void *arg) {
    testparm *parm = (testparm *) arg;
	pthread_setspecific(parmKey, parm);
    
    data_t *item;
    
    for (int j = 0; j < parm->count; j++) {
		sem_wait(&sem);
		pthread_mutex_lock(&mutex);
		item = pop_lf();
        // swap current with item
		item->val = ++dataseq;
		data_t *tmp = item;
		item = atomic_load_explicit(&current, memory_order_consume);
		atomic_store_explicit(&current, tmp, memory_order_release);
        atomic_store_explicit(&item->val, -(item->val), memory_order_relaxed);  // mark stale
		pthread_mutex_unlock(&mutex);
		
        stpcDeferredDelete(parm->proxy, &testFree3, item, exponentialBackoff);
    }
    
    return NULL;
}

void *testWrite5(void *arg) {
    testparm *parm = (testparm *) arg;
	pthread_setspecific(parmKey, parm);
    
    data_t *item;
    
    for (int j = 0; j < parm->count; j++) {
		pthread_mutex_lock(&mutex);
		while((item = pop()) == NULL) {
			pthread_cond_wait(&cvar, &mutex);
		}
		
        // swap current with item
		item->val = ++dataseq;
		data_t *tmp = item;
		item = current;
		current = tmp;
		item->val = -(item->val);

		push(item);	// free item;
		pthread_cond_signal(&cvar);
		pthread_mutex_unlock(&mutex);
    }
    
    return NULL;
}

void *testWrite6(void *arg) {
    testparm *parm = (testparm *) arg;
	pthread_setspecific(parmKey, parm);
    
    data_t *item;
    
    for (int j = 0; j < parm->count; j++) {
		pthread_mutex_lock(&mutex);
		while((item = pop()) == NULL) {
			pthread_cond_wait(&cvar, &mutex);
		}
		pthread_mutex_unlock(&mutex);
		
		pthread_rwlock_wrlock(&rwlock);
		// swap current with item
		item->val = ++dataseq;
		data_t *tmp = item;
		item = current;
		current = tmp;
		item->val = -(item->val);
		pthread_rwlock_unlock(&rwlock);

		pthread_mutex_lock(&mutex);
		push(item);	// free item;
		pthread_cond_signal(&cvar);
		pthread_mutex_unlock(&mutex);
    }
    
    return NULL;
}

void *testRead(void *arg) {
    testparm *parm = (testparm *) arg;
	pthread_setspecific(parmKey, parm);
    stpcNode *refNode;
    data_t *item;
    
    while (atomic_load_explicit(&run, memory_order_relaxed)) {
        refNode = stpcGetProxyNodeReference(parm->proxy);
		int val;
		do {
			item = atomic_load_explicit(&current, memory_order_consume);
		}
		while ((val = atomic_load_explicit(&item->val, memory_order_relaxed)) < 0);
        sched_yield();
        long val2 = atomic_load_explicit(&item->val, memory_order_relaxed);
        parm->reads++;
		if (val == val2)
			;
        else if (val == -val2)
            parm->stale_reads++;
        else
            parm->invalid_reads++;
        
        stpcDropProxyNodeReference(parm->proxy, refNode);
    }

    return NULL;
}

void *testRead5(void *arg) {
    testparm *parm = (testparm *) arg;
	pthread_setspecific(parmKey, parm);
    data_t *item;
    
    while (atomic_load_explicit(&run, memory_order_relaxed)) {
		pthread_mutex_lock(&mutex);
		item = current;
        long val = atomic_load_explicit(&item->val, memory_order_relaxed);
		sched_yield();
        long val2 = atomic_load_explicit(&item->val, memory_order_relaxed);
        parm->reads++;
		if (val == val2)
			;
        else if (val == -val2)
            parm->stale_reads++;			// should never happen
        else
            parm->invalid_reads++;			// should never happen
		pthread_mutex_unlock(&mutex);
    }

    return NULL;
}

void *testRead6(void *arg) {
    testparm *parm = (testparm *) arg;
	pthread_setspecific(parmKey, parm);
    data_t *item;
    
    while (atomic_load_explicit(&run, memory_order_relaxed)) {
        pthread_rwlock_rdlock(&rwlock);
        item = current;
        long val = atomic_load_explicit(&item->val, memory_order_relaxed);
		sched_yield();
        long val2 = atomic_load_explicit(&item->val, memory_order_relaxed);
        parm->reads++;
		if (val == val2)
			;
        else if (val == -val2)
            parm->stale_reads++;			// should never happen
        else
            parm->invalid_reads++;			// should never happen
        pthread_rwlock_unlock(&rwlock);
		sched_yield();
    }

    return NULL;
}

int testReadersWriters(int num_writers, void *(*writer)(void *), int num_readers, void *(*reader)(void *), stpcProxy *proxy, int count, int verbose, testparm *result) {
	long t0, t1;
	int elapsed;
	testparm *writeparms, *readparms;
	
	
	t0 = gettimemillisec();	
	writeparms = starttest(num_writers, writer, proxy, count, verbose);
	readparms = starttest(num_readers, reader, proxy, count, verbose);	
	endtest(num_writers, writeparms, result);	
	atomic_store_explicit(&run, false, memory_order_relaxed);
	t1 = gettimemillisec();
	endtest(num_readers, readparms, result); 
	if (current != NULL)
		testFree1(current);	// free current item
	
	elapsed = t1 - t0;
    if (elapsed == 0)
        elapsed = 1;

	return elapsed;
}

int testWriters(int num_writers, void *(*writer)(void *), stpcProxy *proxy, int count, int verbose, testparm *result) {
	long t0, t1;
	int elapsed;
	
	t0 = gettimemillisec();	
	testparm *writeparms = starttest(num_writers, testWrite0, proxy, count, verbose);
	endtest(num_writers, writeparms, result);	
	t1 = gettimemillisec();	
	elapsed = t1 - t0;
	if (elapsed == 0)
		elapsed = 1;

	return elapsed;
}


int main(int argc, char **argv) {

    char	**xargv;
    int		xargc;
    int		n;

    int		_h = 0;		/* help */

    extern  int     optind;

    int option_index;
    
    int verbose = 0;
    int help = 0;
    
    stpcProxy* proxy;
	
    int count = 5;				// default loop count
    unsigned int maxnodes = 0;  // maximum number of node pool
	unsigned int maxnodes2 = 0;	// reset max number of nodes if not 0
    int num_writers = 1;
    int num_readers = 0;
	int num_free = 0;
	
	int test_num = 0;
	
	char* testdesc[] = {
		"lock-free writers, no readers",
		"lock-free readers, no writers",
		"lock-free readers/writers w/ busy polling ",
		"lock-free readers, locked writers w/ busy polling",
		"lock-free readers, locked writers w/ semaphore",
		"locked reader/writers w/ condvar",
		"reader/writers w/ rwlock, data queue w/ mutex"
	};
	void *(*writeTest[])(void *) = {
		testWrite0,
		testWrite1,
		testWrite2,
		testWrite3,
		testWrite4,
		testWrite5,
		testWrite6
	};
	void *(*readTest[])(void *) = {
		testRead,
		testRead,
		testRead,
		testRead,
		testRead,
		testRead5,
		testRead6
	};
	int max_test_number = sizeof(testdesc)/sizeof(char*);
    
	/*-------------------------------------------------------------------*/
	/* process options (switches)                                        */
	/*-------------------------------------------------------------------*/
	while ((n = getopt(argc, argv, "t:n:hvp:r:w:f:x:"))>-1) {
		switch((char)n) {
            case 0:
                break;
				
			case 't':
				test_num = atoi(optarg);
				break;

            case 'n':
                count = atoi(optarg);
                break;

            case 'w':
                num_writers = atoi(optarg);
                break;
                        
            case 'v':
                verbose = 1;
                break;
            
            case 'p':
                maxnodes = atoi(optarg);
                break;
                
            case 'r':
                num_readers = atoi(optarg);
                break;
				
			case 'f':
				num_free = atoi(optarg);
				break;
				
			case 'x':
				maxnodes2 = atoi(optarg);
				break;
                        
            case 'h':
            case '?':
            default:
                help = 1;
			break;
		}
	}

	xargv=&argv[optind];	/* xargv[0] is first parameter */
	xargc=argc-optind;	/* xargc is number of parameters */

	if (help) {
            fprintf(stderr, "usage %s <options>\n", argv[0]);
            fprintf(stderr, "where options are:\n");
            fprintf(stderr, "\t-n : number of iterations\n");
            fprintf(stderr, "\t-w : number of writer threads\n");
            fprintf(stderr, "\t-r : number of reader threads\n");
            fprintf(stderr, "\t-l : latency\n");
            fprintf(stderr, "\t-p : max node pool size\n");
			fprintf(stderr, "\t-f : number of rw data buffers\n");
			fprintf(stderr, "\t-t : testcase # default 0\n");
			for (int j = 0; j < max_test_number; j++) {
				fprintf(stderr, "\t\ttestcase %d: %s\n", j, testdesc[j]);
			}
            exit(1);
	}
	
    pthread_key_create(&parmKey, NULL);

    proxy = stpcNewProxy();
    if (maxnodes > 0)
        stpcSetMaxNodes(proxy, maxnodes);
	
	int rc;
	if (test_num == 0 || test_num >= max_test_number) {
		test_num = 0;
		num_free = 0;
		num_readers = 0;
	}
	else if (test_num == 1) {
		num_writers = 1;
		count = 0;
	}
	
	if (test_num != 0) {
		if (num_free < 2)
			num_free = num_writers + 1;
		rc = sem_init(&sem, 0, (num_free - 1));
	}
	
    printf("\n================================================================\n");
	printf("Starting testcase %d: %s...\n", test_num, testdesc[test_num]);
	printf("testcase=%d, count=%d, writers=%d, readers=%d, max node pool size=%d, rw data=%d, wordsize=%d\n\n",
		test_num,
		count,
		num_writers,
		num_readers,
		maxnodes,
		num_free,
		__WORDSIZE
		);

	// populate free data pool if necessary
	for (int j = 0; j < num_free; j++) {
		push((data_t *)malloc(sizeof(data_t)));
	}
	current = pop();
	if (current != NULL)
		current->val = dataseq;		
	
	testparm result;
	memset(&result, 0, sizeof(result));

    prtProxy(proxy);

    unsigned int elapsed;

	elapsed = testReadersWriters(num_writers, writeTest[test_num], num_readers, readTest[test_num], proxy, count, verbose, &result);

	if (maxnodes2 != 0) {
		printf("resetting max nodes to %u\n", maxnodes2);
		stpcSetMaxNodes(proxy, maxnodes2);
		unsigned int current = stpcGetNodeCount(proxy);
		if (current > maxnodes2) {
			unsigned int freedNodes = stpcTryDeleteProxyNodes(proxy, current - maxnodes2);
			printf("%u nodes freed\n", freedNodes); 
		}
		
	}
    prtProxy(proxy);
	prtWaits(&result);
	
    long writes = num_writers * count;
    double r = (double)(writes)/(double)(elapsed);
    printf("\n");
    printf("writes = %ld, elapsed time = %d msec, writes/msec = %6.4f\n", writes, elapsed, r);
    
    r = (double)(result.reads)/(double)(elapsed);
	printf("reads=%d stale_reads=%d invalid_reads=%d reads/msec = %6.4f\n", result.reads, result.stale_reads, result.invalid_reads, r);
    
    int ndata = 0;
    data_t *data = freedata;
    while (data != NULL) {
        data_t *next = data->next;
        free(data);
        data = next;
        ndata++;
    }
    printf("# data nodes = %d (expected = %d)\n", ndata, num_free);

	int x = stpcGetNodeCount(proxy);
    int nn = stpcDeleteProxy(proxy);
    proxy = NULL;
	printf("# of nodes = %d (expected = %d\n", nn, x);

	return 0;
}

