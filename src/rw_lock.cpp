// ============================================================
//  rw_lock.cpp  –  Week 4: Readers-Writers Lock Implementation
// ============================================================

#include "../include/rw_lock.h"
#include <cstring>
#include <cerrno>
#include <cstdio>

RWLock::RWLock()
    : active_readers_(0),
      waiting_writers_(0),
      writer_active_(false)
{
    if (pthread_mutex_init(&mutex_, nullptr) != 0)
        throw std::runtime_error("RWLock: mutex_init failed");

    if (pthread_cond_init(&read_go_, nullptr) != 0) {
        pthread_mutex_destroy(&mutex_);
        throw std::runtime_error("RWLock: cond_init(read_go) failed");
    }

    if (pthread_cond_init(&write_go_, nullptr) != 0) {
        pthread_cond_destroy(&read_go_);
        pthread_mutex_destroy(&mutex_);
        throw std::runtime_error("RWLock: cond_init(write_go) failed");
    }
}

RWLock::~RWLock() {
    pthread_cond_destroy(&write_go_);
    pthread_cond_destroy(&read_go_);
    pthread_mutex_destroy(&mutex_);
}

// ── Reader side ───────────────────────────────────────────────────────────────

void RWLock::read_lock() {
    pthread_mutex_lock(&mutex_);

    // Block if a writer is active OR one is waiting (writers-preferred)
    while (writer_active_ || waiting_writers_ > 0)
        pthread_cond_wait(&read_go_, &mutex_);

    ++active_readers_;
    pthread_mutex_unlock(&mutex_);
}

void RWLock::read_unlock() {
    pthread_mutex_lock(&mutex_);

    --active_readers_;

    // If we were the last reader and a writer is waiting, wake it
    if (active_readers_ == 0 && waiting_writers_ > 0)
        pthread_cond_signal(&write_go_);

    pthread_mutex_unlock(&mutex_);
}

// ── Writer side ───────────────────────────────────────────────────────────────

void RWLock::write_lock() {
    pthread_mutex_lock(&mutex_);

    ++waiting_writers_;

    // Wait until all active readers finish AND no other writer is running
    while (active_readers_ > 0 || writer_active_)
        pthread_cond_wait(&write_go_, &mutex_);

    --waiting_writers_;
    writer_active_ = true;
    pthread_mutex_unlock(&mutex_);
}

void RWLock::write_unlock() {
    pthread_mutex_lock(&mutex_);

    writer_active_ = false;

    if (waiting_writers_ > 0)
        // Another writer is queued — let it go next (fairness between writers)
        pthread_cond_signal(&write_go_);
    else
        // No pending writers — wake all blocked readers
        pthread_cond_broadcast(&read_go_);

    pthread_mutex_unlock(&mutex_);
}
