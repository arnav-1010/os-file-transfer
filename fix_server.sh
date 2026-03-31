#!/bin/bash
# Writes the fixed server.cpp directly to ~/file_transfer_project/src/server.cpp
cat > ~/file_transfer_project/src/server.cpp << 'SERVEREOF'
#include <iostream>
#include <fstream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/stat.h>
#include <atomic>
#include <thread>
#include <chrono>
#include "../include/thread_pool.h"
#include "../include/tcb.h"
#include "../include/scheduler.h"
#include "../include/checkpoint.h"
#include "../include/sync_buffer.h"

#define PORT         8080
#define CHUNK_SIZE   4096
#define END_SIGNAL   "END_OF_FILE"
#define PAUSE_SIGNAL "PAUSE_TRANSFER"
#define NUM_WORKERS  8

std::atomic<int> next_file_id(1);
AMFLQScheduler   scheduler;

SyncBuffer   g_chunk_buf(BUFFER_CAPACITY);
DiskIOThread g_disk_io;

void handle_client(int client_fd) {
    char filename[256] = {0};
    int n = recv(client_fd, filename, sizeof(filename) - 1, 0);
    if (n <= 0) { close(client_fd); return; }
    std::string fname(filename);

    int fid = next_file_id++;
    auto tcb = load_checkpoint(fid, fname);
    if (!tcb) tcb = std::make_shared<TCB>(fid, fname);
    tcb->status         = TransferStatus::IN_PROGRESS;
    tcb->last_scheduled = std::chrono::steady_clock::now();

    std::cout << "[TCB #" << tcb->file_id << "] Transfer started: "
              << fname << " (Q" << tcb->priority_level << ")\n";
    std::cout.flush();

    if (tcb->current_offset == 0) {
        mkdir("received_files", 0777);
        std::string filepath = "received_files/" + fname;
        std::ofstream trunc_file(filepath, std::ios::binary | std::ios::trunc);
        if (!trunc_file) {
            std::cerr << "[TCB #" << tcb->file_id
                      << "] Cannot create output file: " << filepath << "\n";
            close(client_fd);
            return;
        }
        trunc_file.close();
    }

    char buffer[CHUNK_SIZE + 64];
    int  chunks_this_quantum = 0;
    int  quantum = QUEUE_QUANTUM[tcb->priority_level - 1];

    while (true) {
        memset(buffer, 0, sizeof(buffer));
        int bytes = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

        if (bytes <= 0) {
            std::cout << "[TCB #" << tcb->file_id
                      << "] Client disconnected at chunk "
                      << tcb->last_chunk_received << " — checkpoint saved\n";
            std::cout.flush();
            save_checkpoint(*tcb);
            ChunkEntry eof_entry;
            strncpy(eof_entry.filename, fname.c_str(), sizeof(eof_entry.filename) - 1);
            eof_entry.file_id = tcb->file_id; eof_entry.seq_num = tcb->last_chunk_received;
            eof_entry.data_len = 0; eof_entry.is_eof = true;
            g_chunk_buf.produce(eof_entry);
            break;
        }

        if (bytes == (int)strlen(END_SIGNAL) &&
            memcmp(buffer, END_SIGNAL, strlen(END_SIGNAL)) == 0) {
            tcb->status = TransferStatus::COMPLETED;
            delete_checkpoint(tcb->file_id);
            auto end = std::chrono::steady_clock::now();
            double s = std::chrono::duration<double>(end - tcb->start_time).count();
            if (s <= 0) s = 0.001;
            std::cout << "[TCB #" << tcb->file_id << "] COMPLETED: " << fname << "\n"
                      << "  Chunks : " << tcb->last_chunk_received << "\n"
                      << "  Bytes  : " << tcb->current_offset << "\n"
                      << "  Time   : " << s << "s\n"
                      << "  Speed  : " << (tcb->current_offset / 1024.0 / s) << " KB/s\n";
            std::cout.flush();
            ChunkEntry eof_entry;
            strncpy(eof_entry.filename, fname.c_str(), sizeof(eof_entry.filename) - 1);
            eof_entry.file_id = tcb->file_id; eof_entry.seq_num = tcb->last_chunk_received;
            eof_entry.data_len = 0; eof_entry.is_eof = true;
            g_chunk_buf.produce(eof_entry);
            break;
        }

        if (bytes == (int)strlen(PAUSE_SIGNAL) &&
            memcmp(buffer, PAUSE_SIGNAL, strlen(PAUSE_SIGNAL)) == 0) {
            tcb->status = TransferStatus::PAUSED;
            save_checkpoint(*tcb);
            std::cout << "[TCB #" << tcb->file_id << "] PAUSED at chunk "
                      << tcb->last_chunk_received << "\n";
            std::cout.flush();
            ChunkEntry eof_entry;
            strncpy(eof_entry.filename, fname.c_str(), sizeof(eof_entry.filename) - 1);
            eof_entry.file_id = tcb->file_id; eof_entry.seq_num = tcb->last_chunk_received;
            eof_entry.data_len = 0; eof_entry.is_eof = true;
            g_chunk_buf.produce(eof_entry);
            break;
        }

        tcb->current_offset      += bytes;
        tcb->last_chunk_received++;
        chunks_this_quantum++;

        ChunkEntry entry;
        strncpy(entry.filename, fname.c_str(), sizeof(entry.filename) - 1);
        entry.file_id  = tcb->file_id;
        entry.seq_num  = tcb->last_chunk_received;
        entry.data_len = static_cast<size_t>(bytes);
        entry.is_eof   = false;
        memcpy(entry.data, buffer, bytes);
        g_chunk_buf.produce(entry);

        std::cout << "[TCB #" << tcb->file_id << "] Chunk "
                  << tcb->last_chunk_received
                  << " (" << bytes << " bytes, Q" << tcb->priority_level
                  << ") queued for disk\n";
        std::cout.flush();

        if (chunks_this_quantum >= quantum) {
            save_checkpoint(*tcb);
            scheduler.requeue(tcb, true);
            std::cout << "[TCB #" << tcb->file_id
                      << "] Quantum exhausted → demoted to Q"
                      << tcb->priority_level << ", checkpointed\n";
            std::cout.flush();
            chunks_this_quantum = 0;
            quantum = QUEUE_QUANTUM[tcb->priority_level - 1];
            tcb->last_scheduled = std::chrono::steady_clock::now();
        }
    }

    close(client_fd);
}

void aging_thread() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        scheduler.apply_aging();
    }
}

int main() {
    int server_fd;
    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { std::cerr << "Socket creation failed\n"; return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port        = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "Bind failed — is port " << PORT << " already in use?\n";
        return 1;
    }
    listen(server_fd, 10);

    if (disk_io_start(g_disk_io, g_chunk_buf, "received_files") != 0) {
        std::cerr << "FATAL: could not start DiskIO thread\n";
        return 1;
    }

    ThreadPool pool(NUM_WORKERS);
    std::thread ager(aging_thread);
    ager.detach();

    std::cout << "=== SERVER STARTED (Week 4 — Sync Integration) ===\n"
              << "Workers  : " << NUM_WORKERS    << "\n"
              << "Port     : " << PORT            << "\n"
              << "Buffer   : " << BUFFER_CAPACITY << " slots\n"
              << "Queues   : " << NUM_QUEUES
              << " (quanta: 8 / 16 / 32 chunks)\n"
              << "Waiting for clients...\n\n";
    std::cout.flush();

    while (true) {
        int client_fd = accept(server_fd, (struct sockaddr*)&address, &addrlen);
        if (client_fd < 0) continue;
        std::cout << "[Server] New client (fd=" << client_fd << ")\n";
        std::cout.flush();
        pool.enqueue([client_fd]() { handle_client(client_fd); });
    }

    disk_io_stop(g_disk_io);
    close(server_fd);
    return 0;
}
SERVEREOF

echo "--- Written. Verifying ---"
grep -n "g_chunk_buf\|sync_buffer\|disk_io_start\|NUM_WORKERS 8\|produce" \
     ~/file_transfer_project/src/server.cpp

echo ""
echo "--- Rebuilding ---"
cd ~/file_transfer_project && make clean && make
