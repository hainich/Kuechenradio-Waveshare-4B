#include <stdbool.h>
#pragma once
void radio_ui_start(void);
#include <stdint.h>
bool radio_ui_interval_set(uint32_t ms);
uint32_t radio_ui_interval_get(void);
