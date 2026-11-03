// Head input: a copper-tape touch pad (ESP32-S3 capacitive touch, channel 1 on
// GPIO1/D0), or a plain push button from D0 to GND (menuconfig option).
//
// The touch hardware tracks a "benchmark" (untouched value) and flags a touch
// when the reading rises above benchmark * (1 + threshold). Through a plastic
// shell the rise is small, so the threshold is a setting. Turn on
// "Print touch readings" in menuconfig to see the numbers while you tune it.

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "app.h"
#include "board.h"
#include "input.h"

#if CONFIG_CALIOS_INPUT_TOUCH
#include "driver/touch_sens.h"
#endif

static const char *TAG = "input";
static QueueHandle_t s_edges;  // bool: true = pressed

static void edge_task(void *arg)
{
    bool pressed = false;
    int64_t down_us = 0;
    bool edge;
    for (;;) {
        if (xQueueReceive(s_edges, &edge, portMAX_DELAY) != pdTRUE || edge == pressed) {
            continue;
        }
        pressed = edge;
        if (pressed) {
            down_us = esp_timer_get_time();
            app_post(EV_BTN_DOWN, 0, NULL);
        } else {
            uint32_t ms = (uint32_t)((esp_timer_get_time() - down_us) / 1000);
            ESP_LOGI(TAG, "press %u ms", (unsigned)ms);
            app_post(EV_BTN_UP, ms, NULL);
        }
    }
}

#if CONFIG_CALIOS_INPUT_TOUCH

static touch_sensor_handle_t s_sens;
static touch_channel_handle_t s_chan;

static bool on_active(touch_sensor_handle_t sens, const touch_active_event_data_t *event, void *ctx)
{
    BaseType_t woken = pdFALSE;
    bool v = true;
    xQueueSendFromISR(s_edges, &v, &woken);
    return woken == pdTRUE;
}

static bool on_inactive(touch_sensor_handle_t sens, const touch_inactive_event_data_t *event, void *ctx)
{
    BaseType_t woken = pdFALSE;
    bool v = false;
    xQueueSendFromISR(s_edges, &v, &woken);
    return woken == pdTRUE;
}

#if CONFIG_CALIOS_TOUCH_DEBUG
static void touch_debug_task(void *arg)
{
    for (;;) {
        uint32_t bm = 0, smooth = 0;
        touch_channel_read_data(s_chan, TOUCH_CHAN_DATA_TYPE_BENCHMARK, &bm);
        touch_channel_read_data(s_chan, TOUCH_CHAN_DATA_TYPE_SMOOTH, &smooth);
        float rise = bm ? 100.0f * ((float)smooth - bm) / bm : 0;
        printf("touch: benchmark %lu  reading %lu  rise %+.2f%%  (threshold %.1f%%)\n",
               (unsigned long)bm, (unsigned long)smooth, rise, CONFIG_CALIOS_TOUCH_THRESHOLD_PERMILLE / 10.0f);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
#endif

static void touch_init(void)
{
    touch_sensor_sample_config_t sample_cfg[1] = {
        TOUCH_SENSOR_V2_DEFAULT_SAMPLE_CONFIG(500, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_2V2),
    };
    touch_sensor_config_t sens_cfg = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(1, sample_cfg);
    ESP_ERROR_CHECK(touch_sensor_new_controller(&sens_cfg, &s_sens));

    touch_channel_config_t chan_cfg = {
        .active_thresh = {2000},  // placeholder, recalculated below
        .charge_speed = TOUCH_CHARGE_SPEED_7,
        .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
    };
    ESP_ERROR_CHECK(touch_sensor_new_channel(s_sens, TOUCH_CHANNEL, &chan_cfg, &s_chan));

    touch_sensor_filter_config_t filter_cfg = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    ESP_ERROR_CHECK(touch_sensor_config_filter(s_sens, &filter_cfg));

    // Measure the untouched value, then set the threshold relative to it.
    ESP_ERROR_CHECK(touch_sensor_enable(s_sens));
    for (int i = 0; i < 3; i++) {
        ESP_ERROR_CHECK(touch_sensor_trigger_oneshot_scanning(s_sens, 2000));
    }
    ESP_ERROR_CHECK(touch_sensor_disable(s_sens));
    uint32_t benchmark = 0;
    ESP_ERROR_CHECK(touch_channel_read_data(s_chan, TOUCH_CHAN_DATA_TYPE_BENCHMARK, &benchmark));
    chan_cfg.active_thresh[0] = benchmark * CONFIG_CALIOS_TOUCH_THRESHOLD_PERMILLE / 1000;
    if (chan_cfg.active_thresh[0] == 0) {
        chan_cfg.active_thresh[0] = 1;
    }
    ESP_ERROR_CHECK(touch_sensor_reconfig_channel(s_chan, &chan_cfg));
    ESP_LOGI(TAG, "touch on GPIO%d: benchmark %lu, threshold +%lu", PIN_TOUCH,
             (unsigned long)benchmark, (unsigned long)chan_cfg.active_thresh[0]);

    touch_event_callbacks_t cbs = {
        .on_active = on_active,
        .on_inactive = on_inactive,
    };
    ESP_ERROR_CHECK(touch_sensor_register_callbacks(s_sens, &cbs, NULL));
    ESP_ERROR_CHECK(touch_sensor_enable(s_sens));
    ESP_ERROR_CHECK(touch_sensor_start_continuous_scanning(s_sens));
#if CONFIG_CALIOS_TOUCH_DEBUG
    xTaskCreate(touch_debug_task, "touch_dbg", 3072, NULL, 2, NULL);
#endif
}

#else  // push button on D0 to GND

static void button_task(void *arg)
{
    bool stable = false;
    int same = 0;
    for (;;) {
        bool now = gpio_get_level(PIN_TOUCH) == 0;
        if (now != stable) {
            if (++same >= 3) {  // 30 ms debounce
                stable = now;
                same = 0;
                xQueueSend(s_edges, &stable, 0);
            }
        } else {
            same = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

#endif

void input_init(void)
{
    s_edges = xQueueCreate(8, sizeof(bool));
    xTaskCreate(edge_task, "input", 3072, NULL, 6, NULL);
#if CONFIG_CALIOS_INPUT_TOUCH
    touch_init();
#else
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_TOUCH,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    xTaskCreate(button_task, "button", 2048, NULL, 6, NULL);
    ESP_LOGI(TAG, "push button on GPIO%d", PIN_TOUCH);
#endif
}
