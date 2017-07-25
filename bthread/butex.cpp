// bthread - A M:N threading library to make applications more concurrent.
// Copyright (c) 2014 Baidu.com, Inc. All Rights Reserved

// Author: Ge,Jun (gejun@baidu.com)
// Date: Tue Jul 22 17:30:12 CST 2014

#include "base/atomicops.h"                // base::atomic
#include "base/scoped_lock.h"              // BAIDU_SCOPED_LOCK
#include "base/macros.h"
#include "base/containers/linked_list.h"   // LinkNode
#include "base/memory/singleton_on_pthread_once.h"
#include "base/logging.h"
#include "bthread/errno.h"                 // EWOULDBLOCK, ESTOP
#include "bthread/sys_futex.h"             // futex_*
#include "bthread/processor.h"             // cpu_relax
#include "bthread/task_control.h"          // TaskControl
#include "bthread/task_group.h"            // TaskGroup
#include "bthread/timer_thread.h"
#include "bthread/butex.h"

// This file implements butex.h
//
// Essence of futex-like semantics is sequenced wait and wake operations
// and guaranteed visibility.
//
// If wait is sequenced before wake:
//    thread1               thread2
//    -------               -------
//    wait()                value = new_value
//                          wake()
// wait() sees unmatched value(fail to wait), or wake() sees the waiter.
//
// If wait is sequenced after wake:
//    thread1               thread2
//    -------               -------
//                          value = new_value
//                          wake()
//    wait()
// wake() must provide some sort of memory fencing to prevent assignment
// of value to be reordered after it. Thus the value is visible to wait()
// as well.

namespace bthread {

struct ButexWaiterCount : public bvar::Adder<int64_t> {
    ButexWaiterCount() : bvar::Adder<int64_t>("bthread_butex_waiter_count") {}
};
inline bvar::Adder<int64_t>& butex_waiter_count() {
    return *base::get_leaky_singleton<ButexWaiterCount>();
}

// Implemented in task_group.cpp
int stop_and_consume_butex_waiter(bthread_t tid, ButexWaiter** pw);
int set_butex_waiter(bthread_t tid, ButexWaiter* w);

// If a thread would suspend for less than so many microseconds, return
// ETIMEDOUT directly.
// Use 1: sleeping for less than 1 microsecond is inefficient and useless.
static const int64_t LEAST_SLEEP_US = 1; 

enum WaiterState {
    WAITER_STATE_NONE,
    WAITER_STATE_TIMED,
    WAITER_STATE_CANCELLED,
    WAITER_STATE_TIMEDOUT
};

struct Butex;

struct ButexWaiter : public base::LinkNode<ButexWaiter> {
    // tids of pthreads are 0
    bthread_t tid;

    // Erasing node from middle of LinkedList is unsafe, namely we can't tell
    // whether a node belongs to a list, have to tag ownership.
    base::atomic<Butex*> container;
};

// non_pthread_task allocates this structure on stack and queue it in
// Butex::waiters.
struct ButexBthreadWaiter : public ButexWaiter {
    TaskMeta* task_meta;
    TimerThread::TaskId sleep_id;
    WaiterState waiter_state;
    int expected_value;
    Butex* initial_butex;
    TaskControl* control;
};

// pthread_task or main_task allocates this structure on stack and queue it
// in Butex::waiters.
struct ButexPthreadWaiter : public ButexWaiter {
    base::atomic<int> sig;
};

typedef base::LinkedList<ButexWaiter> ButexWaiterList;

enum BUTEX_PTHREAD_SIGNAL {
    NOT_SIGNALLED = 0,
    SIGNALLED = 1,
    SAFE_TO_DESTROY
};

struct Butex {
    Butex() : unlock_nref(0) {}
    ~Butex() {}

    base::atomic<int> value;
    base::atomic<int> unlock_nref;
    ButexWaiterList waiters;
    base::Mutex waiter_lock;
};

// Confirm that Butex is consistent with constants in bthread/types.h
BAIDU_CASSERT(sizeof(Butex) == BUTEX_MEMORY_SIZE,
              sizeof_Butex_must_match);
// Confirm that layout of Butex is consistent with impl. of butex_locate()
BAIDU_CASSERT(offsetof(Butex, value) == 0, offsetof_value_must_0);

void wakeup_pthread(ButexPthreadWaiter* pw) {
    // release fence to make sure wait_pthread sees newest changes if it sees
    // new sig
    pw->sig.store(SAFE_TO_DESTROY, base::memory_order_release);
    // At this point, *pw is possibly destroyed if wait_pthread has woken up and
    // seen the new sig. As the futex_wake_private just check the accessibility
    // of the memory and returnes EFAULT in this case, we think it's just fine.
    // If crash happens in the future, we can make pw as tls and never
    // destroyed to resolve this issue.
    futex_wake_private(&pw->sig, 1);
}

bool erase_from_butex(ButexWaiter*, bool);

int wait_pthread(ButexPthreadWaiter& pw, timespec* ptimeout) {
    int expected_value = NOT_SIGNALLED;
    while (true) {
        const int rc = futex_wait_private(&pw.sig, expected_value, ptimeout);
        // Accquire fence to make sure that this thread sees newest changes when
        // it sees the new |sig|
        if (expected_value != pw.sig.load(base::memory_order_acquire)) {
            // After this routine returnes, |pw| is going to be destroyed while
            // the wake threads is possibly holding the reference, but we think
            // it's ok, see the comments at wakeup_pthread for some future work
            return rc;
        }
        if (rc != 0 && errno == ETIMEDOUT) {
            // Remove pw from waiters to make sure no one would wakeup pw after
            // this function returnes.
            if (!erase_from_butex(&pw, false)) {
                // Another thread holds pw, attemping to signal it, spin until
                // it's safe to destroy pw
                // Make sure this thread sees the lastest changes when sig is set to
                // SAFE_TO_DESTROY
                BT_LOOP_WHEN(pw.sig.load(base::memory_order_acquire) 
                                    != SAFE_TO_DESTROY,
                             30/*nops before sched_yield*/);

            }
            return rc;
        }
    }
}

extern BAIDU_THREAD_LOCAL TaskGroup* tls_task_group;

// Returns 0 when no need to unschedule or successfully unscheduled,
// -1 otherwise.
static inline int unsleep_if_necessary(
    ButexBthreadWaiter* w, TimerThread* timer_thread) {
    if (!w->sleep_id) {
        return 0;
    }
    if (timer_thread->unschedule(w->sleep_id) > 0) {
        // the callback is running.
        return -1;
    }
    w->sleep_id = 0;
    return 0;
}

struct BAIDU_CACHELINE_ALIGNMENT CacheAlignedButex : public Butex {
};

void* butex_create() {
    CacheAlignedButex* b = new (std::nothrow) CacheAlignedButex;
    if (b != NULL) {
        return &b->value;
    }
    return NULL;
}

void butex_destroy(void* butex) {
    if (butex != NULL) {
        CacheAlignedButex* b = static_cast<CacheAlignedButex*>(
            container_of(static_cast<base::atomic<int>*>(butex), Butex, value));
        delete b;
    }
}

void* butex_construct(void* butex_memory) {
    Butex* b = new (butex_memory) Butex;
    return &b->value;
}

void butex_destruct(void* butex_memory) {
    if (butex_memory != NULL) {
        Butex* b = static_cast<Butex*>(butex_memory);
        bool first_time = true;
        while (b->unlock_nref.load(base::memory_order_relaxed) != 0) {
            if (first_time) {
                first_time = false;
                LOG(WARNING) << "butex_destruct is racing with butex_wake!";
            }
            cpu_relax();
        }
        base::atomic_thread_fence(base::memory_order_acquire);
        b->~Butex();
    }
}

inline TaskGroup* get_task_group(TaskControl* c) {
    TaskGroup* g = tls_task_group;
    return g ? g : c->choose_one_group();
}

int butex_wake(void* arg) {
    Butex* b = container_of(static_cast<base::atomic<int>*>(arg), Butex, value);
    ButexWaiter* front = NULL;
    {
        BAIDU_SCOPED_LOCK(b->waiter_lock);
        if (b->waiters.empty()) {
            return 0;
        }
        front = b->waiters.head()->value();
        front->RemoveFromList();
        front->container.store(NULL, base::memory_order_relaxed);
    }
    if (front->tid == 0) {
        wakeup_pthread(static_cast<ButexPthreadWaiter*>(front));
        return 1;
    }
    ButexBthreadWaiter* bbw = static_cast<ButexBthreadWaiter*>(front);
    unsleep_if_necessary(bbw, get_global_timer_thread());
    TaskGroup* g = tls_task_group;
    if (g) {
        TaskGroup::exchange(&g, bbw->tid);
    } else {
        bbw->control->choose_one_group()->ready_to_run(bbw->tid);
    }
    return 1;
}

void butex_add_ref_before_wake(void* arg) {
    Butex* b = container_of(static_cast<base::atomic<int>*>(arg), Butex, value);
    b->unlock_nref.fetch_add(1, base::memory_order_relaxed);
}

void butex_remove_ref(void *arg) {
    Butex* b = container_of(static_cast<base::atomic<int>*>(arg), Butex, value);
    b->unlock_nref.fetch_sub(1, base::memory_order_release);
}

int butex_wake_and_remove_ref(void* arg) {
    Butex* b = container_of(static_cast<base::atomic<int>*>(arg), Butex, value);
    ButexWaiter* front = NULL;
    {
        BAIDU_SCOPED_LOCK(b->waiter_lock);
        if (b->waiters.empty()) {
            b->unlock_nref.fetch_sub(1, base::memory_order_release);
            return 0;
        }
        front = b->waiters.head()->value();
        front->RemoveFromList();
        front->container.store(NULL, base::memory_order_relaxed);
    }

    if (front->tid == 0) {  // Which is a pthread
        b->unlock_nref.fetch_sub(1, base::memory_order_release);
        wakeup_pthread(static_cast<ButexPthreadWaiter*>(front));
        return 1;
    }
    
    b->unlock_nref.fetch_sub(1, base::memory_order_release);
    ButexBthreadWaiter* bbw = static_cast<ButexBthreadWaiter*>(front);
    unsleep_if_necessary(bbw, get_global_timer_thread());
    TaskGroup* g = tls_task_group;
    if (g) {
        TaskGroup::exchange(&g, front->tid);
    } else {
        bbw->control->choose_one_group()->ready_to_run(front->tid);
    }
    return 1;
}

static int butex_wake_all(void* arg, bool remove_ref) {
    Butex* b = container_of(static_cast<base::atomic<int>*>(arg), Butex, value);

    ButexWaiterList bthread_waiters;
    ButexWaiterList pthread_waiters;
    {
        BAIDU_SCOPED_LOCK(b->waiter_lock);
        while (!b->waiters.empty()) {
            ButexWaiter* bw = b->waiters.head()->value();
            bw->RemoveFromList();
            bw->container.store(NULL, base::memory_order_relaxed);
            if (bw->tid) {
                bthread_waiters.Append(bw);
            } else {
                pthread_waiters.Append(bw);
            }
        }
    }
    if (remove_ref) {
        b->unlock_nref.fetch_sub(1, base::memory_order_release);
    }

    int nwakeup = 0;
    while (!pthread_waiters.empty()) {
        ButexPthreadWaiter* bw = static_cast<ButexPthreadWaiter*>(
            pthread_waiters.head()->value());
        bw->RemoveFromList();
        wakeup_pthread(bw);
        ++nwakeup;
    }
    if (bthread_waiters.empty()) {
        return nwakeup;
    }
    // We will exchange with first waiter in the end.
    ButexBthreadWaiter* next = static_cast<ButexBthreadWaiter*>(
        bthread_waiters.head()->value());
    next->RemoveFromList();
    unsleep_if_necessary(next, get_global_timer_thread());
    ++nwakeup;
    TaskGroup* g = get_task_group(next->control);
    const int saved_nwakeup = nwakeup;
    while (!bthread_waiters.empty()) {
        // pop reversely
        ButexBthreadWaiter* w = static_cast<ButexBthreadWaiter*>(
            bthread_waiters.tail()->value());
        w->RemoveFromList();
        unsleep_if_necessary(w, get_global_timer_thread());
        g->ready_to_run_nosignal(w->tid);
        ++nwakeup;
    }
    if (saved_nwakeup != nwakeup) {
        g->flush_nosignal_tasks();
    }
    if (g == tls_task_group) {
        TaskGroup::exchange(&g, next->tid);
    } else {
        g->ready_to_run(next->tid);
    }
    return nwakeup;
}

int butex_wake_all_and_remove_ref(void *arg) {
    return butex_wake_all(arg, true);
}

int butex_wake_all(void* arg) {
    return butex_wake_all(arg, false);
}

int butex_wake_except(void* arg, bthread_t excluded_bthread) {
    Butex* b = container_of(static_cast<base::atomic<int>*>(arg), Butex, value);

    ButexWaiterList bthread_waiters;
    ButexWaiterList pthread_waiters;
    {
        ButexWaiter* excluded_waiter = NULL;
        BAIDU_SCOPED_LOCK(b->waiter_lock);
        while (!b->waiters.empty()) {
            ButexWaiter* bw = b->waiters.head()->value();
            bw->RemoveFromList();

            if (bw->tid) {
                if (bw->tid != excluded_bthread) {
                    bthread_waiters.Append(bw);
                    bw->container.store(NULL, base::memory_order_relaxed);
                } else {
                    excluded_waiter = bw;
                }
            } else {
                bw->container.store(NULL, base::memory_order_relaxed);
                pthread_waiters.Append(bw);
            }
        }

        if (excluded_waiter) {
            b->waiters.Append(excluded_waiter);
        }
    }

    int nwakeup = 0;
    while (!pthread_waiters.empty()) {
        ButexPthreadWaiter* bw = static_cast<ButexPthreadWaiter*>(
            pthread_waiters.head()->value());
        bw->RemoveFromList();
        wakeup_pthread(bw);
        ++nwakeup;
    }

    if (bthread_waiters.empty()) {
        return nwakeup;
    }
    ButexBthreadWaiter* front = static_cast<ButexBthreadWaiter*>(
                bthread_waiters.head()->value());

    TaskGroup* g = get_task_group(front->control);
    const int saved_nwakeup = nwakeup;
    do {
        // pop reversely
        ButexBthreadWaiter* w = static_cast<ButexBthreadWaiter*>(
            bthread_waiters.tail()->value());
        w->RemoveFromList();
        unsleep_if_necessary(w, get_global_timer_thread());
        g->ready_to_run_nosignal(w->tid);
        ++nwakeup;
    } while (!bthread_waiters.empty());
    if (saved_nwakeup != nwakeup) {
        g->flush_nosignal_tasks();
    }
    return nwakeup;
}

int butex_requeue(void* arg, void* arg2) {
    Butex* b = container_of(static_cast<base::atomic<int>*>(arg), Butex, value);
    Butex* m = container_of(static_cast<base::atomic<int>*>(arg2), Butex, value);

    ButexWaiter* front = NULL;
    {
        std::unique_lock<base::Mutex> lck1(b->waiter_lock, std::defer_lock);
        std::unique_lock<base::Mutex> lck2(m->waiter_lock, std::defer_lock);
        base::double_lock(lck1, lck2);
        if (b->waiters.empty()) {
            return 0;
        }

        front = b->waiters.head()->value();
        front->RemoveFromList();
        front->container.store(NULL, base::memory_order_relaxed);

        while (!b->waiters.empty()) {
            ButexWaiter* bw = b->waiters.head()->value();
            bw->RemoveFromList();
            m->waiters.Append(bw);
            bw->container.store(m, base::memory_order_relaxed);
        }
    }

    if (front->tid == 0) {  // which is a pthread
        wakeup_pthread(static_cast<ButexPthreadWaiter*>(front));
        return 1;
    }
    ButexBthreadWaiter* bbw = static_cast<ButexBthreadWaiter*>(front);
    unsleep_if_necessary(bbw, get_global_timer_thread());
    TaskGroup* g = tls_task_group;
    if (g) {
        TaskGroup::exchange(&g, front->tid);
    } else {
        bbw->control->choose_one_group()->ready_to_run(front->tid);
    }
    return 1;
}

// Callable from multiple threads, at most one thread may wake up the waiter.
static void erase_from_butex_and_wakeup(void* arg) {
    erase_from_butex(static_cast<ButexWaiter*>(arg), true);
}

inline bool erase_from_butex(ButexWaiter* bw, bool wakeup) {
    // `bw' is guaranteed to be valid inside this function because waiter
    // will wait until this function cancelled or finished.
    // NOTE: This function must be no-op when bw->container is NULL.
    bool erased = false;
    Butex* b;
    int saved_errno = errno;
    while ((b = bw->container.load(base::memory_order_acquire))) {
        // b can be NULL when the waiter is scheduled but queued.
        BAIDU_SCOPED_LOCK(b->waiter_lock);
        if (b == bw->container.load(base::memory_order_relaxed)) {
            bw->RemoveFromList();
            bw->container.store(NULL, base::memory_order_relaxed);
            if (bw->tid) {
                static_cast<ButexBthreadWaiter*>(bw)->waiter_state = WAITER_STATE_TIMEDOUT;
            }
            erased = true;
            break;
        }
    }
    if (erased && wakeup) {
        if (bw->tid) {
            ButexBthreadWaiter* bbw = static_cast<ButexBthreadWaiter*>(bw);
            get_task_group(bbw->control)->ready_to_run(bw->tid);
        } else {
            ButexPthreadWaiter* pw = static_cast<ButexPthreadWaiter*>(bw);
            wakeup_pthread(pw);
        }
    }
    errno = saved_errno;
    return erased;
}

static void wait_for_butex(void* arg) {
    ButexBthreadWaiter* const bw = static_cast<ButexBthreadWaiter*>(arg);
    Butex* const b = bw->initial_butex;
    // 1: waiter with timeout should have waiter_state == WAITER_STATE_TIMED
    //    before they're are queued, otherwise the waiter is already timedout
    //    and removed by TimerThread, in which case we should stop queueing.
    //
    // Visibility of waiter_state:
    //    bthread                           TimerThread
    //    -------                           -----------
    //    waiter_state = TIMED
    //    tt_lock { add task }
    //                                      tt_lock { get task }
    //                                      waiter_lock { waiter_state=TIMEDOUT }
    //    waiter_lock { use waiter_state }
    // tt_lock represents TimerThread::_mutex. Obviously visibility of
    // waiter_state are sequenced by two locks, both threads are guaranteed to
    // see the correct value.
    {
        BAIDU_SCOPED_LOCK(b->waiter_lock);
        if (b->value.load(base::memory_order_relaxed) == bw->expected_value &&
            bw->waiter_state != WAITER_STATE_TIMEDOUT/*1*/ &&
            (!bw->task_meta->stop || !bw->task_meta->interruptible)) {
            b->waiters.Append(bw);
            bw->container.store(b, base::memory_order_relaxed);
            return;
        }
    }
    
    // b->container is NULL which makes erase_from_butex_and_wakeup() and
    // stop_butex_wait() no-op, there's no race between following code and
    // the two functions. The on-stack ButexBthreadWaiter is safe to use and
    // bw->waiter_state will not change again.
    unsleep_if_necessary(bw, get_global_timer_thread());
    if (bw->waiter_state != WAITER_STATE_TIMEDOUT) {
        bw->waiter_state = WAITER_STATE_CANCELLED;
    }
    tls_task_group->ready_to_run(bw->tid);
    // FIXME: jump back to original thread is buggy.
    
    // // Value unmatched or waiter is already woken up by TimerThread, jump
    // // back to original bthread.
    // TaskGroup* g = tls_task_group;
    // g->set_remained(TaskGroup::ready_to_run_in_worker, (void*)g->current_tid());
    // // 2: Don't run remained because we're already in a remained function
    // //    otherwise stack may overflow.
    // TaskGroup::sched_to(&g, bw->tid, false/*2*/);
}

static int butex_wait_from_pthread(TaskGroup* g, Butex* b, int expected_value,
                                   const timespec* abstime) {
    // sys futex needs relative timeout.
    // Compute diff between abstime and now.
    timespec* ptimeout = NULL;
    timespec timeout;
    if (abstime != NULL) {
        const int64_t timeout_us = base::timespec_to_microseconds(*abstime) -
            base::gettimeofday_us();
        if (timeout_us <= LEAST_SLEEP_US) {
            errno = ETIMEDOUT;
            return -1;
        }
        timeout = base::microseconds_to_timespec(timeout_us);
        ptimeout = &timeout;
    }

    TaskMeta* task = NULL;
    bool set_waiter = false;
    ButexPthreadWaiter pw;
    pw.tid = 0;
    pw.sig.store(NOT_SIGNALLED, base::memory_order_relaxed);
    int rc = 0;
    
    if (g) {
        task = g->current_task();
        if (task->interruptible) {
            if (task->stop) {
                errno = ESTOP;
                return -1;
            }
            set_waiter = true;
            task->current_waiter.store(&pw, base::memory_order_release);
        }
    }
    {
        std::unique_lock<base::Mutex> lck(b->waiter_lock);
        if (b->value.load(base::memory_order_relaxed) == expected_value) {
            b->waiters.Append(&pw);
            pw.container.store(b, base::memory_order_relaxed);
            lck.unlock();
            bvar::Adder<int64_t>& num_waiters = butex_waiter_count();
            num_waiters << 1;
            rc = wait_pthread(pw, ptimeout);
            num_waiters << -1;
        } else {
            lck.unlock();
            errno = EWOULDBLOCK;
            rc = -1;
        }
    }
    if (task) {
        if (set_waiter) {
            // If current_waiter is NULL, stop_butex_wait() is running and
            // using pw, spin until current_waiter != NULL.
            BT_LOOP_WHEN(task->current_waiter.exchange(
                             NULL, base::memory_order_acquire) == NULL,
                         30/*nops before sched_yield*/);
        }
        if (task->stop) {
            errno = ESTOP;
            return -1;
        }
    }
    return rc;
}

int butex_wait(void* arg, int expected_value, const timespec* abstime) {
    Butex* b = container_of(static_cast<base::atomic<int>*>(arg), Butex, value);
    if (b->value != expected_value) {
        errno = EWOULDBLOCK;
        // Sometimes we may take actions immediately after unmatched butex,
        // this fence makes sure that we see changes before changing butex.
        base::atomic_thread_fence(base::memory_order_acquire);
        return -1;
    }
    TaskGroup* g = tls_task_group;
    if (NULL == g || g->is_current_pthread_task()) {
        return butex_wait_from_pthread(g, b, expected_value, abstime);
    }
    ButexBthreadWaiter bbw;
    // tid is 0 iff the thread is non-bthread
    bbw.tid = g->current_tid();
    bbw.container.store(NULL, base::memory_order_relaxed);
    bbw.task_meta = g->current_task();
    bbw.sleep_id = 0;
    bbw.waiter_state = WAITER_STATE_NONE;
    bbw.expected_value = expected_value;
    bbw.initial_butex = b;
    bbw.control = g->control();

    if (abstime != NULL) {
        // Schedule timer before queueing. If the timer is triggered before
        // queueing, cancel queueing. This is a kind of optimistic locking.
        bbw.waiter_state = WAITER_STATE_TIMED;
        // Already timed out.
        // TODO(gejun): find general methods to speed up time functions.
        if (base::timespec_to_microseconds(*abstime) <=
            (base::gettimeofday_us() + LEAST_SLEEP_US)) {
            errno = ETIMEDOUT;
            return -1;
        }
        bbw.sleep_id = get_global_timer_thread()->schedule(
            erase_from_butex_and_wakeup, &bbw, *abstime);
        if (!bbw.sleep_id) {  // TimerThread stopped.
            errno = ESTOP;
            return -1;
        }
    }
    bvar::Adder<int64_t>& num_waiters = butex_waiter_count();
    num_waiters << 1;
    // release fence matches with acquire fence in stop_and_consume_butex_waiter
    // in task_group.cpp to guarantee visibility of `interruptible'.
    bbw.task_meta->current_waiter.store(&bbw, base::memory_order_release);
    g->set_remained(wait_for_butex, &bbw);
    TaskGroup::sched(&g);

    // erase_from_butex_and_wakeup (called by TimerThread) is possibly still
    // running and using bbw. The chance is small, just spin until it's done.
    BT_LOOP_WHEN(unsleep_if_necessary(&bbw, get_global_timer_thread()) < 0,
                 30/*nops before sched_yield*/);
    
    // If current_waiter is NULL, stop_butex_wait() is running and using bbw.
    // Spin until current_waiter != NULL.
    BT_LOOP_WHEN(bbw.task_meta->current_waiter.exchange(
                     NULL, base::memory_order_acquire) == NULL,
                 30/*nops before sched_yield*/);
    num_waiters << -1;

    // ESTOP has highest priority.
    if (bbw.task_meta->stop) {
        errno = ESTOP;
        return -1;
    }
    // If timed out as well as value unmatched, return ETIMEDOUT.
    if (WAITER_STATE_TIMEDOUT == bbw.waiter_state) {
        errno = ETIMEDOUT;
        return -1;
    } else if (WAITER_STATE_CANCELLED == bbw.waiter_state) {
        errno = EWOULDBLOCK;
        return -1;
    }
    return 0;
}

int butex_wait_uninterruptible(void* arg, int expected_value, const timespec* abstime) {
    TaskGroup* g = tls_task_group;
    TaskMeta* caller = NULL;
    bool saved_interruptible = true;
    if (NULL != g) {
        caller = g->current_task();
        saved_interruptible = caller->interruptible;
        caller->interruptible = false;
    }
    const int rc = butex_wait(arg, expected_value, abstime);
    if (caller) {
        caller->interruptible = saved_interruptible;
    }
    return rc;
}

int stop_butex_wait(bthread_t tid) {
    // Consume current_waiter in the TaskMeta, wake it up then set it back.
    ButexWaiter* w = NULL;
    if (stop_and_consume_butex_waiter(tid, &w) < 0) {
        return -1;
    }
    if (w != NULL) {
        erase_from_butex(w, true);
        // If butex_wait() already wakes up before we set current_waiter back,
        // the function will spin until current_waiter becomes non-NULL.
        if (__builtin_expect(set_butex_waiter(tid, w) < 0, 0)) {
            LOG(FATAL) << "butex_wait should spin until setting back waiter";
            return -1;
        }
    }
    return 0;
}

}  // namespace bthread
