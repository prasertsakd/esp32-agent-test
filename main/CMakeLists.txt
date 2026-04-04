idf_component_register(
    SRCS
        "main.c"
        "openai_api.c"
        "gpio_ctrl.c"
        "neopixel.c"
        "web_server.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_http_server
        esp_http_client
        spiffs
        driver
        json
        freertos
        log
        esp_netif
        led_strip
        mbedtls
)

spiffs_create_partition_image(spiffs "${CMAKE_CURRENT_SOURCE_DIR}/spiffs" FLASH_IN_PROJECT)
