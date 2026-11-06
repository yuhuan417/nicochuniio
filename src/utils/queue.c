/*
 * utils/queue.c - fixed-capacity circular byte queue
 * (allocated by slider_start / io4_JVS_FSM; the serial data path does not
 * use it, but the exports are kept for compatibility)
 */
#include "nicochuniio.h"
#include <stdlib.h>

Queue *createQueue(int capacity)
{
    Queue *q = (Queue *)malloc(sizeof(Queue));
    if (q == NULL)
        return NULL;

    q->capacity = capacity;
    q->size     = 0;
    q->front    = q->size;
    q->items    = (char *)malloc(q->capacity);
    if (q->items == NULL) {
        free(q);
        return NULL;
    }
    return q;
}

int isFull(Queue *queue)
{
    if (queue == NULL)
        return -1;
    return queue->size == queue->capacity;
}

int isEmpty(Queue *queue)
{
    if (queue == NULL)
        return -1;
    return queue->size == 0;
}

void enqueue(Queue *queue, char item)
{
    if (queue == NULL || isFull(queue))
        return;

    int pos = (queue->front + queue->size) % queue->capacity;
    queue->items[pos] = item;
    queue->size++;
}

char dequeue(Queue *queue)
{
    if (queue == NULL || isEmpty(queue))
        return 0;

    char item = queue->items[queue->front];
    queue->front = (queue->front + 1) % queue->capacity;
    queue->size--;
    return item;
}
