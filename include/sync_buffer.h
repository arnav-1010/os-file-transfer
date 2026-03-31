#pragma once
// ============================================================
//  sync_buffer.h  –  Week 4: Producer-Consumer Chunk Buffer
// ============================================================
//  Mapping (per project spec):
//    Producer      : Worker thread — reads chunks off the network socket
//    Consumer      : DiskIOThread  — writes chunks to disk under received_files/
//    Shared buffer : Fixed-capacity circular ring of ChunkEntry items
//
//  Synchronization primitives:
//    sem_t           empty_   – counts free slots    (init = BUFFER_CAPACITY)
//    sem_t           full_    – counts filled slots  (init = 0)
//    pthread_mutex_t mutex_   – guards head_ / tail_ pointer updates
//
//  How it fits into the existing architecture
//  ──────────────────────────────────────────
//  Previously handle_client() did both recv() AND ofstream::write() in the
//  same worker thread.  Week 4 decouples them:
//
//    Worker thread  →  produce(chunk)  →  [ring buffer]  →  DiskIOThread
//
//  One shared SyncBuffer instance lives in main() alongside the ThreadPool.
//  DiskIOThread is launched once at startup and runs for the server's lifetime.
//  handle_client() is updated to call produce() instead of writing directly.
// ============================================================

#ifndef SYNC_BUFFER_H
#define SYNC_BUFFER_H

#include <semaphore.h>
#include <pthread.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <stdexcept>

// ── Tunables ──────────────────────────────────────────────────────────────────
static constexpr int    BUFFER_CAPACITY = 64;      // ring buffer slots
static constexpr size_t MAX_CHUNK_BYTES = 4096 + 64; // matches server CHUNK_SIZE + headroom

// ── ChunkEntry ────────────────────────────────────────────────────────────────
// One slot in the ring buffer.  Carries everything the DiskIOThread needs.
struct ChunkEntry {
    char     filename[256];   // destination filename under received_files/
    int      file_id;         // TCB file_id  (for logging)
    uint32_t seq_num;         // chunk sequence number (from TCB.last_chunk_received)
    size_t   data_len;        // number of valid bytes in data[]
    bool     is_eof;          // true → sentinel: transfer for this file is done

    uint8_t  data[MAX_CHUNK_BYTES];

    ChunkEntry()
        : file_id(0), seq_num(0), data_len(0), is_eof(false)
    {
        filename[0] = '\0';
        memset(data, 0, sizeof(data));
    }
};

// ── SyncBuffer ────────────────────────────────────────────────────────────────
class SyncBuffer {
public:
    explicit SyncBuffer(int capacity = BUFFER_CAPACITY);
    ~SyncBuffer();

    // Producer (worker thread) — blocks when buffer is full
    void produce(const ChunkEntry& chunk);

    // Consumer (DiskIOThread) — blocks when buffer is empty
    // Returns a zero-initialised ChunkEntry if shutting down
    ChunkEntry consume();

    // Graceful shutdown — unblocks any thread stuck in produce/consume
    void shutdown();
    bool is_shutdown() const { return shutdown_flag_; }

    // Approximate diagnostics (no lock held — only for logging/dashboard)
    int  approx_fill() const;

private:
    int          capacity_;
    ChunkEntry*  ring_;       // heap-allocated circular buffer
    int          head_;       // next write index  (producer advances)
    int          tail_;       // next read  index  (consumer advances)

    sem_t            empty_;  // free slots
    sem_t            full_;   // filled slots
    pthread_mutex_t  mutex_;  // protects head_ / tail_

    volatile bool shutdown_flag_;

    SyncBuffer(const SyncBuffer&)            = delete;
    SyncBuffer& operator=(const SyncBuffer&) = delete;
};

// ── DiskIOThread ──────────────────────────────────────────────────────────────
// Wraps the consumer loop in its own pthread.
// Call disk_io_start() once at server startup; disk_io_stop() on shutdown.
struct DiskIOThread {
    pthread_t     tid;
    SyncBuffer*   buf;
    std::string   output_dir;  // e.g. "received_files"
    volatile bool running;

    DiskIOThread() : tid(0), buf(nullptr), running(false) {}
};

int  disk_io_start(DiskIOThread& t, SyncBuffer& buf,
                   const std::string& output_dir = "received_files");
void disk_io_stop (DiskIOThread& t);

#endif // SYNC_BUFFER_H
