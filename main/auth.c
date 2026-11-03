#include <string.h>
#include "esp_log.h"
#include "sdkconfig.h"

#include "auth.h"

static const char *TAG = "auth";

esp_err_t auth_get_key(char *out, size_t out_len)
{
    if (strlen(CONFIG_CALIOS_OPENAI_API_KEY) < 20) {
        ESP_LOGE(TAG, "No OpenAI API key set. Run `idf.py menuconfig` -> Calios parrot -> OpenAI API key.");
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(out, CONFIG_CALIOS_OPENAI_API_KEY, out_len);
    return ESP_OK;
}
