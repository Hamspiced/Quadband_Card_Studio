file(GLOB UI_SOURCES "ui/*.cpp")
file(GLOB LOCAL_SOURCES "standalone/*.cpp")
idf_component_register(SRCS "main.cpp" "bridge.cpp" "hardware.cpp" "ble.cpp" "workbench.cpp" "local_runtime.cpp" ${UI_SOURCES} ${LOCAL_SOURCES}
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash esp_driver_uart esp_timer esp_hw_support json
             M5Unified M5Unit-RFID M5Unit-NFC M5UnitUnified usb_host_cdc_acm)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
