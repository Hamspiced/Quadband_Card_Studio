#include "local_runtime.hpp"
#include "runtime.hpp"
#include "standalone/controller.hpp"
#include "standalone/device.hpp"
#include <esp_timer.h>
#include <atomic>
#include <cstdio>
#include <memory>
#include <new>

namespace qb {
namespace {
std::unique_ptr<local::DevicePort> device;
std::unique_ptr<local::Workbench> workbench;
local::View published;
std::array<uint32_t, 3> last_generations{};
std::atomic<bool> workflow_active{false}, inventory_active{false};
Status release_status = OK;
int64_t last_progress = 0;
bool unverified_latched = false;

void overlay_unverified() {
    if (!unverified_latched) return;
    published.status = WRITE_UNVERIFIED;
    std::snprintf(published.message.data(),published.message.size(),
        "Previous write partly changed/unverified. Read target; Clear session acknowledges. No retry.");
}

bool prepares_write(local::Command command) {
    using local::Command;
    switch (command) {
    case Command::Confirm: case Command::HfArmBulk: case Command::HfPrepareBulk:
    case Command::HfPrepareUnit: case Command::HfPrepareCopy: case Command::HfPrepareNdef:
    case Command::UhfPrepareUser: case Command::UhfPrepareEpc: case Command::UhfPreparePower:
    case Command::LfPrepareWrite: return true;
    default: return false;
    }
}

void publish() {
    if (!workbench) return;
    const auto& view = workbench->view();
    if (view.status == WRITE_UNVERIFIED) unverified_latched = true;
    workflow_active = view.inventory || view.review || view.bulk_active;
    inventory_active = view.inventory;
    std::lock_guard<std::mutex> guard(state_mutex);
    published = view;
    overlay_unverified();
}
void failure(Status status, const char* message) {
    if (status == WRITE_UNVERIFIED) unverified_latched = true;
    workflow_active = false; inventory_active = false;
    std::lock_guard<std::mutex> guard(state_mutex);
    published.busy = false; published.inventory = false; published.review = false; published.review_token = 0;
    published.status = status; ++published.revision;
    std::snprintf(published.message.data(), published.message.size(), "%s", message);
    overlay_unverified();
}
void publish_start(local::Command command) {
    using local::Command;
    switch (command) {
    case Command::HfRead: case Command::UhfStart: case Command::UhfRead:
    case Command::UhfPrepareEpc: case Command::UhfReadPower: case Command::UhfPreparePower:
    case Command::LfScan: case Command::LfPrepareWrite: case Command::Confirm:
        break;
    default: return;
    }
    std::lock_guard<std::mutex> guard(state_mutex);
    published = workbench->view();
    published.busy = true; published.review = false; published.review_token = 0;
    published.done = published.total = 0; ++published.revision;
    std::snprintf(published.message.data(), published.message.size(), "Working. Stop at a safe command boundary.");
    overlay_unverified();
}
bool configure(uint64_t epoch) {
    if (!device || !workbench) return false;
    std::array<uint32_t, 3> generations;
    {
        std::lock_guard<std::mutex> guard(state_mutex);
        for (size_t i = 0; i < generations.size(); ++i) generations[i] = slots[i].generation;
    }
    const bool changed = last_generations != std::array<uint32_t, 3>{} && generations != last_generations;
    device->rebind(epoch, generations);
    last_generations = generations;
    if (changed) {
        const auto tool = workbench->view().tool;
        workbench->cancel(); workbench->clear();
        local::Intent open;
        open.command = tool == local::Tool::Hf ? local::Command::OpenHf :
            tool == local::Tool::Uhf ? local::Command::OpenUhf :
            tool == local::Tool::Lf ? local::Command::OpenLf : local::Command::None;
        if (open.command != local::Command::None) workbench->process(open);
        publish();
        failure(STALE_GENERATION, "Reader changed. Re-read and review the target.");
    }
    return !changed;
}
void release_used() {
    release_status = OK;
    if (!device) return;
    const auto used = device->used_channels();
    for (uint8_t channel = 1; channel <= 3; ++channel) {
        if (!(used & (1u << (channel - 1)))) continue;
        if (channel == 2 && workbench && workbench->view().inventory) continue;
        const auto status = hardware_release_channel(channel);
        if (release_status == OK) release_status = status;
    }
    if (release_status != OK) {
        if (workbench) workbench->cancel();
        failure(release_status, "Reader release unverified. Restart Core.");
    }
}
}
void local_runtime_initialize() {
    try {
        device = std::make_unique<local::DevicePort>(0, last_generations,
            [](unsigned done, unsigned total, const char* detail) {
                if (!workbench) return;
                workbench->progress(done, total, detail);
                const auto now = esp_timer_get_time();
                if (done == total || now - last_progress >= 100000) { publish(); last_progress = now; }
            });
        workbench = std::make_unique<local::Workbench>(*device);
        hardware_local_uhf_handler([](const Bytes& data) {
            if (workbench && device && !device->cancelled()) workbench->feed_uhf(data);
        });
        publish();
    } catch (const std::bad_alloc&) {
        workbench.reset(); device.reset();
        failure(UNAVAILABLE, "Insufficient memory for local tools.");
    }
}
Status local_runtime_process(const local::Intent& intent, uint64_t epoch) {
    release_status = OK;
    if (!device || !workbench) { failure(UNAVAILABLE, "Local tools unavailable: memory."); return UNAVAILABLE; }
    if (unverified_latched && prepares_write(intent.command)) {
        publish();
        return WRITE_UNVERIFIED;
    }
    Status status = OK;
    try {
        const bool unchanged = configure(epoch);
        if (!unchanged && intent.command != local::Command::OpenHf && intent.command != local::Command::OpenUhf &&
            intent.command != local::Command::OpenLf && intent.command != local::Command::Exit &&
            intent.command != local::Command::ClearSession) return STALE_GENERATION;
        publish_start(intent.command);
        status = workbench->process(intent);
        publish();
    } catch (const std::exception&) {
        status = intent.command == local::Command::Confirm ? WRITE_UNVERIFIED : IO_ERROR;
        workbench->cancel();
        failure(status, status == WRITE_UNVERIFIED ? "Write outcome unverified. Read before retrying." :
            "Local operation failed. Clear session and retry explicitly.");
    }
    release_used();
    if (intent.command == local::Command::ClearSession && status == OK && release_status == OK) {
        const auto acknowledged = unverified_latched ? hardware_release() : OK;
        if (acknowledged == OK) { unverified_latched = false; publish(); }
        else {
            release_status = acknowledged;
            failure(acknowledged,"Reader release unverified; previous write warning not acknowledged.");
        }
    }
    return status;
}
Status local_runtime_tick(uint64_t epoch) {
    if (!device || !workbench || !inventory_active) return OK;
    Status status = OK;
    try {
        if (!configure(epoch)) return STALE_GENERATION;
        status = workbench->tick();
        publish();
    } catch (const std::exception&) {
        workbench->cancel(); status = IO_ERROR;
        failure(status, "Inventory failed. Restart explicitly.");
    }
    release_used();
    return status;
}
Status local_runtime_cancel(uint64_t epoch, bool clear) {
    if (!device || !workbench) return hardware_release();
    workbench->cancel();
    release_status = hardware_release();
    if (clear) workbench->clear();
    // A cancelled port's temporary generation zero is not a reader hotplug.
    (void)epoch;
    publish();
    if (release_status != OK) failure(release_status, "Reader release unverified. Restart Core.");
    return release_status;
}
void local_runtime_snapshot(local::View& view) {
    std::lock_guard<std::mutex> guard(state_mutex);
    view = published;
}
void local_runtime_refresh(uint64_t epoch) {
    if (!device || !workbench) return;
    try {
        const bool unchanged = configure(epoch);
        workbench->refresh();
        publish();
        if (!unchanged) failure(STALE_GENERATION, "Reader changed. Re-read and review the target.");
    } catch (const std::exception&) { failure(IO_ERROR, "Local refresh failed. Clear session."); }
}
bool local_runtime_active() { return workflow_active; }
bool local_runtime_inventory() { return inventory_active; }
Status local_runtime_release_status() { return release_status; }
}
