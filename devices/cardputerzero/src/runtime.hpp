#pragma once
#include "contract.hpp"
#include "standalone/controller.hpp"
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <atomic>

namespace qb::zero {
class Runtime {
public:
    using Factory = std::function<std::unique_ptr<ReaderPort>(const ReaderConfig&, Progress)>;
    using Discoverer = std::function<DiscoveryReport(const ReaderConfig&, const std::function<bool()>&)>;
    explicit Runtime(bool simulated = false, Factory factory = {}, Discoverer discoverer = {});
    ~Runtime();
    bool connect(const ReaderConfig&);
    bool detect(const ReaderConfig&);
    bool submit(const local::Intent&);
    void stop();
    Snapshot snapshot() const;
    bool wait_idle(unsigned milliseconds);
private:
    struct Job {
        bool connection = false;
        bool discovery = false;
        ReaderConfig config;
        local::Intent intent;
    };
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    Snapshot state_;
    Factory factory_;
    Discoverer discoverer_;
    std::atomic<bool> discovery_stop_{false};
    std::unique_ptr<ReaderPort> port_;
    std::unique_ptr<local::Workbench> workbench_;
    std::optional<Job> queued_;
    bool quit_ = false, cancel_ = false;
    uint64_t cancel_epoch_ = 0;
    bool unverified_ = false;
    std::thread worker_;
    void run();
    void publish();
    void progress(unsigned, unsigned, const char*);
    bool cleanup() noexcept;
};
}
