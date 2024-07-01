/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2016 Hugo Guiroux <hugo.guiroux at gmail dot com>
 *               2016 Lockless Inc
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
 *
 *
 * John M. Mellor-Crummey and Michael L. Scott. 1991.
 * Algorithms for scalable synchronization on shared-memory multiprocessors.
 * ACM Trans. Comput. Syst. 9, 1 (February 1991).
 *
 * Lock design summary:
 * The ticket lock is a variant of spinlock that allows limiting the number of
 * atomic instructions on the lock acquire path.
 * This lock is composed of a request variable, and a grant variable.
 * - On lock, the thread atomically increments the request variable to get its
 * ticket, then spinloop while its ticket is different from the grant.
 * - On unlock, the thread increments the grant variable.
 */
#define _GNU_SOURCE
#include <sched.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/mman.h>
#include <pthread.h>
#include <assert.h>
#include <ticket.h>
#include <unistd.h>
#include <time.h>
#include<sys/time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "waiting_policy.h"
#include "interpose.h"
#include "utils.h"
extern __thread unsigned int cur_thread_id;
const int window_size = 4;
const int max_sum_in_ticket=2;
static inline int current_cpu_node(){
    //printf("当前核数为：%d",sched_getcpu());
    return sched_getcpu();
}

int is_big_core(){
    //int nums=sysconf(_SC_NPROCESSORS_CONF);
    //printf("总核数：%d",nums);
    return current_cpu_node()<(6)?1:0;
}

int isunlock(ticket_mutex_t* lock){
    return lock->u.s.grant==lock->u.s.request?1:0;
}

AM_mutex_t *ticket_mutex_create(const pthread_mutexattr_t *attr) {
    AM_mutex_t *impl = (AM_mutex_t *)alloc_cache_align(sizeof(AM_mutex_t));
    impl->native_lock.u.s.request = 0;
    impl->native_lock.u.s.grant   = 0;
    impl->head_of_bigcore         = 0;
    impl->tail_of_bigcore         = 0;
    impl->head_of_smallcore       = 0;
    impl->tail_of_smallcore       = 0;
    impl->popflag_of_bigqueue     = 0;
    impl->popflag_of_smallqueue   = 0;
    impl->big_no_steal            = 0;
    impl->small_no_steal          = 0;
#if COND_VAR
    REAL(pthread_mutex_init)(&impl->native_lock.posix_lock, attr);
#endif

    return impl;
}


void pop_from_bigqueue(AM_mutex_t *impl,queue_node_t *node){
    
    //printf("退出大核队列\n");
    queue_node_t* suc = node->next;
    if(suc==NULL){
        if(__sync_val_compare_and_swap(&impl->tail_of_bigcore, node, 0) == node){
            if(__sync_val_compare_and_swap(&impl->head_of_bigcore, node, 0)!=node){
                //失败代表队列不为空，不再尝试将top置位空;
                free(node);
                impl->popflag_of_bigqueue=0;
                return ;
            }
            __sync_val_compare_and_swap(&impl->big_no_steal, 1, 0) ;
            free(node);
            impl->popflag_of_bigqueue=0;                       
            return ;
            }
        while(1){
            suc=node->next;
            if(suc!=NULL)
                break;
            CPU_PAUSE();
        }
    }
    impl->head_of_bigcore=suc;
    free(node);
    suc->at_head=1;
    impl->popflag_of_bigqueue=0;         //大核pop结束
     
    
}

void pop_from_smallqueue(AM_mutex_t *impl,queue_node_t *node){
        
    //printf("退出小核队列\n");
    queue_node_t* suc = node->next;
    if(suc==NULL){
        if(__sync_val_compare_and_swap(&impl->tail_of_smallcore, node, 0) == node){
            if(__sync_val_compare_and_swap(&impl->head_of_smallcore, node, 0)!=node){
                //失败代表队列不为空，不再尝试将top置位空;
                free(node);
                impl->popflag_of_smallqueue=0;
                return ;
            }  
            __sync_val_compare_and_swap(&impl->small_no_steal, 1, 0) ;
            free(node);
            impl->popflag_of_smallqueue=0;                       
            return ;
            }
        while(1){
            suc=node->next;
            if(suc!=NULL)
                break;
            CPU_PAUSE();
        }
    }
    impl->head_of_smallcore=suc;
    free(node);
    suc->at_head=1;
    impl->popflag_of_smallqueue=0;         //小核pop结束
    
}



double get_currenttime(){
    struct timeval enqueue_time;
    gettimeofday(&enqueue_time, NULL);
    return enqueue_time.tv_sec*1000000L+enqueue_time.tv_usec;
}



void reorder(AM_mutex_t *impl, queue_node_t *window_start,int is_head){
    //printf("发生重排\n");
    
    queue_node_t* window_cur=window_start->next;
    queue_node_t *lastnode=window_start;
    window_start->is_reorder=0;
    //外层四轮循环代表滑动窗口里每个都要重排；
    for(int i=1;i<=window_size;i++){    
        if(window_cur==NULL||window_cur->next==NULL)
            break;
        lastnode=window_cur;
        if(i==1){
            window_cur=window_cur->next;
            continue;
        }
        queue_node_t *save=window_cur->next;      //因为重排过程中，顺序关系可能会变，因此提前保存；
        queue_node_t* window_work=window_start->next;
        long long time_to_finish=0;
        long long current_time=get_currenttime();
        double slowdown_min = 100000000, slowdown_max = 0;
        double slowdown_sum[i];
        int k=0;
        //遍历队列算出队列此时的公平性；
        while(window_work!=window_cur->next){            
            time_to_finish+=window_work->done_alone;
            long long  T_shared=time_to_finish+current_time-window_work->enqueue_time;
            double slowdown=T_shared*1.0/window_work->done_alone;
            slowdown_sum[k++]=slowdown;
            //printf("Tshared = %lld\n",T_shared);
            //printf("slowdonw = %f\n",slowdown);
            if(slowdown_min>slowdown)
                slowdown_min=slowdown;
            if(slowdown_max<slowdown)
                slowdown_max=slowdown;            
            window_work=window_work->next;
        }
        double fairness=slowdown_min/slowdown_max;

        window_work=window_cur;
        queue_node_t* insert=window_cur;
        int y=1;
        while(window_work!=NULL&&window_work!=window_start){
            if(window_work==window_cur){
                time_to_finish-=window_work->done_alone;
                window_work=window_work->pre;               
                continue;
            }
            //printf("%lld",window_work->done_alone);
            double slowdown_pos_after=(time_to_finish+window_cur->done_alone+current_time-window_work->enqueue_time)*1.0/window_work->done_alone;
            double slowdown_new_after=(time_to_finish-window_work->done_alone+window_cur->done_alone+current_time-window_cur->enqueue_time)*1.0/window_cur->done_alone;

            slowdown_sum[i-1]=slowdown_new_after;
            slowdown_sum[i-1-y]=slowdown_pos_after;
            y++;
            //遍历减速比数组；
            double maxval=0,minval=10000000;
            for(int j=0;j<i;j++){
                if(slowdown_sum[j]>maxval)
                    maxval=slowdown_sum[j];
                if(slowdown_sum[j]<minval)
                    minval=slowdown_sum[j];
            }
            double new_fairness=minval/maxval;
            //printf("new fiarness is %f\n",new_fairness);
            if(new_fairness>fairness){
                fairness=new_fairness;
                insert=window_work;
                //printf("%d\n",insert->num);
            }

            time_to_finish-=window_work->done_alone;
            window_work=window_work->pre;
        }

        //插入结点
        if(insert!=window_cur){
            lastnode=window_cur->pre;
            //printf("大小核队列中发生交换\n");
            queue_node_t* pre1=insert->pre;         //insert结点的前后驱
            //queue_node_t* next1=insert->next;
            queue_node_t* pre2=window_cur->pre;     //window_cur结点的前后驱
            queue_node_t* next2=window_cur->next;          
            pre1->next=window_cur;
            window_cur->pre=pre1;
            window_cur->next=insert;
            insert->pre=window_cur;
            pre2->next=next2;
            next2->pre=pre2;
            }
              
        window_cur=save;
        if((!is_head&&window_start->at_head)||(is_head&&isunlock(&impl->native_lock)))           //如果当前结点为头节点，退出去尝试上锁；
            break;

    }
    
    lastnode->is_reorder=1; 

}
int  PW_get(queue_node_t*a,queue_node_t*b){
    //PW=（等待时间+另一个执行时间）/自身执行时间
    //printf("进入PW比较\n");
    if(b==NULL)
        return 1;
    long long curtime=get_currenttime();
    //printf("waiting time : %lld\n",curtime-a->enqueue_time);
    double PW_a = ((curtime-a->enqueue_time)+(b->done_alone))*1.0/a->done_alone;
    double PW_b = ((curtime-b->enqueue_time)+(a->done_alone))*1.0/b->done_alone;
    return PW_a>PW_b?1:0;

}

void wait_until_to_head(AM_mutex_t *impl, queue_node_t *pre,queue_node_t *cur){
    pre->next=cur;
    cur->pre=pre;
    for(;;){
        if(cur->at_head==1)
            return ;
        if(cur->is_reorder==1)
            reorder(impl,cur,0);
        CPU_PAUSE();
    }

}

static int __ticket_mutex_lock(ticket_mutex_t *impl){
    int t = __sync_fetch_and_add(&impl->u.s.request, 1);
    while (impl->u.s.grant != t)
        CPU_PAUSE();

#if COND_VAR
    int ret = REAL(pthread_mutex_lock)(&impl->posix_lock);

    assert(ret == 0);
#endif
    return 0;
}

int get_num_in_ticket(ticket_mutex_t* lock){
    return lock->u.s.request-lock->u.s.grant;
}

int ticket_mutex_lock(AM_mutex_t *impl, ticket_context_t *UNUSED(me)) {
    if(!impl->big_no_steal&&!impl->small_no_steal&&(isunlock(&impl->native_lock)))
        goto  FastPath;
    int ret;
    queue_node_t *new_node=(queue_node_t*)alloc_cache_align(sizeof(queue_node_t));
    new_node->enqueue_time=get_currenttime();
    new_node->next=0;
    new_node->pre=0;
    /*done_alone怎么传入？*/
    new_node->at_head=0;
    new_node->is_reorder=0;
    if(is_big_core()){
        new_node->done_alone=100;
        queue_node_t *pre=xchg_64((void *)&impl->tail_of_bigcore, (void *)new_node); 
        if(pre!=NULL)
            wait_until_to_head(impl,pre,new_node); 
        else{
            __sync_val_compare_and_swap(&impl->big_no_steal, 0, 1);
            impl->head_of_bigcore=new_node; 
            new_node->is_reorder=1;
        }
        for(;;){
            if(new_node->is_reorder)
                reorder(impl,new_node,1);
            if(get_num_in_ticket(&impl->native_lock)<max_sum_in_ticket&&(!impl->popflag_of_smallqueue&&PW_get(new_node,impl->head_of_smallcore))){
                impl->popflag_of_bigqueue=1;
                break;
            }
            CPU_PAUSE();
        } 
        pop_from_bigqueue(impl,new_node);      

    }
    else{
        new_node->done_alone=200;
        queue_node_t *pre=xchg_64((void *)&impl->tail_of_smallcore, (void *)new_node); 
        if(pre!=NULL)
            wait_until_to_head(impl,pre,new_node); 
        else{
            __sync_val_compare_and_swap(&impl->small_no_steal, 0, 1);
            impl->head_of_smallcore=new_node; 
            new_node->is_reorder=1;
        }
        for(;;){
            if(new_node->is_reorder)
                reorder(impl,new_node,1);
            if(get_num_in_ticket(&impl->native_lock)<max_sum_in_ticket&&(!impl->popflag_of_bigqueue&&PW_get(new_node,impl->head_of_bigcore))){
                impl->popflag_of_smallqueue=1;
                break;
            }
            CPU_PAUSE();
        } 
        pop_from_smallqueue(impl,new_node);    
    }
    //printf("mcsnode:%d\n",impl->mcsnode_sum);
    FastPath:
        ret = __ticket_mutex_lock(&impl->native_lock);
        assert(ret == 0);
        return ret;
}

int ticket_mutex_trylock(AM_mutex_t *impl, ticket_context_t *UNUSED(me)) {
    // For a ticket trylock, we need to change both grant & request at the same
    // time
    // Thus we use 32-bit variables that we change to a 64-bit variable
    // and do a cmp&swp on it
    uint32_t me     = impl->native_lock.u.s.request;
    uint32_t menew  = me + 1;
    uint64_t cmp    = ((uint64_t)me << 32) + me;
    uint64_t cmpnew = ((uint64_t)menew << 32) + me;

    if (__sync_val_compare_and_swap(&impl->native_lock.u.u, cmp, cmpnew) != cmp)
        return EBUSY;

#if COND_VAR
    int ret;
    while ((ret = REAL(pthread_mutex_trylock)(&impl->native_lock.posix_lock)) == EBUSY)
        ;
    assert(ret == 0);
#endif
    return 0;
}

void __ticket_mutex_unlock(AM_mutex_t *impl) {
    COMPILER_BARRIER();
    impl->native_lock.u.s.grant++;
}

void ticket_mutex_unlock(AM_mutex_t *impl, ticket_context_t *UNUSED(me)) {
#if COND_VAR
    int ret = REAL(pthread_mutex_unlock)(&impl->native_lock.posix_lock);
    assert(ret == 0);
#endif
    __ticket_mutex_unlock(impl);
}

int ticket_mutex_destroy(AM_mutex_t *lock) {
#if COND_VAR
    REAL(pthread_mutex_destroy)(&lock->native_lock.posix_lock);
#endif
    free(lock);
    lock = NULL;

    return 0;
}

int ticket_cond_init(ticket_cond_t *cond, const pthread_condattr_t *attr) {
#if COND_VAR
    return REAL(pthread_cond_init)(cond, attr);
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

int ticket_cond_timedwait(ticket_cond_t *cond, AM_mutex_t *lock,
                          ticket_context_t *me, const struct timespec *ts) {
#if COND_VAR
    int res;

    __ticket_mutex_unlock(lock);

    if (ts)
        res = REAL(pthread_cond_timedwait)(cond, &lock->native_lock.posix_lock, ts);
    else
        res = REAL(pthread_cond_wait)(cond, &lock->native_lock.posix_lock);

    if (res != 0 && res != ETIMEDOUT) {
        fprintf(stderr, "Error on cond_{timed,}wait %d\n", res);
        assert(0);
    }

    int ret = 0;
    if ((ret = REAL(pthread_mutex_unlock)(&lock->native_lock.posix_lock)) != 0) {
        fprintf(stderr, "Error on mutex_unlock %d\n", ret == EPERM);
        assert(0);
    }

    ticket_mutex_lock(lock, me);

    return res;
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

int ticket_cond_wait(ticket_cond_t *cond, AM_mutex_t *lock,
                     ticket_context_t *me) {
    return ticket_cond_timedwait(cond, lock, me, 0);
}

int ticket_cond_signal(ticket_cond_t *cond) {
#if COND_VAR
    return REAL(pthread_cond_signal)(cond);
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

int ticket_cond_broadcast(ticket_cond_t *cond) {
#if COND_VAR
    return REAL(pthread_cond_broadcast)(cond);
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

int ticket_cond_destroy(ticket_cond_t *cond) {
#if COND_VAR
    return REAL(pthread_cond_destroy)(cond);
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

void ticket_thread_start(void) {
}

void ticket_thread_exit(void) {
}

void ticket_application_init(void) {
}

void ticket_application_exit(void) {
}

void ticket_init_context(lock_mutex_t *UNUSED(impl),
                         lock_context_t *UNUSED(context), int UNUSED(number)) {
}
