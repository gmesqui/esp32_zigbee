idf_component_register(
    SRCS
        "main.c"
        "timebase.c"
        "zb_persistence.c"
        "zb_trace.c"
        "device_table.c"
        "serial_cmd.c"
        "zb_coordinator.c"
        "led_status.c"
    INCLUDE_DIRS
        "."
        "../managed_components/espressif__esp-zigbee-lib/include"
        "../managed_components/espressif__esp-zboss-lib/include"
    REQUIRES nvs_flash esp_timer esp_driver_uart esp_driver_gpio led_strip espressif__esp-zigbee-lib espressif__esp-zboss-lib
)
