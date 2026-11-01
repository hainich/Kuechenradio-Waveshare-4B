/*
 * Kuechenradio: internet streams decoded with Espressif esp_audio_codec,
 * output through the Waveshare ES8311 board support (ESP Codec Dev).
 * The entire audio pipeline is pinned to core 0, leaving core 1 available
 * for the RGB display and touch/LVGL handling.
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
#include <stdatomic.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "audio_ring.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "bsp/esp32_s3_touch_lcd_4b.h"
#include "esp_codec_dev.h"

/* Espressif's optimized frame-aware streaming decoders (MP3, AAC, FLAC, OGG). */
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_aac_dec.h"

#define AUDIO_READ_SIZE 1536
#define AUDIO_PCM_CAPACITY 16384
#define AUDIO_PCM_MAX_CAPACITY 65536
#define AUDIO_HTTP_TIMEOUT_MS 4500
#define AUDIO_NETWORK_PACKET 1024
/* Core 1 is reserved for responsive LVGL touch/display work. On core 0 the
 * HTTP producer runs ahead of the audio consumer when data is available;
 * both stay below ESP-IDF's high-priority Wi-Fi/TCP/IP system tasks. */
#define RADIO_AUDIO_CORE 0
#define RADIO_AUDIO_PRIORITY 4
#define RADIO_HTTP_PRIORITY 5

static const char *TAG = "radio_stream";
static esp_codec_dev_handle_t s_speaker;
static SemaphoreHandle_t s_status_mu;
static char s_status[72] = "Starte ...";

static void status_set(const char *str)
{
    if (!s_status_mu) return;
    xSemaphoreTake(s_status_mu, portMAX_DELAY);
    if (strcmp(s_status, str) != 0) snprintf(s_status, sizeof(s_status), "%s", str);
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
    char content_type[96];
} response_headers_t;

static esp_audio_simple_dec_type_t stream_type(const char *content_type, const char *url)
{
    /* Trust the actual HTTP response over the often extensionless station URL. */
    if (content_type[0]) {
        if (!strncasecmp(content_type, "audio/aac", 9) ||
            !strncasecmp(content_type, "audio/x-aac", 11) ||
            !strncasecmp(content_type, "audio/aacp", 10))
            return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
        if (!strncasecmp(content_type, "audio/flac", 10) ||
            !strncasecmp(content_type, "audio/x-flac", 12))
            return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
        if (!strncasecmp(content_type, "audio/ogg", 9) ||
            !strncasecmp(content_type, "application/ogg", 15))
            return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
        if (!strncasecmp(content_type, "audio/mpeg", 10) ||
            !strncasecmp(content_type, "audio/mp3", 9))
            return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
    if (strstr(url, ".aac") || strstr(url, "/aac"))
        return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    if (strstr(url, ".flac"))
        return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    if (strstr(url, ".ogg"))
        return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
    return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
}

static esp_err_t on_stream_event(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_HEADER || !evt->user_data) return ESP_OK;
    response_headers_t *headers = evt->user_data;
    if (evt->header_key && evt->header_value && !strcasecmp(evt->header_key, "Location")) {
        snprintf(headers->redirect, sizeof(headers->redirect), "%s", evt->header_value);
    }
    if (evt->header_key && evt->header_value && !strcasecmp(evt->header_key, "Content-Type")) {
        snprintf(headers->content_type, sizeof(headers->content_type), "%s", evt->header_value);
    }
    return ESP_OK;
}

/* The config and event callback use stack memory only until fetch_headers completes. */
static esp_http_client_handle_t open_stream(const char *url, esp_audio_simple_dec_type_t *type)
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
        *type = stream_type(headers.content_type, target);
        ESP_LOGI(TAG, "Audioformat %s, Content-Type: %s",
                 esp_audio_simple_dec_get_name(*type), headers.content_type);
        return client;
    }
    return NULL;
}

static bool wanted(uint32_t revision)
{
    return radio_wifi_connected() && radio_config_revision() == revision;
}

/*
 * HTTP reception must continue while MP3 decoding or the ES8311/I2S writer
 * blocks. The old 64-KiB PSRAM FIFO increased contention with RGB DMA.
 * This implementation keeps a modest FIFO exclusively in internal SRAM,
 * leaving the proven single RGB framebuffer configuration untouched.
 *
 * The audio_ring_t itself is not synchronized; its two users take mu for
 * short copies and index updates. HTTP and codec calls never hold mu.
 */
typedef struct {
    audio_ring_t ring;
    SemaphoreHandle_t mu;
    SemaphoreHandle_t finished;
    esp_http_client_handle_t http;
    uint32_t revision;
    atomic_bool stop;
    atomic_bool eof;
} audio_pipe_t;

static size_t pipe_used(audio_pipe_t *pipe)
{
    xSemaphoreTake(pipe->mu, portMAX_DELAY);
    size_t used = audio_ring_available(&pipe->ring);
    xSemaphoreGive(pipe->mu);
    return used;
}

static size_t pipe_space(audio_pipe_t *pipe)
{
    xSemaphoreTake(pipe->mu, portMAX_DELAY);
    size_t space = audio_ring_space(&pipe->ring);
    xSemaphoreGive(pipe->mu);
    return space;
}

static size_t pipe_read(audio_pipe_t *pipe, uint8_t *dst, size_t count)
{
    xSemaphoreTake(pipe->mu, portMAX_DELAY);
    size_t n = audio_ring_read(&pipe->ring, dst, count);
    xSemaphoreGive(pipe->mu);
    return n;
}

static size_t pipe_write(audio_pipe_t *pipe, const uint8_t *src, size_t count)
{
    xSemaphoreTake(pipe->mu, portMAX_DELAY);
    size_t n = audio_ring_write(&pipe->ring, src, count);
    xSemaphoreGive(pipe->mu);
    return n;
}

static void http_receiver_task(void *arg)
{
    audio_pipe_t *pipe = (audio_pipe_t *)arg;
    uint8_t packet[AUDIO_NETWORK_PACKET];
    unsigned read_timeouts = 0;
    TickType_t last_payload = xTaskGetTickCount();
    while (!atomic_load(&pipe->stop) && wanted(pipe->revision)) {
        if (pipe_space(pipe) < sizeof(packet)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        int n = esp_http_client_read(pipe->http, (char *)packet, sizeof(packet));
        if (n > 0) {
            read_timeouts = 0;
            last_payload = xTaskGetTickCount();
            /* Only the consumer can change free space, and it increases it. */
            if (pipe_write(pipe, packet, (size_t)n) != (size_t)n) {
                ESP_LOGE(TAG, "Audio FIFO overflow");
                break;
            }
        } else if (n == 0 && !esp_http_client_is_complete_data_received(pipe->http) &&
                   (xTaskGetTickCount() - last_payload) < pdMS_TO_TICKS(12000)) {
            /* A short read with no bytes is not EOF for a live stream. */
            if (++read_timeouts % 5 == 0) {
                ESP_LOGW(TAG, "HTTP no payload for %u reads", read_timeouts);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            ESP_LOGW(TAG, "HTTP stream ended: %d", n);
            break;
        }
    }
    atomic_store(&pipe->eof, true);
    xSemaphoreGive(pipe->finished);
    vTaskDelete(NULL);
}

static void play_stream(const char *url, uint32_t revision)
{
    esp_audio_simple_dec_type_t type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    esp_http_client_handle_t http = open_stream(url, &type);
    if (!http) {
        status_set("Stream nicht erreichbar");
        return;
    }

    uint8_t *encoded = malloc(AUDIO_READ_SIZE);
    uint8_t *pcm = malloc(AUDIO_PCM_CAPACITY);
    int16_t *mono = malloc(AUDIO_PCM_CAPACITY);
    uint32_t pcm_capacity = AUDIO_PCM_CAPACITY;
    uint32_t mono_capacity = AUDIO_PCM_CAPACITY;
    esp_audio_simple_dec_handle_t decoder = NULL;
    audio_pipe_t pipe = { .http = http, .revision = revision };
    uint8_t *fifo_storage = NULL;
    bool receiver_started = false;
    bool opened = false;
    int active_rate = 0;
    int volume_applied = -1;
    unsigned underruns = 0;
    unsigned slow_writes = 0;
    TickType_t last_audio_report = xTaskGetTickCount();

    atomic_init(&pipe.stop, false);
    atomic_init(&pipe.eof, false);
    pipe.mu = xSemaphoreCreateMutex();
    pipe.finished = xSemaphoreCreateBinary();
    if (!encoded || !pcm || !mono || !pipe.mu || !pipe.finished) {
        status_set("Zu wenig RAM fuer Audio");
        goto done;
    }

    esp_aac_dec_cfg_t aac_cfg = { .aac_plus_enable = true };
    esp_audio_simple_dec_cfg_t dec_cfg = {
        .dec_type = type,
        .dec_cfg = type == ESP_AUDIO_SIMPLE_DEC_TYPE_AAC ? &aac_cfg : NULL,
        .cfg_size = type == ESP_AUDIO_SIMPLE_DEC_TYPE_AAC ? sizeof(aac_cfg) : 0,
        .use_frame_dec = false,
    };
    if (esp_audio_simple_dec_open(&dec_cfg, &decoder) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Decoder konnte %s nicht starten", esp_audio_simple_dec_get_name(type));
        status_set("Audioformat nicht unterstuetzt");
        goto done;
    }

    /* Keep the network producer independent of I2S and decoding. Internal SRAM
       avoids the PSRAM contention observed with the previous large audio FIFO. */
    const size_t fifo_sizes[] = {32768, 24576, 16384, 12288, 8192};
    for (size_t i = 0; i < sizeof(fifo_sizes) / sizeof(fifo_sizes[0]); ++i) {
        size_t capacity = fifo_sizes[i];
        if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
            < capacity + 8192) continue;
        fifo_storage = heap_caps_malloc(capacity, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!fifo_storage) continue;
        audio_ring_init(&pipe.ring, fifo_storage, capacity);
        if (xTaskCreatePinnedToCore(http_receiver_task, "radio_http", 8192,
                                    &pipe, RADIO_HTTP_PRIORITY, NULL, RADIO_AUDIO_CORE) == pdPASS) {
            receiver_started = true;
            ESP_LOGI(TAG, "HTTP/I2S getrennt; interner Audiopuffer: %u Bytes",
                     (unsigned)capacity);
            break;
        }
        heap_caps_free(fifo_storage);
        fifo_storage = NULL;
    }
    if (!receiver_started) {
        ESP_LOGW(TAG, "Interner RAM knapp: direkter Audiopfad");
    }

    /* Resume after a bounded amount of compressed audio, not half the FIFO.
     * This shortens audible gaps after a temporary network starvation. */
    size_t refill = receiver_started ? pipe.ring.capacity / 8 : 0;
    if (refill > 4096) refill = 4096;
    if (receiver_started) {
        size_t start_fill = pipe.ring.capacity * 3 / 4;
        status_set("Puffert Audiostream ...");
        while (wanted(revision) && !atomic_load(&pipe.eof) &&
               pipe_used(&pipe) < start_fill) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    bool buffering = true;
    while (wanted(revision)) {
        int n;
        if (receiver_started) {
            if (pipe_used(&pipe) == 0) {
                if (atomic_load(&pipe.eof)) break;
                if (!buffering) {
                    ++underruns;
                    buffering = true;
                    status_set("Puffert Audiostream ...");
                    ESP_LOGW(TAG, "Audio FIFO underrun %u", underruns);
                }
                while (wanted(revision) && !atomic_load(&pipe.eof) &&
                       pipe_used(&pipe) < refill) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                if (!wanted(revision)) break;
                if (pipe_used(&pipe) == 0 && atomic_load(&pipe.eof)) break;
            }
            n = (int)pipe_read(&pipe, encoded, AUDIO_READ_SIZE);
            if (n == 0) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
        } else {
            n = esp_http_client_read(http, (char *)encoded, AUDIO_READ_SIZE);
            if (n <= 0) {
                ESP_LOGW(TAG, "Direkter HTTP-Stream beendet: %d", n);
                break;
            }
        }

        esp_audio_simple_dec_raw_t raw = { .buffer = encoded, .len = (uint32_t)n };
        while (raw.len && wanted(revision)) {
            esp_audio_simple_dec_out_t out = {
                .buffer = pcm, .len = pcm_capacity
            };
            esp_audio_err_t err = esp_audio_simple_dec_process(decoder, &raw, &out);
            if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH &&
                out.needed_size > pcm_capacity &&
                out.needed_size <= AUDIO_PCM_MAX_CAPACITY) {
                uint8_t *bigger = realloc(pcm, out.needed_size);
                if (!bigger) {
                    status_set("Zu wenig RAM fuer Decoder");
                    goto done;
                }
                pcm = bigger;
                pcm_capacity = out.needed_size;
                continue;
            }
            if (err != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(TAG, "Audio decoder error %d (%s)", err,
                         esp_audio_simple_dec_get_name(type));
                status_set("Stream-Dekodierung fehlgeschlagen");
                goto done;
            }
            if (out.decoded_size) {
                esp_audio_simple_dec_info_t info = {0};
                if (esp_audio_simple_dec_get_info(decoder, &info) != ESP_AUDIO_ERR_OK ||
                    info.bits_per_sample != 16 || info.channel < 1 || info.channel > 2 ||
                    info.sample_rate < 8000 || info.sample_rate > 48000 ||
                    out.decoded_size % (info.channel * sizeof(int16_t))) {
                    status_set("Audioformat nicht unterstuetzt");
                    goto done;
                }
                size_t samples = out.decoded_size / (info.channel * sizeof(int16_t));
                if (samples * sizeof(int16_t) > mono_capacity) {
                    uint32_t needed = (uint32_t)(samples * sizeof(int16_t));
                    int16_t *bigger = realloc(mono, needed);
                    if (!bigger) {
                        status_set("Zu wenig RAM fuer Audio");
                        goto done;
                    }
                    mono = bigger;
                    mono_capacity = needed;
                }
                if (!opened || active_rate != (int)info.sample_rate) {
                    if (opened) {
                        esp_codec_dev_close(s_speaker);
                        opened = false;
                    }
                    esp_codec_dev_sample_info_t fmt = {
                        .sample_rate = (int)info.sample_rate,
                        .channel = 1,
                        .bits_per_sample = 16,
                    };
                    if (esp_codec_dev_open(s_speaker, &fmt) != ESP_CODEC_DEV_OK) {
                        ESP_LOGE(TAG, "ES8311 cannot open %u Hz", info.sample_rate);
                        status_set("ES8311 Audiofehler");
                        goto done;
                    }
                    opened = true;
                    active_rate = (int)info.sample_rate;
                    volume_applied = -1;
                }
                radio_settings_t settings;
                radio_config_get(&settings);
                if (volume_applied != settings.volume) {
                    esp_codec_dev_set_out_vol(s_speaker, settings.volume);
                    volume_applied = settings.volume;
                }
                const int16_t *stereo = (const int16_t *)pcm;
                if (info.channel == 1) {
                    memcpy(mono, pcm, samples * sizeof(int16_t));
                } else {
                    for (size_t i = 0; i < samples; ++i) {
                        mono[i] = (int16_t)(((int32_t)stereo[i * 2] +
                                            (int32_t)stereo[i * 2 + 1]) / 2);
                    }
                }
                int64_t write_start_us = esp_timer_get_time();
                int write_result = esp_codec_dev_write(s_speaker, mono,
                                       (int)(samples * sizeof(int16_t)));
                int64_t write_ms = (esp_timer_get_time() - write_start_us) / 1000;
                if (write_ms > 150) {
                    ++slow_writes;
                    if (slow_writes <= 3 || slow_writes % 20 == 0) {
                        ESP_LOGW(TAG, "Slow codec/I2S write: %lld ms (%u total)",
                                 (long long)write_ms, slow_writes);
                    }
                }
                if (write_result != ESP_CODEC_DEV_OK) {
                    status_set("Audioausgabe fehlgeschlagen");
                    goto done;
                }
                if (buffering) {
                    status_set("Spielt");
                    buffering = false;
                }
                TickType_t now = xTaskGetTickCount();
                if (now - last_audio_report >= pdMS_TO_TICKS(30000)) {
                    last_audio_report = now;
                    size_t fifo_bytes = receiver_started ? pipe_used(&pipe) : 0;
                    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                    ESP_LOGI(TAG, "Audio health: FIFO=%u/%u; underruns=%u; slow I2S=%u; internal largest=%u",
                             (unsigned)fifo_bytes,
                             (unsigned)(receiver_started ? pipe.ring.capacity : 0),
                             underruns, slow_writes, (unsigned)largest);
                }
            }
            if (raw.consumed > raw.len) {
                ESP_LOGE(TAG, "Decoder invalid consumed size");
                goto done;
            }
            if (raw.consumed == 0) {
                ESP_LOGE(TAG, "Decoder made no input progress");
                status_set("Decoder blockiert");
                goto done;
            }
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
        }
    }

done:
    if (receiver_started) {
        atomic_store(&pipe.stop, true);
        xSemaphoreTake(pipe.finished, portMAX_DELAY);
        ESP_LOGI(TAG, "Audiopuffer-Unterlaeufe: %u", underruns);
    }
    if (opened) esp_codec_dev_close(s_speaker);
    if (decoder) esp_audio_simple_dec_close(decoder);
    esp_http_client_cleanup(http);
    if (pipe.finished) vSemaphoreDelete(pipe.finished);
    if (pipe.mu) vSemaphoreDelete(pipe.mu);
    heap_caps_free(fifo_storage);
    free(encoded);
    free(pcm);
    free(mono);
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
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();
    s_speaker = bsp_audio_codec_speaker_init();
    if (!s_speaker) {
        ESP_LOGE(TAG, "ES8311 konnte nicht initialisiert werden");
        status_set("ES8311 nicht gefunden");
        return;
    }
    /* Audio used to run on core 1 at priority 5 and could monopolize the
     * same core as LVGL. Keep all radio CPU work on core 0 instead. */
    BaseType_t ok = xTaskCreatePinnedToCore(stream_task, "radio_audio", 24576,
                                            NULL, RADIO_AUDIO_PRIORITY, NULL, RADIO_AUDIO_CORE);
    ESP_LOGI(TAG, "Audiotask: core %d priority %d; HTTP: core %d priority %d",
             RADIO_AUDIO_CORE, RADIO_AUDIO_PRIORITY, RADIO_AUDIO_CORE, RADIO_HTTP_PRIORITY);
    if (ok != pdPASS) ESP_LOGE(TAG, "Audiotask konnte nicht gestartet werden");
}
