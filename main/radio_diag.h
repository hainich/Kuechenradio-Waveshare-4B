#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

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
    RD_AUDIO_DIRECT,
    RD_AUDIO_FIFO_INTERNAL,
    RD_AUDIO_FIFO_PSRAM,
    RD_NVS_UI_SAVE,
    RD_PCM_EMPTY,
    RD_TOUCH_PRESS,
    RD_USER_MARK,
    RD_I2S_WRITE_ERROR,
    RD_HTTP_CONNECT,
    RD_HTTP_STATUS,
    RD_HTTP_BACKPRESSURE,
    RD_STREAM_FORMAT,
    RD_PCM_LOW,
    RD_HTTP_GAP,
    RD_HEAP_ALLOCATION_FAILED,
    RD_DECODER_GAP,
    RD_PCM_PUSH_BLOCKED,
    RD_CODEC_MUTEX_WAIT
} radio_diag_kind_t;

typedef struct {
    uint32_t ms;
    uint32_t value;
    uint32_t fifo_bytes;
    uint32_t pcm_bytes;
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
    uint32_t fifo_backend; /* 0=direct, 1=internal, 2=PSRAM */
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
    uint32_t pcm_capacity;
    uint32_t pcm_bytes;
    uint32_t pcm_min_bytes;
    uint32_t pcm_max_bytes;
    uint32_t pcm_underruns;
    uint32_t pcm_near_touch;
    uint32_t i2s_write_calls;
    uint32_t i2s_write_errors;
    uint32_t i2s_stack_min_bytes;
    uint32_t last_touch_ms;
    uint32_t touch_presses;
    uint32_t marks;
    uint32_t event_count;
    /* #69 extended diagnostics: counters only, no extra work in audio writer */
    uint32_t http_last_read_ms;
    int32_t http_last_result;
    uint32_t http_read_started_ms;
    uint32_t http_inflight;
    uint32_t http_connect_attempts;
    uint32_t http_connect_failures;
    uint32_t http_connect_last_ms;
    uint32_t http_connect_max_ms;
    uint32_t http_status;
    int32_t http_open_error;
    uint32_t http_redirects;
    uint32_t http_wait_full_ms;
    uint32_t http_backpressure_count;
    uint32_t http_age_max_ms;
    uint32_t http_read_gt_100ms;
    uint32_t http_read_gt_1000ms;
    uint32_t http_read_gt_4000ms;
    uint32_t http_negative_reads;
    uint32_t http_zero_reads;
    uint32_t pcm_low_events;
    uint32_t fifo_low_events;
    uint32_t i2s_max_stack_free;
    uint32_t audio_stack_free;
    uint32_t http_stack_free;
    uint32_t wifi_stack_free;
    int32_t wifi_rssi;
    uint32_t wifi_channel;
    uint32_t wifi_last_reason;
    uint32_t wifi_connects;
    uint32_t wifi_is_connected;
    uint32_t wifi_last_change_ms;
    uint32_t stream_sample_rate;
    uint32_t stream_channels;
    uint32_t stream_station;
    uint32_t mode_at_last_mark;
    uint32_t decode_calls;
    uint32_t codec_calls;
    uint32_t decoder_total_ms;
    uint32_t codec_total_ms;
    uint32_t cpu_mhz_config;
    uint32_t psram_mhz_config;
    uint32_t capture_overflows;
    /* #67 new cause-isolation counters, no new runtime tasks */
    uint32_t heap_alloc_fail_count;
    uint32_t heap_alloc_fail_internal_count;
    uint32_t heap_alloc_fail_last_bytes;
    uint32_t heap_alloc_fail_last_caps;
    uint32_t heap_alloc_fail_last_ms;
    uint32_t heap_alloc_fail_function_hash; /* FNV-1a hash of allocator API name */
    uint32_t heap_internal_min_bytes;
    uint32_t heap_internal_largest_min_bytes;
    uint32_t heap_psram_min_bytes;
    uint32_t decoder_gap_last_ms;
    uint32_t decoder_gap_max_ms;
    uint32_t decoder_gap_count_250ms;
    uint32_t decoder_gap_count_1000ms;
    uint32_t decoder_gap_while_fifo_full;
    uint32_t pcm_push_block_count;
    uint32_t pcm_push_block_max_ms;
    uint32_t pcm_push_block_total_ms;
    uint32_t codec_mutex_wait_count;
    uint32_t codec_mutex_wait_max_ms;
    uint32_t web_request_count;
    uint32_t web_error_count;
    uint32_t web_page_max_ms;
    uint32_t web_state_max_ms;
    uint32_t web_history_max_ms;
    uint32_t web_action_max_ms;
    uint32_t web_csv_max_ms;
    uint32_t web_last_ms;
    uint32_t web_last_path;
    uint32_t decoder_last_ms;
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

/* One sample per second, recorded independently of the browser's presence. */
#define RADIO_DIAG_SAMPLES 1800 /* 30 minutes, 1 sample/s */
#define RADIO_DIAG_EXT_EVENTS 320
typedef struct {
    uint32_t ms;
    uint32_t pcm_bytes;
    uint32_t fifo_bytes;
    uint32_t http_bytes;
    uint32_t http_age_ms;
    uint32_t http_last_read_ms;
    uint32_t pcm_underruns;
    uint32_t fifo_underruns;
    uint32_t i2s_errors;
    uint32_t touch_presses;
    uint32_t internal_free;
    uint32_t internal_largest;
    uint32_t psram_free;
    uint32_t internal_dma_free;
    uint32_t internal_dma_largest;
    uint32_t internal_32bit_free;
    uint32_t heap_fail_count;
    uint32_t heap_fail_last_size;
    uint32_t heap_fail_last_caps;
    uint32_t heap_fail_function_hash;
    uint32_t heap_fail_internal_count;
    uint32_t decoder_gap_max_ms;
    uint32_t web_max_ms;
    uint32_t audio_stack_free;
    uint32_t http_stack_free;
    uint32_t i2s_stack_free;
    int16_t wifi_rssi;
    uint8_t wifi_channel;
    uint8_t wifi_connected;
    uint16_t mode;
    uint16_t reserved;
} radio_diag_sample_t;

void radio_diag_init(void);
void radio_diag_pcm(uint32_t used, uint32_t capacity);
void radio_diag_pcm_underrun(void);
void radio_diag_i2s_result(bool ok);
void radio_diag_writer_stack(uint32_t remaining_bytes);
void radio_diag_touch_press(int control);
void radio_diag_mark(void);
void radio_diag_sample(void);
size_t radio_diag_history(radio_diag_sample_t *out, size_t capacity);

/* Diagnostics run at existing safe call sites; no new FreeRTOS tasks. */
void radio_diag_http_inflight(bool busy);
void radio_diag_http_open(int32_t error, int status, uint32_t elapsed_ms, bool redirect);
void radio_diag_http_backpressure(uint32_t ms);
void radio_diag_wifi(bool connected, int rssi, uint8_t channel, uint32_t reason);
void radio_diag_stack(uint32_t task, uint32_t remaining);
void radio_diag_stream_format(uint32_t sample_rate, uint32_t channels, uint32_t station);
size_t radio_diag_extended_events(radio_diag_event_t *out, size_t capacity);

void radio_diag_live_snapshot(radio_diag_snapshot_t *out);
void radio_diag_pcm_push_wait(uint32_t ms);
void radio_diag_codec_mutex_wait(uint32_t ms);
void radio_diag_web_request(uint32_t route, uint32_t ms, bool ok);
