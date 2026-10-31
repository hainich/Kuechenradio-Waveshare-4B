#include "radio_web.h"
#include "radio_config.h"
#include "radio_wifi.h"
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
static void emit(httpd_req_t *req, const char *text)
{
    httpd_resp_sendstr_chunk(req, text);
}
static esp_err_t home_get(httpd_req_t *req)
{
    radio_settings_t cfg;
    radio_config_get(&cfg);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    emit(req, "<!doctype html><html lang='de'><head><meta charset='utf-8'>"
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
    char escaped[1024];
    emit(req, "<label>WLAN-Name (SSID)</label><input name='ssid' maxlength='32' value=\"");
    html_escape(cfg.ssid, escaped, sizeof(escaped)); emit(req, escaped);
    emit(req, "\" required><label>WLAN-Passwort</label><input name='pass' type='password' "
              "maxlength='64' placeholder='Leer lassen, um bisheriges Passwort beizubehalten'>");
    for (int i = 0; i < RADIO_PRESETS; ++i) {
        char line[128];
        snprintf(line, sizeof(line), "<div class='box'><strong>Sendertaste %d</strong>", i + 1);
        emit(req, line);
        snprintf(line, sizeof(line), "<label>Name</label><input name='name%d' maxlength='39' value=\"", i);
        emit(req, line);
        html_escape(cfg.stations[i].name, escaped, sizeof(escaped)); emit(req, escaped);
        snprintf(line, sizeof(line), "\"><label>Stream-URL (MP3)</label><input name='url%d' maxlength='319' value=\"", i);
        emit(req, line);
        html_escape(cfg.stations[i].url, escaped, sizeof(escaped)); emit(req, escaped);
        emit(req, "\"></div>");
    }
    emit(req, "<button type='submit'>Speichern und neu starten</button></form>"
              "<small>Setup-WLAN: Kuechenradio-Setup / Passwort: Kuechenradio123. "
              "Einstellungen im Heimnetz ueber die IP-Adresse des Displays.</small></body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
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
void radio_web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 4;
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
    ESP_LOGI(TAG, "Konfiguration: http://192.168.4.1/ oder http://<radio-ip>/");
}
