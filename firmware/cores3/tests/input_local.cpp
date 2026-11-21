#include "ui/input.hpp"
#include <cassert>
#include <climits>
#include <cstring>
#include <iostream>
#include <new>
#include <set>
#include <string>
using namespace qb::ui;

static InputKey key(const InputEditor& editor, const char* label) {
    for (size_t i = 0; i < editor.key_count(); ++i)
        if (std::strcmp(editor.keys()[i].label, label) == 0) return editor.keys()[i];
    assert(false);
    return {};
}
static InputResult press(InputEditor& editor, const char* label) {
    auto k = key(editor, label);
    return editor.touch(k.x + k.w / 2, k.y + k.h / 2);
}
static void prepare(InputEditor& editor, unsigned page, const char* initial = "A") {
    editor.open("Input", initial, 256, page < 4 ? InputKind::Text : page == 4 ? InputKind::Hex : InputKind::Number);
    if (page == 1) press(editor, "ABC");
    if (page == 2) press(editor, "123");
    if (page == 3) press(editor, "#+=");
}
static void pixels() {
    unsigned tested = 0;
    for (unsigned page = 0; page < 6; ++page) {
        InputEditor editor;
        prepare(editor, page);
        const auto keys = editor.keys();
        const size_t count = editor.key_count();
        assert(count <= InputEditor::MAX);
        for (size_t i = 0; i < count; ++i) {
            const auto& k = keys[i];
            assert(k.x >= 0 && k.y >= 76 && k.x + k.w <= 320 && k.y + k.h <= 240);
            assert(k.w >= 32 && k.h >= 32 && k.label[0] && std::strlen(k.label) <= 8);
        }
        for (int y = 0; y < 240; ++y) {
            for (int x = 0; x < 320; ++x) {
                const InputKey* hit = nullptr;
                for (size_t i = 0; i < count; ++i) {
                    const auto& k = keys[i];
                    if (x >= k.x && y >= k.y && x < k.x + k.w && y < k.y + k.h) {
                        assert(!hit); hit = &k;
                    }
                }
                prepare(editor, page);
                const std::string before = editor.value();
                const auto result = editor.touch(x, y);
                if (!hit) {
                    assert(result == InputResult::None && editor.value() == before);
                } else if (std::strcmp(hit->label, "Done") == 0) {
                    assert(result == InputResult::Accept && editor.value() == before && editor.active());
                } else if (std::strcmp(hit->label, "Cancel") == 0) {
                    assert(result == InputResult::Cancel && editor.value() == before && editor.active());
                } else {
                    assert(result == InputResult::Changed || result == InputResult::None);
                    if (std::strcmp(hit->label, "Back") == 0 || std::strcmp(hit->label, "Clear") == 0)
                        assert(!editor.length());
                    else if (std::strcmp(hit->label, "Space") == 0)
                        assert(std::string(editor.value()) == before + " ");
                    else if (std::strlen(hit->label) == 1)
                        assert(std::string(editor.value()) == before + hit->label);
                    else assert(editor.value() == before);
                }
                ++tested;
            }
        }
        for (const auto& point : {std::array<int,2>{-1, 100}, {320, 100}, {0, -1}, {10, 240},
                                  {INT_MIN, INT_MAX}, {INT_MAX, INT_MIN}}) {
            assert(editor.touch(point[0], point[1]) == InputResult::None);
        }
        editor.close();
        for (int y = 0; y < 240; ++y) for (int x = 0; x < 320; ++x)
            assert(editor.touch(x, y) == InputResult::None);
    }
    std::cout << "Validated " << tested << " keyboard pixels across six layouts\n";
}
static void characters() {
    std::set<char> typed;
    for (unsigned page = 0; page < 4; ++page) {
        InputEditor editor;
        prepare(editor, page, "");
        const auto keys = editor.keys();
        for (size_t i = 0; i < editor.key_count(); ++i) {
            if (std::strlen(keys[i].label) == 1) typed.insert(keys[i].label[0]);
            if (std::strcmp(keys[i].label, "Space") == 0) typed.insert(' ');
        }
    }
    assert(typed.size() == 95);
    for (int c = 32; c <= 126; ++c) assert(typed.count(char(c)));
    InputEditor editor;
    editor.open("URL", "", 256, InputKind::Text);
    for (const char* label : {"h", "t", "t", "p", "s", "123", ":", "#+=", "/", "/", "abc", "a",
                              ".", "b", "/", "123", "?", "abc", "q", "123", "=", "1", "&",
                              "abc", "x", "123", "=", "%", "2", "0"}) {
        assert(press(editor, label) == InputResult::Changed);
    }
    assert(std::string(editor.value()) == "https://a.b/?q=1&x=%20");
    assert(press(editor, "abc") == InputResult::Changed);
    assert(press(editor, "ABC") == InputResult::Changed);
    assert(press(editor, "Z") == InputResult::Changed);
    assert(press(editor, "abc") == InputResult::Changed);
    assert(press(editor, "z") == InputResult::Changed);
    assert(std::string(editor.value()).substr(editor.length() - 2) == "Zz");
    editor.open("hex", "aBcD01239ef X:\n", 256, InputKind::Hex);
    assert(std::string(editor.value()) == "ABCD01239EF");
    std::set<char> hex;
    for (size_t i = 0; i < editor.key_count(); ++i)
        if (std::strlen(editor.keys()[i].label) == 1) hex.insert(editor.keys()[i].label[0]);
    assert(hex.size() == 16 && !hex.count('G') && !hex.count('-'));
    editor.open("number", "-1.25+3 nope\n", 256, InputKind::Number);
    assert(std::string(editor.value()) == "-1.25+3");
    editor.open("controls\n\t", "a\1b\nc\t\x7f\xff", 256, InputKind::Text);
    assert(std::string(editor.value()) == "abc" && std::string(editor.title()) == "controls");
}
static void bounds_and_secrets() {
    InputEditor editor;
    assert(!editor.active() && !editor.key_count() && !editor.value()[0]);
    const std::string huge(600, 'v');
    editor.open(huge.c_str(), huge.c_str(), 999999, InputKind::Text);
    assert(editor.length() == 256 && editor.limit() == 256 && std::strlen(editor.title()) == 40);
    assert(std::strlen(editor.display()) == 30 && std::string(editor.display()) == "..." + std::string(27, 'v'));
    assert(press(editor, "a") == InputResult::None && editor.length() == 256);
    assert(press(editor, "Back") == InputResult::Changed && editor.length() == 255);
    assert(press(editor, "z") == InputResult::Changed && editor.length() == 256);
    assert(editor.display()[29] == 'z');
    assert(press(editor, "Done") == InputResult::Accept && editor.active() && editor.length() == 256);
    assert(press(editor, "Cancel") == InputResult::Cancel && editor.active() && editor.length() == 256);
    editor.close();
    assert(!editor.active() && editor.limit() == 0 && !editor.key_count());
    for (size_t i = 0; i < 257; ++i) assert(!editor.value()[i]);
    for (size_t i = 0; i < 31; ++i) assert(!editor.display()[i]);
    for (size_t limit : {size_t(0), size_t(1), size_t(16), size_t(255), size_t(256)}) {
        editor.open("bounds", "", limit, InputKind::Text);
        for (size_t i = 0; i < limit; ++i) assert(press(editor, "a") == InputResult::Changed);
        assert(press(editor, "a") == InputResult::None && editor.length() == limit);
        if (limit) assert(press(editor, "Clear") == InputResult::Changed);
        assert(press(editor, "Clear") == InputResult::None && press(editor, "Back") == InputResult::None);
    }
    const std::string secret = "Long sensitive input that must never appear on the display";
    editor.open("Password", secret.c_str(), 256, InputKind::Text, true);
    assert(std::string(editor.value()) == secret);
    assert(std::string(editor.display()) == std::string(30, '*'));
    assert(press(editor, "Back") == InputResult::Changed);
    assert(editor.value()[secret.size() - 1] == 0);
    assert(press(editor, "Clear") == InputResult::Changed && !editor.length() && !editor.display()[0]);
    for (size_t i = 0; i < 257; ++i) assert(!editor.value()[i]);
    editor.open("Secret", "private", 32, InputKind::Text, true);
    editor.clear();
    assert(!editor.title()[0] && !editor.value()[0] && !editor.display()[0] && !editor.active());
    editor.open(nullptr, nullptr, 0, InputKind::Text);
    assert(editor.active() && !editor.title()[0] && !editor.length());
    editor.open("invalid", "foo", 32, static_cast<InputKind>(255));
    assert(!editor.active() && !editor.value()[0]);

    alignas(InputEditor) unsigned char storage[sizeof(InputEditor)]{};
    auto* placed = new (storage) InputEditor;
    placed->open("secret-title", "UNIQUEPASSWORD", 256, InputKind::Text, true);
    const char* value = placed->value();
    const size_t value_offset = reinterpret_cast<const unsigned char*>(value) - storage;
    placed->~InputEditor();
    for (size_t i = 0; i < 257; ++i) assert(storage[value_offset + i] == 0);
}
int main() {
    pixels(); characters(); bounds_and_secrets();
    std::cout << "Input model bounds, ASCII pages, explicit accept/cancel and secret wipe tests passed\n";
}
