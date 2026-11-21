#pragma once
#include "local_model.hpp"

namespace qb {
void local_runtime_initialize();
Status local_runtime_process(const local::Intent&, uint64_t epoch);
Status local_runtime_tick(uint64_t epoch);
Status local_runtime_cancel(uint64_t epoch, bool clear);
void local_runtime_refresh(uint64_t epoch);
void local_runtime_snapshot(local::View&);
bool local_runtime_active();
bool local_runtime_inventory();
Status local_runtime_release_status();
}
