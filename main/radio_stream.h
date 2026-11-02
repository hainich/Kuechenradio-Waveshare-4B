#pragma once
#include <stdbool.h>
#include <stdint.h>
void radio_stream_start(void);
const char *radio_stream_status(void);

typedef enum {
    RADIO_LAB_RADIO = 0,
    RADIO_LAB_DIRECT_TONE = 1,
    RADIO_LAB_PCM_TONE = 2
} radio_lab_mode_t;

/* Temporary RAM-only laboratory settings; reset to radio on reboot. */
int radio_lab_mode(void);
bool radio_lab_set_mode(int mode);
uint32_t radio_lab_prime_kb(void);
bool radio_lab_set_prime_kb(uint32_t kb);
