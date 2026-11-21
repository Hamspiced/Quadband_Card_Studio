#pragma once
#include "../local_model.hpp"
#include <memory>

namespace qb::local {
class Workbench {
public:
    explicit Workbench(Port&);
    ~Workbench();
    Workbench(const Workbench&) = delete;
    Workbench& operator=(const Workbench&) = delete;
    Status process(const Intent&);
    Status tick();
    void feed_uhf(const Bytes&);
    void cancel();
    void clear();
    const View& view() const;
    void progress(unsigned done, unsigned total, const char* detail);
    void refresh();
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
