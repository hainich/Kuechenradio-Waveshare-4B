#include "radio_ui.h"
#include "radio_config.h"
#include "radio_wifi.h"
#include "radio_stream.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "bsp/esp32_s3_touch_lcd_4b.h"
#include "lvgl.h"
#include "esp_log.h"

static lv_obj_t *s_btn[RADIO_PRESETS];
static lv_obj_t *s_label[RADIO_PRESETS];
static lv_obj_t *s_status;
static lv_obj_t *s_title;
static lv_obj_t *s_clock;
static lv_obj_t *s_slider;
static lv_obj_t *s_play;
static lv_obj_t *s_play_label;
static int s_selected = -1;
static bool s_playing = true;
static uint32_t s_last_rev;

static void preset_click(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    radio_config_select(index);
}
static void slider_event(lv_event_t *event)
{
    lv_event_code_t type = lv_event_get_code(event);
    int value = lv_slider_get_value(s_slider);
    radio_config_volume(value, type == LV_EVENT_RELEASED);
}
static void play_event(lv_event_t *event)
{
    (void)event;
    radio_config_play(!s_playing);
}
static void station_color(int i, bool active)
{
    lv_obj_set_style_bg_color(s_btn[i], lv_color_hex(active ? 0x0F766E : 0x233449), 0);
    lv_obj_set_style_border_width(s_btn[i], active ? 2 : 0, 0);
    lv_obj_set_style_border_color(s_btn[i], lv_color_hex(0x67E8F9), 0);
}
static void refresh(lv_timer_t *timer)
{
    (void)timer;
    radio_settings_t cfg;
    radio_config_get(&cfg);
    if (s_last_rev != radio_config_revision()) {
        s_last_rev = radio_config_revision();
        for (int i = 0; i < RADIO_PRESETS; ++i) {
            lv_label_set_text(s_label[i], cfg.stations[i].name);
        }
    }
    if (s_selected != cfg.station) {
        s_selected = cfg.station;
        for (int i = 0; i < RADIO_PRESETS; ++i) station_color(i, i == s_selected);
    }
    if (s_playing != cfg.playing) {
        s_playing = cfg.playing;
        lv_label_set_text(s_play_label, cfg.playing ? "II" : "> ");
    }
    lv_label_set_text(s_title, cfg.stations[cfg.station].name);
    if (!lv_obj_has_state(s_slider, LV_STATE_PRESSED)) {
        lv_slider_set_value(s_slider, cfg.volume, LV_ANIM_OFF);
    }
    lv_label_set_text(s_status, radio_stream_status());
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    char clock_text[12];
    if (t.tm_year > 120) strftime(clock_text, sizeof(clock_text), "%H:%M", &t);
    else snprintf(clock_text, sizeof(clock_text), "--:--");
    lv_label_set_text(s_clock, clock_text);
}
void radio_ui_start(void)
{
    lv_display_t *display = bsp_display_start();
    if (!display) { ESP_LOGE("radio_ui", "Display init fehlgeschlagen"); return; }
    bsp_display_brightness_set(80);
    if (!bsp_display_lock(5000)) return;
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0B1422), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xF1F5F9), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *headline = lv_label_create(screen);
    lv_label_set_text(headline, "KUECHENRADIO");
    lv_obj_set_pos(headline, 24, 15);
    lv_obj_set_style_text_color(headline, lv_color_hex(0x67E8F9), 0);
#if LV_FONT_MONTSERRAT_20
    lv_obj_set_style_text_font(headline, &lv_font_montserrat_20, 0);
#endif
    s_clock = lv_label_create(screen);
    lv_obj_set_pos(s_clock, 382, 20);
    lv_label_set_text(s_clock, "--:--");

    s_title = lv_label_create(screen);
    lv_obj_set_width(s_title, 430);
    lv_obj_set_pos(s_title, 24, 64);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_DOTS);
#if LV_FONT_MONTSERRAT_20
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_20, 0);
#endif
    s_status = lv_label_create(screen);
    lv_obj_set_width(s_status, 430);
    lv_obj_set_pos(s_status, 24, 104);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x94A3B8), 0);
    lv_label_set_text(s_status, "Starte ...");

    for (int i = 0; i < RADIO_PRESETS; ++i) {
        int x = (i % 2) ? 244 : 24;
        int y = 143 + (i / 2) * 86;
        s_btn[i] = lv_button_create(screen);
        lv_obj_set_pos(s_btn[i], x, y);
        lv_obj_set_size(s_btn[i], 212, 74);
        lv_obj_set_style_radius(s_btn[i], 13, 0);
        station_color(i, false);
        lv_obj_add_event_cb(s_btn[i], preset_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_label[i] = lv_label_create(s_btn[i]);
        lv_obj_set_width(s_label[i], 190);
        lv_label_set_long_mode(s_label[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(s_label[i], LV_TEXT_ALIGN_CENTER, 0);
#if LV_FONT_MONTSERRAT_16
        lv_obj_set_style_text_font(s_label[i], &lv_font_montserrat_16, 0);
#endif
        lv_obj_center(s_label[i]);
    }
    lv_obj_t *volume_label = lv_label_create(screen);
    lv_label_set_text(volume_label, "VOL");
    lv_obj_set_pos(volume_label, 24, 432);
    s_slider = lv_slider_create(screen);
    lv_obj_set_pos(s_slider, 70, 433);
    lv_obj_set_size(s_slider, 296, 14);
    lv_slider_set_range(s_slider, 0, 100);
    lv_obj_add_event_cb(s_slider, slider_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_slider, slider_event, LV_EVENT_RELEASED, NULL);
    lv_obj_set_style_bg_color(s_slider, lv_color_hex(0x233449), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider, lv_color_hex(0x14B8A6), LV_PART_INDICATOR);
    s_play = lv_button_create(screen);
    lv_obj_set_pos(s_play, 387, 412);
    lv_obj_set_size(s_play, 69, 54);
    lv_obj_set_style_radius(s_play, 12, 0);
    lv_obj_set_style_bg_color(s_play, lv_color_hex(0x0F766E), 0);
    lv_obj_add_event_cb(s_play, play_event, LV_EVENT_CLICKED, NULL);
    s_play_label = lv_label_create(s_play);
    lv_label_set_text(s_play_label, "II");
    lv_obj_center(s_play_label);

    radio_settings_t cfg;
    radio_config_get(&cfg);
    s_selected = cfg.station;
    s_playing = cfg.playing;
    lv_label_set_text(s_play_label, cfg.playing ? "II" : ">");
    s_last_rev = 0;
    refresh(NULL);
    lv_timer_create(refresh, 500, NULL);
    bsp_display_unlock();
}
