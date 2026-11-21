#include <iostream>
#if defined(QB_ZERO_I2C_STUB_TEST)
#include <cassert>
#include <cstdarg>
#include <cstdlib>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
namespace {
unsigned reads = 0, writes = 0, ioctls = 0, timeouts = 0, retries = 0;
bool fail = false;
int stub_ioctl(int, unsigned long operation, ...) {
    ++ioctls; va_list args; va_start(args, operation);
    int result = 0;
    if (operation == I2C_FUNCS) *va_arg(args, unsigned long*) = I2C_FUNC_I2C;
    else if (operation == I2C_TIMEOUT) { assert(va_arg(args, int) == 10); ++timeouts; }
    else if (operation == I2C_RETRIES) { assert(va_arg(args, int) == 0); ++retries; }
    else {
        assert(operation == I2C_RDWR);
        auto* data = va_arg(args, i2c_rdwr_ioctl_data*);
        assert(data->nmsgs == 1 || data->nmsgs == 2);
        for (unsigned i = 0; i < data->nmsgs; ++i) assert(data->msgs[i].addr == 0x28);
        if (data->nmsgs == 2) {
            assert(data->msgs[0].flags == 0 && data->msgs[0].len == 1 && data->msgs[0].buf[0] == 0x37);
            assert(data->msgs[1].flags == I2C_M_RD && data->msgs[1].len == 1);
            data->msgs[1].buf[0] = 0x15; ++reads;
        } else {
            assert(data->msgs[0].flags == 0 && data->msgs[0].len == 2);
            assert(data->msgs[0].buf[0] == 0x14 && data->msgs[0].buf[1] == 0); ++writes;
        }
        result = fail ? -1 : int(data->nmsgs);
    }
    va_end(args); return result;
}
}
// Include a separately named implementation so generic zero_core-linked builds
// cannot accidentally enable actual non-Linux hardware endpoint opening.
#define ioctl stub_ioctl
#define make_posix_wire make_stub_linux_wire
#define uhf_frame stub_uhf_frame
#define ultra_frame stub_ultra_frame
#define ultra_success stub_ultra_success
#include "../src/platform/posix_wire.cpp"
#undef ioctl
#undef make_posix_wire
#undef uhf_frame
#undef ultra_frame
#undef ultra_success
int main() {
    using namespace qb; using namespace qb::zero; using namespace qb::zero::platform;
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    assert(master >= 0 && !grantpt(master) && !unlockpt(master));
    ReaderConfig c; c.hf_i2c = ptsname(master); c.grove_i2c_confirmed = true;
    auto wire = make_stub_linux_wire(); assert(wire->open(c) == OK);
    assert(timeouts == 1 && retries == 1);
    uint8_t version = 0;
    assert(wire->read_register(0x37, version) == OK && version == 0x15);
    assert(wire->write_register(0x14, 0) == OK); assert(reads == 1 && writes == 1);
    fail = true;
    assert(wire->read_register(0x37, version) == IO_ERROR); assert(reads == 2);
    assert(wire->write_register(0x14, 0) == IO_ERROR); assert(writes == 2);
    auto calls = ioctls; assert(wire->write_register(0x80, 0) == UNAVAILABLE); assert(ioctls == calls);
    wire->close(); ::close(master);
    std::cout << "platform_linux_test: Linux I2C branch compiled/executed via stub ioctls; explicit address, repeated-start shape, bounded timeout, zero retries, error propagation passed (not native Linux ABI)\n";
}
#else
int main() {
    std::cout << "platform_linux_test: skipped (run tests/platform_test.sh for explicit stub harness)\n";
}
#endif
