// OV2640/OV3660 on the XIAO Sense board.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t camera_init(void);
bool camera_available(void);
// Captures a fresh JPEG for the parrot to look at, and keeps a copy as the
// "last photo". *jpg is heap-allocated; free() it when done.
esp_err_t camera_capture(uint8_t **jpg, size_t *len);

// For the preview page: the newest frame (may be up to one frame old), not
// remembered as the last photo.
esp_err_t camera_grab_frame(uint8_t **jpg, size_t *len);

// A copy of the last photo the parrot took. ESP_ERR_NOT_FOUND if none yet.
esp_err_t camera_copy_last_photo(uint8_t **jpg, size_t *len);
