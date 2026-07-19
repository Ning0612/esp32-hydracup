file(GLOB_RECURSE HYDRACUP_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/main.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../lib/*/*.cpp"
)

idf_component_register(
    SRCS ${HYDRACUP_SOURCES}
    INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../include"
    REQUIRES
        app_update
        driver
        esp_http_client
        esp_http_server
        esp_event
        esp_netif
        esp_system
        esp_timer
        esp_wifi
        freertos
        json
        log
        lwip
        mbedtls
        mqtt
        nvs_flash
        littlefs
)
