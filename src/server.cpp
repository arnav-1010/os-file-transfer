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

#define PORT         8080
#define CHUNK_SIZE   4096
#define END_SIGNAL   "END_OF_FILE"
#define PAUSE_SIGNAL "PAUSE_TRANSFER"
#define NUM_WORKERS  4

std::atomic<int> next_file_id(1);
AMFLQScheduler   scheduler;

void handle_client(int client_fd) {
    // ── 1. Receive filename ──────────────────────────────────────────────────
    char filename[256] = {0};
    int n = recv(client_fd, filename, sizeof(filename) - 1, 0);
    if (n <= 0) { close(client_fd); return; }
    std::string fname(filename);

    // ── 2. Set up TCB (resume from checkpoint if one exists) ─────────────────
    int fid = next_file_id++;
    auto tcb = load_checkpoint(fid, fname);
    if (!tcb) {
        tcb = std::make_shared<TCB>(fid, fname);
    }
    tcb->status        = TransferStatus::IN_PROGRESS;
    tcb->last_scheduled = std::chrono::steady_clock::now();

    std::cout << "[TCB #" << tcb->file_id << "] Transfer started: "
              << fname << " (Q" << tcb->priority_level << ")\n";
    std::cout.flush();

    // ── 3. Open output file ───────────────────────────────────────────────────
    mkdir("received_files", 0777);
    std::string filepath = "received_files/" + fname;
    std::ofstream outfile(filepath,
        std::ios::binary |
        (tcb->current_offset > 0 ? std::ios::app : std::ios::trunc));

    if (!outfile) {
        std::cerr << "[TCB #" << tcb->file_id << "] Cannot open output file: "
                  << filepath << "\n";
        close(client_fd);
        return;
    }

    // ── 4. Receive loop ───────────────────────────────────────────────────────
    char buffer[CHUNK_SIZE + 64];
    int  chunks_this_quantum = 0;
    int  quantum  = QUEUE_QUANTUM[tcb->priority_level - 1];
    bool paused   = false;

    while (true) {
        memset(buffer, 0, sizeof(buffer));
        int bytes = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

        if (bytes <= 0) {
            // Client disconnected — save checkpoint so we can resume later
            std::cout << "[TCB #" << tcb->file_id
                      << "] Client disconnected at chunk "
                      << tcb->last_chunk_received << " — checkpoint saved\n";
            std::cout.flush();
            save_checkpoint(*tcb);
            break;
        }

        // ── Check for END signal ─────────────────────────────────────────────
        // The END signal arrives as its own recv() after a 100ms delay,
        // but be safe: check if this recv contains it anywhere.
        if (bytes == (int)strlen(END_SIGNAL) &&
            memcmp(buffer, END_SIGNAL, strlen(END_SIGNAL)) == 0) {

            tcb->status = TransferStatus::COMPLETED;
            delete_checkpoint(tcb->file_id);

            auto end = std::chrono::steady_clock::now();
            double s = std::chrono::duration<double>(
                           end - tcb->start_time).count();
            if (s <= 0) s = 0.001;

            std::cout << "[TCB #" << tcb->file_id << "] COMPLETED: " << fname << "\n"
                      << "  Chunks : " << tcb->last_chunk_received << "\n"
                      << "  Bytes  : " << tcb->current_offset      << "\n"
                      << "  Time   : " << s << "s\n"
                      << "  Speed  : "
                      << (tcb->current_offset / 1024.0 / s) << " KB/s\n";
            std::cout.flush();
            break;
        }

        // ── Check for PAUSE signal ───────────────────────────────────────────
        if (bytes == (int)strlen(PAUSE_SIGNAL) &&
            memcmp(buffer, PAUSE_SIGNAL, strlen(PAUSE_SIGNAL)) == 0) {

            tcb->status = TransferStatus::PAUSED;
            save_checkpoint(*tcb);
            std::cout << "[TCB #" << tcb->file_id << "] PAUSED at chunk "
                      << tcb->last_chunk_received << "\n";
            std::cout.flush();
            paused = true;
            break;
        }

        // ── Normal data chunk ────────────────────────────────────────────────
        outfile.write(buffer, bytes);
        outfile.flush();
        tcb->current_offset      += bytes;
        tcb->last_chunk_received++;
        chunks_this_quantum++;

        std::cout << "[TCB #" << tcb->file_id << "] Chunk "
                  << tcb->last_chunk_received
                  << " (" << bytes << " bytes, Q" << tcb->priority_level << ")\n";
        std::cout.flush();

        // ── AMLFQ quantum exhausted → demote and checkpoint ──────────────────
        if (chunks_this_quantum >= quantum) {
            save_checkpoint(*tcb);
            scheduler.requeue(tcb, true);   // demotes priority_level

            std::cout << "[TCB #" << tcb->file_id
                      << "] Quantum exhausted → demoted to Q"
                      << tcb->priority_level << ", checkpointed\n";
            std::cout.flush();

            chunks_this_quantum = 0;
            quantum = QUEUE_QUANTUM[tcb->priority_level - 1];
            tcb->last_scheduled = std::chrono::steady_clock::now();
        }
    }

    outfile.close();
    close(client_fd);
}

// Background aging thread — runs every 5s, promotes starved transfers
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
    if (server_fd < 0) {
        std::cerr << "Socket creation failed\n";
        return 1;
    }

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

    ThreadPool pool(NUM_WORKERS);

    std::thread ager(aging_thread);
    ager.detach();

    std::cout << "=== SERVER STARTED (AMLFQ Mode) ===\n"
              << "Workers  : " << NUM_WORKERS   << "\n"
              << "Port     : " << PORT           << "\n"
              << "Queues   : " << NUM_QUEUES
              << " (quanta: 8 / 16 / 32 chunks)\n"
              << "Waiting for clients...\n\n";
    std::cout.flush();

    while (true) {
        int client_fd = accept(server_fd, (struct sockaddr*)&address, &addrlen);
        if (client_fd < 0) continue;
        std::cout << "[Server] New client accepted (fd=" << client_fd << ")\n";
        std::cout.flush();
        pool.enqueue([client_fd]() { handle_client(client_fd); });
    }

    close(server_fd);
    return 0;
}
