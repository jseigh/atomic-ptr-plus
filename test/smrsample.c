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

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>


#include <fastsmr.h>
#include <atomix.h>

#include <utime.h>


#define NUMRDRS 4
#define NUMWTRS 2




//------------------------------------------------------------------------------
// FIFO queue
// tail -> x -> x -> 0
//              ^head
//
//------------------------------------------------------------------------------

typedef struct qnode_tt {
	struct qnode_tt	*next;
	rcu_defer_t		defer;	// rcu deferred work
	int				seqnum;	// state sequence #
} qnode_t;

typedef struct q_tt {
	qnode_t			*head;
	qnode_t			*tail;
	sequence_t		sequence;	// fifo sequence #
} q_t;


//------------------------------------------------------------------------------
// node_cb -- 
//------------------------------------------------------------------------------
void node_cb(void *arg, refcb_t cb) {
	qnode_t *node = (qnode_t *)arg;

	// trace all links
	while ((node = node->next) != NULL && cb(&(node->defer))) {}

	return;
}


//------------------------------------------------------------------------------
// queue_init -- initialize queue
//------------------------------------------------------------------------------
void queue_init(q_t *q) {
	q->head = NULL;
	q->tail = NULL;
	q->sequence  = 0;	// can be arbitrary
	return;
}


//------------------------------------------------------------------------------
// enqueue -- enqueue onto head
//------------------------------------------------------------------------------
void enqueue(q_t *q, qnode_t *node) {
	node->next = NULL;

	if (q->head == NULL)
		atomic_store_rel(&(q->tail), node);
	else
		atomic_store_rel(&(q->head->next), node);

	q->head = node;

	return;
}


//------------------------------------------------------------------------------
// dequeue -- dequeue from tail
//------------------------------------------------------------------------------
qnode_t *dequeue(q_t *q) {
	qnode_t *node;

	node = q->tail;
	if (node == NULL)
		return NULL;

	atomic_store(&(q->tail), node->next);
	if (q->tail == NULL) {
		q->head = NULL;		// was q->head == q->tail == node
	}


	return node;
}

//==============================================================================



int		maxWrites = 500;
int		maxDefers = 100;

pthread_mutex_t	mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cvar = PTHREAD_COND_INITIALIZER;

int		writers = 1;
int		defers = 0;
struct timespec ts = {0, 1000000};	// 1 msec
int		preempt = 0;			// read preemption
int		_trace = 0;				// use trace instead of fifo on defer

q_t		q;						// queue anchor


//--------------------------------------------------------------------
// defer_free -- reqeue node
//--------------------------------------------------------------------
void defer_free(void * arg) {
	qnode_t	*node = (qnode_t *)arg;
	int			temp;

	pthread_mutex_lock(&mutex);
	node->seqnum++;				// increment sequence count again
	enqueue(&q, node);			// enqueue on end
	temp = defers--;
	pthread_mutex_unlock(&mutex);
	if (maxDefers == temp)		// max defers reached?
		pthread_cond_broadcast(&cvar);
}


//--------------------------------------------------------------------
// testwrite --
//
//--------------------------------------------------------------------
void *testwrite(void *arg) {
	int		j;
	qnode_t	*node;


	for (j = 0; j < maxWrites; j++) {
		//--------------------------------------------------------
		// dequeue item
		//--------------------------------------------------------


		pthread_mutex_lock(&mutex);
		
		while (defers == maxDefers)
			pthread_cond_wait(&cvar, &mutex);
		
		node = dequeue(&q);

		if (node) {
			defers++;
			node->seqnum++;

			//
			// free item
			//
			node->defer.func = &defer_free;
			node->defer.arg = node;
			if (_trace) {
				node->defer.type = trace;
				node->defer.forrefs = &node_cb;
				pthread_mutex_unlock(&mutex);
				nanosleep(&ts, NULL);
				smr_defer(&(node->defer));
			}
			else {
				node->defer.type = fifo;
				node->defer.psequence = &q.sequence;
				smr_defer(&(node->defer));
				pthread_mutex_unlock(&mutex);
				nanosleep(&ts, NULL);
			}
		}

		else {
			pthread_mutex_unlock(&mutex);
			nanosleep(&ts, NULL);
		}


	}

	return NULL;
}


//--------------------------------------------------------------------
// testread --
//
//--------------------------------------------------------------------
void * testread(void *arg) {
	smr_t	*local;			// hazard pointer
	qnode_t		*node;
	int			seqnum0, seqnum1;
	int			numReads = 0;
	int			numNodes = 0;
	int			stale = 0;	// count of stale nodes seen


	if ((local = smr_acquire()) == NULL) abort();

	while (writers > 0) {
		numReads++;

		for (node = smrload(local, &q.tail);
			node != NULL;
			node = smrload(local, &(node->next)))
		{
			//
			// do anything
			//

			numNodes++;

			seqnum0 = node->seqnum;
			if (preempt)
				nanosleep(&ts, NULL);		// something to cause preemption
			seqnum1 = node->seqnum;
			// check if node has been reused
			if (seqnum1 != seqnum0)			// stale node seen
				// node has been deferred for reclaimantion
				// but not yet reclaimed
				stale++;
			if ((seqnum1 - seqnum0) > 1)	// too stale
				// node had been reclaimed.  That's bad.
				abort();

		}
		smrnull(local);		// clear hazard pointer
	}

	printf("#queue traversals = %d, #nodes = %d, #stale nodes = %d\n",
				numReads, numNodes, stale);

	return NULL;
}


//--------------------------------------------------------------------
// main --
//
//--------------------------------------------------------------------
int main(int argc, char *argv[]) {

pthread_t		wrtid[NUMWTRS];
pthread_t		rdtid[NUMRDRS];
int				rc;
qnode_t			*node;		
int				queuesize = 200;
int				j;

char			opts[] = "hpt";
extern	int		optind;
int				n;
char			**xargv;
int				xargc;
int				_h = 0;

while ((n = getopt(argc, argv, opts)) > -1) {
	switch ((char)n) {

		case 'p':
			preempt = 1;
			break;

		case 't':
			_trace = 1;
			break;

		case 'h':
		default:
			_h = 1;
			break;
	}
}

xargv = &argv[optind];	// xargv[0] is first argument
xargc = argc = optind;	// xargc is number of arguments

if (_h) {
	fprintf(stderr, "usage: %s -<opts>\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "where opts =\n");
	fprintf(stderr, "\t-p  :  preempt reader threads during traversal\n");
	fprintf(stderr, "\t-t  :  use defer type = trace instead of fifo\n");
	fprintf(stderr, "\t-h  :  print this help message\n");
	exit(1);
}


printf("options used:\n");
printf("\tdefer type = %s\n", _trace ? "trace" : "fifo");
printf("\tpreempt = %s\n", preempt ? "on" : "off");
printf("...\n");

//
// initialize queue
//
queue_init(&q);
node = (qnode_t *)calloc(queuesize, sizeof(qnode_t));
memset(node, 0, (queuesize * sizeof(qnode_t)));

for (j = 0; j < queuesize; j++) {
	enqueue(&q, &node[j]);
}

//
// start fastsmr polling thread
//
rcu_startup();

//
// start reader threads
//
for (j = 0; j < NUMRDRS; j++) {
	if ((rc = pthread_create(&rdtid[j], NULL, testread, NULL)) != 0) {
		printf("pthread_create: %s\n", strerror(rc));
		exit(1);
	}
}

//
//	start writer threads
//
for (j = 0; j < NUMWTRS; j++) {
	if ((rc = pthread_create(&wrtid[j], NULL, testwrite, NULL)) != 0) {
		printf("pthread_create: %s\n", strerror(rc));
		exit(1);
	}
}

//
// wait for writer threads to complets
//
for (j = 0; j < NUMWTRS; j++)
	pthread_join(wrtid[j], NULL);
//
// wait for reader threads to complets
//
writers = 0;
for (j = 0; j < NUMRDRS; j++)
	pthread_join(rdtid[j], NULL);

//
// shutdown fastsmr polling thread
//
rcu_shutdown();

//
//
//
pthread_mutex_destroy(&mutex);


//
// free queue
//
free(node);


//
//
//
return 0;

}

/*-*/
