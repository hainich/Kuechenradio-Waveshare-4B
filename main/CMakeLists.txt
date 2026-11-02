# Fetch the actual, inspectable CC0 decoder source at configuration time.
# Pinned to a reviewed upstream Git commit (2026-07-27), not a mutable branch.
# Waveshare BSP components are fetched by the ESP-IDF Component Manager.
set(MINIMP3_H "${CMAKE_CURRENT_LIST_DIR}/vendor/minimp3.h")
if(NOT EXISTS "${MINIMP3_H}")
    message(STATUS "Downloading CC0 minimp3.h source (not a prebuilt binary)")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/lieff/minimp3/ea99364f61c14656440e8d77e9c233ccf3124633/minimp3.h"
        "${MINIMP3_H}"
        STATUS MINIMP3_DOWNLOAD_STATUS
        TIMEOUT 60
        TLS_VERIFY ON
    )
    list(GET MINIMP3_DOWNLOAD_STATUS 0 MINIMP3_DOWNLOAD_CODE)
    if(NOT MINIMP3_DOWNLOAD_CODE EQUAL 0)
        file(REMOVE "${MINIMP3_H}")
        message(FATAL_ERROR "Could not fetch minimp3 source: ${MINIMP3_DOWNLOAD_STATUS}")
    endif()
endif()

idf_component_register(
    SRCS "main.c" "radio_config.c" "radio_stream.c" "radio_wifi.c" "radio_web.c" "radio_lab_web.c" "radio_ui.c" "radio_diag.c" "radio_power_button.c" "url_codec.c"
    INCLUDE_DIRS "." "vendor"
    REQUIRES esp_audio_codec esp_timer esp_driver_i2c nvs_flash esp_wifi esp_event esp_netif esp_http_client esp_http_server esp-tls mbedtls lwip
)
