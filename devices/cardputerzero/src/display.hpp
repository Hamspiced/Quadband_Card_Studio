#pragma once
#include "lvgl.h"
#include <memory>
#include <string>

namespace qb::zero {
class Display {
public:
    Display(bool headless, const std::string& framebuffer);
    ~Display();
    bool screenshot(const std::string&) const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
