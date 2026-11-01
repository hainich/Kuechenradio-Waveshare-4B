#include "radio_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "radio_diag.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static radio_settings_t s_cfg;
static SemaphoreHandle_t s_mu;
static SemaphoreHandle_t s_flash_mu;
/* UI changes are saved outside LVGL in the existing low-priority Wi-Fi
 * housekeeping task. Do not allocate another FreeRTOS stack in scarce SRAM. */
static uint32_t s_ui_change_serial;
static uint32_t s_ui_saved_serial;
static int64_t s_ui_last_change_us;
#define UI_NVS_QUIET_US (8000000LL)
static uint32_t s_revision = 1;
static const char *TAG = "radio_cfg";

static const radio_preset_t defaults[RADIO_PRESETS] = {
    {"SRF 1 Ostschweiz", "http://stream.srg-ssr.ch/srgssr/regi_ost/mp3/128"},
    {"SRF 3", "http://stream.srg-ssr.ch/srgssr/srf3/mp3/128"},
    {"Swiss Pop", "http://stream.srg-ssr.ch/srgssr/rsp/mp3/128"},
    {"SRF 2 Kultur", "http://stream.srg-ssr.ch/srgssr/srf2/mp3/128"},
    {"Swiss Jazz", "http://stream.srg-ssr.ch/srgssr/rsj/mp3/128"},
    {"Radio Paradise", "http://stream-uk1.radioparadise.com/mp3-128"},
};

void radio_config_init(void)
{
    s_mu = xSemaphoreCreateMutex();
    s_flash_mu = xSemaphoreCreateMutex();
    if (!s_mu || !s_flash_mu) abort();
    memset(&s_cfg, 0, sizeof(s_cfg));
    memcpy(s_cfg.stations, defaults, sizeof(defaults));
    s_cfg.volume = 45;
    s_cfg.playing = true;
    nvs_handle_t h;
    if (nvs_open("kitchenradio", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_cfg.ssid);
    nvs_get_str(h, "ssid", s_cfg.ssid, &len);
    len = sizeof(s_cfg.password);
    nvs_get_str(h, "wifi_pass", s_cfg.password, &len);
    uint8_t v;
    if (nvs_get_u8(h, "station", &v) == ESP_OK && v < RADIO_PRESETS) s_cfg.station = v;
    if (nvs_get_u8(h, "volume", &v) == ESP_OK && v <= 100) s_cfg.volume = v;
    if (nvs_get_u8(h, "playing", &v) == ESP_OK) s_cfg.playing = (v != 0);
    for (int i = 0; i < RADIO_PRESETS; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "name%d", i);
        len = sizeof(s_cfg.stations[i].name);
        nvs_get_str(h, key, s_cfg.stations[i].name, &len);
        snprintf(key, sizeof(key), "url%d", i);
        len = sizeof(s_cfg.stations[i].url);
        nvs_get_str(h, key, s_cfg.stations[i].url, &len);
    }
    nvs_close(h);
}

void radio_config_get(radio_settings_t *out)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    memcpy(out, &s_cfg, sizeof(*out));
    xSemaphoreGive(s_mu);
}

/* Audio requires only this one-byte value, not a complete settings struct
 * containing six 320-byte URLs and the Wi-Fi credentials per PCM frame. */
uint8_t radio_config_get_volume(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    uint8_t volume = s_cfg.volume;
    xSemaphoreGive(s_mu);
    return volume;
}

static void ui_mark_changed_locked(void)
{
    ++s_ui_change_serial;
    s_ui_last_change_us = esp_timer_get_time();
}

static void persist(const radio_settings_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("kitchenradio", NVS_READWRITE, &h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "NVS: %s", esp_err_to_name(err)); return; }
    nvs_set_str(h, "ssid", cfg->ssid);
    nvs_set_str(h, "wifi_pass", cfg->password);
    nvs_set_u8(h, "station", cfg->station);
    nvs_set_u8(h, "volume", cfg->volume);
    nvs_set_u8(h, "playing", cfg->playing ? 1 : 0);
    for (int i = 0; i < RADIO_PRESETS; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "name%d", i);
        nvs_set_str(h, key, cfg->stations[i].name);
        snprintf(key, sizeof(key), "url%d", i);
        nvs_set_str(h, key, cfg->stations[i].url);
    }
    err = nvs_commit(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "NVS commit: %s", esp_err_to_name(err));
    nvs_close(h);
}

/* Only save the 3 UI settings when the user has stopped interacting.
 * This write is triggered by wifi_fallback_task, never LVGL or the PCM task.
 * It avoids re-writing 6 station URLs and Wi-Fi credentials on every tap. */
static esp_err_t persist_ui(uint8_t station, uint8_t volume, bool playing)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("kitchenradio", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "station", station);
    if (err == ESP_OK) err = nvs_set_u8(h, "volume", volume);
    if (err == ESP_OK) err = nvs_set_u8(h, "playing", playing ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void radio_config_ui_persist_poll(void)
{
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_mu, portMAX_DELAY);
    bool dirty = s_ui_change_serial != s_ui_saved_serial &&
                 now - s_ui_last_change_us >= UI_NVS_QUIET_US;
    xSemaphoreGive(s_mu);
    if (!dirty) return;

    xSemaphoreTake(s_flash_mu, portMAX_DELAY);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    /* Recheck after obtaining both locks, including edits made while a
     * web configuration commit was in progress. */
    now = esp_timer_get_time();
    dirty = s_ui_change_serial != s_ui_saved_serial &&
            now - s_ui_last_change_us >= UI_NVS_QUIET_US;
    uint32_t serial = s_ui_change_serial;
    uint8_t station = s_cfg.station;
    uint8_t volume = s_cfg.volume;
    bool playing = s_cfg.playing;
    xSemaphoreGive(s_mu);
    if (dirty) {
        int64_t started = esp_timer_get_time();
        esp_err_t err = persist_ui(station, volume, playing);
        uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - started) / 1000);
        radio_diag_note(RD_NVS_UI_SAVE, elapsed_ms, err);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NVS UI save: %s", esp_err_to_name(err));
        } else {
            xSemaphoreTake(s_mu, portMAX_DELAY);
            s_ui_saved_serial = serial;
            xSemaphoreGive(s_mu);
        }
    }
    xSemaphoreGive(s_flash_mu);
}

void radio_config_save(const radio_settings_t *cfg)
{
    if (cfg->station >= RADIO_PRESETS || cfg->volume > 100) return;
    /* Full configuration saves come from the web setup, which explicitly
     * restarts the device. They are serialized against pending UI saves. */
    xSemaphoreTake(s_flash_mu, portMAX_DELAY);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_cfg = *cfg;
    s_cfg.ssid[RADIO_SSID_MAX - 1] = 0;
    s_cfg.password[RADIO_PASS_MAX - 1] = 0;
    for (int i = 0; i < RADIO_PRESETS; ++i) {
        s_cfg.stations[i].name[RADIO_NAME_MAX - 1] = 0;
        s_cfg.stations[i].url[RADIO_URL_MAX - 1] = 0;
    }
    ++s_revision;
    persist(&s_cfg);
    s_ui_saved_serial = s_ui_change_serial;
    xSemaphoreGive(s_mu);
    xSemaphoreGive(s_flash_mu);
}

void radio_config_select(int index)
{
    if (index < 0 || index >= RADIO_PRESETS) return;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_cfg.station != index || !s_cfg.playing) {
        s_cfg.station = (uint8_t)index;
        s_cfg.playing = true;
        ++s_revision;
        ui_mark_changed_locked();
    }
    xSemaphoreGive(s_mu);
}

void radio_config_volume(int volume, bool persist_now)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_cfg.volume = (uint8_t)volume;
    if (persist_now) ui_mark_changed_locked();
    xSemaphoreGive(s_mu);
}

void radio_config_play(bool playing)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_cfg.playing != playing) {
        s_cfg.playing = playing;
        ++s_revision;
        ui_mark_changed_locked();
    }
    xSemaphoreGive(s_mu);
}

uint32_t radio_config_revision(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    uint32_t revision = s_revision;
    xSemaphoreGive(s_mu);
    return revision;
}
