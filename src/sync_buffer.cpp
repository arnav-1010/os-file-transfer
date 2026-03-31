// ============================================================
//  sync_buffer.cpp  –  Week 4: Producer-Consumer Chunk Buffer
// ============================================================

#include "../include/sync_buffer.h"

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>

// ── SyncBuffer ────────────────────────────────────────────────────────────────

SyncBuffer::SyncBuffer(int capacity)
    : capacity_(capacity), head_(0), tail_(0), shutdown_flag_(false)
{
    if (capacity_ <= 0)
        throw std::invalid_argument("SyncBuffer: capacity must be > 0");

    ring_ = new ChunkEntry[capacity_];

    // All slots start empty → empty_ = capacity, full_ = 0
    if (sem_init(&empty_, 0, static_cast<unsigned>(capacity_)) != 0) {
        delete[] ring_;
        throw std::runtime_error("SyncBuffer: sem_init(empty) failed");
    }
    if (sem_init(&full_, 0, 0) != 0) {
        sem_destroy(&empty_);
        delete[] ring_;
        throw std::runtime_error("SyncBuffer: sem_init(full) failed");
    }
    if (pthread_mutex_init(&mutex_, nullptr) != 0) {
        sem_destroy(&full_);
        sem_destroy(&empty_);
        delete[] ring_;
        throw std::runtime_error("SyncBuffer: pthread_mutex_init failed");
    }
}

SyncBuffer::~SyncBuffer() {
    shutdown();
    pthread_mutex_destroy(&mutex_);
    sem_destroy(&full_);
    sem_destroy(&empty_);
    delete[] ring_;
}

// ── Producer (called from worker thread inside handle_client) ─────────────────
void SyncBuffer::produce(const ChunkEntry& chunk) {
    // Block until at least one free slot is available
    if (sem_wait(&empty_) != 0) return;

    if (shutdown_flag_) {
        sem_post(&empty_);  // return the slot we just claimed
        return;
    }

    pthread_mutex_lock(&mutex_);
    ring_[head_] = chunk;                   // copy chunk into ring
    head_ = (head_ + 1) % capacity_;
    pthread_mutex_unlock(&mutex_);

    sem_post(&full_);   // tell consumer: one more item is ready
}

// ── Consumer (called from DiskIOThread) ──────────────────────────────────────
ChunkEntry SyncBuffer::consume() {
    if (sem_wait(&full_) != 0) return ChunkEntry{};

    if (shutdown_flag_) {
        sem_post(&full_);
        return ChunkEntry{};
    }

    pthread_mutex_lock(&mutex_);
    ChunkEntry chunk = ring_[tail_];         // copy out
    tail_ = (tail_ + 1) % capacity_;
    pthread_mutex_unlock(&mutex_);

    sem_post(&empty_);  // tell producer: one slot just freed up
    return chunk;
}

void SyncBuffer::shutdown() {
    shutdown_flag_ = true;
    // Post enough times to unblock any thread stuck in sem_wait
    for (int i = 0; i < capacity_; ++i) {
        sem_post(&empty_);
        sem_post(&full_);
    }
}

int SyncBuffer::approx_fill() const {
    int val = 0;
    sem_getvalue(const_cast<sem_t*>(&full_), &val);
    return val;
}

// ── DiskIOThread — consumer loop ──────────────────────────────────────────────

static void* disk_io_fn(void* arg) {
    DiskIOThread* t = static_cast<DiskIOThread*>(arg);
    mkdir(t->output_dir.c_str(), 0777);

    std::cout << "[DiskIO] Thread started, output_dir='"
              << t->output_dir << "'\n";
    std::cout.flush();

    while (t->running) {
        ChunkEntry chunk = t->buf->consume();

        // Shutdown sentinel: zero-length, not EOF, shutdown flag set
        if (t->buf->is_shutdown() && chunk.data_len == 0 && !chunk.is_eof)
            break;

        if (chunk.data_len == 0 && !chunk.is_eof)
            continue;  // spurious / empty entry — skip

        std::string path = t->output_dir + "/" + chunk.filename;

        // Append mode — consistent with the existing Week-1 server behaviour
        // (file was opened in trunc by server.cpp on first chunk;
        //  subsequent chunks from the same transfer append naturally)
        FILE* fp = fopen(path.c_str(), "ab");
        if (!fp) {
            std::cerr << "[DiskIO] ERROR: cannot open '" << path
                      << "': " << strerror(errno) << "\n";
            continue;
        }

        if (!chunk.is_eof && chunk.data_len > 0) {
            size_t written = fwrite(chunk.data, 1, chunk.data_len, fp);
            fflush(fp);

            if (written != chunk.data_len) {
                std::cerr << "[DiskIO] WARNING: partial write for '"
                          << chunk.filename << "' ("
                          << written << "/" << chunk.data_len << " bytes)\n";
            } else {
                std::cout << "[DiskIO] TCB #" << chunk.file_id
                          << " chunk #" << chunk.seq_num
                          << " -> '" << chunk.filename
                          << "' (" << written << " bytes)\n";
                std::cout.flush();
            }
        }

        if (chunk.is_eof) {
            std::cout << "[DiskIO] TCB #" << chunk.file_id
                      << " EOF received — '" << chunk.filename
                      << "' fully written to disk.\n";
            std::cout.flush();
        }

        fclose(fp);
    }

    std::cout << "[DiskIO] Thread exiting.\n";
    std::cout.flush();
    return nullptr;
}

int disk_io_start(DiskIOThread& t, SyncBuffer& buf,
                  const std::string& output_dir) {
    t.buf        = &buf;
    t.output_dir = output_dir;
    t.running    = true;

    int rc = pthread_create(&t.tid, nullptr, disk_io_fn, &t);
    if (rc != 0) {
        t.running = false;
        std::cerr << "[DiskIO] pthread_create failed: " << strerror(rc) << "\n";
    }
    return rc;
}

void disk_io_stop(DiskIOThread& t) {
    if (!t.running) return;
    t.running = false;
    if (t.buf) t.buf->shutdown();
    pthread_join(t.tid, nullptr);
}
