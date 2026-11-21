#pragma once
#include "contract.hpp"
#include <array>
#include <optional>
#include <vector>

namespace qb::zero {
class KeyTranslator {
public:
    std::optional<KeyEvent> evdev(unsigned code, int value);
    void reset();
private:
    bool left_shift_ = false, right_shift_ = false, left_control_ = false, right_control_ = false;
    std::array<char, 768> characters_{};
    std::array<Key, 768> actions_{};
};
class Keyboard {
public:
    Keyboard(bool headless, const std::string& device);
    ~Keyboard();
    std::vector<KeyEvent> poll();
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
