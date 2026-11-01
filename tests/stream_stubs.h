#pragma once
/* Host-only stand-ins for the SDK. The regression test includes the production
 * radio_stream.c, so its HTTP and decoder control flow is exercised unchanged. */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_HTTP_EAGAIN 0x7007
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
typedef unsigned TickType_t;
typedef int BaseType_t;
typedef void *SemaphoreHandle_t;
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
#define pdPASS 1
#define pdTRUE 1
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
int xSemaphoreTake(SemaphoreHandle_t, TickType_t);
int xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
void vTaskDelay(TickType_t);
void vTaskDelete(void *);
TickType_t xTaskGetTickCount(void);
BaseType_t xTaskCreatePinnedToCore(void (*)(void *), const char *, unsigned,
                                 void *, unsigned, void *, unsigned);
size_t heap_caps_get_largest_free_block(unsigned);
void *heap_caps_malloc(size_t, unsigned);
void heap_caps_free(void *);
int64_t esp_timer_get_time(void);
void stream_test_log(const char *tag, const char *fmt, ...);
#define ESP_LOGI(...) stream_test_log(__VA_ARGS__)
#define ESP_LOGW(...) stream_test_log(__VA_ARGS__)
#define ESP_LOGE(...) stream_test_log(__VA_ARGS__)

typedef void *esp_http_client_handle_t;
#define HTTP_EVENT_ON_HEADER 1
typedef struct {
    int event_id;
    void *user_data;
    const char *header_key;
    const char *header_value;
} esp_http_client_event_t;
typedef struct {
    const char *url;
    unsigned timeout_ms;
    const char *user_agent;
    esp_err_t (*crt_bundle_attach)(void *);
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    void *user_data;
    unsigned buffer_size;
    bool keep_alive_enable;
} esp_http_client_config_t;
esp_err_t esp_crt_bundle_attach(void *);
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t, const char *, const char *);
esp_err_t esp_http_client_open(esp_http_client_handle_t, int);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t, void *);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
int esp_http_client_read(esp_http_client_handle_t, char *, int);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);

typedef void *esp_codec_dev_handle_t;
typedef struct { int sample_rate, channel, bits_per_sample; } esp_codec_dev_sample_info_t;
#define ESP_CODEC_DEV_OK 0
esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void);
int esp_codec_dev_open(esp_codec_dev_handle_t, esp_codec_dev_sample_info_t *);
int esp_codec_dev_close(esp_codec_dev_handle_t);
int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t, int);
int esp_codec_dev_write(esp_codec_dev_handle_t, void *, int);

typedef int esp_audio_err_t;
#define ESP_AUDIO_ERR_OK 0
#define ESP_AUDIO_ERR_BUFF_NOT_ENOUGH 1
typedef enum {
    ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
    ESP_AUDIO_SIMPLE_DEC_TYPE_AAC,
    ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC,
    ESP_AUDIO_SIMPLE_DEC_TYPE_OGG,
} esp_audio_simple_dec_type_t;
typedef void *esp_audio_simple_dec_handle_t;
typedef struct {
    esp_audio_simple_dec_type_t dec_type;
    void *dec_cfg;
    int cfg_size;
    bool use_frame_dec;
} esp_audio_simple_dec_cfg_t;
typedef struct { uint8_t *buffer; uint32_t len; bool eos; uint32_t consumed; } esp_audio_simple_dec_raw_t;
typedef struct { uint8_t *buffer; uint32_t len, needed_size, decoded_size; } esp_audio_simple_dec_out_t;
typedef struct { uint32_t sample_rate; uint8_t bits_per_sample, channel; uint32_t bitrate, frame_size; } esp_audio_simple_dec_info_t;
typedef struct { bool aac_plus_enable; } esp_aac_dec_cfg_t;
const char *esp_audio_simple_dec_get_name(esp_audio_simple_dec_type_t);
esp_audio_err_t esp_audio_simple_dec_open(esp_audio_simple_dec_cfg_t *, esp_audio_simple_dec_handle_t *);
esp_audio_err_t esp_audio_simple_dec_process(esp_audio_simple_dec_handle_t,
                                          esp_audio_simple_dec_raw_t *, esp_audio_simple_dec_out_t *);
esp_audio_err_t esp_audio_simple_dec_get_info(esp_audio_simple_dec_handle_t, esp_audio_simple_dec_info_t *);
void esp_audio_simple_dec_close(esp_audio_simple_dec_handle_t);
void esp_audio_dec_register_default(void);
void esp_audio_simple_dec_register_default(void);
