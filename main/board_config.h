#pragma once

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32C5

#define BOARD_SOC_NAME      "ESP32-C5"
#define BOARD_NAME          "ESP32-C5-KITC-A V1.2"

#define LED_STRIP_GPIO      27
#define LED_STRIP_PIXELS    1

#define BOOT_BUTTON_GPIO    28

#define ETH_MOSI_GPIO       4
#define ETH_MISO_GPIO       5
#define ETH_SCLK_GPIO       6
#define ETH_CS_GPIO         23
#define ETH_INT_GPIO        24
#define ETH_RST_GPIO        25

#elif CONFIG_IDF_TARGET_ESP32C6

#define BOARD_SOC_NAME      "ESP32-C6"
#define BOARD_NAME          "ESP32-C6 Super Mini"

#define LED_STRIP_GPIO      8
#define LED_STRIP_PIXELS    1

#define BOOT_BUTTON_GPIO    9

#define ETH_MOSI_GPIO       2
#define ETH_MISO_GPIO       3
#define ETH_SCLK_GPIO       6
#define ETH_CS_GPIO         7
#define ETH_INT_GPIO        18
#define ETH_RST_GPIO        14

#else
#error "Unsupported ESP-IDF target. This project supports ESP32-C5 and ESP32-C6."
#endif

#define ETH_SPI_HOST        SPI2_HOST
#define ETH_SPI_CLOCK_HZ    (20 * 1000 * 1000)
