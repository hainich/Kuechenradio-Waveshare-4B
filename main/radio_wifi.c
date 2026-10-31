#include "radio_wifi.h"
#include "radio_config.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/inet.h"

static const char *TAG = "radio_wifi";
static volatile bool s_connected;
static char s_address[20] = "192.168.4.1";
static char s_home_ssid[RADIO_SSID_MAX];
static volatile unsigned s_disconnect_reason;
static volatile unsigned s_connect_attempts;

static void wifi_event(void *ctx, esp_event_base_t base, int32_t id, void *data)
{
    (void)ctx;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ++s_connect_attempts;
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        s_disconnect_reason = 0;
        ESP_LOGI(TAG, "Heim-WLAN verbunden, warte auf IP");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        const wifi_event_sta_disconnected_t *ev = (const wifi_event_sta_disconnected_t *)data;
        s_disconnect_reason = (unsigned)ev->reason;
        ESP_LOGW(TAG, "Heim-WLAN getrennt: reason=%u; neuer Verbindungsversuch", (unsigned)ev->reason);
        /* Reconnect in STA and APSTA mode; when credentials are wrong,
         * the fallback task exposes the configuration access point. */
        ++s_connect_attempts;
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        snprintf(s_address, sizeof(s_address), IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        s_disconnect_reason = 0;
        ESP_LOGI(TAG, "Verbunden, Einstellungen: http://%s", s_address);
    }
}

/* Keep the configuration AP available even when the station cannot connect.
 * This avoids locking the user out after saving incorrect or incompatible
 * router credentials.  APSTA is supported by ESP32-S3. */
void radio_wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    radio_settings_t cfg;
    radio_config_get(&cfg);
    snprintf(s_home_ssid, sizeof(s_home_ssid), "%s", cfg.ssid);

    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, RADIO_AP_SSID, sizeof(ap.ap.ssid) - 1);
    strncpy((char *)ap.ap.password, RADIO_AP_PASSWORD, sizeof(ap.ap.password) - 1);
    ap.ap.ssid_len = strlen(RADIO_AP_SSID);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    /* Keep setup access active while joining the home network. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    if (cfg.ssid[0]) {
        wifi_config_t sta = {0};
        strncpy((char *)sta.sta.ssid, cfg.ssid, sizeof(sta.sta.ssid) - 1);
        strncpy((char *)sta.sta.password, cfg.password, sizeof(sta.sta.password) - 1);
        sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    if (cfg.ssid[0]) {
        ESP_LOGI(TAG, "Verbinde mit Heim-WLAN '%s'; Setup-WLAN bleibt aktiv", cfg.ssid);
    } else {
        ESP_LOGI(TAG, "Ersteinrichtung: Setup WLAN %s / http://192.168.4.1", RADIO_AP_SSID);
    }
}
bool radio_wifi_connected(void) { return s_connected; }
const char *radio_wifi_address(void) { return s_address; }

void radio_wifi_diagnostic(char *out, size_t size)
{
    if (!out || size == 0) return;
    if (!s_home_ssid[0]) {
        snprintf(out, size, "WLAN: keine SSID gespeichert");
    } else if (s_connected) {
        snprintf(out, size, "WLAN verbunden: %s", s_address);
    } else if (s_disconnect_reason != 0) {
        snprintf(out, size, "WLAN Fehler %u: %.28s", s_disconnect_reason, s_home_ssid);
    } else {
        snprintf(out, size, "WLAN verbindet: %.28s (%u)", s_home_ssid, s_connect_attempts);
    }
}
