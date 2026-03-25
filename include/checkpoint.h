#pragma once
#include "tcb.h"
#include <string>
#include <memory>

// Saves TCB state to checkpoints/<file_id>.ckpt
void save_checkpoint(const TCB& tcb);

// Loads TCB state from checkpoint file. Returns nullptr if not found.
std::shared_ptr<TCB> load_checkpoint(int file_id, const std::string& filename);

// Deletes checkpoint once transfer is complete
void delete_checkpoint(int file_id);
