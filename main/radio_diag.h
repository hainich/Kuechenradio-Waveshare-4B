#pragma once
#include <stddef.h>
#include <stdint.h>

#define RADIO_DIAG_HISTORY 48

typedef enum {
    RD_HTTP_READ_SLOW = 1,
    RD_HTTP_EMPTY,
    RD_HTTP_ERROR,
    RD_HTTP_STALL,
    RD_HTTP_END,
    RD_FIFO_UNDERRUN,
    RD_AUDIO_GAP,
    RD_DECODER_SLOW,
    RD_CODEC_SLOW,
    RD_UI_LAG,
    RD_UI_ACTION,
    RD_WIFI_LOST,
    RD_WIFI_OK,
    RD_STREAM_START,
    RD_STREAM_END,
    RD_STREAM_OPEN_FAILED,
    RD_AUDIO_DIRECT
} radio_diag_kind_t;

typedef struct {
    uint32_t ms;
    uint32_t value;
    uint32_t fifo_bytes;
    uint32_t network_age_ms;
    uint32_t audio_age_ms;
    uint32_t ui_age_ms;
    uint16_t kind;
    int16_t aux;
} radio_diag_event_t;

typedef struct {
    uint32_t uptime_ms;
    uint32_t last_http_ms;
    uint32_t last_audio_ms;
    uint32_t last_ui_ms;
    uint32_t http_reads;
    uint32_t http_bytes;
    uint32_t http_no_payload;
    uint32_t http_slow;
    uint32_t http_errors;
    uint32_t http_max_ms;
    uint32_t decoder_max_ms;
    uint32_t codec_max_ms;
    uint32_t audio_gap_max_ms;
    uint32_t fifo_capacity;
    uint32_t fifo_bytes;
    uint32_t fifo_underruns;
    uint32_t audio_gaps;
    uint32_t decoder_slow;
    uint32_t codec_slow;
    uint32_t ui_lags;
    uint32_t ui_max_lag_ms;
    uint32_t ui_actions;
    uint32_t wifi_disconnects;
    uint32_t stream_starts;
    uint32_t stream_ends;
    uint32_t event_count;
    radio_diag_event_t events[RADIO_DIAG_HISTORY]; /* newest first */
} radio_diag_snapshot_t;

void radio_diag_note(radio_diag_kind_t kind, uint32_t value, int aux);
void radio_diag_http_read(int result, uint32_t duration_ms);
void radio_diag_decode(uint32_t duration_ms);
void radio_diag_codec(uint32_t duration_ms);
void radio_diag_audio_written(void);
void radio_diag_ui_tick(void);
void radio_diag_fifo(uint32_t used, uint32_t capacity);
void radio_diag_snapshot(radio_diag_snapshot_t *out);
const char *radio_diag_name(radio_diag_kind_t kind);
