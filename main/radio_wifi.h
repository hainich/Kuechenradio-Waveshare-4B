#pragma once
#include <stdbool.h>
#include <stddef.h>
void radio_wifi_start(void);
bool radio_wifi_connected(void);
const char *radio_wifi_address(void);

/* Human-readable station connection state for the display. */
void radio_wifi_diagnostic(char *out, size_t size);
