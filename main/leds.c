// Status LED patterns:
//   purple blink      looking for Wi-Fi
//   purple breathing  connecting to the voice service
//   dim teal breath   idle, ready
//   solid blue        listening
//   amber pulse       thinking
//   green (with voice) speaking
//   red blinks        error
// Privacy LED: red while the camera is taking a photo.

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "sdkconfig.h"

#include "audio.h"
#include "board.h"
#include "leds.h"

static const char *TAG = "leds";

static led_strip_handle_t s_strip;
static volatile parrot_state_t s_state = PS_BOOT;
static volatile int64_t s_error_until_us;
static volatile int64_t s_privacy_until_us;

void leds_set_state(parrot_state_t state)
{
    s_state = state;
}

void leds_flash_error(void)
{
    s_error_until_us = esp_timer_get_time() + 1200 * 1000;
}

void leds_show_privacy(uint32_t ms)
{
    s_privacy_until_us = esp_timer_get_time() + (int64_t)ms * 1000;
}

static float breathe(float t, float hz, float lo, float hi)
{
    float s = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * hz * t);
    return lo + (hi - lo) * s;
}

static void led_task(void *arg)
{
    const float scale = CONFIG_CALIOS_LED_BRIGHTNESS / 255.0f;
    for (;;) {
        int64_t now = esp_timer_get_time();
        float t = now / 1e6f;
        float r = 0, g = 0, b = 0, k = 1;

        switch (s_state) {
        case PS_BOOT:       r = 1; g = 1; b = 1; k = 0.2f; break;
        case PS_WIFI:       r = 0.6f; b = 1; k = fmodf(t, 1.0f) < 0.5f ? 0.6f : 0; break;
        case PS_CONNECTING: r = 0.6f; b = 1; k = breathe(t, 0.8f, 0.1f, 0.8f); break;
        case PS_IDLE:       g = 0.8f; b = 0.6f; k = breathe(t, 0.25f, 0.05f, 0.3f); break;
        case PS_LISTENING:  b = 1; k = 1; break;
        case PS_THINKING:   r = 1; g = 0.55f; k = breathe(t, 1.5f, 0.25f, 1.0f); break;
        case PS_SPEAKING:   g = 1; k = 0.3f + 0.7f * audio_get_level(); break;
        }
        if (now < s_error_until_us) {
            r = 1; g = 0; b = 0;
            k = fmodf(t, 0.25f) < 0.125f ? 1 : 0;
        }
        k *= scale;
        led_strip_set_pixel(s_strip, PIXEL_STATUS, (uint32_t)(r * k * 255), (uint32_t)(g * k * 255), (uint32_t)(b * k * 255));

        if (now < s_privacy_until_us) {
            led_strip_set_pixel(s_strip, PIXEL_PRIVACY, (uint32_t)(255 * scale), 0, 0);
        } else {
            led_strip_set_pixel(s_strip, PIXEL_PRIVACY, 0, 0, 0);
        }
        led_strip_refresh(s_strip);
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

void leds_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = PIN_NEOPIXEL,
        .max_leds = NUM_PIXELS,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    if (led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip) != ESP_OK) {
        ESP_LOGE(TAG, "LED init failed");
        return;
    }
    led_strip_clear(s_strip);
    xTaskCreatePinnedToCore(led_task, "leds", 3072, NULL, 3, NULL, 0);
}
