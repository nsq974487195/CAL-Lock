/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2016 Hugo Guiroux <hugo.guiroux at gmail dot com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of his software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#ifndef __TICKET_H__
#define __TICKET_H__

#include "padding.h"
#define LOCK_ALGORITHM "TICKET"
#define NEED_CONTEXT 0
#define SUPPORT_WAITING 0

typedef union __ticket_lock {
    volatile uint64_t u;
    struct {
        volatile uint32_t grant;
        volatile uint32_t request;
    } s;
} ticket_lock_t __attribute__((aligned(L_CACHE_LINE_SIZE)));

typedef struct ticket_mutex {
    ticket_lock_t u __attribute__((aligned(L_CACHE_LINE_SIZE)));
#if COND_VAR
    char __pad[pad_to_cache_line(sizeof(ticket_lock_t))];
    pthread_mutex_t posix_lock;
#endif
} ticket_mutex_t __attribute__((aligned(L_CACHE_LINE_SIZE)));

//添加我们的大小核队列节点信息
typedef struct queue_node{                       //队列结点;
    struct queue_node *pre;
    struct queue_node *next;
    double enqueue_time;                      //入队时间
    double done_alone;                        //单独执行临界区时间
    volatile int is_reorder; 
    volatile int at_head;
}queue_node_t __attribute__((aligned(L_CACHE_LINE_SIZE)));

//添加我们的锁结构信息
typedef struct AM_mutex{
    ticket_mutex_t native_lock ;
    queue_node_t *volatile head_of_bigcore ;           //大核首尾指针
    queue_node_t *volatile tail_of_bigcore ;
    
    queue_node_t *volatile head_of_smallcore ;         //小核首尾指针；
    queue_node_t *volatile tail_of_smallcore ;
    
    volatile int big_no_steal;
    volatile int small_no_steal;

    volatile int popflag_of_bigqueue ;       //大核队首是否在出队
    volatile int popflag_of_smallqueue ;      //小核队首是否在出队   
       
}AM_mutex_t __attribute__((aligned(L_CACHE_LINE_SIZE)));




typedef pthread_cond_t ticket_cond_t;
typedef void *ticket_context_t; // Unused, take the less space as possible

void wait_until_to_head(AM_mutex_t *impl, queue_node_t *pre,queue_node_t *cur);
int is_big_core();
void pop_from_smallqueue(AM_mutex_t *impl,queue_node_t *node);
void pop_from_bigqueue(AM_mutex_t *impl,queue_node_t *node);
double get_currenttime();
void reorder(AM_mutex_t *impl, queue_node_t *window_start,int is_head);
int  PW_get(queue_node_t*a,queue_node_t*b);

AM_mutex_t  *ticket_mutex_create(const pthread_mutexattr_t *attr);
int ticket_mutex_lock(AM_mutex_t *impl, ticket_context_t *me);
int ticket_mutex_trylock(AM_mutex_t *impl, ticket_context_t *me);
void ticket_mutex_unlock(AM_mutex_t *impl, ticket_context_t *me);
int ticket_mutex_destroy(AM_mutex_t *lock);
int ticket_cond_init(ticket_cond_t *cond, const pthread_condattr_t *attr);
int ticket_cond_timedwait(ticket_cond_t *cond, AM_mutex_t *lock,
                          ticket_context_t *me, const struct timespec *ts);
int ticket_cond_wait(ticket_cond_t *cond, AM_mutex_t *lock,
                     ticket_context_t *me);
int ticket_cond_signal(ticket_cond_t *cond);
int ticket_cond_broadcast(ticket_cond_t *cond);
int ticket_cond_destroy(ticket_cond_t *cond);
void ticket_thread_start(void);
void ticket_thread_exit(void);
void ticket_application_init(void);
void ticket_application_exit(void);

typedef AM_mutex_t  lock_mutex_t;
typedef ticket_context_t lock_context_t;
typedef ticket_cond_t lock_cond_t;

#define lock_mutex_create ticket_mutex_create
#define lock_mutex_lock ticket_mutex_lock
#define lock_mutex_trylock ticket_mutex_trylock
#define lock_mutex_unlock ticket_mutex_unlock
#define lock_mutex_destroy ticket_mutex_destroy
#define lock_cond_init ticket_cond_init
#define lock_cond_timedwait ticket_cond_timedwait
#define lock_cond_wait ticket_cond_wait
#define lock_cond_signal ticket_cond_signal
#define lock_cond_broadcast ticket_cond_broadcast
#define lock_cond_destroy ticket_cond_destroy
#define lock_thread_start ticket_thread_start
#define lock_thread_exit ticket_thread_exit
#define lock_application_init ticket_application_init
#define lock_application_exit ticket_application_exit
#define lock_init_context ticket_init_context

#endif // __TICKET_H__
