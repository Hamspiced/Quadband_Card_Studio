#include "runtime.hpp"
#include "platform/discovery.hpp"
#include "platform/bluetooth.hpp"
#include <chrono>
#include <cstdio>
#include <stdexcept>

namespace qb::zero {
Runtime::Runtime(bool simulated, Factory factory, Discoverer discoverer, BluetoothFactory bluetooth)
    : factory_(std::move(factory)), discoverer_(std::move(discoverer)), bluetooth_factory_(std::move(bluetooth)) {
    if (!bluetooth_factory_) bluetooth_factory_ = [](platform::BleSession::Observer observer, platform::Stop stop) {
        return std::make_shared<platform::BleSession>(std::move(observer), std::move(stop));
    };
    state_.simulated = simulated;
    if (!factory_) factory_ = [simulated](const ReaderConfig& config, Progress callback) {
        return simulated ? make_simulated_port(std::move(callback)) : make_linux_port(config, std::move(callback));
    };
    if (!discoverer_) discoverer_ = [simulated](const ReaderConfig& config, const std::function<bool()>& stop) {
        if (!simulated) return platform::discover_linux(config,stop);
        DiscoveryReport report; report.config = config; report.attempted = report.completed = true;
        report.devices[0] = {Detection::Detected,"","Synthetic UHF (simulation)",""};
        report.devices[1] = {Detection::Detected,"","Synthetic NTAG (simulation)",""};
        return report;
    };
    worker_ = std::thread([this] { run(); });
}
Runtime::~Runtime() {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        quit_ = true; ++cancel_epoch_; queued_.reset(); discovery_stop_ = true;
        if (port_) port_->request_cancel();
    }
    changed_.notify_all();
    worker_.join();
}
bool Runtime::connect(const ReaderConfig& config) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (ble_cleanup_failed_ || (state_.connected &&
        (!state_.config.ultra_ble_token.empty() || !config.ultra_ble_token.empty()))) return false;
    if (quit_ || state_.pending || state_.view.inventory || state_.view.review || state_.view.bulk_active) return false;
    queued_.emplace(); queued_->connection = true; queued_->config = config;
    state_.pending = true;
    changed_.notify_all();
    return true;
}
bool Runtime::detect(const ReaderConfig& config) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (ble_cleanup_failed_ || (state_.connected && !state_.config.ultra_ble_token.empty())) return false;
    if (quit_ || state_.pending || state_.view.inventory || state_.view.review || state_.view.bulk_active) return false;
    queued_.emplace(); queued_->connection = queued_->discovery = true; queued_->config = config;
    state_.pending = true; state_.detail = "Identifying modules only; no RF/card scan.";
    changed_.notify_all(); return true;
}
bool Runtime::scan_bluetooth() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (quit_ || ble_cleanup_failed_ || state_.pending || state_.connected || state_.cleanup_failed ||
        state_.view.inventory || state_.view.review || state_.view.bulk_active) return false;
    queued_.emplace(); queued_->bluetooth = true;
    state_.pending = true; state_.bluetooth.peers.clear(); state_.bluetooth.pin = false;
    state_.bluetooth.state = "Checking Bluetooth runtime"; ++state_.bluetooth.revision;
    changed_.notify_all(); return true;
}
bool Runtime::bluetooth_pin(const std::string& pin) {
    std::lock_guard<std::mutex> guard(mutex_);
    return ble_ && state_.bluetooth.pin && ble_->pin(pin);
}
bool Runtime::disconnect() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (quit_ || state_.pending || state_.view.inventory || state_.view.review || state_.view.bulk_active) return false;
    queued_.emplace(); queued_->disconnection = true; state_.pending = true;
    changed_.notify_all(); return true;
}
bool Runtime::submit(const local::Intent& intent) {
    if (intent.command == local::Command::Stop) { stop(); return true; }
    std::lock_guard<std::mutex> guard(mutex_);
    if (quit_ || state_.pending || state_.cleanup_failed || !workbench_ || !state_.connected) return false;
    if (state_.view.inventory && intent.command != local::Command::Exit) return false;
    if (state_.view.inventory && port_) port_->request_cancel();
    queued_.emplace(); queued_->intent = intent; state_.pending = true;
    changed_.notify_all();
    return true;
}
void Runtime::stop() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (quit_) return;
    cancel_ = true; ++cancel_epoch_; queued_.reset(); state_.pending = true; discovery_stop_ = true;
    if (port_) port_->request_cancel();
    changed_.notify_all();
}
Snapshot Runtime::snapshot() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return state_;
}
bool Runtime::wait_idle(unsigned milliseconds) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, std::chrono::milliseconds(milliseconds), [this] { return !state_.pending; });
}
void Runtime::publish() {
    if (!workbench_) return;
    if (workbench_->view().status == WRITE_UNVERIFIED) unverified_ = true;
    std::lock_guard<std::mutex> guard(mutex_);
    state_.view = workbench_->view();
    if (unverified_ && state_.view.status != WRITE_UNVERIFIED) {
        state_.view.status = WRITE_UNVERIFIED;
        std::snprintf(state_.view.message.data(), state_.view.message.size(),
            "Previous write outcome unverified. Read target; Clear session acknowledges this warning.");
    }
}
void Runtime::progress(unsigned done, unsigned total, const char* detail) {
    if (!workbench_) return;
    workbench_->progress(done, total, detail);
    publish();
}
bool Runtime::cleanup() noexcept {
    if (!port_) return true;
    Status status = IO_ERROR;
    try { status = port_->release(); } catch (...) {}
    if (status == OK) return true;
    try { if (workbench_) { workbench_->cancel(); publish(); } } catch (...) {}
    std::lock_guard<std::mutex> guard(mutex_);
    state_.cleanup_failed = true; state_.connected = false;
    state_.view.busy = state_.view.inventory = state_.view.review = false;
    state_.view.review_token = 0;
    if (state_.view.status != WRITE_UNVERIFIED) state_.view.status = status;
    ++state_.view.revision;
    state_.detail = "Reader release unverified. Check hardware and reconnect before further operations.";
    return false;
}
void Runtime::run() {
    for (;;) {
        std::optional<Job> job;
        bool cancel = false, exit = false, inventory = false;
        uint64_t epoch;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait_for(lock, std::chrono::milliseconds(100), [this] { return quit_ || cancel_ || queued_; });
            exit = quit_; cancel = cancel_; cancel_ = false; epoch = cancel_epoch_;
            if (queued_) { job = std::move(queued_); queued_.reset(); }
            inventory = state_.view.inventory && !state_.cleanup_failed;
        }
        if (exit) { cleanup(); break; }
        try {
            if (job && job->disconnection) {
                cleanup();
                const bool ble_clean = !ble_ || ble_->disconnect() == OK;
                std::unique_ptr<ReaderPort> previous;
                std::shared_ptr<platform::BleSession> old_ble;
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    previous = std::move(port_); old_ble = std::move(ble_);
                    if (!ble_clean) ble_cleanup_failed_ = state_.cleanup_failed = true;
                    state_.connected = false; state_.bluetooth = {};
                    state_.bluetooth.state = "Disconnected; no automatic reconnect";
                    if (!ble_clean) state_.bluetooth.state = "Bluetooth cleanup unverified; restart application";
                    ++state_.bluetooth.revision;
                    state_.config.ultra_ble_token.clear(); state_.config.ble_session.reset();
                    auto revision = state_.view.revision + 1;
                    state_.view = {}; state_.view.revision = revision;
                    if (unverified_) state_.view.status = WRITE_UNVERIFIED;
                }
                workbench_.reset(); previous.reset();
                if (old_ble) old_ble->shutdown();
            } else if (job && job->bluetooth) {
                discovery_stop_ = false;
                auto next = bluetooth_factory_([this](const std::string& state) {
                    std::lock_guard<std::mutex> guard(mutex_);
                    state_.bluetooth.state = state; state_.bluetooth.pin = state == "pin";
                    ++state_.bluetooth.revision;
                }, [this] { return discovery_stop_.load(); });
                {
                    std::lock_guard<std::mutex> guard(mutex_); ble_ = next;
                }
                std::string detail;
                auto status = next->preflight(detail);
                std::vector<BlePeer> peers;
                if (status == OK) {
                    status = next->scan(peers);
                    if (status != OK) detail = status == CANCELLED ? "Bluetooth scan canceled; scan again explicitly" :
                        "Bluetooth scan failed; check adapter power, BlueZ permissions and runtime dependencies.";
                }
                std::lock_guard<std::mutex> guard(mutex_);
                state_.bluetooth.peers = std::move(peers); state_.bluetooth.pin = false;
                state_.bluetooth.state = status == OK ? "Select a reader; names are not trusted identity" : detail;
                state_.detail = state_.bluetooth.state; ++state_.bluetooth.revision;
            } else if (cancel && workbench_) {
                workbench_->cancel();
                if (cleanup()) {
                    {
                        std::lock_guard<std::mutex> guard(mutex_);
                        if (!quit_ && !cancel_) port_->resume();
                    }
                    workbench_->refresh(); publish();
                }
            } else if (job && job->connection) {
                discovery_stop_ = false;
                if (!job->config.ultra_ble_token.empty()) {
                    std::lock_guard<std::mutex> guard(mutex_);
                    bool selected = false;
                    for (const auto& peer : state_.bluetooth.peers)
                        if (peer.token == job->config.ultra_ble_token) selected = true;
                    if (!selected || !ble_) throw std::runtime_error("Expired Bluetooth selection.");
                    job->config.ble_session = ble_;
                    state_.bluetooth.peers.clear();
                }
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    if (cancel_epoch_ != epoch || quit_) throw std::runtime_error("Connection canceled.");
                    if (port_) port_->resume();
                }
                if (!cleanup()) throw std::runtime_error("Previous reader release failed.");
                if (job->discovery) {
                    std::unique_ptr<ReaderPort> previous;
                    {
                        std::lock_guard<std::mutex> guard(mutex_);
                        if (cancel_epoch_ != epoch || quit_) throw std::runtime_error("Discovery canceled.");
                        previous = std::move(port_); state_.connected = false;
                        discovery_stop_ = false;
                        const auto revision = state_.view.revision + 1;
                        state_.view = {}; state_.view.revision = revision;
                        if (unverified_) state_.view.status = WRITE_UNVERIFIED;
                    }
                    workbench_.reset();
                    previous.reset();
                    auto report = discoverer_(job->config,[this] { return discovery_stop_.load(); });
                    job->config = report.config;
                    {
                        std::lock_guard<std::mutex> guard(mutex_);
                        report.sequence = state_.discovery.sequence + 1;
                        state_.discovery = std::move(report); state_.config = job->config;
                        if (cancel_epoch_ != epoch || quit_) throw std::runtime_error("Discovery canceled.");
                        state_.detail = "Module discovery finished. Card scans require an explicit action.";
                    }
                }
                const bool have_endpoints = !job->config.hf_i2c.empty() || !job->config.uhf_serial.empty() ||
                    !job->config.ultra_serial.empty() || !job->config.ultra_ble_token.empty() || state_.simulated;
                if (job->discovery && (!have_endpoints || !state_.discovery.completed)) {
                    std::lock_guard<std::mutex> guard(mutex_);
                    state_.view.status = unverified_ ? WRITE_UNVERIFIED : UNAVAILABLE;
                    state_.cleanup_failed = false; state_.pending = cancel_ || bool(queued_);
                    changed_.notify_all(); continue;
                }
                auto next = factory_(job->config, [this](unsigned n, unsigned total, const char* text) {
                    progress(n, total, text);
                });
                // Keep the old port alive until its workbench and snapshots are gone.
                std::unique_ptr<ReaderPort> previous;
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    if (cancel_epoch_ != epoch || quit_) {
                        if (next) next->request_cancel();
                        throw std::runtime_error("Connection canceled.");
                    }
                    previous = std::move(port_);
                    port_ = std::move(next);
                    state_.config = job->config; state_.connected = false;
                    state_.cleanup_failed = false;
                }
                workbench_.reset(); previous.reset();
                if (!port_) throw std::runtime_error("Reader adapter allocation failed.");
                const auto status = port_->connect();
                workbench_ = std::make_unique<local::Workbench>(*port_);
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    state_.connected = status == OK;
                    state_.detail = port_->detail();
                    if (job->config.ble_session && job->config.ble_session->cleanup_unverified()) {
                        ble_cleanup_failed_ = state_.cleanup_failed = true;
                        state_.connected = false;
                        state_.detail = "Bluetooth cleanup unverified; restart before any transport change.";
                    }
                    if (!job->config.ultra_ble_token.empty()) {
                        state_.bluetooth.pin = false;
                        state_.bluetooth.state = status == OK ? "Ready: validated Chameleon Ultra (Bluetooth)" :
                            "Bluetooth validation failed; disconnect and scan again";
                        ++state_.bluetooth.revision;
                    }
                }
                publish();
                if (status != OK) {
                    std::lock_guard<std::mutex> guard(mutex_);
                    state_.view.status = status; ++state_.view.revision;
                }
                if (status != OK) cleanup();
            } else if (cancel && ble_) {
                ble_->shutdown();
                std::lock_guard<std::mutex> guard(mutex_);
                state_.bluetooth.peers.clear(); state_.bluetooth.pin = false;
                state_.bluetooth.state = "Bluetooth canceled; scan again"; ++state_.bluetooth.revision;
            } else if (job && workbench_) {
                bool allowed;
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    allowed = cancel_epoch_ == epoch && !quit_;
                    if (allowed) port_->resume();
                    state_.view.busy = true; state_.view.review = false; state_.view.review_token = 0;
                    ++state_.view.revision;
                    std::snprintf(state_.view.message.data(), state_.view.message.size(), "Working. Stop at a safe boundary.");
                }
                if (allowed) {
                    const auto status = workbench_->process(job->intent);
                    if (status == OK && job->intent.command == local::Command::ClearSession) unverified_ = false;
                } else workbench_->cancel();
                publish();
                if (!workbench_->view().inventory) cleanup();
            } else if (inventory && workbench_) {
                workbench_->tick(); publish();
                if (!workbench_->view().inventory) cleanup();
            } else if (ble_ && !ble_->healthy()) {
                std::lock_guard<std::mutex> guard(mutex_);
                if (!state_.config.ultra_ble_token.empty() && state_.connected) {
                    ble_cleanup_failed_ = state_.cleanup_failed = true;
                    state_.connected = false; state_.view.reader_present = false;
                    state_.view.review = false; state_.view.review_token = 0;
                    state_.view.status = unverified_ ? WRITE_UNVERIFIED : UNAVAILABLE;
                    ++state_.view.revision;
                    state_.bluetooth.state = "Bluetooth disconnected; cleanup unverified, restart required";
                    state_.bluetooth.pin = false; state_.bluetooth.peers.clear();
                    ++state_.bluetooth.revision;
                }
            }
        } catch (...) {
            if (job && job->intent.command == local::Command::Confirm) unverified_ = true;
            try { if (workbench_) workbench_->cancel(); } catch (...) {}
            try { cleanup(); } catch (...) {}
            std::lock_guard<std::mutex> guard(mutex_);
            state_.view.busy = state_.view.review = state_.view.inventory = false;
            state_.view.review_token = 0; ++state_.view.revision;
            const bool unverified = state_.view.status == WRITE_UNVERIFIED ||
                (job && job->intent.command == local::Command::Confirm);
            state_.view.status = unverified ? WRITE_UNVERIFIED : IO_ERROR;
            if (!state_.cleanup_failed)
                state_.detail = "Operation failed or canceled. Read the target again before any new write review.";
        }
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (ble_ && ble_->cleanup_unverified()) {
                ble_cleanup_failed_ = state_.cleanup_failed = true;
                state_.connected = false; state_.bluetooth.pin = false; state_.bluetooth.peers.clear();
                state_.view.reader_present = false; state_.view.review = false; state_.view.review_token = 0;
                state_.bluetooth.state = "Bluetooth cleanup unverified; restart application";
                state_.detail = state_.bluetooth.state;
            }
            state_.pending = bool(queued_) || cancel_;
        }
        changed_.notify_all();
    }
}
}
