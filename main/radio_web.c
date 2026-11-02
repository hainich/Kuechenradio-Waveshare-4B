#include "radio_web.h"
#include "radio_config.h"
#include "radio_wifi.h"
#include "radio_diag.h"
#include "radio_power_button.h"
#include "radio_lab_web.h"
#include "radio_stream.h"
#include "esp_heap_caps.h"
#include "url_codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "radio_web";
/* Build the complete setup page before sending it.  The previous version
 * sent dozens of tiny HTTP chunks and ignored transmission errors, which could
 * leave browsers displaying only the first part of the form. */
static bool page_append(char *page, size_t capacity, size_t *used, const char *text)
{
    size_t n = strlen(text);
    if (*used >= capacity || n >= capacity - *used) return false;
    memcpy(page + *used, text, n);
    *used += n;
    page[*used] = '\0';
    return true;
}

static esp_err_t home_get(httpd_req_t *req)
{
    radio_settings_t cfg;
    radio_config_get(&cfg);

    const size_t capacity = 24576;
    char *page = malloc(capacity);
    if (!page) {
        ESP_LOGE(TAG, "Kein Speicher fuer Setup-Seite");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    size_t used = 0;
    page[0] = '\0';
    char escaped[2048];
    char line[160];

#define ADD(text) do { if (!page_append(page, capacity, &used, (text))) goto too_large; } while (0)

    ADD("<!doctype html><html lang='de'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Kuechenradio</title><style>body{font:16px system-ui,sans-serif;"
        "background:#0b1422;color:#eef2f7;max-width:700px;margin:24px auto;padding:12px}"
        "h1{color:#67e8f9}label{display:block;margin:16px 0 5px}"
        "input{width:100%;box-sizing:border-box;padding:12px;border:1px solid #475569;"
        "background:#1e293b;color:white;border-radius:8px}button{background:#0f766e;"
        "color:white;border:0;border-radius:8px;padding:15px 22px;margin:24px 0;font-size:18px}"
        ".box{padding:12px;background:#172338;border-radius:10px;margin:12px 0}"
        "small{color:#94a3b8}</style></head><body><h1>Kuechenradio</h1>"
        "<p>WLAN und Lieblingssender konfigurieren. Nach Speichern startet das Radio neu.</p>"
        "<form method='post' action='/save'>");
    ADD("<label>WLAN-Name (SSID)</label><input name='ssid' maxlength='32' value=\"");
    html_escape(cfg.ssid, escaped, sizeof(escaped));
    ADD(escaped);
    ADD("\" required><label>WLAN-Passwort</label><input name='pass' type='password' "
        "maxlength='64' placeholder='Leer lassen, um bisheriges Passwort beizubehalten'>");

    for (int i = 0; i < RADIO_PRESETS; ++i) {
        snprintf(line, sizeof(line), "<div class='box'><strong>Sendertaste %d</strong>", i + 1);
        ADD(line);
        snprintf(line, sizeof(line), "<label>Name</label><input name='name%d' maxlength='39' value=\"", i);
        ADD(line);
        html_escape(cfg.stations[i].name, escaped, sizeof(escaped));
        ADD(escaped);
        snprintf(line, sizeof(line), "\"><label>Stream-URL (MP3)</label><input name='url%d' maxlength='319' value=\"", i);
        ADD(line);
        html_escape(cfg.stations[i].url, escaped, sizeof(escaped));
        ADD(escaped);
        ADD("\"></div>");
    }
    ADD("<button type='submit'>Speichern und neu starten</button></form>"
        "<p><a style='color:#67e8f9' href='/lab'>Diagnoselabor #65 oeffnen</a></p>"
        "<p><a style='color:#67e8f9' href='/diagnose'>Textdiagnose ansehen</a></p>"
        "<small>Setup-WLAN: Kuechenradio-Setup / Passwort: Kuechenradio123. "
        "Einstellungen im Heimnetz ueber die IP-Adresse des Displays.</small></body></html>");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    esp_err_t result = httpd_resp_send(req, page, used);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Setup-Seite: HTTP-Uebertragung fehlgeschlagen: %s", esp_err_to_name(result));
    } else {
        ESP_LOGI(TAG, "Setup-Seite vollstaendig gesendet: %u Bytes", (unsigned)used);
    }
    free(page);
    return result;

too_large:
    ESP_LOGE(TAG, "Setup-Seite zu gross (Kapazitaet %u Bytes)", (unsigned)capacity);
    free(page);
    httpd_resp_send_500(req);
    return ESP_FAIL;
#undef ADD
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(700));
    esp_restart();
}
static esp_err_t save_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 6000) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Formular zu gross");
        return ESP_FAIL;
    }
    char *body = calloc(1, req->content_len + 1);
    if (!body) { httpd_resp_send_500(req); return ESP_FAIL; }
    int total = 0;
    while (total < req->content_len) {
        int n = httpd_req_recv(req, body + total, req->content_len - total);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) { free(body); return ESP_FAIL; }
        total += n;
    }
    radio_settings_t cfg;
    radio_config_get(&cfg);
    char pass[RADIO_PASS_MAX] = {0};
    url_form_get(body, "ssid", cfg.ssid, sizeof(cfg.ssid));
    if (url_form_get(body, "pass", pass, sizeof(pass)) && pass[0]) {
        snprintf(cfg.password, sizeof(cfg.password), "%s", pass);
    }
    for (int i = 0; i < RADIO_PRESETS; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "name%d", i);
        url_form_get(body, key, cfg.stations[i].name, sizeof(cfg.stations[i].name));
        snprintf(key, sizeof(key), "url%d", i);
        url_form_get(body, key, cfg.stations[i].url, sizeof(cfg.stations[i].url));
        if (cfg.stations[i].url[0] &&
            strncmp(cfg.stations[i].url, "http://", 7) &&
            strncmp(cfg.stations[i].url, "https://", 8)) {
            free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Stream-URL muss mit http:// oder https:// beginnen");
            return ESP_FAIL;
        }
    }
    free(body);
    radio_config_save(&cfg);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t result = httpd_resp_sendstr(req, "<html><meta charset='utf-8'><body><h2>Gespeichert.</h2>"
                                            "<p>Das Kuechenradio startet neu.</p></body></html>");
    xTaskCreate(reboot_task, "restart", 2048, NULL, 2, NULL);
    return result;
}
/* Human-readable snapshot for support; no Wi-Fi password, SSID or stream URL.
 * Page is generated only on demand, never during normal playback. */
static esp_err_t diagnose_get(httpd_req_t *req)
{
    radio_diag_snapshot_t d;
    radio_diag_snapshot(&d);
    const size_t capacity = 13500;
    char *report = malloc(capacity);
    if (!report) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    size_t used = 0;
#define REPORT(...) do { \
    int n = snprintf(report + used, capacity - used, __VA_ARGS__); \
    if (n < 0 || (size_t)n >= capacity - used) goto diagnose_full; \
    used += (size_t)n; \
} while (0)
    REPORT("KUECHENRADIO DIAGNOSE / Build #65 Messlabor\n");
    REPORT("Web-Diagnoselabor: http://<RADIO-IP>/lab | CSV: /lab/csv\n");
    REPORT("Modus: %d (0=Radio, 1=Testton direkt, 2=Testton PCM) | PCM-Start: %lu KB\n",
           radio_lab_mode(), (unsigned long)radio_lab_prime_kb());
    REPORT("PCM aktuell %lu/%lu B, Min %lu B, Max %lu B | PCM-Ring-Leerlaeufe %lu (bis 1s nach Touch: %lu)\n",
           (unsigned long)d.pcm_bytes, (unsigned long)d.pcm_capacity,
           (unsigned long)d.pcm_min_bytes, (unsigned long)d.pcm_max_bytes,
           (unsigned long)d.pcm_underruns, (unsigned long)d.pcm_near_touch);
    REPORT("I2S Writes %lu, Fehler %lu, Writer-Stackreserve %lu B | Touch-PRESS %lu, manuelle Stoerungsmarken %lu\n",
           (unsigned long)d.i2s_write_calls, (unsigned long)d.i2s_write_errors,
           (unsigned long)d.i2s_stack_min_bytes, (unsigned long)d.touch_presses,
           (unsigned long)d.marks);
    REPORT("Physische PWR-Taste: %s\n", radio_power_button_status());
    REPORT("Uptime: %lu s | Resetgrund: %d | WLAN verbunden: %s\n",
           (unsigned long)(d.uptime_ms / 1000), (int)esp_reset_reason(),
           radio_wifi_connected() ? "ja" : "nein");
    REPORT("Intern frei: %lu B | groesster Block: %lu B | PSRAM frei: %lu B\n",
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    REPORT("HTTP-Task/Puffer: %s | Fuellung: %lu / %lu B | Underruns: %lu | Audio-Luecken >=250ms: %lu (Maximum %lu ms)\n",
           d.fifo_backend == 1 ? "AKTIV (interner RAM)" :
           d.fifo_backend == 2 ? "AKTIV (PSRAM)" : "INAKTIV (direkter HTTP-Empfang)",
           (unsigned long)d.fifo_bytes, (unsigned long)d.fifo_capacity,
           (unsigned long)d.fifo_underruns, (unsigned long)d.audio_gaps,
           (unsigned long)d.audio_gap_max_ms);
    REPORT("HTTP: %lu Reads | %lu Bytes | %lu ohne Daten | %lu langsam >=350ms | %lu Fehler | Maximum %lu ms\n",
           (unsigned long)d.http_reads, (unsigned long)d.http_bytes,
           (unsigned long)d.http_no_payload, (unsigned long)d.http_slow,
           (unsigned long)d.http_errors, (unsigned long)d.http_max_ms);
    REPORT("Decoder: %lu langsam >=100ms, Maximum %lu ms | Codec: %lu langsam >=150ms, Maximum %lu ms\n",
           (unsigned long)d.decoder_slow, (unsigned long)d.decoder_max_ms,
           (unsigned long)d.codec_slow, (unsigned long)d.codec_max_ms);
    REPORT("LVGL-Timer: %lu Verzoegerungen >=1800ms, groesstes Intervall %lu ms | Touchaktionen: %lu\n",
           (unsigned long)d.ui_lags, (unsigned long)d.ui_max_lag_ms,
           (unsigned long)d.ui_actions);
    REPORT("WLAN-Abbrueche: %lu | Stream-Starts: %lu | Stream-Enden: %lu\n",
           (unsigned long)d.wifi_disconnects, (unsigned long)d.stream_starts,
           (unsigned long)d.stream_ends);
    REPORT("Letzte erfolgreiche Daten: HTTP %s%lu ms | Audio %s%lu ms | LVGL %s%lu ms\n",
           d.last_http_ms ? "" : "nie/", (unsigned long)(d.last_http_ms ? d.uptime_ms - d.last_http_ms : 0),
           d.last_audio_ms ? "" : "nie/", (unsigned long)(d.last_audio_ms ? d.uptime_ms - d.last_audio_ms : 0),
           d.last_ui_ms ? "" : "nie/", (unsigned long)(d.last_ui_ms ? d.uptime_ms - d.last_ui_ms : 0));
    REPORT("\nLetzte %lu Ereignisse (neueste zuerst). Alle Zeitangaben in ms seit Neustart.\n",
           (unsigned long)d.event_count);
    REPORT("Zeit(ms)       Ereignis                Wert  Zusatz FIFO  HTTP-Alter Audio-Alter LVGL-Alter\n");
    for (uint32_t i = 0; i < d.event_count; ++i) {
        const radio_diag_event_t *ev = &d.events[i];
        REPORT("%10lu  %-24s %7lu %6d %5lu %10ld %11ld %10ld\n",
               (unsigned long)ev->ms, radio_diag_name((radio_diag_kind_t)ev->kind),
               (unsigned long)ev->value, (int)ev->aux,
               (unsigned long)ev->fifo_bytes,
               ev->network_age_ms == UINT32_MAX ? -1L : (long)ev->network_age_ms,
               ev->audio_age_ms == UINT32_MAX ? -1L : (long)ev->audio_age_ms,
               ev->ui_age_ms == UINT32_MAX ? -1L : (long)ev->ui_age_ms);
    }
    REPORT("\nLegende: HTTP-Alter = Zeit seit letztem empfangenen Audio-Byte, Audio-Alter = Zeit seit letzter abgeschlossener PCM-Ausgabe, LVGL-Alter = Zeit seit letztem 1s-UI-Timer. -1 = noch keine Messung.\n");
    REPORT("AUDIO_GAP misst Abstand zwischen PCM-Schreibaufrufen und beweist allein keine DAC-Stille.\n");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t result = httpd_resp_send(req, report, used);
    free(report);
    return result;
diagnose_full:
    free(report);
    httpd_resp_send_500(req);
    return ESP_FAIL;
#undef REPORT
}
void radio_web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 10;
    cfg.recv_wait_timeout = 6;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Webserver start fehlgeschlagen");
        return;
    }
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = home_get};
    httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_post};
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &save);
    httpd_uri_t diag = {.uri = "/diagnose", .method = HTTP_GET, .handler = diagnose_get};
    httpd_register_uri_handler(server, &diag);
    esp_err_t lab_err = radio_lab_web_register(server);
    if (lab_err != ESP_OK)
        ESP_LOGE(TAG, "Diagnoselabor konnte nicht registriert werden: %s",
                 esp_err_to_name(lab_err));
    ESP_LOGI(TAG, "Diagnose: http://<radio-ip>/diagnose");
    ESP_LOGI(TAG, "Messlabor: http://<radio-ip>/lab"); 
    ESP_LOGI(TAG, "Konfiguration: http://192.168.4.1/ oder http://<radio-ip>/");
}
