#pragma once
// ============================================================
//  rw_lock.h  –  Week 4: Readers-Writers Lock
// ============================================================
//  Mapping (per project spec):
//    Readers : Worker threads reading TCB (priority, offset, status)
//    Writers : Scheduler thread modifying AMLFQ queues and TCB fields
//    Shared  : queues[] and TCB structures inside AMFLQScheduler
//
//  Strategy: writers-preferred variant
//    - Multiple readers run concurrently when no writer is pending/active
//    - A pending writer blocks new readers from entering
//    - Exactly one writer runs at a time; all active readers must exit first
//
//  Primitives used:
//    pthread_mutex_t  mutex_       – guards the counters below
//    pthread_cond_t   read_go_     – broadcast to wake readers (writer done)
//    pthread_cond_t   write_go_    – signal to wake next writer (readers done)
//
//  RAII wrappers ReadGuard / WriteGuard prevent unlock-on-early-return bugs.
// ============================================================

#ifndef RW_LOCK_H
#define RW_LOCK_H

#include <pthread.h>
#include <stdexcept>

class RWLock {
public:
    RWLock();
    ~RWLock();

    void read_lock();
    void read_unlock();

    void write_lock();
    void write_unlock();

    // Diagnostics (approximate – not holding the lock)
    int  active_readers()  const { return active_readers_;  }
    bool writer_active()   const { return writer_active_;   }
    int  waiting_writers() const { return waiting_writers_; }

private:
    pthread_mutex_t mutex_;
    pthread_cond_t  read_go_;       // writer → readers  (broadcast)
    pthread_cond_t  write_go_;      // last reader / writer → next writer (signal)

    int  active_readers_;
    int  waiting_writers_;
    bool writer_active_;

    // Non-copyable
    RWLock(const RWLock&)            = delete;
    RWLock& operator=(const RWLock&) = delete;
};

// ── RAII helpers ──────────────────────────────────────────────────────────────
struct ReadGuard {
    explicit ReadGuard(RWLock& l) : lock_(l) { lock_.read_lock();  }
    ~ReadGuard()                              { lock_.read_unlock(); }
    ReadGuard(const ReadGuard&)               = delete;
private:
    RWLock& lock_;
};

struct WriteGuard {
    explicit WriteGuard(RWLock& l) : lock_(l) { lock_.write_lock();  }
    ~WriteGuard()                              { lock_.write_unlock(); }
    WriteGuard(const WriteGuard&)              = delete;
private:
    RWLock& lock_;
};

#endif // RW_LOCK_H
