#pragma once
// Minimal Linux UAPI shapes/constants for compiling the Linux branch on macOS.
// These test-only declarations are not a Linux toolchain or a device ABI test.
#include <cstdint>
struct i2c_msg { uint16_t addr, flags, len; uint8_t* buf; };
#define I2C_M_RD 0x0001
#define I2C_FUNC_I2C 0x00000001
