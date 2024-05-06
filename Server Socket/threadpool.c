#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include "threadpool.h"

// Globle variables:

threadpool *create_threadpool(int num_threads_in_pool)
{
    if (num_threads_in_pool <= 0 || num_threads_in_pool > MAXT_IN_POOL)
    {
        return NULL;
    }

    threadpool *p;
    p = (threadpool *)malloc(sizeof(threadpool));
    if (p == NULL)
    {
        perror("error: malloc\n");
        return NULL;
    }

    p->num_threads = num_threads_in_pool;
    p->qsize = 0;
    p->threads = (pthread_t *)malloc((p->num_threads + 1) * sizeof(pthread_t));
    if (p->threads == NULL)
    {
        perror("error: malloc\n");
        return NULL;
    }
    p->qhead = NULL;
    p->qtail = NULL;
    pthread_mutex_init(&(p->qlock), NULL);
    pthread_cond_init(&(p->q_empty), NULL);
    pthread_cond_init(&(p->q_not_empty), NULL);
    p->shutdown = 0;
    p->dont_accept = 0;

    for (int i = 0; i < p->num_threads; i++)
    {
        if (pthread_create(&(p->threads[i]), NULL, do_work, p) != 0)
        {
            perror("error: pthread_create\n");
            pthread_mutex_destroy(&(p->qlock));
            pthread_cond_destroy(&(p->q_empty));
            pthread_cond_destroy(&(p->q_not_empty));
            free(p->threads);
            free(p);
            return NULL;
        }
    }

    return p;
}

void *do_work(void *p)
{
    threadpool *pool = (threadpool *)p;
    work_t *crnt;

    while (1)
    {

        pthread_mutex_lock(&(pool->qlock));

        while (pool->qsize == 0)
        {

            if (pool->dont_accept == 1)
            {
                // printf("got to dont accept\n");
                pthread_mutex_unlock(&(pool->qlock));
                pthread_exit(NULL);
            }
            pthread_cond_wait(&(pool->q_not_empty), &(pool->qlock));
        }

        crnt = pool->qhead;
        pool->qsize--;

        if (pool->qsize == 0)
        {
            pool->qhead = NULL;
            pool->qtail = NULL;
        }
        else
        {
            pool->qhead = crnt->next;
        }

        if (pool->qsize == 0 && pool->shutdown == 1)
        {
            pthread_cond_signal(&(pool->q_empty));
        }

        pthread_mutex_unlock(&(pool->qlock));
        (crnt->routine)(crnt->arg);
        free(crnt);
    }
}

void dispatch(threadpool *from_me, dispatch_fn dispatch_to_here, void *arg)
{
    threadpool *p = (threadpool *)from_me;
    work_t *task;
    task = (work_t *)malloc(sizeof(work_t));
    if (task == NULL)
    {
        perror("error: malloc\n");
        return;
    }
    task->routine = dispatch_to_here;
    task->arg = arg;
    task->next = NULL;

    if (p->dont_accept == 1)
    {
        return;
    }

    pthread_mutex_lock(&(p->qlock));
    if (p->qsize == 0)
    {
        p->qsize++;
        p->qhead = task;
        p->qtail = task;
        pthread_cond_signal(&(p->q_not_empty));
    }
    else
    {
        p->qsize++;
        p->qtail->next = task;
        p->qtail = task;
        pthread_cond_signal(&(p->q_not_empty));
    }
    pthread_mutex_unlock(&(p->qlock));
}

void destroy_threadpool(threadpool *destroyme)
{
    destroyme->dont_accept = 1;
    destroyme->shutdown = 1;

    pthread_mutex_lock(&(destroyme->qlock));
    while (destroyme->qsize > 0)
    {
        // printf("waiting\n");
        pthread_cond_wait(&(destroyme->q_empty), &(destroyme->qlock));
    }
    pthread_mutex_unlock(&(destroyme->qlock));

    // printf("got to end of wait in destroy\n");

    pthread_cond_broadcast(&(destroyme->q_not_empty));

    for (int i = 0; i < destroyme->num_threads; i++)
    {
        pthread_join(destroyme->threads[i], NULL);
    }

    pthread_mutex_destroy(&(destroyme->qlock));
    pthread_cond_destroy(&(destroyme->q_empty));
    pthread_cond_destroy(&(destroyme->q_not_empty));

    destroyme->num_threads = 0;
    free(destroyme->threads);
    free(destroyme);
    return;
}
