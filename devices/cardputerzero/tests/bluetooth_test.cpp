#include "platform/bluetooth.hpp"
#include "runtime.hpp"
#include <cassert>
#include <thread>
using namespace qb;
using namespace qb::zero;
using namespace qb::zero::platform;
int main() {
    auto session = [] { return std::make_unique<BleSession>(BleSession::Observer{}, Stop{}, QB_BLE_FIXTURE); };
    {
        auto s = session();
        std::string detail;
        assert(s->preflight(detail) == OK);
        std::vector<BlePeer> peers;
        assert(s->scan(peers) == OK && peers.size() == 4 && peers[0].name == "One");
        assert(s->connect("arbitrary peer") == INVALID_ARGUMENT);
        assert(s->connect(std::string(16, 'a')) == OK);
        Bytes response;
        assert(s->exchange(ultra_frame(2004), response, 500, [] { return false; }) == OK);
        assert(ultra_success(response));
        assert(!s->pin(std::string(6, '0')));
        assert(s->disconnect() == OK && !s->cleanup_unverified());
    }
    for (auto command : {2000, 2001, 2002, 2005}) {
        auto s = session();
        assert(s->connect(std::string(16, 'a')) == OK);
        Bytes response;
        assert(s->exchange(ultra_frame(command), response, 500, [] { return false; }) != OK);
        assert(!s->healthy());
        assert(s->exchange(ultra_frame(command), response, 500, [] { return false; }) == UNAVAILABLE);
    }
    {
        auto s = session();
        assert(s->connect(std::string(16, 'a')) == OK);
        Bytes response;
        auto start = Clock::now();
        assert(s->exchange(ultra_frame(2003), response, 50, [] { return false; }) == TIMEOUT);
        assert(Clock::now() - start < std::chrono::seconds(1));
    }
    {
        std::unique_ptr<BleSession> s;
        unsigned prompts = 0;
        s = std::make_unique<BleSession>([&](const std::string& state) {
            if (state == "pin") {
                ++prompts;
                assert(!s->pin("invalid"));
                assert(s->pin(std::string(6, '0')));
                assert(!s->pin(std::string(6, '0')));
            }
        }, Stop{}, QB_BLE_FIXTURE);
        assert(s->connect(std::string(16, 'b')) == OK && prompts == 1);
        assert(!s->pin(std::string(6, '0')));
    }
    {
        auto s = session();
        assert(s->connect(std::string(16, 'a')) == OK);
        Bytes response;
        assert(s->exchange(ultra_frame(2003), response, 500, [] { return true; }) == CANCELLED);
        assert(!s->healthy());
    }
    {
        auto s = session();
        assert(s->connect(std::string(16, 'c')) != OK && !s->healthy());
    }
    {
        auto s = session();
        assert(s->connect(std::string(16, 'e')) != OK);
        assert(s->cleanup_unverified());
        assert(s->detail().find("pairing before PIN") != std::string::npos);
        assert(s->detail().find("timeout") != std::string::npos);
    }
    {
        auto s = session();
        assert(s->connect(std::string(16, 'f')) != OK && s->cleanup_unverified());
        assert(s->detail().find("sensitive") == std::string::npos);
        assert(s->detail().find("withheld") != std::string::npos);
    }
    {
        auto s = session();
        assert(s->connect(std::string(16, 'a')) == OK);
        Bytes response;
        assert(s->exchange(ultra_frame(2006), response, 500, [] { return false; }) == OK);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        assert(s->exchange(ultra_frame(2006), response, 500, [] { return false; }) == UNAVAILABLE);
        assert(s->detail().find("between requests") != std::string::npos);
    }
    {
        Runtime runtime(true, [](const ReaderConfig& config, Progress progress) {
            assert(config.ble_session && config.ble_session->connect(config.ultra_ble_token) == OK);
            return make_simulated_port(std::move(progress));
        }, {}, [](BleSession::Observer observer, Stop stop) {
            return std::make_shared<BleSession>(std::move(observer), std::move(stop), QB_BLE_FIXTURE);
        });
        assert(runtime.scan_bluetooth() && runtime.wait_idle(2000));
        auto snapshot = runtime.snapshot();
        assert(snapshot.bluetooth.peers.size() == 4);
        ReaderConfig selected; selected.ultra_ble_token = std::string(16, 'd');
        assert(runtime.connect(selected) && runtime.wait_idle(2000));
        assert(runtime.snapshot().connected);
        assert(!runtime.connect({}) && !runtime.detect({}));
        assert(runtime.disconnect() && runtime.wait_idle(2000));
        snapshot = runtime.snapshot();
        assert(snapshot.cleanup_failed && !snapshot.connected);
        assert(!runtime.connect({}) && !runtime.detect({}) && !runtime.scan_bluetooth());
    }
    {
        Runtime runtime(false, {}, {}, [](BleSession::Observer observer, Stop stop) {
            return std::make_shared<BleSession>(std::move(observer), std::move(stop), QB_BLE_FIXTURE);
        });
        assert(runtime.scan_bluetooth() && runtime.wait_idle(2000));
        ReaderConfig selected; selected.ultra_ble_token = std::string(16, 'e');
        assert(runtime.connect(selected) && runtime.wait_idle(2000));
        const auto snapshot = runtime.snapshot();
        assert(!snapshot.connected && snapshot.cleanup_failed);
        assert(snapshot.detail.find("pairing before PIN") != std::string::npos);
        assert(snapshot.detail.find("timeout") != std::string::npos);
        assert(!runtime.connect({}) && !runtime.detect({}) && !runtime.scan_bluetooth());
    }
    {
        Runtime runtime(true);
        assert(!runtime.bluetooth_pin(std::string(6, '0')));
        assert(runtime.connect({}) && runtime.wait_idle(2000));
        assert(!runtime.scan_bluetooth());
        assert(runtime.disconnect() && runtime.wait_idle(2000));
        auto state = runtime.snapshot();
        assert(!state.connected && !state.view.reader_present);
    }
}
