#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")/.."
stub_dir=$(mktemp -d)
trap 'rm -rf "$stub_dir"' EXIT HUP INT TERM
mkdir -p "$stub_dir/freertos" "$stub_dir/bsp"
for header in esp_heap_caps.h esp_timer.h freertos/FreeRTOS.h freertos/task.h \
    freertos/semphr.h esp_http_client.h esp_crt_bundle.h esp_log.h \
    bsp/esp32_s3_touch_lcd_4b.h esp_codec_dev.h esp_audio_dec_default.h \
    esp_audio_simple_dec_default.h esp_audio_simple_dec.h esp_aac_dec.h; do
    printf '#include "stream_stubs.h"\n' > "$stub_dir/$header"
done
cc -std=c11 -Wall -Wextra -Werror -I"$stub_dir" -Itests -Imain \
    tests/test_radio_stream.c -o "$stub_dir/stream-test"
"$stub_dir/stream-test"
