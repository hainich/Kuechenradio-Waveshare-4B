#include "radio_diag.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "radio_ui.h"
#include "radio_stream.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <limits.h>
#include <string.h>
#include <stdatomic.h>
#include <stddef.h>

/* Entirely RAM-resident, no NVS, malloc, HTTP or serial writes on hot paths.
 * Critical sections protect brief counter operations and short ring events. */
static portMUX_TYPE s_diag_lock = portMUX_INITIALIZER_UNLOCKED;
static radio_diag_snapshot_t s_diag;
static uint32_t s_next_event;
static uint32_t s_num_events;
static uint32_t s_last_http_no_data_event_ms;
/* The 120-point history must NOT consume scarce internal SRAM.
 * Allocate it once in external PSRAM during boot. */
static radio_diag_sample_t *s_samples;
static size_t s_samples_count;
static size_t s_samples_next;
static radio_diag_event_t *s_extended;
static size_t s_ext_count;
static size_t s_ext_next;
static uint32_t s_diag_last_backpressure_event;
static uint32_t s_diag_last_age_event;
static const char *TAG = "radio_diag";
/* Allocation-failure hook runs while allocator locks may be held: only atomics. */
static atomic_uint s_heap_fail_count;
static atomic_uint s_heap_fail_internal_count;
static atomic_uint s_heap_fail_last_size;
static atomic_uint s_heap_fail_last_caps;
static atomic_uint s_heap_fail_last_ms;
static atomic_uint s_heap_fail_function_hash;
static uint32_t s_heap_fail_seen;
static void on_alloc_failed(size_t size, uint32_t caps, const char *function_name)
{
    /* FNV-1a: bounded work, no allocation or logging inside heap hook. */
    uint32_t hash = 2166136261u;
    if (function_name) {
        for (unsigned i = 0; i < 48 && function_name[i]; ++i) {
            hash = (hash ^ (uint8_t)function_name[i]) * 16777619u;
        }
    }
    atomic_store(&s_heap_fail_function_hash, hash);
    atomic_fetch_add(&s_heap_fail_count, 1);
    if (caps & MALLOC_CAP_INTERNAL) atomic_fetch_add(&s_heap_fail_internal_count, 1);
    atomic_store(&s_heap_fail_last_size, (uint32_t)size);
    atomic_store(&s_heap_fail_last_caps, caps);
    /* esp_timer_get_time does not allocate; avoid other functions here. */
    atomic_store(&s_heap_fail_last_ms, (uint32_t)(esp_timer_get_time() / 1000));
}


void radio_diag_init(void)
{
    s_samples = heap_caps_calloc(RADIO_DIAG_SAMPLES,
                                 sizeof(radio_diag_sample_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_extended = heap_caps_calloc(RADIO_DIAG_EXT_EVENTS,
                                  sizeof(radio_diag_event_t),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_samples || !s_extended)
        ESP_LOGW(TAG, "PSRAM-Messverlauf teilweise nicht verfuegbar");
    esp_err_t hook_err = heap_caps_register_failed_alloc_callback(on_alloc_failed);
    if (hook_err != ESP_OK)
        ESP_LOGW(TAG, "Heap failure hook nicht aktiv: %d", (int)hook_err);
#ifdef CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
    s_diag.cpu_mhz_config = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
#endif
#ifdef CONFIG_SPIRAM_SPEED
    s_diag.psram_mhz_config = CONFIG_SPIRAM_SPEED;
#endif
}
static uint32_t ms_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static uint32_t age(uint32_t now, uint32_t then) {
    return then ? now - then : UINT32_MAX;
}

void radio_diag_note(radio_diag_kind_t kind, uint32_t value, int aux)
{
    uint32_t now = ms_now();
    taskENTER_CRITICAL(&s_diag_lock);
    /* Count every empty/error read, but cap their event rate. Otherwise
     * a stalled server returning zero rapidly would overwrite all history. */
    if (kind == RD_HTTP_EMPTY || kind == RD_HTTP_ERROR) {
        if (kind == RD_HTTP_EMPTY) ++s_diag.http_no_payload;
        else ++s_diag.http_errors;
        if (s_last_http_no_data_event_ms != 0 &&
            now - s_last_http_no_data_event_ms < 1500) {
            taskEXIT_CRITICAL(&s_diag_lock);
            return;
        }
        s_last_http_no_data_event_ms = now;
    }
    radio_diag_event_t *ev = &s_diag.events[s_next_event];
    ev->ms = now;
    ev->value = value;
    ev->kind = (uint16_t)kind;
    ev->aux = (int16_t)aux;
    ev->fifo_bytes = s_diag.fifo_bytes;
    ev->pcm_bytes = s_diag.pcm_bytes;
    ev->network_age_ms = age(now, s_diag.last_http_ms);
    ev->audio_age_ms = age(now, s_diag.last_audio_ms);
    ev->ui_age_ms = age(now, s_diag.last_ui_ms);
    if (s_extended) {
        s_extended[s_ext_next] = *ev;
        s_ext_next = (s_ext_next + 1) % RADIO_DIAG_EXT_EVENTS;
        if (s_ext_count < RADIO_DIAG_EXT_EVENTS) ++s_ext_count;
    }
    s_next_event = (s_next_event + 1) % RADIO_DIAG_HISTORY;
    if (s_num_events < RADIO_DIAG_HISTORY) ++s_num_events;

    switch (kind) {
    case RD_HTTP_READ_SLOW: ++s_diag.http_slow; break;
    case RD_FIFO_UNDERRUN: ++s_diag.fifo_underruns; break;
    case RD_AUDIO_GAP: ++s_diag.audio_gaps; break;
    case RD_DECODER_SLOW: ++s_diag.decoder_slow; break;
    case RD_CODEC_SLOW: ++s_diag.codec_slow; break;
    case RD_UI_LAG: ++s_diag.ui_lags; break;
    case RD_UI_ACTION: ++s_diag.ui_actions; break;
    case RD_WIFI_LOST: ++s_diag.wifi_disconnects; break;
    case RD_STREAM_START: ++s_diag.stream_starts; s_diag.decoder_last_ms = 0; break;
    case RD_STREAM_END: ++s_diag.stream_ends; s_diag.decoder_last_ms = 0; break;
    case RD_AUDIO_FIFO_INTERNAL: s_diag.fifo_backend = 1; break;
    case RD_AUDIO_FIFO_PSRAM: s_diag.fifo_backend = 2; break;
    case RD_AUDIO_DIRECT: s_diag.fifo_backend = 0; break;
    case RD_USER_MARK: ++s_diag.marks; break;
    case RD_PCM_LOW: ++s_diag.pcm_low_events; break;
    default: break;
    }
    taskEXIT_CRITICAL(&s_diag_lock);
}

void radio_diag_http_read(int result, uint32_t duration_ms)
{
    uint32_t now = ms_now();
    taskENTER_CRITICAL(&s_diag_lock);
    ++s_diag.http_reads;
    s_diag.http_last_read_ms = duration_ms;
    s_diag.http_last_result = result;
    if (duration_ms >= 100) ++s_diag.http_read_gt_100ms;
    if (duration_ms >= 1000) ++s_diag.http_read_gt_1000ms;
    if (duration_ms >= 4000) ++s_diag.http_read_gt_4000ms;
    if (result == 0) ++s_diag.http_zero_reads;
    if (result < 0) ++s_diag.http_negative_reads;
    if (duration_ms > s_diag.http_max_ms) s_diag.http_max_ms = duration_ms;
    if (result > 0) {
        s_diag.http_bytes += (uint32_t)result;
        s_diag.last_http_ms = now;
    }
    taskEXIT_CRITICAL(&s_diag_lock);
    if (duration_ms >= 350) radio_diag_note(RD_HTTP_READ_SLOW, duration_ms, result > 0 ? 1 : 0);
    if (result == 0 || result == -ESP_ERR_HTTP_EAGAIN)
        radio_diag_note(RD_HTTP_EMPTY, duration_ms, result);
    else if (result < 0)
        radio_diag_note(RD_HTTP_ERROR, duration_ms, result);
}

void radio_diag_decode(uint32_t duration_ms)
{
    uint32_t now = ms_now();
    uint32_t gap_ms = 0;
    uint32_t fifo = 0;
    bool report = false;
    taskENTER_CRITICAL(&s_diag_lock);
    ++s_diag.decode_calls;
    s_diag.decoder_total_ms += duration_ms;
    if (duration_ms > s_diag.decoder_max_ms) s_diag.decoder_max_ms = duration_ms;
    if (s_diag.decoder_last_ms) {
        gap_ms = now - s_diag.decoder_last_ms;
        if (gap_ms > s_diag.decoder_gap_max_ms) s_diag.decoder_gap_max_ms = gap_ms;
        s_diag.decoder_gap_last_ms = gap_ms;
        if (gap_ms >= 250) {
            ++s_diag.decoder_gap_count_250ms;
            if (gap_ms >= 1000) ++s_diag.decoder_gap_count_1000ms;
            fifo = s_diag.fifo_bytes;
            if (fifo >= s_diag.fifo_capacity / 2 && s_diag.fifo_capacity)
                ++s_diag.decoder_gap_while_fifo_full;
            report = true;
        }
    }
    s_diag.decoder_last_ms = now;
    taskEXIT_CRITICAL(&s_diag_lock);
    if (report) radio_diag_note(RD_DECODER_GAP, gap_ms, fifo >= 8192 ? 1 : 0);
    if (duration_ms >= 100) radio_diag_note(RD_DECODER_SLOW, duration_ms, 0);
}

void radio_diag_codec(uint32_t duration_ms)
{
    taskENTER_CRITICAL(&s_diag_lock);
    ++s_diag.codec_calls;
    s_diag.codec_total_ms += duration_ms;
    if (duration_ms > s_diag.codec_max_ms) s_diag.codec_max_ms = duration_ms;
    taskEXIT_CRITICAL(&s_diag_lock);
    if (duration_ms >= 150) radio_diag_note(RD_CODEC_SLOW, duration_ms, 0);
}

void radio_diag_audio_written(void)
{
    uint32_t now = ms_now();
    taskENTER_CRITICAL(&s_diag_lock);
    uint32_t prior = s_diag.last_audio_ms;
    s_diag.last_audio_ms = now;
    uint32_t gap = prior ? now - prior : 0;
    if (gap > s_diag.audio_gap_max_ms) s_diag.audio_gap_max_ms = gap;
    taskEXIT_CRITICAL(&s_diag_lock);
    /* Measures elapsed time between *completed* writes, not DAC silence. */
    if (prior && gap >= 250) radio_diag_note(RD_AUDIO_GAP, gap, 0);
}

void radio_diag_ui_tick(void)
{
    uint32_t now = ms_now();
    taskENTER_CRITICAL(&s_diag_lock);
    uint32_t previous = s_diag.last_ui_ms;
    s_diag.last_ui_ms = now;
    uint32_t interval = previous ? now - previous : 0;
    if (interval > s_diag.ui_max_lag_ms) s_diag.ui_max_lag_ms = interval;
    taskEXIT_CRITICAL(&s_diag_lock);
    /* Compare to the selected information-refresh interval, not a hardcoded
     * 1000 ms. The true LVGL renderer has its own independent scheduling. */
    if (previous && interval >= radio_ui_interval_get() + 800)
        radio_diag_note(RD_UI_LAG, interval, 0);
}

void radio_diag_fifo(uint32_t used, uint32_t capacity)
{
    taskENTER_CRITICAL(&s_diag_lock);
    s_diag.fifo_bytes = used;
    s_diag.fifo_capacity = capacity;
    taskEXIT_CRITICAL(&s_diag_lock);
}

void radio_diag_snapshot(radio_diag_snapshot_t *out)
{
    if (!out) return;
    uint32_t now = ms_now();
    taskENTER_CRITICAL(&s_diag_lock);
    /* Snapshot scalars while holding the lock; then reorder event ring. */
    memcpy(out, &s_diag, sizeof(*out));
    out->uptime_ms = now;
    out->event_count = s_num_events;
    for (uint32_t i = 0; i < s_num_events; ++i) {
        uint32_t index = (s_next_event + RADIO_DIAG_HISTORY - 1 - i) % RADIO_DIAG_HISTORY;
        out->events[i] = s_diag.events[index];
    }
    taskEXIT_CRITICAL(&s_diag_lock);
}

const char *radio_diag_name(radio_diag_kind_t kind)
{
    switch (kind) {
    case RD_HTTP_READ_SLOW: return "HTTP_READ_SLOW";
    case RD_HTTP_EMPTY: return "HTTP_NO_DATA";
    case RD_HTTP_ERROR: return "HTTP_READ_ERROR";
    case RD_HTTP_STALL: return "HTTP_STALL_12S";
    case RD_HTTP_END: return "HTTP_STREAM_END";
    case RD_FIFO_UNDERRUN: return "FIFO_UNDERRUN";
    case RD_AUDIO_GAP: return "PCM_WRITE_GAP";
    case RD_DECODER_SLOW: return "DECODER_SLOW";
    case RD_CODEC_SLOW: return "CODEC_WRITE_SLOW";
    case RD_UI_LAG: return "LVGL_TIMER_LATE";
    case RD_UI_ACTION: return "TOUCH_ACTION";
    case RD_WIFI_LOST: return "WIFI_DISCONNECTED";
    case RD_WIFI_OK: return "WIFI_CONNECTED";
    case RD_STREAM_START: return "STREAM_START";
    case RD_STREAM_END: return "STREAM_END";
    case RD_STREAM_OPEN_FAILED: return "STREAM_OPEN_FAILED";
    case RD_AUDIO_DIRECT: return "AUDIO_DIRECT_HTTP_FALLBACK";
    case RD_AUDIO_FIFO_INTERNAL: return "HTTP_FIFO_INTERNAL";
    case RD_AUDIO_FIFO_PSRAM: return "HTTP_FIFO_PSRAM";
    case RD_NVS_UI_SAVE: return "DEFERRED_NVS_SAVE";
    case RD_PCM_EMPTY: return "PCM_RING_EMPTY";
    case RD_TOUCH_PRESS: return "TOUCH_PRESSED";
    case RD_USER_MARK: return "USER_HEARD_GLITCH";
    case RD_I2S_WRITE_ERROR: return "I2S_WRITE_ERROR";
    case RD_HTTP_CONNECT: return "HTTP_CONNECT";
    case RD_HTTP_STATUS: return "HTTP_RESPONSE_ERROR";
    case RD_HTTP_BACKPRESSURE: return "HTTP_FIFO_FULL_WAIT";
    case RD_STREAM_FORMAT: return "STREAM_FORMAT";
    case RD_PCM_LOW: return "PCM_LOW_LEVEL";
    case RD_HTTP_GAP: return "HTTP_NO_PAYLOAD_STREAK";
    case RD_HEAP_ALLOCATION_FAILED: return "HEAP_ALLOC_FAILED";
    case RD_DECODER_GAP: return "DECODER_INTERVAL_GAP";
    case RD_PCM_PUSH_BLOCKED: return "PCM_RING_WRITE_WAIT";
    case RD_CODEC_MUTEX_WAIT: return "CODEC_MUTEX_WAIT";
    default: return "UNKNOWN";
    }
}

void radio_diag_pcm(uint32_t used, uint32_t capacity)
{
    taskENTER_CRITICAL(&s_diag_lock);
    if (capacity == 0) {
        s_diag.pcm_capacity = 0;
        s_diag.pcm_bytes = 0;
    } else {
        if (s_diag.pcm_capacity != capacity) {
            s_diag.pcm_min_bytes = capacity;
            s_diag.pcm_max_bytes = 0;
        }
        s_diag.pcm_capacity = capacity;
        s_diag.pcm_bytes = used;
        /* Exclude the initially empty, unprimed ring from the low-water mark.
         * Real emptiness after playback starts is counted separately. */
        if (used > 0 && used < s_diag.pcm_min_bytes) s_diag.pcm_min_bytes = used;
        if (used > s_diag.pcm_max_bytes) s_diag.pcm_max_bytes = used;
    }
    taskEXIT_CRITICAL(&s_diag_lock);
}

void radio_diag_pcm_underrun(void)
{
    uint32_t now = ms_now();
    taskENTER_CRITICAL(&s_diag_lock);
    ++s_diag.pcm_underruns;
    if (s_diag.last_touch_ms && now - s_diag.last_touch_ms <= 1000)
        ++s_diag.pcm_near_touch;
    taskEXIT_CRITICAL(&s_diag_lock);
    radio_diag_note(RD_PCM_EMPTY, 0, 0);
}

void radio_diag_i2s_result(bool ok)
{
    taskENTER_CRITICAL(&s_diag_lock);
    ++s_diag.i2s_write_calls;
    if (!ok) ++s_diag.i2s_write_errors;
    taskEXIT_CRITICAL(&s_diag_lock);
    if (!ok) radio_diag_note(RD_I2S_WRITE_ERROR, 0, 0);
}

void radio_diag_writer_stack(uint32_t remaining_bytes)
{
    taskENTER_CRITICAL(&s_diag_lock);
    if (!s_diag.i2s_stack_min_bytes || remaining_bytes < s_diag.i2s_stack_min_bytes)
        s_diag.i2s_stack_min_bytes = remaining_bytes;
    taskEXIT_CRITICAL(&s_diag_lock);
}

void radio_diag_touch_press(int control)
{
    uint32_t now = ms_now();
    taskENTER_CRITICAL(&s_diag_lock);
    s_diag.last_touch_ms = now;
    ++s_diag.touch_presses;
    taskEXIT_CRITICAL(&s_diag_lock);
    radio_diag_note(RD_TOUCH_PRESS, (uint32_t)control, 0);
}

void radio_diag_mark(void)
{
    radio_diag_note(RD_USER_MARK, 0, 0);
}

void radio_diag_sample(void)
{
    if (!s_samples) return;
    uint32_t free_internal = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t largest_internal = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t free_psram = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t dma_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    uint32_t dma_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    uint32_t internal_32bit = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_32BIT);
    uint32_t fail_count = atomic_load(&s_heap_fail_count);
    uint32_t fail_internal = atomic_load(&s_heap_fail_internal_count);
    uint32_t fail_size = atomic_load(&s_heap_fail_last_size);
    uint32_t fail_caps = atomic_load(&s_heap_fail_last_caps);
    uint32_t fail_time = atomic_load(&s_heap_fail_last_ms);
    uint32_t fail_function_hash = atomic_load(&s_heap_fail_function_hash);
    uint32_t now = ms_now();
    bool report_http_gap = false;
    uint32_t gap_value = 0;
    taskENTER_CRITICAL(&s_diag_lock);
    radio_diag_sample_t *s = &s_samples[s_samples_next];
    s->ms = now;
    s->pcm_bytes = s_diag.pcm_bytes;
    s->fifo_bytes = s_diag.fifo_bytes;
    s->http_bytes = s_diag.http_bytes;
    s->http_age_ms = age(now, s_diag.last_http_ms);
    s->http_last_read_ms = s_diag.http_last_read_ms;
    s->pcm_underruns = s_diag.pcm_underruns;
    s->fifo_underruns = s_diag.fifo_underruns;
    s->i2s_errors = s_diag.i2s_write_errors;
    s->touch_presses = s_diag.touch_presses;
    s->internal_free = free_internal;
    s->internal_largest = largest_internal;
    s->psram_free = free_psram;
    s->internal_dma_free = dma_free;
    s->internal_dma_largest = dma_largest;
    s->internal_32bit_free = internal_32bit;
    s->heap_fail_count = fail_count;
    s->heap_fail_last_size = fail_size;
    s->heap_fail_last_caps = fail_caps;
    s->heap_fail_function_hash = fail_function_hash;
    s->heap_fail_internal_count = fail_internal;
    s->decoder_gap_max_ms = s_diag.decoder_gap_max_ms;
    s->web_max_ms = s_diag.web_page_max_ms > s_diag.web_state_max_ms ? s_diag.web_page_max_ms : s_diag.web_state_max_ms;
    s_diag.heap_alloc_fail_count = fail_count;
    s_diag.heap_alloc_fail_internal_count = fail_internal;
    s_diag.heap_alloc_fail_last_bytes = fail_size;
    s_diag.heap_alloc_fail_last_caps = fail_caps;
    s_diag.heap_alloc_fail_last_ms = fail_time;
    s_diag.heap_alloc_fail_function_hash = fail_function_hash;
    if (!s_diag.heap_internal_min_bytes || free_internal < s_diag.heap_internal_min_bytes)
        s_diag.heap_internal_min_bytes = free_internal;
    if (!s_diag.heap_internal_largest_min_bytes || largest_internal < s_diag.heap_internal_largest_min_bytes)
        s_diag.heap_internal_largest_min_bytes = largest_internal;
    if (!s_diag.heap_psram_min_bytes || free_psram < s_diag.heap_psram_min_bytes)
        s_diag.heap_psram_min_bytes = free_psram;
    s->audio_stack_free = s_diag.audio_stack_free;
    s->http_stack_free = s_diag.http_stack_free;
    s->i2s_stack_free = s_diag.i2s_stack_min_bytes;
    s->wifi_rssi = (int16_t)s_diag.wifi_rssi;
    s->wifi_channel = (uint8_t)s_diag.wifi_channel;
    s->wifi_connected = (uint8_t)s_diag.wifi_is_connected;
    s->mode = (uint16_t)radio_lab_mode();
    s_samples_next = (s_samples_next + 1) % RADIO_DIAG_SAMPLES;
    if (s_samples_count < RADIO_DIAG_SAMPLES) ++s_samples_count;
    /* A missing HTTP payload is only meaningful while actively streaming.
     * The audio pipeline can intentionally be paused in lab modes. */
    uint32_t http_age_ms = age(now, s_diag.last_http_ms);
    if (radio_lab_mode() == RADIO_LAB_RADIO && s_diag.stream_starts > s_diag.stream_ends &&
        http_age_ms != UINT32_MAX && http_age_ms >= 1500 &&
        now - s_diag_last_age_event >= 1500) {
        s_diag_last_age_event = now;
        if (http_age_ms > s_diag.http_age_max_ms) s_diag.http_age_max_ms = http_age_ms;
        gap_value = http_age_ms;
        report_http_gap = true;
    }
    taskEXIT_CRITICAL(&s_diag_lock);
    if (report_http_gap) radio_diag_note(RD_HTTP_GAP, gap_value, 0);
    if (fail_count != s_heap_fail_seen) {
        s_heap_fail_seen = fail_count;
        radio_diag_note(RD_HEAP_ALLOCATION_FAILED, fail_size, (int)(fail_caps & 0x7fff));
    }
}

size_t radio_diag_history(radio_diag_sample_t *out, size_t capacity)
{
    if (!out || !capacity || !s_samples) return 0;
    size_t total, start;
    taskENTER_CRITICAL(&s_diag_lock);
    total = s_samples_count < capacity ? s_samples_count : capacity;
    start = (s_samples_next + RADIO_DIAG_SAMPLES - total) % RADIO_DIAG_SAMPLES;
    taskEXIT_CRITICAL(&s_diag_lock);
    /* Short fixed-size critical sections so HTTP and I2S counters are not
     * blocked while exporting a 30-minute record from PSRAM. */
    for (size_t i = 0; i < total; i += 12) {
        size_t len = (total-i < 12) ? (total-i) : 12;
        taskENTER_CRITICAL(&s_diag_lock);
        for (size_t j = 0; j < len; ++j)
            out[i+j] = s_samples[(start + i+j) % RADIO_DIAG_SAMPLES];
        taskEXIT_CRITICAL(&s_diag_lock);
    }
    return total;
}

size_t radio_diag_extended_events(radio_diag_event_t *out, size_t capacity)
{
    if (!out || !capacity || !s_extended) return 0;
    size_t count, start;
    taskENTER_CRITICAL(&s_diag_lock);
    count = s_ext_count < capacity ? s_ext_count : capacity;
    start = (s_ext_next + RADIO_DIAG_EXT_EVENTS - count) % RADIO_DIAG_EXT_EVENTS;
    taskEXIT_CRITICAL(&s_diag_lock);
    for (size_t i=0; i<count; i+=12) {
        size_t len = count-i < 12 ? count-i : 12;
        taskENTER_CRITICAL(&s_diag_lock);
        for (size_t j=0;j<len;j++)
            out[i+j] = s_extended[(start+i+j) % RADIO_DIAG_EXT_EVENTS];
        taskEXIT_CRITICAL(&s_diag_lock);
    }
    return count;
}

void radio_diag_http_inflight(bool busy)
{
    taskENTER_CRITICAL(&s_diag_lock);
    s_diag.http_inflight = busy ? 1 : 0;
    s_diag.http_read_started_ms = busy ? ms_now() : 0;
    taskEXIT_CRITICAL(&s_diag_lock);
}

void radio_diag_http_open(int32_t error, int status, uint32_t elapsed_ms, bool redirect)
{
    taskENTER_CRITICAL(&s_diag_lock);
    ++s_diag.http_connect_attempts;
    s_diag.http_open_error = error;
    s_diag.http_status = status > 0 ? (uint32_t)status : 0;
    s_diag.http_connect_last_ms = elapsed_ms;
    if (elapsed_ms > s_diag.http_connect_max_ms) s_diag.http_connect_max_ms = elapsed_ms;
    if (error != 0 || (status > 0 && status != 200 && !redirect)) ++s_diag.http_connect_failures;
    if (redirect) ++s_diag.http_redirects;
    taskEXIT_CRITICAL(&s_diag_lock);
    radio_diag_note(error || (status != 200 && !redirect) ? RD_HTTP_STATUS : RD_HTTP_CONNECT,
                    elapsed_ms, error ? (int)error : status);
}

void radio_diag_http_backpressure(uint32_t ms)
{
    uint32_t now = ms_now();
    bool emit = false;
    taskENTER_CRITICAL(&s_diag_lock);
    s_diag.http_wait_full_ms += ms;
    ++s_diag.http_backpressure_count;
    if (now - s_diag_last_backpressure_event >= 60000) {
        emit = true;
        s_diag_last_backpressure_event = now;
    }
    taskEXIT_CRITICAL(&s_diag_lock);
    if (emit) radio_diag_note(RD_HTTP_BACKPRESSURE, ms, 0);
}

void radio_diag_wifi(bool connected, int rssi, uint8_t channel, uint32_t reason)
{
    taskENTER_CRITICAL(&s_diag_lock);
    if (s_diag.wifi_is_connected != (uint32_t)connected) {
        s_diag.wifi_last_change_ms = ms_now();
        if (connected) ++s_diag.wifi_connects;
    }
    s_diag.wifi_is_connected = connected ? 1 : 0;
    s_diag.wifi_rssi = connected ? rssi : -127;
    s_diag.wifi_channel = connected ? channel : 0;
    if (reason) s_diag.wifi_last_reason = reason;
    taskEXIT_CRITICAL(&s_diag_lock);
}

void radio_diag_stack(uint32_t task, uint32_t remaining)
{
    taskENTER_CRITICAL(&s_diag_lock);
    uint32_t *target = task == 0 ? &s_diag.audio_stack_free :
                       task == 1 ? &s_diag.http_stack_free :
                       &s_diag.wifi_stack_free;
    if (*target == 0 || remaining < *target) *target = remaining;
    taskEXIT_CRITICAL(&s_diag_lock);
}

void radio_diag_stream_format(uint32_t sample_rate, uint32_t channels, uint32_t station)
{
    bool changed;
    taskENTER_CRITICAL(&s_diag_lock);
    changed = s_diag.stream_sample_rate != sample_rate ||
              s_diag.stream_channels != channels;
    s_diag.stream_sample_rate = sample_rate;
    s_diag.stream_channels = channels;
    s_diag.stream_station = station;
    taskEXIT_CRITICAL(&s_diag_lock);
    if (changed) radio_diag_note(RD_STREAM_FORMAT, sample_rate, (int)channels);
}

/* Web endpoints need scalars, not a sorted copy of the event ring. */
void radio_diag_live_snapshot(radio_diag_snapshot_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_diag_lock);
    memcpy(out, &s_diag, offsetof(radio_diag_snapshot_t, events));
    taskEXIT_CRITICAL(&s_diag_lock);
    out->uptime_ms = ms_now();
    out->heap_alloc_fail_count = atomic_load(&s_heap_fail_count);
    out->heap_alloc_fail_internal_count = atomic_load(&s_heap_fail_internal_count);
    out->heap_alloc_fail_last_bytes = atomic_load(&s_heap_fail_last_size);
    out->heap_alloc_fail_last_caps = atomic_load(&s_heap_fail_last_caps);
    out->heap_alloc_fail_last_ms = atomic_load(&s_heap_fail_last_ms);
    out->heap_alloc_fail_function_hash = atomic_load(&s_heap_fail_function_hash);
}

void radio_diag_pcm_push_wait(uint32_t ms)
{
    if (!ms) return;
    taskENTER_CRITICAL(&s_diag_lock);
    ++s_diag.pcm_push_block_count;
    s_diag.pcm_push_block_total_ms += ms;
    if (ms > s_diag.pcm_push_block_max_ms) s_diag.pcm_push_block_max_ms = ms;
    taskEXIT_CRITICAL(&s_diag_lock);
    if (ms >= 100) radio_diag_note(RD_PCM_PUSH_BLOCKED, ms, 0);
}

void radio_diag_codec_mutex_wait(uint32_t ms)
{
    if (!ms) return;
    taskENTER_CRITICAL(&s_diag_lock);
    ++s_diag.codec_mutex_wait_count;
    if (ms > s_diag.codec_mutex_wait_max_ms) s_diag.codec_mutex_wait_max_ms = ms;
    taskEXIT_CRITICAL(&s_diag_lock);
    if (ms >= 50) radio_diag_note(RD_CODEC_MUTEX_WAIT, ms, 0);
}

/* route: 1=/lab, 2=/lab/state, 3=/lab/history, 4=/lab/set|mark, 5=/lab/csv. */
void radio_diag_web_request(uint32_t route, uint32_t ms, bool ok)
{
    taskENTER_CRITICAL(&s_diag_lock);
    ++s_diag.web_request_count;
    if (!ok) ++s_diag.web_error_count;
    s_diag.web_last_ms = ms;
    s_diag.web_last_path = route;
    uint32_t *max = route == 1 ? &s_diag.web_page_max_ms :
                    route == 2 ? &s_diag.web_state_max_ms :
                    route == 3 ? &s_diag.web_history_max_ms :
                    route == 5 ? &s_diag.web_csv_max_ms : &s_diag.web_action_max_ms;
    if (ms > *max) *max = ms;
    taskEXIT_CRITICAL(&s_diag_lock);
}
