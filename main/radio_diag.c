#include "radio_diag.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <limits.h>
#include <string.h>

/* Entirely RAM-resident, no NVS, malloc, HTTP or serial writes on hot paths.
 * Critical sections protect brief counter operations and short ring events. */
static portMUX_TYPE s_diag_lock = portMUX_INITIALIZER_UNLOCKED;
static radio_diag_snapshot_t s_diag;
static uint32_t s_next_event;
static uint32_t s_num_events;
static uint32_t s_last_http_no_data_event_ms;
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
    ev->network_age_ms = age(now, s_diag.last_http_ms);
    ev->audio_age_ms = age(now, s_diag.last_audio_ms);
    ev->ui_age_ms = age(now, s_diag.last_ui_ms);
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
    case RD_STREAM_START: ++s_diag.stream_starts; break;
    case RD_STREAM_END: ++s_diag.stream_ends; break;
    case RD_AUDIO_FIFO_INTERNAL: s_diag.fifo_backend = 1; break;
    case RD_AUDIO_FIFO_PSRAM: s_diag.fifo_backend = 2; break;
    case RD_AUDIO_DIRECT: s_diag.fifo_backend = 0; break;
    default: break;
    }
    taskEXIT_CRITICAL(&s_diag_lock);
}

void radio_diag_http_read(int result, uint32_t duration_ms)
{
    uint32_t now = ms_now();
    taskENTER_CRITICAL(&s_diag_lock);
    ++s_diag.http_reads;
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
    taskENTER_CRITICAL(&s_diag_lock);
    if (duration_ms > s_diag.decoder_max_ms) s_diag.decoder_max_ms = duration_ms;
    taskEXIT_CRITICAL(&s_diag_lock);
    if (duration_ms >= 100) radio_diag_note(RD_DECODER_SLOW, duration_ms, 0);
}

void radio_diag_codec(uint32_t duration_ms)
{
    taskENTER_CRITICAL(&s_diag_lock);
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
    /* LVGL refresh period is 1000 ms; >1800 ms means a late callback. */
    if (previous && interval >= 1800) radio_diag_note(RD_UI_LAG, interval, 0);
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
    default: return "UNKNOWN";
    }
}
