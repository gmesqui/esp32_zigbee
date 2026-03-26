cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ENV{ESP_MATTER_PATH} AND EXISTS "C:/em/components/esp_matter/CMakeLists.txt")
    set(ENV{ESP_MATTER_PATH} "C:/em")
endif()

if(EXISTS "C:/tools/gn/pkg/gn.exe")
    set(ENV{PATH} "C:/tools/gn/pkg;$ENV{PATH}")
endif()

if(DEFINED ENV{ESP_MATTER_PATH})
    if(NOT DEFINED ENV{ESP_MATTER_DEVICE_PATH})
        set(ENV{ESP_MATTER_DEVICE_PATH} "$ENV{ESP_MATTER_PATH}/device_hal/device/hollow")
    endif()

    set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
    set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)
    set(PIGWEED_ENV_ROOT "${MATTER_SDK_PATH}/third_party/pigweed/repo/environment")

    if(NOT DEFINED ENV{_PW_ACTUAL_ENVIRONMENT_ROOT} AND EXISTS "${PIGWEED_ENV_ROOT}/build_overrides/pigweed_environment.gni")
        set(ENV{_PW_ACTUAL_ENVIRONMENT_ROOT} "${PIGWEED_ENV_ROOT}")
    endif()

    include($ENV{ESP_MATTER_DEVICE_PATH}/esp_matter_device.cmake)

    set(EXTRA_COMPONENT_DIRS
        "${ESP_MATTER_PATH}/components"
        "${MATTER_SDK_PATH}/config/esp32/components"
        "${ESP_MATTER_PATH}/device_hal/device"
        ${extra_components_dirs_append})
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_zigbee2)

if(DEFINED ENV{ESP_MATTER_PATH})
    idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-fpermissive;-Wno-overloaded-virtual" APPEND)
    idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
    idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security;-Wno-error=cpp" APPEND)
endif()
