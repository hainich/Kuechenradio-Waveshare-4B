# Quellcode, Lizenzen und verifizierte API-Basis

Stand: 31.10.2026. Alle externen Bausteine sind im Projekt über Paketversionen bzw. einen konkreten Git-Commit festgelegt.

| Baustein | Fixierung | Referenz |
|---|---|---|
| ESP-IDF | 5.5.5 (GitHub Actions Dockerimage) | https://github.com/espressif/esp-idf |
| Waveshare ESP32-S3-Touch-LCD-4B BSP | 2.0.0 | https://components.espressif.com/components/waveshare/esp32_s3_touch_lcd_4b/versions/2.0.0/readme |
| LVGL | 9.3.0 | https://github.com/lvgl/lvgl/tree/v9.3.0 |
| minimp3 | ea99364f61c14656440e8d77e9c233ccf3124633 (27.07.2026) | https://github.com/lieff/minimp3/commit/ea99364f61c14656440e8d77e9c233ccf3124633 |
| GitHub Action (Espressif) | v1 | https://github.com/espressif/esp-idf-ci-action/tree/v1 |

minimp3 ist CC0. Die eigenständige Anwendungslogik steht unter MIT (siehe LICENSE).
ESP-IDF und Waveshare sind separate Abhängigkeiten mit eigenen Lizenzen und teils hardwareabhängigen Firmware-Binaries. Die Aussage "quelloffene Radiologik" umfasst deshalb nicht die vollständige Funk-Chip-Firmware.

## API-Abgleich
- Waveshare-BSP: `bsp_audio_codec_speaker_init`, `bsp_display_start`, `bsp_display_lock`, `bsp_display_unlock` in `include/bsp/esp32_s3_touch_lcd_4b.h` belegt.
- LVGL 9.3.0: Long-Label-Konstante ist `LV_LABEL_LONG_MODE_DOTS`, nicht `LV_LABEL_LONG_DOT` (im ursprünglichen Stand fehlerhaft).
- ESP-IDF 5.5: `esp_http_client_set_user_data` ist vorhanden.

API-Abgleich ist **keine** vollständige Cross-Kompilierung.