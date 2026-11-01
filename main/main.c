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
#include "esp_system.h"
#include "bsp/esp32_s3_touch_lcd_4b.h"

/*
 * Waveshare ESP32-S3-Touch-LCD-4B PWRKEY:
 * TCA9554 P4 / EXIO4 (active low), NOT an ESP32 GPIO.
 *
 * Poll at 12.5 Hz with low priority to minimize shared I2C activity.
 * Short press + release => software reboot. Long press remains available
 * to the AXP2101 PMIC's hardware power-management function.
 */
static void power_button_restart_task(void *arg)
{
    (void)arg;
    esp_io_expander_handle_t expander = bsp_io_expander_init();
    if (!expander) {
        ESP_LOGE("kuechenradio", "PWR-Taste: I2C-Expander nicht gefunden");
        vTaskDelete(NULL);
        return;
    }

    const uint32_t key_mask = IO_EXPANDER_PIN_NUM_4;
    if (esp_io_expander_set_dir(expander, key_mask, IO_EXPANDER_INPUT) != ESP_OK) {
        ESP_LOGE("kuechenradio", "PWR-Taste: Eingang EXIO4 konnte nicht aktiviert werden");
        vTaskDelete(NULL);
        return;
    }

    bool ready = false;
    bool was_pressed = false;
    TickType_t pressed_at = 0;
    const TickType_t min_press = pdMS_TO_TICKS(100);
    const TickType_t max_press = pdMS_TO_TICKS(2000);

    for (;;) {
        uint32_t level = key_mask;
        if (esp_io_expander_get_level(expander, key_mask, &level) != ESP_OK) {
            /* Skip failed I2C reads instead of treating them as button presses. */
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        bool pressed = (level & key_mask) == 0;
        TickType_t now = xTaskGetTickCount();

        /* Never restart on a button held before firmware initialization. */
        if (!ready) {
            if (!pressed) ready = true;
        } else if (pressed && !was_pressed) {
            pressed_at = now;
        } else if (!pressed && was_pressed) {
            TickType_t held = now - pressed_at;
            if (held >= min_press && held < max_press) {
                ESP_LOGW("kuechenradio", "PWR-Taste kurz gedrueckt: Neustart");
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
        }
        was_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(80));
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
    if (xTaskCreate(power_button_restart_task, "pwr_restart", 3072, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE("kuechenradio", "PWR-Tasten-Task konnte nicht gestartet werden");
    }
    ESP_LOGI("kuechenradio", "Start abgeschlossen");
}
