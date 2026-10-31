#include "radio_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static radio_settings_t s_cfg;
static SemaphoreHandle_t s_mu;
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
    if (!s_mu) abort();
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

void radio_config_save(const radio_settings_t *cfg)
{
    if (cfg->station >= RADIO_PRESETS || cfg->volume > 100) return;
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
}
void radio_config_select(int index)
{
    if (index < 0 || index >= RADIO_PRESETS) return;
    radio_settings_t cfg;
    radio_config_get(&cfg);
    cfg.station = index;
    cfg.playing = true;
    radio_config_save(&cfg);
}
void radio_config_volume(int volume, bool persist_now)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_cfg.volume = volume;
    if (persist_now) persist(&s_cfg);
    xSemaphoreGive(s_mu);
}
void radio_config_play(bool playing)
{
    radio_settings_t cfg;
    radio_config_get(&cfg);
    cfg.playing = playing;
    radio_config_save(&cfg);
}
uint32_t radio_config_revision(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    uint32_t revision = s_revision;
    xSemaphoreGive(s_mu);
    return revision;
}
