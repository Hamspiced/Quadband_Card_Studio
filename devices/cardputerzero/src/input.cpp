#include "input.hpp"
#include <cstring>
#include <stdexcept>
#if QB_DESKTOP
#include <SDL.h>
#else
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace qb::zero {
void KeyTranslator::reset() {
    left_shift_ = right_shift_ = left_control_ = right_control_ = false;
    characters_.fill(0);
    actions_.fill(Key::Character);
}
std::optional<KeyEvent> KeyTranslator::evdev(unsigned code, int value) {
    if (code >= characters_.size() || value < 0 || value > 2) return {};
    const bool pressed = value != 0;
    if (code == 42) { left_shift_ = pressed; return {}; }
    if (code == 54) { right_shift_ = pressed; return {}; }
    if (code == 29) { left_control_ = pressed; return {}; }
    if (code == 97) { right_control_ = pressed; return {}; }
    const bool shift = left_shift_ || right_shift_;
    KeyEvent event; event.pressed = pressed; event.repeat = value == 2; event.shift = shift;
    if (!pressed && (characters_[code] || actions_[code] != Key::Character)) {
        event.key = actions_[code]; event.character = characters_[code];
        actions_[code] = Key::Character; characters_[code] = 0; return event;
    }
    const auto save = [&](KeyEvent result) -> std::optional<KeyEvent> {
        if (pressed && !result.repeat) { actions_[code] = result.key; characters_[code] = result.character; }
        return result;
    };
    switch (code) {
    case 1: event.key = Key::Escape; return save(event);
    case 14: event.key = Key::Backspace; return save(event);
    case 15: event.key = Key::Tab; return save(event);
    case 28: case 96: event.key = Key::Enter; return save(event);
    case 102: event.key = Key::Home; return save(event);
    case 103: event.key = Key::Up; return save(event);
    case 104: event.key = Key::PageUp; return save(event);
    case 105: event.key = Key::Left; return save(event);
    case 106: event.key = Key::Right; return save(event);
    case 107: event.key = Key::End; return save(event);
    case 108: event.key = Key::Down; return save(event);
    case 109: event.key = Key::PageDown; return save(event);
    case 111: event.key = Key::Delete; return save(event);
    default: break;
    }
    if ((left_control_ || right_control_) && code == 46) { event.key = Key::Stop; return save(event); }
    if ((left_control_ || right_control_) && code == 16) { event.key = Key::Quit; return save(event); }
    char character = 0;
    if (code >= 2 && code <= 11) character = (shift ? "!@#$%^&*()" : "1234567890")[code - 2];
    else if (code >= 16 && code <= 25) character = "qwertyuiop"[code - 16];
    else if (code >= 30 && code <= 38) character = "asdfghjkl"[code - 30];
    else if (code >= 44 && code <= 50) character = "zxcvbnm"[code - 44];
    else {
        switch (code) {
        case 12: character = shift ? '_' : '-'; break;
        case 13: character = shift ? '+' : '='; break;
        case 26: character = shift ? '{' : '['; break;
        case 27: character = shift ? '}' : ']'; break;
        case 39: character = shift ? ':' : ';'; break;
        case 40: character = shift ? '"' : '\''; break;
        case 41: character = shift ? '~' : '`'; break;
        case 43: character = shift ? '|' : '\\'; break;
        case 51: character = shift ? '<' : ','; break;
        case 52: character = shift ? '>' : '.'; break;
        case 53: character = shift ? '?' : '/'; break;
        case 57: character = ' '; break;
        default: break;
        }
    }
    if (character >= 'a' && character <= 'z' && shift) character = char(character - 'a' + 'A');
    if (!pressed && characters_[code]) { character = characters_[code]; characters_[code] = 0; }
    else if (pressed && !event.repeat) characters_[code] = character;
    if (!character) return {};
    event.character = character; return save(event);
}
struct Keyboard::State {
    bool headless;
    KeyTranslator keys;
#if !QB_DESKTOP
    int fd = -1;
    ~State() { if (fd >= 0) close(fd); }
#else
    std::array<char, SDL_NUM_SCANCODES> characters{};
    std::array<Key, SDL_NUM_SCANCODES> actions{};
#endif
    explicit State(bool hidden) : headless(hidden) {}
};
Keyboard::Keyboard(bool headless, const std::string& device) : state_(std::make_unique<State>(headless)) {
#if QB_DESKTOP
    (void)device;
#else
    if (headless) return;
    const char* configured = std::getenv("APPLAUNCH_LINUX_KEYBOARD_DEVICE");
    const std::string path = !device.empty() ? device : configured && *configured ? configured :
        "/dev/input/by-path/platform-3f804000.i2c-event";
    state_->fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (state_->fd < 0) throw std::runtime_error("Cannot open keyboard; set --keyboard and check input-group permissions.");
    unsigned long keys[(KEY_MAX / (8 * sizeof(unsigned long))) + 1]{};
    if (ioctl(state_->fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0)
        throw std::runtime_error("Configured input device does not expose keyboard events.");
    // Leave the launcher its shared fd for long-Escape supervision.
#endif
}
Keyboard::~Keyboard() = default;
std::vector<KeyEvent> Keyboard::poll() {
    std::vector<KeyEvent> result;
    if (state_->headless) return result;
#if QB_DESKTOP
    SDL_Event input;
    while (SDL_PollEvent(&input)) {
        if (input.type == SDL_QUIT) { result.push_back({Key::Quit, 0, true}); continue; }
        if (input.type == SDL_WINDOWEVENT && input.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            for (size_t i = 0; i < state_->actions.size(); ++i) {
                if (state_->characters[i] || state_->actions[i] != Key::Character)
                    result.push_back({state_->actions[i], state_->characters[i], false});
            }
            state_->characters.fill(0);
            state_->actions.fill(Key::Character);
            result.push_back({Key::Character, 0, false}); continue;
        }
        if (input.type != SDL_KEYDOWN && input.type != SDL_KEYUP) continue;
        KeyEvent event; event.pressed = input.type == SDL_KEYDOWN; event.repeat = input.key.repeat;
        event.shift = input.key.keysym.mod & KMOD_SHIFT;
        const auto scan = size_t(input.key.keysym.scancode);
        if (scan >= state_->characters.size()) continue;
        if (!event.pressed && (state_->characters[scan] || state_->actions[scan] != Key::Character)) {
            event.key = state_->actions[scan]; event.character = state_->characters[scan];
            state_->actions[scan] = Key::Character; state_->characters[scan] = 0;
            result.push_back(event); continue;
        }
        switch (input.key.keysym.sym) {
        case SDLK_UP: event.key = Key::Up; break;
        case SDLK_DOWN: event.key = Key::Down; break;
        case SDLK_LEFT: event.key = Key::Left; break;
        case SDLK_RIGHT: event.key = Key::Right; break;
        case SDLK_RETURN: case SDLK_KP_ENTER: event.key = Key::Enter; break;
        case SDLK_ESCAPE: event.key = Key::Escape; break;
        case SDLK_TAB: event.key = Key::Tab; break;
        case SDLK_BACKSPACE: event.key = Key::Backspace; break;
        case SDLK_DELETE: event.key = Key::Delete; break;
        case SDLK_HOME: event.key = Key::Home; break;
        case SDLK_END: event.key = Key::End; break;
        case SDLK_PAGEUP: event.key = Key::PageUp; break;
        case SDLK_PAGEDOWN: event.key = Key::PageDown; break;
        default: {
            if ((input.key.keysym.mod & KMOD_CTRL) && input.key.keysym.sym == SDLK_c) { event.key = Key::Stop; break; }
            if ((input.key.keysym.mod & KMOD_CTRL) && input.key.keysym.sym == SDLK_q) { event.key = Key::Quit; break; }
            const auto code = input.key.keysym.scancode;
            if (input.key.keysym.sym < 32 || input.key.keysym.sym > 126 || code >= SDL_NUM_SCANCODES) continue;
            char character = char(input.key.keysym.sym);
            if (event.shift) {
                const char* plain = "`1234567890-=[]\\;',./";
                const char* shifted = "~!@#$%^&*()_+{}|:\"<>?";
                const char* found = std::strchr(plain, character);
                if (found) character = shifted[found - plain];
                else if (character >= 'a' && character <= 'z') character = char(character - 'a' + 'A');
            }
            if (event.pressed) state_->characters[size_t(code)] = character;
            else {
                if (state_->characters[size_t(code)]) character = state_->characters[size_t(code)];
                state_->characters[size_t(code)] = 0;
            }
            event.character = character; break;
        }
        }
        if (event.pressed && !event.repeat) state_->actions[scan] = event.key;
        result.push_back(event);
    }
#else
    input_event input{};
    for (unsigned n = 0; n < 128; ++n) {
        const auto size = read(state_->fd, &input, sizeof(input));
        if (size < 0 && (errno == EAGAIN || errno == EINTR)) break;
        if (size != sizeof(input)) throw std::runtime_error("Keyboard disconnected; reader cleanup requested.");
        if (input.type == EV_SYN && input.code == SYN_DROPPED) {
            throw std::runtime_error("Keyboard event overflow; stopping rather than accepting stale keys.");
        }
        if (input.type != EV_KEY) continue;
        if (auto event = state_->keys.evdev(input.code, input.value)) result.push_back(*event);
    }
#endif
    return result;
}
}
