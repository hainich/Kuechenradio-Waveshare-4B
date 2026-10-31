#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "bsp/esp32_s3_touch_lcd_4b.h"

static const char *TAG="4B-bringup";
static lv_obj_t *label;
static unsigned int taps;

static void touch_cb(lv_event_t *evt) {
    (void)evt;
    taps++;
    lv_label_set_text_fmt(label,"Touch: %u", taps);
}

void app_main(void) {
    ESP_LOGI(TAG,"Starting hardware smoke test");
    lv_display_t *disp=bsp_display_start();
    if (!disp) {
        ESP_LOGE(TAG,"Display initialization failed");
        return;
    }
    ESP_ERROR_CHECK(bsp_display_brightness_set(75));
    esp_codec_dev_handle_t speaker=bsp_audio_codec_speaker_init();
    ESP_LOGI(TAG, "ES8311 speaker: %s", speaker ? "ready" : "FAILED");
    if (!bsp_display_lock(5000)) {
        ESP_LOGE(TAG,"LVGL lock failed");
        return;
    }
    lv_obj_t *screen=lv_screen_active();
    lv_obj_set_style_bg_color(screen,lv_color_hex(0x0B1422),0);
    lv_obj_clear_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *headline=lv_label_create(screen);
    lv_label_set_text(headline,"WAVESHARE 4B");
    lv_obj_set_style_text_color(headline,lv_color_hex(0x67E8F9),0);
    lv_obj_set_pos(headline,80,90);
    label=lv_label_create(screen);
    lv_label_set_text(label,"Touch: 0");
    lv_obj_set_style_text_color(label,lv_color_hex(0xFFFFFF),0);
    lv_obj_set_pos(label,130,190);
    lv_obj_t *btn=lv_button_create(screen);
    lv_obj_set_size(btn,240,100);
    lv_obj_set_pos(btn,120,290);
    lv_obj_t *btnlabel=lv_label_create(btn);
    lv_label_set_text(btnlabel,"ANTIPPEN");
    lv_obj_center(btnlabel);
    lv_obj_add_event_cb(btn,touch_cb,LV_EVENT_CLICKED,NULL);
    bsp_display_unlock();
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}
