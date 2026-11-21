#include "input.hpp"
#include <algorithm>

namespace qb::ui {
namespace {
void wipe(void* memory, size_t size) {
    auto* bytes = static_cast<volatile unsigned char*>(memory);
    while (size--) *bytes++ = 0;
}
char upper_hex(char c) { return c >= 'a' && c <= 'f' ? char(c - 'a' + 'A') : c; }
}
InputEditor::~InputEditor() { clear(); }
void InputEditor::clear() {
    wipe(value_.data(), value_.size());
    wipe(title_.data(), title_.size());
    wipe(display_.data(), display_.size());
    wipe(keys_.data(), sizeof(keys_));
    wipe(characters_.data(), characters_.size());
    actions_.fill(Action::Character);
    length_ = limit_ = key_count_ = 0;
    active_ = secret_ = false;
    kind_ = InputKind::Text;
    page_ = Page::Lower;
}
void InputEditor::close() { clear(); }
bool InputEditor::allowed(char c) const {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 32 || byte > 126) return false;
    if (kind_ == InputKind::Text) return true;
    if (kind_ == InputKind::Number) return (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+';
    c = upper_hex(c);
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
}
void InputEditor::open(const char* title, const char* initial, size_t max_length, InputKind kind, bool secret) {
    clear();
    if (kind != InputKind::Text && kind != InputKind::Hex && kind != InputKind::Number) return;
    kind_ = kind;
    secret_ = secret;
    limit_ = std::min(max_length, CAPACITY);
    if (title) {
        size_t n = 0;
        for (size_t i = 0; i < title_.size() - 1 && title[i]; ++i) {
            const auto c = static_cast<unsigned char>(title[i]);
            if (c >= 32 && c <= 126) title_[n++] = title[i];
        }
    }
    if (initial) {
        for (size_t i = 0; i < CAPACITY && initial[i] && length_ < limit_; ++i) {
            if (allowed(initial[i])) value_[length_++] = kind_ == InputKind::Hex ? upper_hex(initial[i]) : initial[i];
        }
    }
    active_ = true;
    update_display();
    layout();
}
void InputEditor::update_display() {
    wipe(display_.data(), display_.size());
    const size_t visible = std::min(length_, DISPLAY_LENGTH);
    if (secret_) {
        std::fill_n(display_.begin(), visible, '*');
    } else if (length_ <= DISPLAY_LENGTH) {
        std::copy_n(value_.begin(), visible, display_.begin());
    } else {
        std::fill_n(display_.begin(), 3, '.');
        std::copy_n(value_.begin() + length_ - (DISPLAY_LENGTH - 3), DISPLAY_LENGTH - 3, display_.begin() + 3);
    }
}
void InputEditor::add(int x, int y, int w, int h, const char* label, Action action, char character) {
    if (key_count_ >= MAX) return;
    auto& key = keys_[key_count_];
    key.x = x; key.y = y; key.w = w; key.h = h;
    for (size_t i = 0; i < sizeof(key.label) - 1 && label[i]; ++i) key.label[i] = label[i];
    actions_[key_count_] = action;
    characters_[key_count_] = character;
    ++key_count_;
}
void InputEditor::layout() {
    keys_.fill(InputKey{});
    actions_.fill(Action::Character);
    characters_.fill(0);
    key_count_ = 0;
    if (!active_) return;
    if (kind_ == InputKind::Text) {
        const char* characters = page_ == Page::Lower ? "abcdefghijklmnopqrstuvwxyz.@/-" :
            page_ == Page::Upper ? "ABCDEFGHIJKLMNOPQRSTUVWXYZ.@/-" :
            page_ == Page::Digits ? "0123456789!\"#$%&'()*+,-.:;=?@_" :
            "[\\]^`{|}~<>/:;@_-=+?!\"#$%&'()*";
        for (unsigned i = 0; i < 30; ++i) {
            char label[2]{characters[i], 0};
            add(int(i % 10) * 32, 76 + int(i / 10) * 32, 32, 32, label, Action::Character, characters[i]);
        }
        const char* labels[]{"ABC", "123", "#+=", "Space", "Back", "Clear"};
        if (page_ != Page::Lower) labels[0] = "abc";
        const Action actions[]{Action::Case, Action::Digits, Action::Symbols, Action::Space, Action::Back, Action::Clear};
        for (unsigned i = 0; i < 6; ++i) {
            const int x = int(i * 320 / 6), right = int((i + 1) * 320 / 6);
            add(x, 172, right - x, 32, labels[i], actions[i]);
        }
    } else {
        const char* characters = kind_ == InputKind::Hex ? "0123456789ABCDEF" : "7894561230.-+";
        const unsigned columns = kind_ == InputKind::Hex ? 8 : 7;
        const unsigned count = kind_ == InputKind::Hex ? 16 : 13;
        for (unsigned i = 0; i < count; ++i) {
            const unsigned column = i % columns;
            const int x = int(column * 320 / columns), right = int((column + 1) * 320 / columns);
            char label[2]{characters[i], 0};
            add(x, 76 + int(i / columns) * 48, right - x, 48, label, Action::Character, characters[i]);
        }
        add(0, 172, 160, 32, "Back", Action::Back);
        add(160, 172, 160, 32, "Clear", Action::Clear);
    }
    add(0, 208, 160, 32, "Cancel", Action::Cancel);
    add(160, 208, 160, 32, "Done", Action::Done);
}
InputResult InputEditor::touch(int x, int y) {
    if (!active_ || x < 0 || y < 0 || x >= 320 || y >= 240) return InputResult::None;
    for (size_t i = 0; i < key_count_; ++i) {
        const auto& key = keys_[i];
        if (x < key.x || y < key.y || x >= key.x + key.w || y >= key.y + key.h) continue;
        const auto action = actions_[i];
        if (action == Action::Done) return InputResult::Accept;
        if (action == Action::Cancel) return InputResult::Cancel;
        if (action == Action::Case || action == Action::Digits || action == Action::Symbols) {
            if (action == Action::Case) page_ = page_ == Page::Lower ? Page::Upper : Page::Lower;
            else page_ = action == Action::Digits ? Page::Digits : Page::Symbols;
            layout();
            return InputResult::Changed;
        }
        if (action == Action::Back) {
            if (!length_) return InputResult::None;
            value_[--length_] = 0;
        } else if (action == Action::Clear) {
            if (!length_) return InputResult::None;
            wipe(value_.data(), value_.size());
            length_ = 0;
        } else {
            const char character = action == Action::Space ? ' ' : characters_[i];
            if (length_ >= limit_ || !allowed(character)) return InputResult::None;
            value_[length_++] = character;
            value_[length_] = 0;
        }
        update_display();
        return InputResult::Changed;
    }
    return InputResult::None;
}
}
