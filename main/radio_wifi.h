#pragma once
#include <stdbool.h>
void radio_wifi_start(void);
bool radio_wifi_connected(void);
const char *radio_wifi_address(void);
