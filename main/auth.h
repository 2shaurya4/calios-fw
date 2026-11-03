// Gets the credential used to open the voice session.
#pragma once

#include <stddef.h>
#include "esp_err.h"

// Copies the OpenAI API key set in menuconfig.
esp_err_t auth_get_key(char *out, size_t out_len);
