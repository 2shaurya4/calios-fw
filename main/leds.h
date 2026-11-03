// Status NeoPixel (pixel 0) and camera privacy NeoPixel (pixel 1).
#pragma once

#include <stdint.h>
#include "app.h"

void leds_init(void);
void leds_set_state(parrot_state_t state);
void leds_flash_error(void);
void leds_show_privacy(uint32_t ms);  // light the privacy LED for ms
