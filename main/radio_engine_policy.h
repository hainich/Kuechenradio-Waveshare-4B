#pragma once
/* Audio engine policy: network jitter is handled in the compressed domain.
 * These values are deliberately isolated from the Waveshare BSP and UI.
 */
#include <stddef.h>
#include <stdint.h>
#define RADIO_ENGINE_CORE_AUDIO 0
#define RADIO_ENGINE_CORE_NETWORK 0
#define RADIO_ENGINE_PRIORITY_AUDIO 4
#define RADIO_ENGINE_PRIORITY_NETWORK 5
#define RADIO_ENGINE_HTTP_READ_BYTES 1024
#define RADIO_ENGINE_HTTP_TIMEOUT_MS 4500
static inline size_t radio_engine_start_threshold(size_t capacity) {
    return capacity * 3 / 4;
}
static inline size_t radio_engine_resume_threshold(size_t capacity) {
    return capacity * 2 / 3;
}
static inline size_t radio_engine_low_watermark(size_t capacity) {
    return capacity / 4;
}
