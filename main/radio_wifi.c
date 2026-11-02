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
#include "radio_diag.h"

static const char *TAG = "radio_wifi";
static volatile bool s_connected;
static char s_address[20] = "192.168.4.1";
static char s_home_ssid[RADIO_SSID_MAX];
static volatile unsigned s_disconnect_reason;
static volatile unsigned s_connect_attempts;
static volatile bool s_setup_ap_active;
static bool s_home_configured;

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
        radio_diag_wifi(false, -127, 0, s_disconnect_reason);
        radio_diag_note(RD_WIFI_LOST, s_disconnect_reason, 0);
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
        radio_diag_wifi(true, -127, 0, 0);
        radio_diag_note(RD_WIFI_OK, 0, 0);
        ESP_LOGI(TAG, "Verbunden, Einstellungen: http://%s", s_address);
    }
}

/*
 * Normally use STA only: an always-on setup AP adds Wi-Fi airtime.
 * If saved credentials fail for 25 seconds, expose the setup AP again.
 * Keep the fallback AP until reboot to avoid mode switches mid-stream.
 */
static void wifi_fallback_task(void *arg)
{
    (void)arg;
    unsigned disconnected_seconds = 0;
    for (;;) {
        /* Coalesce touch-driven UI saves using this already-existing low
         * priority task, rather than allocating another SRAM task stack. */
        radio_config_ui_persist_poll();
        /* One radio-signal query per second; never in decoder/codec hot paths. */
        wifi_ap_record_t ap_info = {0};
        bool signal_ok = s_connected && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
        radio_diag_wifi(s_connected, signal_ok ? ap_info.rssi : -127,
                        signal_ok ? ap_info.primary : 0, s_disconnect_reason);
        radio_diag_stack(2, (uint32_t)uxTaskGetStackHighWaterMark(NULL));
        radio_diag_sample();
        if (s_connected) {
            disconnected_seconds = 0;
        } else if (s_home_configured && !s_setup_ap_active) {
            if (++disconnected_seconds >= 25) {
                wifi_mode_t mode;
                if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_STA) {
                    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
                    if (err == ESP_OK) {
                        s_setup_ap_active = true;
                        ESP_LOGW(TAG, "Heim-WLAN nicht erreichbar: Setup-AP aktiviert");
                    } else {
                        ESP_LOGE(TAG, "Setup-AP konnte nicht starten: %s", esp_err_to_name(err));
                    }
                }
                disconnected_seconds = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

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
    s_home_configured = cfg.ssid[0] != '\0';

    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, RADIO_AP_SSID, sizeof(ap.ap.ssid) - 1);
    strncpy((char *)ap.ap.password, RADIO_AP_PASSWORD, sizeof(ap.ap.password) - 1);
    ap.ap.ssid_len = strlen(RADIO_AP_SSID);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;

    /* Configure both interfaces before selecting the normal operating mode. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    if (s_home_configured) {
        wifi_config_t sta = {0};
        size_t ssid_len = strnlen(cfg.ssid, sizeof(cfg.ssid));
        if (ssid_len > sizeof(sta.sta.ssid)) ssid_len = sizeof(sta.sta.ssid);
        memcpy(sta.sta.ssid, cfg.ssid, ssid_len);
        size_t pass_len = strnlen(cfg.password, sizeof(cfg.password));
        if (pass_len >= sizeof(sta.sta.password)) pass_len = sizeof(sta.sta.password) - 1;
        memcpy(sta.sta.password, cfg.password, pass_len);
        sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        s_setup_ap_active = true;
    }
    ESP_ERROR_CHECK(esp_wifi_start());

    if (s_home_configured) {
        /* ESP-IDF defaults to WIFI_PS_MIN_MODEM, which delays TCP reception. */
        esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "WLAN Power Save konnte nicht deaktiviert werden: %s", esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "Verbinde mit Heim-WLAN '%s' im STA-Modus", cfg.ssid);
    } else {
        ESP_LOGI(TAG, "Ersteinrichtung: Setup WLAN %s / http://192.168.4.1", RADIO_AP_SSID);
    }
    /* Run housekeeping in both STA and initial-setup modes. This task
     * existed before #61; only its existing loop gains deferred UI saves. */
    if (xTaskCreate(wifi_fallback_task, "wifi_fallback", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "WLAN-Housekeeping task konnte nicht starten");
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
    } else if (s_setup_ap_active) {
        snprintf(out, size, "Setup-WLAN aktiv: %s", RADIO_AP_SSID);
    } else if (s_disconnect_reason != 0) {
        snprintf(out, size, "WLAN Fehler %u: %.28s", s_disconnect_reason, s_home_ssid);
    } else {
        snprintf(out, size, "WLAN verbindet: %.28s (%u)", s_home_ssid, s_connect_attempts);
    }
}
