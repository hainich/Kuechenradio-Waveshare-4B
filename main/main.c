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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_system.h"

/*
 * Waveshare ESP32-S3-Touch-LCD-4B BOOT button: GPIO0 (active-low).
 * Restart ONLY after release so the ESP32-S3 does not enter ROM
 * download mode due to the GPIO0 bootstrapping pin being held low.
 */
static void boot_button_restart_task(void *arg)
{
    (void)arg;
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGE("kuechenradio", "BOOT-Taste GPIO0 konnte nicht eingerichtet werden");
        vTaskDelete(NULL);
        return;
    }

    /* Don't interpret a button that was held during power-on as a click. */
    bool released_since_boot = false;
    for (;;) {
        if (gpio_get_level(GPIO_NUM_0) != 0) {
            released_since_boot = true;
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }
        if (!released_since_boot) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        /* Debounce: the key must stay down for >=90ms. */
        vTaskDelay(pdMS_TO_TICKS(90));
        if (gpio_get_level(GPIO_NUM_0) != 0) continue;

        /* Restart only when GPIO0 is HIGH again (normal boot mode). */
        while (gpio_get_level(GPIO_NUM_0) == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        vTaskDelay(pdMS_TO_TICKS(60));
        if (gpio_get_level(GPIO_NUM_0) != 0) {
            ESP_LOGW("kuechenradio", "BOOT-Taste losgelassen: Neustart");
            esp_restart();
        }
    }
}


void app_main(void)
{
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
    if (xTaskCreate(boot_button_restart_task, "boot_restart", 2048, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE("kuechenradio", "BOOT-Tasten-Task konnte nicht gestartet werden");
    }
    ESP_LOGI("kuechenradio", "Start abgeschlossen");
}
