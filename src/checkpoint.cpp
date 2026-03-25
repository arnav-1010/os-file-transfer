#include "../include/checkpoint.h"
#include <fstream>
#include <iostream>
#include <sys/stat.h>

static std::string ckpt_path(int file_id) {
    return "checkpoints/" + std::to_string(file_id) + ".ckpt";
}

void save_checkpoint(const TCB& tcb) {
    mkdir("checkpoints", 0777);
    std::ofstream f(ckpt_path(tcb.file_id));
    if (!f) return;
    f << tcb.file_id        << "\n"
      << tcb.filename       << "\n"
      << tcb.current_offset << "\n"
      << tcb.last_chunk_received << "\n"
      << tcb.priority_level << "\n";
    std::cout << "[Checkpoint] Saved TCB #" << tcb.file_id
              << " at offset " << tcb.current_offset << "\n";
}

std::shared_ptr<TCB> load_checkpoint(int file_id, const std::string& filename) {
    std::ifstream f(ckpt_path(file_id));
    if (!f) return nullptr;

    auto tcb = std::make_shared<TCB>();
    std::string fname;
    f >> tcb->file_id >> fname
      >> tcb->current_offset
      >> tcb->last_chunk_received
      >> tcb->priority_level;
    tcb->filename = fname;
    tcb->status   = TransferStatus::PAUSED;

    std::cout << "[Checkpoint] Resumed TCB #" << tcb->file_id
              << " from offset " << tcb->current_offset << "\n";
    return tcb;
}

void delete_checkpoint(int file_id) {
    std::string path = ckpt_path(file_id);
    std::remove(path.c_str());
    std::cout << "[Checkpoint] Deleted checkpoint for TCB #" << file_id << "\n";
}
