#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
cxx="${CXX:-clang++}"
out="${PLATFORM_TEST_OUT:-.build/cardputerzero-platform-tests}"
mkdir -p "$out"
flags=(-std=c++17 -Wall -Wextra -Werror -pthread -fno-omit-frame-pointer
       -fsanitize=address,undefined -Idevices/cardputerzero/src -Ifirmware/cores3/main)
sources=(devices/cardputerzero/src/platform/linux_port.cpp
         devices/cardputerzero/src/platform/posix_wire.cpp
         devices/cardputerzero/src/platform/rfid.cpp firmware/cores3/main/bridge.cpp)
for name in port rfid; do
    "$cxx" "${flags[@]}" "devices/cardputerzero/tests/platform_${name}_test.cpp" "${sources[@]}" -o "$out/platform_${name}_test"
    "$out/platform_${name}_test"
done
"$cxx" "${flags[@]}" -DQB_ZERO_PTY_TEST=1 devices/cardputerzero/tests/platform_pty_test.cpp \
    devices/cardputerzero/src/platform/posix_wire.cpp firmware/cores3/main/bridge.cpp -o "$out/platform_pty_test"
"$out/platform_pty_test"
if [[ "$(uname -s)" == Darwin ]]; then
    "$cxx" "${flags[@]}" -D__linux__=1 -DQB_ZERO_I2C_STUB_TEST=1 -Idevices/cardputerzero/tests/platform_linux_stubs \
        devices/cardputerzero/tests/platform_linux_test.cpp firmware/cores3/main/bridge.cpp -o "$out/platform_linux_test"
else
    "$cxx" "${flags[@]}" -DQB_ZERO_I2C_STUB_TEST=1 \
        devices/cardputerzero/tests/platform_linux_test.cpp firmware/cores3/main/bridge.cpp -o "$out/platform_linux_test"
fi
"$out/platform_linux_test"
