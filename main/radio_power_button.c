#include "radio_power_button.h"
#include "bsp/esp32_s3_touch_lcd_4b.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include <stdint.h>

#define AXP2101_ADDR              0x34
#define AXP2101_IRQ_ENABLE_PKEY   0x41
#define AXP2101_IRQ_STATUS_PKEY   0x49
#define AXP2101_PKEY_SHORT_BIT    0x08

static const char *TAG = "power_button";
static i2c_master_dev_handle_t s_dev;
static const char *s_status = "nicht initialisiert";
static uint32_t s_read_errors = 0;

static esp_err_t pmu_read(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, value, 1, 50);
}
static esp_err_t pmu_write(uint8_t reg, uint8_t value)
{
    const uint8_t cmd[2] = {reg, value};
    return i2c_master_transmit(s_dev, cmd, sizeof(cmd), 50);
}

void radio_power_button_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        s_status = "I2C-Bus nicht verfuegbar";
        return;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        s_status = "AXP2101-Geraet nicht ansprechbar";
        ESP_LOGE(TAG, "PMU add device: %s", esp_err_to_name(err));
        return;
    }

    uint8_t irq_enabled = 0;
    err = pmu_read(AXP2101_IRQ_ENABLE_PKEY, &irq_enabled);
    if (err == ESP_OK) {
        /* Set PKEY short-press event only: preserve all other PMU IRQ bits,
         * power rails, long-press/poweroff behaviour and charge settings. */
        err = pmu_write(AXP2101_IRQ_ENABLE_PKEY,
                        (uint8_t)(irq_enabled | AXP2101_PKEY_SHORT_BIT));
    }
    if (err == ESP_OK) {
        /* PKEY IRQ status is write-one-to-clear. Drop events preceding boot. */
        err = pmu_write(AXP2101_IRQ_STATUS_PKEY, AXP2101_PKEY_SHORT_BIT);
    }
    if (err != ESP_OK) {
        s_status = "AXP2101-IRQ Initialisierung fehlgeschlagen";
        ESP_LOGE(TAG, "PMU PKEY IRQ configuration: %s", esp_err_to_name(err));
        return;
    }
    s_status = "AXP2101 PWR kurz = Neustart (aktiv)";
    ESP_LOGI(TAG, "AXP2101 PKEY short-press IRQ enabled (I2C 0x34)");
}

void radio_power_button_poll(void)
{
    if (!s_dev || s_status[0] != 'A') return;
    uint8_t flags = 0;
    esp_err_t err = pmu_read(AXP2101_IRQ_STATUS_PKEY, &flags);
    if (err != ESP_OK) {
        ++s_read_errors;
        if (s_read_errors == 1 || s_read_errors % 100 == 0)
            ESP_LOGW(TAG, "PMU PKEY poll failed (%lu): %s",
                     (unsigned long)s_read_errors, esp_err_to_name(err));
        return;
    }
    if (flags & AXP2101_PKEY_SHORT_BIT) {
        ESP_LOGW(TAG, "Physical PWR short press -> esp_restart()");
        /* Acknowledge and reboot. Do not touch power regulators. */
        pmu_write(AXP2101_IRQ_STATUS_PKEY, AXP2101_PKEY_SHORT_BIT);
        esp_restart();
    }
}

const char *radio_power_button_status(void)
{
    return s_status;
}
