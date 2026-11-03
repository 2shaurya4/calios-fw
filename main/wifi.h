#pragma once

#include <stdbool.h>
#include "freertos/FreeRTOS.h"

void wifi_start(void);
bool wifi_wait_connected(TickType_t timeout);
