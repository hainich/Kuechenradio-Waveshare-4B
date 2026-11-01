#include "radio_config.h"
#include "radio_wifi.h"
#include "radio_web.h"
#include "radio_ui.h"
#include "radio_stream.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include <stdlib.h>
#include <time.h>
#include "esp_system.h"

void app_main(void)
{
    ESP_LOGI("kuechenradio", "Reset reason: %d", (int)esp_reset_reason());
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    radio_config_init();
    radio_wifi_start();
    radio_web_start();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    radio_stream_start();
    radio_ui_start();
    /* Leave the BSP expander configuration unchanged: the physical PWR-key
     * wiring is unverified. Do not infer a software reset from EXIO4. */
    ESP_LOGI("kuechenradio", "Start abgeschlossen");
}
