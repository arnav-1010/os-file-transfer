#pragma once
#include <string>
#include <chrono>

enum class TransferStatus { PENDING, IN_PROGRESS, PAUSED, COMPLETED, FAILED };

struct TCB {
    int         file_id;
    std::string filename;
    long        current_offset;
    int         last_chunk_received;
    int         priority_level;
    TransferStatus status;
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point last_scheduled;

    TCB() : file_id(0), current_offset(0),
            last_chunk_received(0), priority_level(1),
            status(TransferStatus::PENDING),
            start_time(std::chrono::steady_clock::now()),
            last_scheduled(std::chrono::steady_clock::now()) {}

    TCB(int id, const std::string& fname)
        : file_id(id), filename(fname), current_offset(0),
          last_chunk_received(0), priority_level(1),
          status(TransferStatus::PENDING),
          start_time(std::chrono::steady_clock::now()),
          last_scheduled(std::chrono::steady_clock::now()) {}
};
