//
// Created by vithurshan on 10/22/26.
//

#ifndef HTTPSERVER_WORKERMANAGER_H
#define HTTPSERVER_WORKERMANAGER_H

#include <pthread.h>
#include <stdbool.h>

#define MAX_WORKERS 64

/**
 * Array-based bounded ring buffer (circular queue) without heap allocations.
 * Uses a sentinel slot (capacity = MAX_WORKERS + 1) to distinguish full from empty:
 *  - Empty: head == tail
 *  - Full:  (tail + 1) % QUEUE_CAPACITY == head
 * Protected with head_lock (for pop) and tail_lock (for push).
 */
#define QUEUE_CAPACITY (MAX_WORKERS + 1)

typedef struct {
    int buffer[QUEUE_CAPACITY];
    int head; // read index
    int tail; // write index
    pthread_mutex_t head_lock;
    pthread_mutex_t tail_lock;
} WorkerQueue;

/**
 * Initializes the array-based WorkerQueue and its mutexes.
 */
void worker_queue_init(WorkerQueue *q);

/**
 * Pushes a worker thread into the queue array using tail_lock.
 * Returns true if successfully pushed, false if queue is full.
 */
bool worker_queue_push(WorkerQueue *q, int worker_index);

/**
 * Pops a worker thread index from the queue array using head_lock.
 * Returns the worker index from the head, or -1 if empty.
 */
int worker_queue_pop(WorkerQueue *q);

/**
 * Cleans up queue mutexes (zero heap allocations to free).
 */
void worker_queue_destroy(WorkerQueue *q);

#endif //HTTPSERVER_WORKERMANAGER_H
