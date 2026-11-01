#include <assert.h>
#include "radio_stream.c"

static unsigned decoder_calls, writes, reads, cleaned;
static bool connected;
static int64_t header_result;
static bool first_read_timeout, decoder_stalls;
static TickType_t ticks;
static esp_http_client_config_t http_config;
static const uint8_t payload[] = {10, 11, 12, 13};

static void reset_test(void)
{
    decoder_calls = writes = reads = cleaned = 0;
    connected = true;
    header_result = 0; /* Indefinite live response, no Content-Length. */
    first_read_timeout = decoder_stalls = false;
    ticks = 0;
    s_status_mu = (void *)1;
    s_speaker = (void *)1;
}

int main(void)
{
    reset_test();
    play_stream("https://radio.test/live.mp3", 1);
    assert(decoder_calls == 3 && writes == 3 && cleaned == 1);

    reset_test();
    first_read_timeout = true;
    play_stream("https://radio.test/live.mp3", 1);
    assert(reads == 2 && writes == 3 && cleaned == 1);

    reset_test();
    decoder_stalls = true;
    play_stream("https://radio.test/live.mp3", 1);
    assert(decoder_calls == 1 && writes == 0 && cleaned == 1);

    reset_test();
    header_result = -ESP_ERR_HTTP_EAGAIN;
    esp_audio_simple_dec_type_t type;
    assert(open_stream("https://radio.test/live.mp3", &type) == NULL);
    assert(cleaned == 1 && reads == 0);

    puts("Audio streaming tests: OK (cached PCM, transient read, stalled decoder, incomplete headers)");
    return 0;
}

/* The first input chunk contains two frames. The decoder returns a cached PCM
 * frame on its second call, leaving the last two input bytes untouched. */
esp_audio_err_t esp_audio_simple_dec_process(esp_audio_simple_dec_handle_t d,
    esp_audio_simple_dec_raw_t *raw, esp_audio_simple_dec_out_t *out)
{
    (void)d;
    ++decoder_calls;
    if (decoder_stalls) {
        raw->consumed = out->decoded_size = 0;
        return ESP_AUDIO_ERR_OK;
    }
    assert(decoder_calls <= 3);
    if (decoder_calls == 1) {
        assert(raw->len == 4 && memcmp(raw->buffer, payload, 4) == 0);
    } else {
        assert(raw->len == 2 && memcmp(raw->buffer, payload + 2, 2) == 0);
    }
    raw->consumed = decoder_calls == 2 ? 0 : 2;
    int16_t frame[] = {100, -100};
    assert(out->len >= sizeof(frame));
    memcpy(out->buffer, frame, sizeof(frame));
    out->decoded_size = sizeof(frame);
    return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_audio_simple_dec_get_info(esp_audio_simple_dec_handle_t d,
                                           esp_audio_simple_dec_info_t *info)
{
    (void)d;
    *info = (esp_audio_simple_dec_info_t){ .sample_rate = 44100,
        .bits_per_sample = 16, .channel = 1, .bitrate = 128000 };
    return ESP_AUDIO_ERR_OK;
}
int esp_codec_dev_write(esp_codec_dev_handle_t d, void *data, int bytes)
{
    (void)d;
    const int16_t *pcm = data;
    assert(bytes == 4 && pcm[0] == 100 && pcm[1] == -100);
    if (++writes == 3) connected = false;
    return ESP_CODEC_DEV_OK;
}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg)
{ http_config = *cfg; return (void *)1; }
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client)
{
    (void)client;
    esp_http_client_event_t e = {.event_id = HTTP_EVENT_ON_HEADER,
        .user_data = http_config.user_data, .header_key = "Content-Type", .header_value = "audio/mpeg"};
    http_config.event_handler(&e);
    return header_result;
}
int esp_http_client_get_status_code(esp_http_client_handle_t client)
{ (void)client; return 200; /* Even when only the status line arrived. */ }
int esp_http_client_read(esp_http_client_handle_t client, char *data, int size)
{
    (void)client;
    assert(++reads <= 2);
    if (first_read_timeout && reads == 1) return -ESP_ERR_HTTP_EAGAIN;
    assert(size >= (int)sizeof(payload));
    memcpy(data, payload, sizeof(payload));
    return sizeof(payload);
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client)
{ (void)client; return false; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{ (void)client; ++cleaned; return ESP_OK; }
esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t client, void *p)
{ (void)client; http_config.user_data = p; return ESP_OK; }
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int n)
{ (void)client; (void)n; return ESP_OK; }
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *k, const char *v)
{ (void)client; (void)k; (void)v; return ESP_OK; }
esp_err_t esp_crt_bundle_attach(void *p) { (void)p; return ESP_OK; }

bool radio_wifi_connected(void) { return connected; }
uint32_t radio_config_revision(void) { return 1; }
void radio_config_get(radio_settings_t *s) { memset(s, 0, sizeof(*s)); s->volume = 50; }
size_t heap_caps_get_largest_free_block(unsigned caps)
{ (void)caps; return 0; /* Force the supported direct path; no threads needed. */ }
void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; return malloc(n); }
void heap_caps_free(void *p) { free(p); }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return (void *)1; }
int xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; return 1; }
int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return 1; }
void vSemaphoreDelete(SemaphoreHandle_t s) { (void)s; }
void vTaskDelay(TickType_t t) { ticks += t; }
void vTaskDelete(void *p) { (void)p; }
TickType_t xTaskGetTickCount(void) { return ticks; }
int64_t esp_timer_get_time(void) { return (int64_t)ticks * 1000; }
BaseType_t xTaskCreatePinnedToCore(void (*f)(void *), const char *n, unsigned st,
                                 void *a, unsigned pr, void *h, unsigned core)
{ (void)f; (void)n; (void)st; (void)a; (void)pr; (void)h; (void)core; return 0; }
void stream_test_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void) { return (void *)1; }
int esp_codec_dev_open(esp_codec_dev_handle_t d, esp_codec_dev_sample_info_t *f)
{ (void)d; assert(f->sample_rate == 44100 && f->channel == 1); return ESP_CODEC_DEV_OK; }
int esp_codec_dev_close(esp_codec_dev_handle_t d) { (void)d; return ESP_CODEC_DEV_OK; }
int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t d, int v)
{ (void)d; (void)v; return ESP_CODEC_DEV_OK; }
esp_audio_err_t esp_audio_simple_dec_open(esp_audio_simple_dec_cfg_t *c, esp_audio_simple_dec_handle_t *d)
{ (void)c; *d = (void *)1; return ESP_AUDIO_ERR_OK; }
const char *esp_audio_simple_dec_get_name(esp_audio_simple_dec_type_t t) { (void)t; return "MP3"; }
void esp_audio_simple_dec_close(esp_audio_simple_dec_handle_t d) { (void)d; }
void esp_audio_dec_register_default(void) { }
void esp_audio_simple_dec_register_default(void) { }
