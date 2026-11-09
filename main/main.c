// Calios parrot MVP firmware for the Seeed XIAO ESP32S3 Sense.
// See README.md for setup and docs/WIRING.md for the wiring.

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "app.h"
#include "audio.h"
#include "camera.h"
#include "input.h"
#include "leds.h"
#include "preview.h"
#include "realtime.h"
#include "servos.h"
#include "wifi.h"

static const char *TAG = "calios";

// Replies arrive as large JSON messages; parse them in PSRAM.
static void *psram_malloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Calios parrot starting");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    cJSON_Hooks hooks = {.malloc_fn = psram_malloc, .free_fn = free};
    cJSON_InitHooks(&hooks);

    app_init();
    leds_init();
    servos_init();
    ESP_ERROR_CHECK(audio_init(realtime_send_audio));
#if CONFIG_CALIOS_CAMERA_ENABLE
    if (camera_init() != ESP_OK) {
        ESP_LOGW(TAG, "continuing without the camera");
    }
#endif
    input_init();
    wifi_start();
#if CONFIG_CALIOS_CAMERA_PREVIEW
    if (camera_available()) {
        preview_start();
    }
#endif
    realtime_start();

    ESP_LOGI(TAG, "free internal RAM %u, free PSRAM %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    app_run();
}
