cmake_minimum_required(VERSION 3.16)
set(IDF_COMPONENT_MANAGER OFF)
set(EXTRA_COMPONENT_DIRS
    ".build/deps/M5GFX" ".build/deps/M5Unified"
    ".build/deps/M5Utility" ".build/deps/M5HAL"
    ".build/deps/M5UnitUnified" ".build/deps/M5Unit-NFC"
    ".build/deps/M5Unit-RFID"
    ".build/deps/esp-usb/host/class/cdc/usb_host_cdc_acm")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(quadband_cores3 VERSION 0.5.0)
