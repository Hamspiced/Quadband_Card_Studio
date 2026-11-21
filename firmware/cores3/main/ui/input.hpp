#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace qb::ui {
enum class InputKind : uint8_t { Text, Hex, Number };
enum class InputResult : uint8_t { None, Changed, Accept, Cancel };
struct InputKey { int x = 0, y = 0, w = 0, h = 0; char label[9]{}; };

class InputEditor {
public:
    static constexpr size_t MAX = 40;
    static constexpr size_t CAPACITY = 256;
    static constexpr size_t DISPLAY_LENGTH = 30;
    InputEditor() = default;
    InputEditor(const InputEditor&) = delete;
    InputEditor& operator=(const InputEditor&) = delete;
    ~InputEditor();
    void open(const char* title, const char* initial, size_t max_length, InputKind, bool secret = false);
    bool active() const { return active_; }
    void close();
    void clear();
    const char* value() const { return value_.data(); }
    const char* title() const { return title_.data(); }
    const char* display() const { return display_.data(); }
    size_t length() const { return length_; }
    size_t limit() const { return limit_; }
    InputResult touch(int x, int y);
    const std::array<InputKey, MAX>& keys() const { return keys_; }
    size_t key_count() const { return key_count_; }

private:
    enum class Action : uint8_t { Character, Case, Digits, Symbols, Space, Back, Clear, Done, Cancel };
    enum class Page : uint8_t { Lower, Upper, Digits, Symbols };
    std::array<char, CAPACITY + 1> value_{};
    std::array<char, 41> title_{};
    std::array<char, DISPLAY_LENGTH + 1> display_{};
    std::array<InputKey, MAX> keys_{};
    std::array<Action, MAX> actions_{};
    std::array<char, MAX> characters_{};
    size_t length_ = 0, limit_ = 0, key_count_ = 0;
    bool active_ = false, secret_ = false;
    InputKind kind_ = InputKind::Text;
    Page page_ = Page::Lower;
    bool allowed(char) const;
    void update_display();
    void layout();
    void add(int x, int y, int w, int h, const char* label, Action, char character = 0);
};
}
