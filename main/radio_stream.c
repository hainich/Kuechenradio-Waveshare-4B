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
#include "audio_ring.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "radio_diag.h"
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
/* Decoded-PCM jitter buffer between decoder and I2S (mono, 16 bit). It lives
 * in PSRAM and decouples decoder/LVGL/PSRAM stalls from the I2S DMA: 65536 B
 * are about 740 ms at 44.1 kHz. A dedicated writer task, one priority above
 * the HTTP task, is the only caller of esp_codec_dev_write(). */
#define PCM_RING_BYTES 65536
#define PCM_PRIME_BYTES 24576
#define PCM_WRITE_CHUNK 1024
#define RADIO_I2S_PRIORITY 6

static const char *TAG = "radio_stream";
static esp_codec_dev_handle_t s_speaker;
static SemaphoreHandle_t s_status_mu;
static char s_status[72] = "Starte ...";
static atomic_int s_lab_mode = ATOMIC_VAR_INIT(RADIO_LAB_RADIO);
static atomic_uint s_lab_prime_kb = ATOMIC_VAR_INIT(24);

int radio_lab_mode(void) { return atomic_load(&s_lab_mode); }
bool radio_lab_set_mode(int mode)
{
    if (mode < RADIO_LAB_RADIO || mode > RADIO_LAB_PCM_TONE) return false;
    atomic_store(&s_lab_mode, mode);
    return true;
}
uint32_t radio_lab_prime_kb(void) { return atomic_load(&s_lab_prime_kb); }
bool radio_lab_set_prime_kb(uint32_t kb)
{
    if (kb < 8 || kb > 48 || kb % 4) return false;
    atomic_store(&s_lab_prime_kb, kb);
    return true;
}

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
        if (!client) {
            radio_diag_http_open(ESP_ERR_NO_MEM, 0, 0, false);
            return NULL;
        }
        /* Prevent metadata blocks being inserted into the compressed MP3 stream. */
        esp_http_client_set_header(client, "Icy-MetaData", "0");
        int64_t connection_started = esp_timer_get_time();
        esp_err_t open_err = esp_http_client_open(client, 0);
        if (open_err != ESP_OK) {
            radio_diag_http_open((int32_t)open_err, 0,
                (uint32_t)((esp_timer_get_time() - connection_started) / 1000), false);
            esp_http_client_cleanup(client);
            return NULL;
        }
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        bool is_redirect = status == 301 || status == 302 || status == 303 ||
                           status == 307 || status == 308;
        radio_diag_http_open(0, status,
            (uint32_t)((esp_timer_get_time() - connection_started) / 1000), is_redirect);
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
    return radio_lab_mode() == RADIO_LAB_RADIO &&
           radio_wifi_connected() && radio_config_revision() == revision;
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
    radio_diag_fifo((uint32_t)audio_ring_available(&pipe->ring),
                    (uint32_t)pipe->ring.capacity);
    xSemaphoreGive(pipe->mu);
    return n;
}

static size_t pipe_write(audio_pipe_t *pipe, const uint8_t *src, size_t count)
{
    xSemaphoreTake(pipe->mu, portMAX_DELAY);
    size_t n = audio_ring_write(&pipe->ring, src, count);
    radio_diag_fifo((uint32_t)audio_ring_available(&pipe->ring),
                    (uint32_t)pipe->ring.capacity);
    xSemaphoreGive(pipe->mu);
    return n;
}

static void http_receiver_task(void *arg)
{
    audio_pipe_t *pipe = (audio_pipe_t *)arg;
    uint8_t packet[AUDIO_NETWORK_PACKET];
    int64_t last_payload_us = esp_timer_get_time();
    int64_t last_warning_us = last_payload_us;
    uint32_t reads_since_stack_check = 0;
    uint32_t backpressure_ticks = 0;
    while (!atomic_load(&pipe->stop) && wanted(pipe->revision)) {
        if (pipe_space(pipe) < sizeof(packet)) {
            /* Differentiates server silence from intentional receive backpressure. */
            if (++backpressure_ticks >= 10) {
                radio_diag_http_backpressure(backpressure_ticks * 10);
                backpressure_ticks = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (backpressure_ticks) {
            radio_diag_http_backpressure(backpressure_ticks * 10);
            backpressure_ticks = 0;
        }
        int64_t read_started = esp_timer_get_time();
        radio_diag_http_inflight(true);
        int n = esp_http_client_read(pipe->http, (char *)packet, sizeof(packet));
        radio_diag_http_inflight(false);
        radio_diag_http_read(n, (uint32_t)((esp_timer_get_time() - read_started) / 1000));
        if ((++reads_since_stack_check & 127U) == 0)
            radio_diag_stack(1, (uint32_t)uxTaskGetStackHighWaterMark(NULL));
        if (n > 0) {
            last_payload_us = esp_timer_get_time();
            /* Only the consumer can change free space, and it increases it. */
            if (pipe_write(pipe, packet, (size_t)n) != (size_t)n) {
                ESP_LOGE(TAG, "Audio FIFO overflow");
                break;
            }
        } else if (n == 0 || n == -ESP_ERR_HTTP_EAGAIN) {
            /* Live radio may temporarily supply no payload. Do not treat
             * a few consecutive zero/EAGAIN reads as a finished stream. */
            int64_t now_us = esp_timer_get_time();
            if (now_us - last_payload_us >= 12000000LL) {
                radio_diag_note(RD_HTTP_STALL, 12000, 0);
                ESP_LOGW(TAG, "HTTP stream stalled for 12 s; reconnecting");
                break;
            }
            if (now_us - last_warning_us >= 10000000LL) {
                ESP_LOGW(TAG, "HTTP waiting for live stream payload");
                last_warning_us = now_us;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            radio_diag_note(RD_HTTP_END, (uint32_t)(-n), n);
            ESP_LOGW(TAG, "HTTP stream ended: %d", n);
            break;
        }
    }
    atomic_store(&pipe->eof, true);
    xSemaphoreGive(pipe->finished);
    vTaskDelete(NULL);
}

/* ---- PCM sink: decoder -> ring (PSRAM) -> writer task -> ES8311/I2S ---- */
typedef struct {
    audio_ring_t ring;
    SemaphoreHandle_t mu;        /* protects ring only; short copies */
    SemaphoreHandle_t codec_mu;  /* serialises every esp_codec_dev_* call */
    SemaphoreHandle_t finished;
    uint8_t *storage;
    atomic_bool stop;
    atomic_bool codec_open;
    atomic_bool flushing;        /* play out a partial ring (rate change) */
    atomic_bool failed;
    atomic_uint underruns;
    bool task_started;
} pcm_sink_t;

static size_t sink_used(pcm_sink_t *sink)
{
    xSemaphoreTake(sink->mu, portMAX_DELAY);
    size_t used = audio_ring_available(&sink->ring);
    xSemaphoreGive(sink->mu);
    return used;
}

static void pcm_writer_task(void *arg)
{
    pcm_sink_t *sink = (pcm_sink_t *)arg;
    uint8_t chunk[PCM_WRITE_CHUNK];
    bool primed = false;
    bool low_recorded = false;
    uint32_t writer_calls = 0;
    while (!atomic_load(&sink->stop)) {
        size_t used = sink_used(sink);
        if (!primed) {
            if (used >= radio_lab_prime_kb() * 1024 || (used > 0 && atomic_load(&sink->flushing))) {
                primed = true;
            } else {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
        }
        if (primed && used < 2048 && !low_recorded) {
            radio_diag_note(RD_PCM_LOW, (uint32_t)used, 0);
            low_recorded = true;
        } else if (used >= 4096) {
            low_recorded = false;
        }
        if (used == 0) {
            /* Real PCM underrun: wait until the ring is primed again. */
            primed = false;
            atomic_fetch_add(&sink->underruns, 1);
            radio_diag_pcm_underrun();
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        xSemaphoreTake(sink->mu, portMAX_DELAY);
        size_t n = audio_ring_read(&sink->ring, chunk, sizeof(chunk));
        radio_diag_pcm((uint32_t)audio_ring_available(&sink->ring),
                       (uint32_t)sink->ring.capacity);
        xSemaphoreGive(sink->mu);
        if (!n) continue;
        int64_t codec_lock_started = esp_timer_get_time();
        xSemaphoreTake(sink->codec_mu, portMAX_DELAY);
        uint32_t lock_wait_ms = (uint32_t)((esp_timer_get_time() - codec_lock_started) / 1000);
        if (lock_wait_ms) radio_diag_codec_mutex_wait(lock_wait_ms);
        if (atomic_load(&sink->codec_open)) {
            int64_t t0 = esp_timer_get_time();
            int rc = esp_codec_dev_write(s_speaker, chunk, (int)n);
            radio_diag_codec((uint32_t)((esp_timer_get_time() - t0) / 1000));
            radio_diag_i2s_result(rc == ESP_CODEC_DEV_OK);
            if (rc != ESP_CODEC_DEV_OK) atomic_store(&sink->failed, true);
            else radio_diag_audio_written();
            if ((++writer_calls & 127U) == 0)
                radio_diag_writer_stack((uint32_t)uxTaskGetStackHighWaterMark(NULL));
        }
        xSemaphoreGive(sink->codec_mu);
    }
    xSemaphoreGive(sink->finished);
    vTaskDelete(NULL);
}

/* Blocks while the ring is full: the decoder is paced by the writer. */
static bool sink_push(pcm_sink_t *sink, const uint8_t *data, size_t len,
                      uint32_t revision, bool test_tone)
{
    int64_t wait_started_us = 0;
    while (len) {
        bool active = test_tone ? radio_lab_mode() == RADIO_LAB_PCM_TONE : wanted(revision);
        if (!active || atomic_load(&sink->failed)) return false;
        xSemaphoreTake(sink->mu, portMAX_DELAY);
        size_t w = audio_ring_write(&sink->ring, data, len);
        radio_diag_pcm((uint32_t)audio_ring_available(&sink->ring),
                       (uint32_t)sink->ring.capacity);
        xSemaphoreGive(sink->mu);
        if (!w) {
            if (!wait_started_us) wait_started_us = esp_timer_get_time();
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (wait_started_us) {
            radio_diag_pcm_push_wait((uint32_t)((esp_timer_get_time() - wait_started_us) / 1000));
            wait_started_us = 0;
        }
        data += w;
        len -= w;
    }
    return true;
}

/* Play out everything queued (needed before changing the sample rate). */
static void sink_drain(pcm_sink_t *sink, uint32_t revision)
{
    atomic_store(&sink->flushing, true);
    for (int i = 0; i < 400 && wanted(revision) && !atomic_load(&sink->failed) &&
                    sink_used(sink) > 0; ++i) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    xSemaphoreTake(sink->codec_mu, portMAX_DELAY); /* waits for in-flight write */
    xSemaphoreGive(sink->codec_mu);
    atomic_store(&sink->flushing, false);
}

static void play_stream(const char *url, uint32_t revision, uint32_t station)
{
    esp_audio_simple_dec_type_t type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    esp_http_client_handle_t http = open_stream(url, &type);
    if (!http) {
        radio_diag_note(RD_STREAM_OPEN_FAILED, 0, 0);
        status_set("Stream nicht erreichbar");
        return;
    }
    radio_diag_note(RD_STREAM_START, 0, 0);

    /* Keep one PCM workspace in PSRAM. The decoder returns interleaved
     * stereo PCM here; downmix it IN PLACE before sending it to I2S. This
     * eliminates the second 16-KiB PSRAM workspace and the memcpy of every
     * decoded mono frame, both competing with RGB LCD DMA bandwidth. */
    uint8_t *encoded = malloc(AUDIO_READ_SIZE);
    uint8_t *pcm = heap_caps_malloc(AUDIO_PCM_CAPACITY,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t pcm_capacity = AUDIO_PCM_CAPACITY;
    esp_audio_simple_dec_handle_t decoder = NULL;
    audio_pipe_t pipe = { .http = http, .revision = revision };
    pcm_sink_t sink = {0};
    uint8_t *fifo_storage = NULL;
    bool receiver_started = false;
    bool opened = false;
    int active_rate = 0;
    int volume_applied = -1;
    unsigned underruns = 0;
    uint32_t audio_loops = 0;
    radio_diag_stack(0, (uint32_t)uxTaskGetStackHighWaterMark(NULL));

    atomic_init(&pipe.stop, false);
    atomic_init(&pipe.eof, false);
    atomic_init(&sink.stop, false);
    atomic_init(&sink.codec_open, false);
    atomic_init(&sink.flushing, false);
    atomic_init(&sink.failed, false);
    atomic_init(&sink.underruns, 0);
    pipe.mu = xSemaphoreCreateMutex();
    pipe.finished = xSemaphoreCreateBinary();
    sink.mu = xSemaphoreCreateMutex();
    sink.codec_mu = xSemaphoreCreateMutex();
    sink.finished = xSemaphoreCreateBinary();
    sink.storage = heap_caps_malloc(PCM_RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!encoded || !pcm || !pipe.mu || !pipe.finished ||
        !sink.mu || !sink.codec_mu || !sink.finished || !sink.storage) {
        ESP_LOGE(TAG, "Audio-Workbuffers: encoded=%p PCM=%p, PSRAM free=%u",
                 (void *)encoded, (void *)pcm,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
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

    audio_ring_init(&sink.ring, sink.storage, PCM_RING_BYTES);
    radio_diag_pcm(0, PCM_RING_BYTES);
    if (xTaskCreatePinnedToCore(pcm_writer_task, "radio_i2s", 4096, &sink,
                                RADIO_I2S_PRIORITY, NULL, RADIO_AUDIO_CORE) != pdPASS) {
        status_set("Zu wenig RAM fuer Audio");
        goto done;
    }
    sink.task_started = true;
    ESP_LOGI(TAG, "PCM-Puffer %u B (PSRAM), I2S-Writer-Task prio %d",
             (unsigned)PCM_RING_BYTES, RADIO_I2S_PRIORITY);

    /* Give the HTTP task and its stack first claim on scarce INTERNAL RAM.
     * The PCM workspace above is in PSRAM. The original code
     * allocated both as malloc(16384), then rejected all FIFO choices if
     * largest_free_block < capacity + 8192; hardware fell back to direct
     * HTTP reads, leaving decoder and I2S starved for seconds.
     *
     * Allocate actual FIFO block and task instead of assuming that a single
     * contiguous block must fit both (these allocations are separate).
     * Prefer a moderate SRAM FIFO.  If SRAM fragmentation still prevents
     * that, a 24-KiB PSRAM FIFO is safer than *no* independent HTTP task.
     * Direct-HTTP path remains a final recovery fallback only. */
    /* The compressed FIFO needs no internal-RAM guarantee any more: the PCM
     * ring in front of the I2S absorbs PSRAM latency. Keeping it out of
     * internal SRAM frees 16 KiB+ for Wi-Fi, lwIP and the RGB bounce buffers. */
    const size_t psram_fifo_sizes[] = {32768, 24576, 16384, 8192};
    ESP_LOGI(TAG, "Before FIFO: internal free=%u largest=%u, PSRAM free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    for (size_t i = 0; i < sizeof(psram_fifo_sizes) / sizeof(psram_fifo_sizes[0]); ++i) {
        size_t capacity = psram_fifo_sizes[i];
        fifo_storage = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!fifo_storage) continue;
        audio_ring_init(&pipe.ring, fifo_storage, capacity);
        if (xTaskCreatePinnedToCore(http_receiver_task, "radio_http", 8192,
                                    &pipe, RADIO_HTTP_PRIORITY, NULL, RADIO_AUDIO_CORE) == pdPASS) {
            receiver_started = true;
            radio_diag_fifo(0, (uint32_t)capacity);
            radio_diag_note(RD_AUDIO_FIFO_PSRAM, (uint32_t)capacity, 0);
            ESP_LOGI(TAG, "HTTP task ACTIVE, PSRAM audio FIFO: %u B", (unsigned)capacity);
            break;
        }
        heap_caps_free(fifo_storage);
        fifo_storage = NULL;
    }
    if (!receiver_started) {
        radio_diag_fifo(0, 0);
        radio_diag_note(RD_AUDIO_DIRECT, 0,
                        (int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        ESP_LOGE(TAG, "HTTP task still unavailable: internal free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }

    /* Old half-FIFO refill forced about one second of silence at 128 kbps
     * (16384 bytes at the largest FIFO). Keep this short and bounded. */
    const size_t refill = receiver_started ? 2048 : 0;
    if (receiver_started) {
        size_t start_fill = pipe.ring.capacity * 3 / 4;
        status_set("Puffert Audiostream ...");
        while (wanted(revision) && !atomic_load(&pipe.eof) &&
               pipe_used(&pipe) < start_fill) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    /* Set "Spielt" only on the transition from buffering to audio output.
     * Firmware #56 needlessly acquired a mutex for every decoded frame. */
    bool buffering = true;
    int64_t last_direct_payload_us = esp_timer_get_time();
    while (wanted(revision)) {
        int n;
        if (receiver_started) {
            if (pipe_used(&pipe) == 0) {
                if (atomic_load(&pipe.eof)) break;
                if (!buffering) {
                    ++underruns;
                    radio_diag_note(RD_FIFO_UNDERRUN, underruns, 0);
                    buffering = true;
                    status_set("Puffert Audiostream ...");
                    ESP_LOGW(TAG, "Audio FIFO underrun %u", underruns);
                }
                TickType_t refill_started = xTaskGetTickCount();
                while (wanted(revision) && !atomic_load(&pipe.eof) &&
                       pipe_used(&pipe) < refill &&
                       xTaskGetTickCount() - refill_started < pdMS_TO_TICKS(100)) {
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
            /* The direct fallback must not treat EAGAIN or a momentary
             * zero-byte read as an ended live stream. Measure this path too:
             * previously the /diagnose report falsely showed HTTP=0 for it. */
            int64_t direct_started = esp_timer_get_time();
            radio_diag_http_inflight(true);
            n = esp_http_client_read(http, (char *)encoded, AUDIO_READ_SIZE);
            radio_diag_http_inflight(false);
            radio_diag_http_read(n,
                (uint32_t)((esp_timer_get_time() - direct_started) / 1000));
            if (n > 0) {
                last_direct_payload_us = esp_timer_get_time();
            } else if (n == 0 || n == -ESP_ERR_HTTP_EAGAIN) {
                if (esp_timer_get_time() - last_direct_payload_us >= 12000000LL) {
                    radio_diag_note(RD_HTTP_STALL, 12000, 0);
                    ESP_LOGW(TAG, "Direct HTTP stream: no payload for 12s");
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            } else {
                radio_diag_note(RD_HTTP_END, (uint32_t)(-n), n);
                ESP_LOGW(TAG, "Direkter HTTP-Stream beendet: %d", n);
                break;
            }
        }

        esp_audio_simple_dec_raw_t raw = { .buffer = encoded, .len = (uint32_t)n };
        while (raw.len && wanted(revision)) {
            esp_audio_simple_dec_out_t out = {
                .buffer = pcm, .len = pcm_capacity
            };
            int64_t decode_started = esp_timer_get_time();
            esp_audio_err_t err = esp_audio_simple_dec_process(decoder, &raw, &out);
            radio_diag_decode((uint32_t)((esp_timer_get_time() - decode_started) / 1000));
            if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH &&
                out.needed_size > pcm_capacity &&
                out.needed_size <= AUDIO_PCM_MAX_CAPACITY) {
                uint8_t *bigger = heap_caps_realloc(pcm, out.needed_size,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
                if (!opened || active_rate != (int)info.sample_rate)
                    radio_diag_stream_format(info.sample_rate, info.channel, station);
                if (!opened || active_rate != (int)info.sample_rate) {
                    if (opened) {
                        sink_drain(&sink, revision);
                        xSemaphoreTake(sink.codec_mu, portMAX_DELAY);
                        atomic_store(&sink.codec_open, false);
                        esp_codec_dev_close(s_speaker);
                        xSemaphoreGive(sink.codec_mu);
                        opened = false;
                    }
                    esp_codec_dev_sample_info_t fmt = {
                        .sample_rate = (int)info.sample_rate,
                        .channel = 1,
                        .bits_per_sample = 16,
                    };
                    xSemaphoreTake(sink.codec_mu, portMAX_DELAY);
                    int open_rc = esp_codec_dev_open(s_speaker, &fmt);
                    if (open_rc == ESP_CODEC_DEV_OK) atomic_store(&sink.codec_open, true);
                    xSemaphoreGive(sink.codec_mu);
                    if (open_rc != ESP_CODEC_DEV_OK) {
                        ESP_LOGE(TAG, "ES8311 cannot open %u Hz", info.sample_rate);
                        status_set("ES8311 Audiofehler");
                        goto done;
                    }
                    opened = true;
                    active_rate = (int)info.sample_rate;
                    volume_applied = -1;
                }
                uint8_t volume = radio_config_get_volume();
                if (volume_applied != (int)volume) {
                    xSemaphoreTake(sink.codec_mu, portMAX_DELAY);
                    esp_codec_dev_set_out_vol(s_speaker, volume);
                    xSemaphoreGive(sink.codec_mu);
                    volume_applied = (int)volume;
                }
                /* In-place fold is safe: destination i always precedes both
                 * source indices 2*i and 2*i+1. For mono, no copy is needed. */
                if (info.channel == 2) {
                    int16_t *frame = (int16_t *)pcm;
                    for (size_t i = 0; i < samples; ++i) {
                        frame[i] = (int16_t)(((int32_t)frame[i * 2] +
                                             (int32_t)frame[i * 2 + 1]) / 2);
                    }
                }
                if ((++audio_loops & 127U) == 0)
                    radio_diag_stack(0, (uint32_t)uxTaskGetStackHighWaterMark(NULL));
                if (!sink_push(&sink, pcm, samples * sizeof(int16_t), revision, false)) {
                    if (atomic_load(&sink.failed)) status_set("Audioausgabe fehlgeschlagen");
                    goto done;
                }
                if (buffering) {
                    status_set("Spielt");
                    buffering = false;
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
    radio_diag_note(RD_STREAM_END, underruns, 0);
    if (receiver_started) {
        atomic_store(&pipe.stop, true);
        xSemaphoreTake(pipe.finished, portMAX_DELAY);
        ESP_LOGI(TAG, "Audiopuffer-Unterlaeufe: %u", underruns);
    }
    radio_diag_stack(0, (uint32_t)uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI(TAG, "Audiotask stack high-water: %u B free",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    if (sink.task_started) {
        atomic_store(&sink.stop, true);
        xSemaphoreTake(sink.finished, portMAX_DELAY);
        ESP_LOGI(TAG, "PCM-Unterlaeufe: %u", (unsigned)atomic_load(&sink.underruns));
    }
    radio_diag_pcm(0, 0);
    if (opened) esp_codec_dev_close(s_speaker);
    if (decoder) esp_audio_simple_dec_close(decoder);
    esp_http_client_cleanup(http);
    if (pipe.finished) vSemaphoreDelete(pipe.finished);
    if (pipe.mu) vSemaphoreDelete(pipe.mu);
    if (sink.finished) vSemaphoreDelete(sink.finished);
    if (sink.codec_mu) vSemaphoreDelete(sink.codec_mu);
    if (sink.mu) vSemaphoreDelete(sink.mu);
    heap_caps_free(sink.storage);
    heap_caps_free(fifo_storage);
    free(encoded);
    heap_caps_free(pcm);
}


/* Two strictly isolated audio tests:
 * direct: generated 1kHz tone -> codec/I2S (no Wi-Fi, no decoder, no PCM ring)
 * pcm: generated tone -> PSRAM PCM ring -> same I2S writer as radio.
 * The existing radio_audio task executes test control, so ownership of the
 * codec remains exclusive across mode transitions. */
static const int16_t s_test_sine44[44] = {0, 384, 761, 1122, 1460, 1768, 2041, 2271, 2456, 2591, 2673, 2700, 2673, 2591, 2456, 2271, 2041, 1768, 1460, 1122, 761, 384, 0, -384, -761, -1122, -1460, -1768, -2041, -2271, -2456, -2591, -2673, -2700, -2673, -2591, -2456, -2271, -2041, -1768, -1460, -1122, -761, -384};

static void test_tone_play(int mode)
{
    if (mode != RADIO_LAB_DIRECT_TONE && mode != RADIO_LAB_PCM_TONE) return;
    status_set(mode == RADIO_LAB_DIRECT_TONE ? "TEST: Ton direkt an I2S" :
                                                 "TEST: Ton ueber PCM-Puffer");
    radio_diag_fifo(0, 0);
    radio_diag_pcm(0, 0);
    esp_codec_dev_sample_info_t fmt = {
        .sample_rate = 44100, .channel = 1, .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(s_speaker, &fmt) != ESP_CODEC_DEV_OK) {
        status_set("TEST: Codec konnte nicht geoeffnet werden");
        return;
    }
    /* The diagnostic sine has a low PCM amplitude to avoid clipping. */
    uint8_t initial_volume = radio_config_get_volume();
    esp_codec_dev_set_out_vol(s_speaker, initial_volume);
    pcm_sink_t sink = {0};
    if (mode == RADIO_LAB_PCM_TONE) {
        atomic_init(&sink.stop, false);
        atomic_init(&sink.codec_open, true);
        atomic_init(&sink.flushing, false);
        atomic_init(&sink.failed, false);
        atomic_init(&sink.underruns, 0);
        sink.mu = xSemaphoreCreateMutex();
        sink.codec_mu = xSemaphoreCreateMutex();
        sink.finished = xSemaphoreCreateBinary();
        sink.storage = heap_caps_malloc(PCM_RING_BYTES,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!sink.mu || !sink.codec_mu || !sink.finished || !sink.storage) {
            status_set("TEST: Zu wenig RAM fuer PCM-Ring");
            goto finish;
        }
        audio_ring_init(&sink.ring, sink.storage, PCM_RING_BYTES);
        radio_diag_pcm(0, PCM_RING_BYTES);
        if (xTaskCreatePinnedToCore(pcm_writer_task, "radio_i2s", 4096, &sink,
                                    RADIO_I2S_PRIORITY, NULL, RADIO_AUDIO_CORE) != pdPASS) {
            status_set("TEST: I2S-Task konnte nicht starten");
            goto finish;
        }
        sink.task_started = true;
    }
    uint8_t tone_chunk[PCM_WRITE_CHUNK];
    uint32_t phase = 0;
    int64_t next_direct_us = esp_timer_get_time();
    while (radio_lab_mode() == mode) {
        uint8_t volume = radio_config_get_volume();
        if (volume != initial_volume) {
            if (sink.task_started) xSemaphoreTake(sink.codec_mu, portMAX_DELAY);
            esp_codec_dev_set_out_vol(s_speaker, volume);
            if (sink.task_started) xSemaphoreGive(sink.codec_mu);
            initial_volume = volume;
        }
        int16_t *samples = (int16_t *)tone_chunk;
        for (size_t i = 0; i < sizeof(tone_chunk) / sizeof(int16_t); ++i) {
            samples[i] = s_test_sine44[phase];
            phase = (phase + 1) % 44;
        }
        if (mode == RADIO_LAB_PCM_TONE) {
            if (!sink_push(&sink, tone_chunk, sizeof(tone_chunk), 0, true)) {
                if (atomic_load(&sink.failed)) status_set("TEST: I2S-Schreibfehler");
                break;
            }
        } else {
            int64_t started = esp_timer_get_time();
            int rc = esp_codec_dev_write(s_speaker, tone_chunk, sizeof(tone_chunk));
            radio_diag_codec((uint32_t)((esp_timer_get_time() - started) / 1000));
            radio_diag_i2s_result(rc == ESP_CODEC_DEV_OK);
            if (rc != ESP_CODEC_DEV_OK) {
                status_set("TEST: I2S-Schreibfehler");
                break;
            }
            radio_diag_audio_written();
            /* Limit the generated stream to exactly the audio sample rate
             * even if the codec write completes before hardware DMA. */
            next_direct_us += 1000000LL * (sizeof(tone_chunk) / sizeof(int16_t)) / 44100LL;
            int64_t remaining = next_direct_us - esp_timer_get_time();
            if (remaining > 2000)
                vTaskDelay(pdMS_TO_TICKS((uint32_t)(remaining / 1000)));
            else if (remaining < -500000)
                next_direct_us = esp_timer_get_time();
        }
    }

finish:
    if (sink.task_started) {
        atomic_store(&sink.stop, true);
        xSemaphoreTake(sink.finished, portMAX_DELAY);
    }
    radio_diag_pcm(0, 0);
    esp_codec_dev_close(s_speaker);
    if (sink.finished) vSemaphoreDelete(sink.finished);
    if (sink.codec_mu) vSemaphoreDelete(sink.codec_mu);
    if (sink.mu) vSemaphoreDelete(sink.mu);
    heap_caps_free(sink.storage);
}

static void stream_task(void *arg)
{
    (void)arg;
    while (!radio_wifi_connected() && radio_lab_mode() == RADIO_LAB_RADIO) {
        status_set("WLAN einrichten: 192.168.4.1");
        vTaskDelay(pdMS_TO_TICKS(750));
    }
    while (1) {
        int mode = radio_lab_mode();
        if (mode != RADIO_LAB_RADIO) {
            test_tone_play(mode);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
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
        play_stream(cfg.stations[cfg.station].url, revision, (uint32_t)cfg.station);
        vTaskDelay(pdMS_TO_TICKS(radio_lab_mode() == RADIO_LAB_RADIO ? 1500 : 100));
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
