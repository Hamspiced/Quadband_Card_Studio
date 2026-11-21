#include "display.hpp"
#include "framebuffer.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#if QB_DESKTOP
#include <SDL.h>
#else
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace qb::zero {
struct Display::State {
    lv_display_t* display = nullptr;
    std::array<uint16_t, 320 * 20> draw{};
    std::array<uint16_t, 320 * 170> pixels{};
    bool headless;
#if QB_DESKTOP
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
#else
    int fd = -1;
    uint8_t* memory = nullptr;
    FramebufferLayout layout;
#endif
    explicit State(bool hidden) : headless(hidden) {}
    ~State() {
        if (display) lv_display_delete(display);
#if QB_DESKTOP
        if (texture) SDL_DestroyTexture(texture);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        if (!headless) SDL_Quit();
#else
        if (memory) munmap(memory, layout.size);
        if (fd >= 0) close(fd);
#endif
    }
    void flush(const lv_area_t* area, uint8_t* bytes) {
        const int width = lv_area_get_width(area);
        const auto* source = reinterpret_cast<const uint16_t*>(bytes);
        for (int y = area->y1; y <= area->y2; ++y) for (int x = area->x1; x <= area->x2; ++x) {
            if (x < 0 || y < 0 || x >= 320 || y >= 170) continue;
            const auto value = source[(y - area->y1) * width + x - area->x1];
            pixels[size_t(y * 320 + x)] = value;
#if !QB_DESKTOP
            if (!headless) {
                const uint32_t native = layout.convert(value);
                auto* destination = memory + size_t(y + layout.yoffset) * layout.stride +
                    size_t(x + layout.xoffset) * (layout.bits / 8);
                std::memcpy(destination, &native, layout.bits / 8);
            }
#endif
        }
#if QB_DESKTOP
        if (!headless && lv_display_flush_is_last(display)) {
            SDL_UpdateTexture(texture, nullptr, pixels.data(), 320 * sizeof(uint16_t));
            SDL_RenderClear(renderer); SDL_RenderCopy(renderer, texture, nullptr, nullptr); SDL_RenderPresent(renderer);
        }
#endif
        lv_display_flush_ready(display);
    }
};
Display::Display(bool headless, const std::string& framebuffer) : state_(std::make_unique<State>(headless)) {
#if QB_DESKTOP
    (void)framebuffer;
    if (!headless) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) throw std::runtime_error(SDL_GetError());
        state_->window = SDL_CreateWindow("Quadband Card Studio - CardputerZero",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 510, 0);
        if (!state_->window) throw std::runtime_error(SDL_GetError());
        state_->renderer = SDL_CreateRenderer(state_->window, -1, SDL_RENDERER_SOFTWARE);
        if (!state_->renderer) throw std::runtime_error(SDL_GetError());
        state_->texture = SDL_CreateTexture(state_->renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, 320, 170);
        if (!state_->texture) throw std::runtime_error(SDL_GetError());
    }
#else
    if (!headless) {
        state_->fd = open(framebuffer.c_str(), O_RDWR | O_CLOEXEC);
        if (state_->fd < 0) throw std::runtime_error("Cannot open configured framebuffer; check video-group permissions.");
        fb_var_screeninfo variable{}; fb_fix_screeninfo fixed{};
        if (ioctl(state_->fd, FBIOGET_VSCREENINFO, &variable) < 0 || ioctl(state_->fd, FBIOGET_FSCREENINFO, &fixed) < 0 ||
            variable.red.msb_right || variable.green.msb_right || variable.blue.msb_right ||
            fixed.type != FB_TYPE_PACKED_PIXELS || fixed.visual != FB_VISUAL_TRUECOLOR)
            throw std::runtime_error("Framebuffer must expose a packed true-color RGB layout.");
        auto& layout = state_->layout;
        layout = {variable.xres, variable.yres, variable.xres_virtual, variable.yres_virtual,
            variable.xoffset, variable.yoffset, fixed.line_length, variable.bits_per_pixel,
            variable.red.offset, variable.green.offset, variable.blue.offset,
            variable.red.length, variable.green.length, variable.blue.length, fixed.smem_len};
        layout.validate();
        void* mapped = mmap(nullptr, layout.size, PROT_READ | PROT_WRITE, MAP_SHARED, state_->fd, 0);
        if (mapped == MAP_FAILED) throw std::runtime_error("Unable to map configured framebuffer.");
        state_->memory = static_cast<uint8_t*>(mapped);
    }
#endif
    state_->display = lv_display_create(320, 170);
    if (!state_->display) throw std::runtime_error("Cannot allocate LVGL display.");
    lv_display_set_color_format(state_->display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(state_->display, state_->draw.data(), nullptr, sizeof(state_->draw), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(state_->display, state_.get());
    lv_display_set_flush_cb(state_->display, [](lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
        static_cast<State*>(lv_display_get_user_data(display))->flush(area, pixels);
    });
}
Display::~Display() = default;
bool Display::screenshot(const std::string& path) const {
    FILE* output = std::fopen(path.c_str(), "wb");
    if (!output) return false;
    bool okay = std::fprintf(output, "P6\n320 170\n255\n") > 0;
    for (uint16_t pixel : state_->pixels) {
        const uint8_t rgb[]{uint8_t((pixel >> 11) * 255 / 31), uint8_t(((pixel >> 5) & 63) * 255 / 63), uint8_t((pixel & 31) * 255 / 31)};
        if (std::fwrite(rgb, 1, sizeof(rgb), output) != sizeof(rgb)) okay = false;
    }
    return std::fclose(output) == 0 && okay;
}
}
