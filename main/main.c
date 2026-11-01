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
 * TCA9554 P4 / EXIO4; the board signal is active-low.
 *
 * Previous firmware polled every 80ms AND required a press lasting >=100ms,
 * so brief real-world clicks could be missed. Poll every 40ms and debounce
 * with two consistent samples. Restart on release, not on the first edge.
 *
 * NOTE: This only runs after the application starts. It cannot exit the
 * ESP32 ROM download mode immediately after a flash operation.
 */
static void power_button_restart_task(void *arg)
{
    (void)arg;
    esp_io_expander_handle_t expander = bsp_io_expander_init();
    if (!expander) {
        ESP_LOGE("kuechenradio", "PWRKEY: TCA9554 nicht gefunden");
        vTaskDelete(NULL);
        return;
    }

    const uint32_t key_mask = IO_EXPANDER_PIN_NUM_4;
    if (esp_io_expander_set_dir(expander, key_mask, IO_EXPANDER_INPUT) != ESP_OK) {
        ESP_LOGE("kuechenradio", "PWRKEY: EXIO4 nicht als Eingang konfigurierbar");
        vTaskDelete(NULL);
        return;
    }

    bool armed = false;
    bool sampled_pressed = false;
    bool stable_pressed = false;
    unsigned consecutive = 0;
    unsigned valid_released = 0;
    unsigned errors = 0;
    TickType_t pressed_at = 0;
    const TickType_t min_press = pdMS_TO_TICKS(40);
    const TickType_t max_press = pdMS_TO_TICKS(4800);

    for (;;) {
        uint32_t level = 0;
        esp_err_t err = esp_io_expander_get_level(expander, key_mask, &level);
        if (err != ESP_OK) {
            if (++errors == 10) {
                ESP_LOGW("kuechenradio", "PWRKEY: I2C-Abfrage fehlgeschlagen: %s", esp_err_to_name(err));
                errors = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(80));
            continue;
        }
        errors = 0;
        bool pressed = (level & key_mask) == 0;
        if (!armed) {
            if (!pressed && ++valid_released >= 3) {
                armed = true;
                sampled_pressed = false;
                stable_pressed = false;
                consecutive = 0;
                ESP_LOGI("kuechenradio", "PWRKEY EXIO4 bereit (kurzer Druck: Neustart)");
            } else if (pressed) {
                valid_released = 0;
            }
        } else {
            if (pressed != sampled_pressed) {
                sampled_pressed = pressed;
                consecutive = 1;
            } else if (consecutive < 3) {
                ++consecutive;
            }

            if (consecutive >= 2 && sampled_pressed != stable_pressed) {
                stable_pressed = sampled_pressed;
                if (stable_pressed) {
                    pressed_at = xTaskGetTickCount();
                    ESP_LOGI("kuechenradio", "PWRKEY gedrueckt");
                } else {
                    TickType_t held = xTaskGetTickCount() - pressed_at;
                    ESP_LOGI("kuechenradio", "PWRKEY losgelassen (%u ms)",
                             (unsigned)(held * portTICK_PERIOD_MS));
                    if (held >= min_press && held < max_press) {
                        ESP_LOGW("kuechenradio", "PWRKEY Neustart");
                        vTaskDelay(pdMS_TO_TICKS(80));
                        esp_restart();
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(40));
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
