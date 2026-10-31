/*
 * Küchenradio: internet MP3 streams decoded by minimp3 (CC0),
 * output by Waveshare's ES8311 board support (ESP Codec Dev).
 * No binary AtlasCube or ESP Audio Codec decoder libraries.
 */
#include "radio_stream.h"
#include "radio_config.h"
#include "radio_wifi.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "bsp/esp32_s3_touch_lcd_4b.h"
#include "esp_codec_dev.h"

/* MP3 decoder source: https://github.com/lieff/minimp3, CC0-1.0. */
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#define AUDIO_INPUT_CAPACITY 12288
#define AUDIO_READ_SIZE 1536
#define AUDIO_HTTP_TIMEOUT_MS 4500

static const char *TAG = "radio_stream";
static esp_codec_dev_handle_t s_speaker;
static SemaphoreHandle_t s_status_mu;
static char s_status[72] = "Starte ...";

static void status_set(const char *str)
{
    if (!s_status_mu) return;
    xSemaphoreTake(s_status_mu, portMAX_DELAY);
    snprintf(s_status, sizeof(s_status), "%s", str);
    xSemaphoreGive(s_status_mu);
}
const char *radio_stream_status(void)
{
    static char snap[72];
    if (!s_status_mu) return "Initialisiere Radio ...";
    xSemaphoreTake(s_status_mu, portMAX_DELAY);
    strcpy(snap, s_status);
    xSemaphoreGive(s_status_mu);
    return snap;
}

typedef struct {
    char redirect[384];
} response_headers_t;

static esp_err_t on_stream_event(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_HEADER || !evt->user_data) return ESP_OK;
    response_headers_t *headers = evt->user_data;
    if (evt->header_key && evt->header_value && !strcasecmp(evt->header_key, "Location")) {
        snprintf(headers->redirect, sizeof(headers->redirect), "%s", evt->header_value);
    }
    return ESP_OK;
}

/* The config and event callback use stack memory only until fetch_headers completes. */
static esp_http_client_handle_t open_stream(const char *url)
{
    char target[384];
    snprintf(target, sizeof(target), "%s", url);
    for (int redirects = 0; redirects < 5; ++redirects) {
        response_headers_t headers = {0};
        esp_http_client_config_t cfg = {
            .url = target,
            .timeout_ms = AUDIO_HTTP_TIMEOUT_MS,
            .user_agent = "KitchenRadio-ESP32/1.0",
            .crt_bundle_attach = esp_crt_bundle_attach,
            .event_handler = on_stream_event,
            .user_data = &headers,
            .buffer_size = 4096,
            .keep_alive_enable = true,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) return NULL;
        /* Prevent metadata blocks being inserted into the compressed MP3 stream. */
        esp_http_client_set_header(client, "Icy-MetaData", "0");
        if (esp_http_client_open(client, 0) != ESP_OK) {
            esp_http_client_cleanup(client);
            return NULL;
        }
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP %d %s", status, target);
        if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && headers.redirect[0]) {
            /* Absolute HTTP(S) redirects only. */
            if (strncmp(headers.redirect, "http://", 7) && strncmp(headers.redirect, "https://", 8)) {
                esp_http_client_cleanup(client);
                return NULL;
            }
            snprintf(target, sizeof(target), "%s", headers.redirect);
            esp_http_client_cleanup(client);
            continue;
        }
        /* Stop user callback referencing expired headers memory. */
        esp_http_client_set_user_data(client, NULL);
        if (status != 200) {
            esp_http_client_cleanup(client);
            return NULL;
        }
        return client;
    }
    return NULL;
}

static bool wanted(uint32_t revision)
{
    return radio_wifi_connected() && radio_config_revision() == revision;
}

static void play_stream(const char *url, uint32_t revision)
{
    esp_http_client_handle_t http = open_stream(url);
    if (!http) {
        status_set("Stream nicht erreichbar");
        return;
    }

    /* For MP3, a frame is 1152 PCM samples per channel at most. */
    uint8_t *encoded = malloc(AUDIO_INPUT_CAPACITY);
    mp3d_sample_t *pcm = malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(mp3d_sample_t));
    int16_t *mono = malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
    mp3dec_t *decoder = malloc(sizeof(mp3dec_t));
    bool opened = false;
    int active_rate = 0;
    int volume_applied = -1;
    if (!encoded || !pcm || !mono || !decoder) {
        status_set("Zu wenig RAM fuer Audio");
        goto done;
    }
    mp3dec_init(decoder);
    status_set("Verbinde Audiostream ...");
    int pending = 0;
    while (wanted(revision)) {
        if (pending + AUDIO_READ_SIZE > AUDIO_INPUT_CAPACITY) {
            ESP_LOGW(TAG, "MP3 frame synchronization lost");
            status_set("Ungueltiger MP3-Stream");
            break;
        }
        int n = esp_http_client_read(http, (char *)encoded + pending, AUDIO_READ_SIZE);
        if (n <= 0) {
            ESP_LOGW(TAG, "Stream Ende/Timeout: %d", n);
            break;
        }
        pending += n;
        int offset = 0;
        while (pending - offset > 0 && wanted(revision)) {
            mp3dec_frame_info_t info = {0};
            int samples = mp3dec_decode_frame(decoder, encoded + offset, pending - offset, pcm, &info);
            if (info.frame_bytes == 0) break;  /* Need more input. */
            if (info.frame_bytes > pending - offset) {
                ESP_LOGE(TAG, "Decoder frame larger than buffer");
                goto done;
            }
            offset += info.frame_bytes;
            if (samples <= 0) continue;  /* Skipped ID3/junk/frame without audio. */
            if (samples > MINIMP3_MAX_SAMPLES_PER_FRAME / 2 ||
                (info.channels != 1 && info.channels != 2) ||
                info.hz < 8000 || info.hz > 48000) {
                ESP_LOGW(TAG, "Unsupported stream format: %d Hz, %d channels, %d samples",
                         info.hz, info.channels, samples);
                status_set("MP3-Format nicht unterstuetzt");
                goto done;
            }
            if (!opened || active_rate != info.hz) {
                if (opened) {
                    esp_codec_dev_close(s_speaker);
                    opened = false;
                }
                esp_codec_dev_sample_info_t fmt = {
                    .sample_rate = info.hz,
                    .channel = 1,
                    .bits_per_sample = 16,
                };
                if (esp_codec_dev_open(s_speaker, &fmt) != ESP_CODEC_DEV_OK) {
                    ESP_LOGE(TAG, "ES8311 cannot open %d Hz", info.hz);
                    status_set("ES8311 Audiofehler");
                    goto done;
                }
                opened = true;
                active_rate = info.hz;
                volume_applied = -1;
                status_set("Spielt");
            }
            radio_settings_t settings;
            radio_config_get(&settings);
            if (volume_applied != settings.volume) {
                esp_codec_dev_set_out_vol(s_speaker, settings.volume);
                volume_applied = settings.volume;
            }
            if (info.channels == 1) {
                memcpy(mono, pcm, (size_t)samples * sizeof(int16_t));
            } else {
                for (int i = 0; i < samples; ++i) {
                    mono[i] = (int16_t)(((int32_t)pcm[i * 2] + (int32_t)pcm[i * 2 + 1]) / 2);
                }
            }
            if (esp_codec_dev_write(s_speaker, mono, samples * (int)sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
                ESP_LOGE(TAG, "Audioausgabe fehlgeschlagen");
                status_set("Audioausgabe fehlgeschlagen");
                goto done;
            }
        }
        pending -= offset;
        if (pending && offset) memmove(encoded, encoded + offset, pending);
    }

done:
    if (opened) esp_codec_dev_close(s_speaker);
    if (http) esp_http_client_cleanup(http);
    free(encoded);
    free(pcm);
    free(mono);
    free(decoder);
}

static void stream_task(void *arg)
{
    (void)arg;
    while (!radio_wifi_connected()) {
        status_set("WLAN einrichten: 192.168.4.1");
        vTaskDelay(pdMS_TO_TICKS(750));
    }
    while (1) {
        radio_settings_t cfg;
        radio_config_get(&cfg);
        if (!radio_wifi_connected()) {
            status_set("WLAN getrennt - verbinde ...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!cfg.playing) {
            status_set("Pause");
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if (!cfg.stations[cfg.station].url[0]) {
            status_set("Kein Stream eingestellt");
            vTaskDelay(pdMS_TO_TICKS(1500));
            continue;
        }
        uint32_t revision = radio_config_revision();
        status_set("Verbinde mit Sender ...");
        play_stream(cfg.stations[cfg.station].url, revision);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}

void radio_stream_start(void)
{
    s_status_mu = xSemaphoreCreateMutex();
    if (!s_status_mu) abort();
    s_speaker = bsp_audio_codec_speaker_init();
    if (!s_speaker) {
        ESP_LOGE(TAG, "ES8311 konnte nicht initialisiert werden");
        status_set("ES8311 nicht gefunden");
        return;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(stream_task, "radio_audio", 24576, NULL, 5, NULL, 1);
    if (ok != pdPASS) ESP_LOGE(TAG, "Audiotask konnte nicht gestartet werden");
}
