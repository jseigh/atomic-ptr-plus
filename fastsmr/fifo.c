/*
Copyright 2006 Joseph W. Seigh 

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

#include <qcount.h>
#include <userrcu.h>
#include <fastsmr.h>


//------------------------------------------------------------------------------
// fifo_init -- initialize fifo defer queue
//------------------------------------------------------------------------------
void fifo_init(fifo_t *q) {
	q->head = q->tail = NULL;
}


//------------------------------------------------------------------------------
// fifo_enqueue -- 
//------------------------------------------------------------------------------
void fifo_enqueue(fifo_t *q, rcu_defer_t *item) {
	item->next = NULL;

	if (q->head == NULL)
		q->tail = item;

	else
		q->head->next = item;

	q->head = item;
}


//------------------------------------------------------------------------------
// fifo_dequeue -- 
//------------------------------------------------------------------------------
rcu_defer_t *fifo_dequeue(fifo_t *q) {
	rcu_defer_t	*item;

	if ((item = q->tail) != NULL) {
		if ((q->tail = item->next) == NULL)
			q->head = NULL;
	}
	return item;
}


//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
rcu_defer_t *fifo_dequeueall(fifo_t *q) {
	rcu_defer_t	*item;
	item = q->tail;
	q->tail = NULL;
	q->head = NULL;
	return item;
}


//-----------------------------------------------------------------------------
// requeue -- requeue work onto another queue
//
//-----------------------------------------------------------------------------
void fifo_requeue(fifo_t *dst, fifo_t *src) {

	if (dst->head == NULL) {
		dst->head = src->head;
		dst->tail = src->tail;
	}

	else if (src->head != NULL) {
		dst->head->next = src->tail;
		dst->head = src->head;
	}

	src->head = NULL;
	src->tail = NULL;
}


/*-*/
