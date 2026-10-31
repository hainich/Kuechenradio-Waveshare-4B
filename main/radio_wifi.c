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

static void wifi_event(void *ctx, esp_event_base_t base, int32_t id, void *data)
{
    (void)ctx;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        /* Reconnect in STA and APSTA mode; when credentials are wrong,
         * the fallback task exposes the configuration access point. */
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        snprintf(s_address, sizeof(s_address), IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        ESP_LOGI(TAG, "Verbunden, Einstellungen: http://%s", s_address);
    }
}

/* Recovery remains active for the full lifetime: the router can fail long
 * after boot. The setup AP is enabled after 45 consecutive seconds without
 * station connectivity and disabled again after recovery.
 * All Wi-Fi mode changes occur in this one task, not the event callback.
 */
static void recovery_task(void *arg)
{
    (void)arg;
    unsigned seconds_offline = 0;
    bool recovery_ap = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (s_connected) {
            seconds_offline = 0;
            if (recovery_ap) {
                esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "WLAN wiederhergestellt; Setup-Netz ausgeschaltet");
                    recovery_ap = false;
                } else {
                    ESP_LOGW(TAG, "Setup-Netz konnte nicht deaktiviert werden: %s",
                             esp_err_to_name(err));
                }
            }
        } else {
            if (seconds_offline < 45) seconds_offline += 5;
            if (seconds_offline >= 45 && !recovery_ap) {
                esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
                if (err == ESP_OK) {
                    ESP_LOGW(TAG, "WLAN seit 45 Sekunden getrennt; Setup-Netz aktiv");
                    recovery_ap = true;
                } else {
                    ESP_LOGW(TAG, "Setup-Netz konnte nicht aktiviert werden: %s",
                             esp_err_to_name(err));
                }
            }
        }
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

    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, RADIO_AP_SSID, sizeof(ap.ap.ssid) - 1);
    strncpy((char *)ap.ap.password, RADIO_AP_PASSWORD, sizeof(ap.ap.password) - 1);
    ap.ap.ssid_len = strlen(RADIO_AP_SSID);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    /* The AP configuration is retained for recovery even when initially
     * starting only in STA mode. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    if (cfg.ssid[0]) {
        wifi_config_t sta = {0};
        strncpy((char *)sta.sta.ssid, cfg.ssid, sizeof(sta.sta.ssid) - 1);
        strncpy((char *)sta.sta.password, cfg.password, sizeof(sta.sta.password) - 1);
        sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    if (cfg.ssid[0]) {
        if (xTaskCreate(recovery_task, "wifi_recovery", 4096, NULL, 3, NULL) != pdPASS) {
            ESP_LOGE(TAG, "WLAN-Recovery-Task konnte nicht starten");
        }
    } else {
        ESP_LOGI(TAG, "Ersteinrichtung: Setup WLAN %s / http://192.168.4.1", RADIO_AP_SSID);
    }
}
bool radio_wifi_connected(void) { return s_connected; }
const char *radio_wifi_address(void) { return s_address; }
