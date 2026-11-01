#pragma once
/* Uses the AXP2101 PKEY short-press interrupt, not GPIO0 or TCA9554 EXIO4. */
void radio_power_button_init(void);
void radio_power_button_poll(void);
const char *radio_power_button_status(void);
