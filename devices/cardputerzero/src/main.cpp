#include "display.hpp"
#include "input.hpp"
#include "runtime.hpp"
#include "ui.hpp"
#include "platform/discovery.hpp"
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void signal_handler(int) { interrupted = 1; }
uint32_t ticks() {
    using namespace std::chrono;
    return uint32_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
}
int main(int argc, char** argv) {
    using namespace qb::zero;
    bool simulated = false, device = false, headless = false, autodetect = true, detect_only = false;
    unsigned seconds = 0;
    std::string framebuffer = "/dev/fb0", keyboard, screenshot;
    ReaderConfig config;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            const auto value = [&]() -> std::string {
                if (++i >= argc) throw std::runtime_error("Missing option value.");
                return argv[i];
            };
            if (argument == "--simulate") simulated = true;
            else if (argument == "--device") device = true;
            else if (argument == "--no-autodetect") autodetect = false;
            else if (argument == "--detect-only") detect_only = true;
            else if (argument == "--headless") headless = true;
            else if (argument == "--seconds") {
                const auto text = value();
                size_t used = 0; const auto n = std::stoul(text, &used);
                if (used != text.size() || n > 3600) throw std::runtime_error("Seconds must be 0-3600.");
                seconds = unsigned(n);
            } else if (argument == "--framebuffer") framebuffer = value();
            else if (argument == "--keyboard") keyboard = value();
            else if (argument == "--screenshot") screenshot = value();
            else if (argument == "--hf-i2c") config.hf_i2c = value();
            else if (argument == "--uhf-serial") config.uhf_serial = value();
            else if (argument == "--ultra-serial") config.ultra_serial = value();
            else if (argument == "--grove-i2c-confirmed") config.grove_i2c_confirmed = true;
            else if (argument == "--help") {
                std::puts("Quadband CardputerZero 0.3.0\n"
                    "  --simulate [--headless] [--seconds N] [--screenshot image.ppm]\n"
                    "  --device [--framebuffer PATH] [--keyboard PATH]\n"
                    "  Optional endpoints: --hf-i2c PATH --grove-i2c-confirmed\n"
                    "                      --uhf-serial PATH --ultra-serial PATH\n"
                    "Device launch identifies known modules without scanning cards.\n"
                    "  --no-autodetect disables startup detection; --detect-only prints metadata without opening the screen.\n"
                    "Arrow/Fn keys or Tab select; Enter activates; Esc backs/cancels.\n"
                    "Ctrl+C stops a reader operation; Ctrl+Q exits.");
                return 0;
            } else throw std::runtime_error("Unknown option: " + argument);
        }
        if (simulated == device || (headless && !simulated))
            throw std::runtime_error("Select exactly one of --simulate or --device; headless mode is simulation-only.");
#if QB_DESKTOP
        if (device) throw std::runtime_error("This is a host preview build; use the Linux framebuffer build on the device.");
#endif
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        if (detect_only) {
            Runtime runtime(simulated);
            if (!runtime.detect(config)) throw std::runtime_error("Discovery could not start.");
            for (unsigned n = 0; n < 100 && !runtime.wait_idle(50); ++n)
                if (interrupted) runtime.stop();
            const auto result = runtime.snapshot();
            if (result.pending) { runtime.stop(); throw std::runtime_error("Discovery deadline exceeded."); }
            const char* names[]{"UHF","RFID2","CC1101","Cap NFC"};
            for (unsigned n = 0; n < 4; ++n) {
                const auto& item = result.discovery.devices[n];
                std::printf("%s: %s | %s | %s | %s\n",names[n],platform::detection_name(item.state),
                            item.endpoint.c_str(),item.identity.c_str(),item.detail.c_str());
            }
            std::printf("Reader tools connected: %s\n",result.connected ? "yes" : "no");
            return result.discovery.completed ? 0 : 1;
        }
        lv_init(); lv_tick_set_cb(ticks);
        int status = 0;
        {
            Display display(headless, framebuffer);
            Keyboard keys(headless, keyboard);
            Runtime runtime(simulated);
            bool quit = false;
            Callbacks callbacks;
            callbacks.submit = [&](const qb::local::Intent& intent) { return runtime.submit(intent); };
            callbacks.connect = [&](const ReaderConfig& endpoints) { return runtime.connect(endpoints); };
            callbacks.stop = [&] { runtime.stop(); };
            callbacks.quit = [&] { runtime.stop(); quit = true; };
            callbacks.detect = [&](const ReaderConfig& endpoints) { return runtime.detect(endpoints); };
            ApplicationUi ui(lv_screen_active(), std::move(callbacks));
            if (simulated) {
                runtime.connect(config);
                if (!runtime.wait_idle(2000)) throw std::runtime_error("Simulator initialization timed out.");
            }
            else if (autodetect) runtime.detect(config);
            auto snapshot = runtime.snapshot(); snapshot.config = config; ui.update(snapshot);
            const auto start = std::chrono::steady_clock::now();
            while (!quit && !interrupted) {
                ui.update(runtime.snapshot());
                for (const auto& event : keys.poll()) {
                    ui.update(runtime.snapshot());
                    ui.key(event);
                }
                lv_timer_handler();
                if (seconds && std::chrono::steady_clock::now() - start >= std::chrono::seconds(seconds)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            runtime.stop();
            runtime.wait_idle(2500);
            if (runtime.snapshot().cleanup_failed) {
                std::fputs("Reader cleanup unverified; inspect hardware before reconnecting.\n", stderr);
                status = 3;
            }
            lv_refr_now(nullptr);
            if (!screenshot.empty() && !display.screenshot(screenshot))
                throw std::runtime_error("Cannot save requested screenshot.");
        }
        lv_deinit();
        return status;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "CardputerZero: %s\n", error.what());
        return 1;
    }
}
