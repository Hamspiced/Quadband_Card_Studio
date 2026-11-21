#pragma once
#include "rfid.hpp"

namespace qb::zero::platform {
// Injection seam used by fake-wire tests; production always uses the POSIX wire.
std::unique_ptr<ReaderPort> make_port(const ReaderConfig&, Progress,
                                     std::unique_ptr<Wire>, std::unique_ptr<Radio> = {});
}
