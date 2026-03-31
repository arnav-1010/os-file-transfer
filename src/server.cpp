// ============================================================
// server.cpp — Weeks 1-5: TCP + ThreadPool + AMLFQ + SyncBuffer + AES + SHA256
// ============================================================
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
#include "../include/crypto.h"

#define PORT         8080
#define CHUNK_SIZE   4096
#define NUM_WORKERS  8

std::atomic<int>  next_file_id(1);
AMFLQScheduler    scheduler;
SyncBuffer        g_chunk_buf(BUFFER_CAPACITY);
DiskIOThread      g_disk_io;

void handle_client(int client_fd) {
    char filename[256] = {0};
    int n = recv(client_fd, filename, sizeof(filename)-1, 0);
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

    int chunks_this_quantum = 0;
    int quantum = QUEUE_QUANTUM[tcb->priority_level - 1];

    while (true) {
        uint32_t frame_len_net = 0;
        int r = recv(client_fd, &frame_len_net, 4, MSG_WAITALL);
        if (r <= 0) {
            std::cout << "[TCB #" << fid << "] Client disconnected — checkpoint saved\n";
            std::cout.flush();
            save_checkpoint(*tcb);
            break;
        }
        uint32_t frame_len = ntohl(frame_len_net);

        if (frame_len == 0xFFFFFFFF) {
            tcb->status = TransferStatus::COMPLETED;
            delete_checkpoint(tcb->file_id);
            auto end = std::chrono::steady_clock::now();
            double s = std::chrono::duration<double>(end - tcb->start_time).count();
            if (s <= 0) s = 0.001;
            std::cout << "[TCB #" << fid << "] COMPLETED: " << fname << "\n"
                      << "  Chunks : " << tcb->last_chunk_received << "\n"
                      << "  Bytes  : " << tcb->current_offset      << "\n"
                      << "  Time   : " << s << "s\n"
                      << "  Speed  : " << (tcb->current_offset/1024.0/s) << " KB/s\n";
            std::cout.flush();
            break;
        }

        if (frame_len > MAX_CHUNK_BYTES + CRYPTO_HDR + 64) {
            std::cerr << "[TCB #" << fid << "] Frame too large\n";
            save_checkpoint(*tcb); break;
        }

        uint8_t enc_buf[MAX_CHUNK_BYTES + CRYPTO_HDR + 64];
        int got = recv(client_fd, enc_buf, frame_len, MSG_WAITALL);
        if (got != (int)frame_len) {
            std::cerr << "[TCB #" << fid << "] Incomplete frame\n";
            save_checkpoint(*tcb); break;
        }

        uint8_t plain_buf[MAX_CHUNK_BYTES];
        int plain_len = crypto_decrypt(enc_buf, frame_len, plain_buf, sizeof(plain_buf));

        if (plain_len == -2) {
            std::cerr << "[TCB #" << fid << "] SHA-256 MISMATCH on chunk "
                      << tcb->last_chunk_received + 1 << "\n";
            std::cout.flush();
        } else if (plain_len < 0) {
            std::cerr << "[TCB #" << fid << "] Decrypt error\n";
            save_checkpoint(*tcb); break;
        }

        std::string hex = hash_to_hex(enc_buf + IV_SIZE);

        tcb->last_chunk_received++;
        tcb->current_offset += plain_len;
        chunks_this_quantum++;

        std::cout << "[TCB #" << fid << "] Chunk " << tcb->last_chunk_received
                  << " OK (" << plain_len << "B)"
                  << " sha256=" << hex.substr(0,16) << "..."
                  << (plain_len >= 0 ? " [OK]" : " [HASH MISMATCH!]") << "\n";
        std::cout.flush();

        ChunkEntry entry;
        strncpy(entry.filename, fname.c_str(), sizeof(entry.filename)-1);
        entry.filename[sizeof(entry.filename)-1] = '\0';
        entry.file_id  = fid;
        entry.seq_num  = tcb->last_chunk_received;
        entry.data_len = (plain_len > 0) ? (size_t)plain_len : 0;
        entry.is_eof   = false;
        if (entry.data_len > 0)
            memcpy(entry.data, plain_buf, entry.data_len);
        g_chunk_buf.produce(entry);

        if (chunks_this_quantum >= quantum) {
            save_checkpoint(*tcb);
            scheduler.requeue(tcb, true);
            std::cout << "[TCB #" << fid << "] Quantum exhausted → Q"
                      << tcb->priority_level << "\n";
            std::cout.flush();
            chunks_this_quantum = 0;
            quantum = QUEUE_QUANTUM[tcb->priority_level - 1];
            tcb->last_scheduled = std::chrono::steady_clock::now();
        }
    }

    ChunkEntry eof_entry;
    strncpy(eof_entry.filename, fname.c_str(), sizeof(eof_entry.filename)-1);
    eof_entry.file_id  = fid;
    eof_entry.seq_num  = 0;
    eof_entry.data_len = 0;
    eof_entry.is_eof   = true;
    g_chunk_buf.produce(eof_entry);

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
        std::cerr << "Bind failed\n"; return 1;
    }
    listen(server_fd, 10);

    mkdir("received_files", 0777);
    disk_io_start(g_disk_io, g_chunk_buf, "received_files");

    ThreadPool pool(NUM_WORKERS);
    std::thread ager(aging_thread);
    ager.detach();

    std::cout << "=== SERVER STARTED (Weeks 1-5) ===\n"
              << "Workers : " << NUM_WORKERS << "\n"
              << "Port    : " << PORT << "\n"
              << "AES-128-CBC + SHA-256 enabled\n"
              << "Waiting for clients...\n\n";
    std::cout.flush();

    while (true) {
        int client_fd = accept(server_fd, (struct sockaddr*)&address, &addrlen);
        if (client_fd < 0) continue;
        std::cout << "[Server] Client accepted (fd=" << client_fd << ")\n";
        std::cout.flush();
        pool.enqueue([client_fd]() { handle_client(client_fd); });
    }

    close(server_fd);
    return 0;
}
