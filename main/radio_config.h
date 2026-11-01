#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define RADIO_PRESETS 6
#define RADIO_NAME_MAX 40
#define RADIO_URL_MAX 320
#define RADIO_SSID_MAX 33
#define RADIO_PASS_MAX 65
#define RADIO_AP_SSID "Kuechenradio-Setup"
#define RADIO_AP_PASSWORD "Kuechenradio123"
typedef struct { char name[RADIO_NAME_MAX]; char url[RADIO_URL_MAX]; } radio_preset_t;
typedef struct {
    char ssid[RADIO_SSID_MAX];
    char password[RADIO_PASS_MAX];
    radio_preset_t stations[RADIO_PRESETS];
    uint8_t station;
    uint8_t volume;
    bool playing;
} radio_settings_t;
void radio_config_init(void);
void radio_config_get(radio_settings_t *out);
uint8_t radio_config_get_volume(void);
void radio_config_ui_persist_poll(void);
void radio_config_save(const radio_settings_t *settings);
void radio_config_select(int index);
void radio_config_volume(int volume, bool persist);
void radio_config_play(bool playing);
uint32_t radio_config_revision(void);
