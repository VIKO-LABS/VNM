//
// Created by vithurshan on 10/22/26.
//

#include "WorkerManager.h"

void worker_queue_init(WorkerQueue *q) {
    q->head = 0;
    q->tail = 0;
    pthread_mutex_init(&q->head_lock, NULL);
    pthread_mutex_init(&q->tail_lock, NULL);
}

bool worker_queue_push(WorkerQueue *q, int worker_index) {
    pthread_mutex_lock(&q->tail_lock);

    int next_tail = (q->tail + 1) % QUEUE_CAPACITY;

    // Sentinel slot check: queue is full if advancing tail hits head
    if (next_tail == q->head) {
        pthread_mutex_unlock(&q->tail_lock);
        return false;
    }

    q->buffer[q->tail] = worker_index;
    q->tail = next_tail;

    pthread_mutex_unlock(&q->tail_lock);
    return true;
}

int worker_queue_pop(WorkerQueue *q) {
    pthread_mutex_lock(&q->head_lock);

    // Queue is empty when head == tail
    if (q->head == q->tail) {
        pthread_mutex_unlock(&q->head_lock);
        return -1;
    }

    int worker_index = q->buffer[q->head];
    q->head = (q->head + 1) % QUEUE_CAPACITY;

    pthread_mutex_unlock(&q->head_lock);
    return worker_index;
}

void worker_queue_destroy(WorkerQueue *q) {
    pthread_mutex_destroy(&q->head_lock);
    pthread_mutex_destroy(&q->tail_lock);
}
