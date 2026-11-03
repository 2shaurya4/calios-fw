// OV2640/OV3660 on the XIAO Sense board.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t camera_init(void);
bool camera_available(void);
// Captures a fresh JPEG. *jpg is heap-allocated; free() it when done.
esp_err_t camera_capture(uint8_t **jpg, size_t *len);
