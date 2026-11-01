#include "radio_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static radio_settings_t s_cfg;
static SemaphoreHandle_t s_mu;
static SemaphoreHandle_t s_flash_mu;
static TaskHandle_t s_ui_persist_task;
static void ui_persist_task(void *arg);
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
    /* The UI must never perform synchronous NVS flash writes. A separate
     * task coalesces station/play/volume changes; the web setup keeps its
     * synchronous full save because it reboots shortly afterwards. */
    if (xTaskCreate(ui_persist_task, "radio_nvs", 4096, NULL, 2,
                    &s_ui_persist_task) != pdPASS) abort();
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

/* Only persist UI-controlled values. Flash writes can suspend external-memory
 * cache access and disturb RGB DMA; never execute them in an LVGL callback. */
static void persist_ui(uint8_t station, uint8_t volume, bool playing)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("kitchenradio", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS UI open: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(h, "station", station);
    if (err == ESP_OK) err = nvs_set_u8(h, "volume", volume);
    if (err == ESP_OK) err = nvs_set_u8(h, "playing", playing ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "NVS UI save: %s", esp_err_to_name(err));
    nvs_close(h);
}

static void ui_persist_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        /* One flash commit after a short period without UI changes. */
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1500)) != 0) { }
        xSemaphoreTake(s_flash_mu, portMAX_DELAY);
        xSemaphoreTake(s_mu, portMAX_DELAY);
        uint8_t station = s_cfg.station;
        uint8_t volume = s_cfg.volume;
        bool playing = s_cfg.playing;
        xSemaphoreGive(s_mu);
        persist_ui(station, volume, playing);
        xSemaphoreGive(s_flash_mu);
    }
}

void radio_config_save(const radio_settings_t *cfg)
{
    if (cfg->station >= RADIO_PRESETS || cfg->volume > 100) return;
    /* Serialize full web configuration with the delayed UI-only writer. */
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
    /* Serialize Flash writes with configuration changes so an old snapshot
       cannot overwrite a more recent station or volume selection. */
    persist(&s_cfg);
    xSemaphoreGive(s_mu);
    xSemaphoreGive(s_flash_mu);
}
void radio_config_select(int index)
{
    if (index < 0 || index >= RADIO_PRESETS) return;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    bool changed = s_cfg.station != index || !s_cfg.playing;
    if (changed) {
        s_cfg.station = (uint8_t)index;
        s_cfg.playing = true;
        ++s_revision;
    }
    xSemaphoreGive(s_mu);
    if (changed) xTaskNotifyGive(s_ui_persist_task);
}
void radio_config_volume(int volume, bool persist_now)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_cfg.volume = (uint8_t)volume;
    xSemaphoreGive(s_mu);
    if (persist_now) xTaskNotifyGive(s_ui_persist_task);
}
void radio_config_play(bool playing)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    bool changed = s_cfg.playing != playing;
    if (changed) {
        s_cfg.playing = playing;
        ++s_revision;
    }
    xSemaphoreGive(s_mu);
    if (changed) xTaskNotifyGive(s_ui_persist_task);
}
uint32_t radio_config_revision(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    uint32_t revision = s_revision;
    xSemaphoreGive(s_mu);
    return revision;
}
