// Beak and head-tilt servos (SG92R) on LEDC, 50 Hz.
//
// Beak: opens with the loudness of what the parrot is saying.
// Head: center when idle (with an occasional curious tilt), tilted while
// listening, swaying while thinking, small bobs while speaking.
//
// When nothing is moving the PWM is switched off, so the servos don't hum or
// jitter and draw less current. Turn that off in menuconfig if the beak droops.

#include <math.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "audio.h"
#include "board.h"
#include "servos.h"

#define SERVO_TIMER      LEDC_TIMER_0       // camera XCLK uses LEDC_TIMER_1
#define SERVO_MODE       LEDC_LOW_SPEED_MODE
#define SERVO_RES        LEDC_TIMER_14_BIT
#define CH_BEAK          LEDC_CHANNEL_0
#define CH_HEAD          LEDC_CHANNEL_1
#define TICK_MS          20
#define RELAX_AFTER_MS   1500
#if CONFIG_CALIOS_SERVO_RELAX_WHEN_IDLE
#define RELAX_ENABLED    1
#else
#define RELAX_ENABLED    0
#endif

static volatile parrot_state_t s_state = PS_BOOT;

void servos_set_state(parrot_state_t state)
{
    s_state = state;
}

static void servo_write(ledc_channel_t ch, float deg)
{
    if (deg < 0) {
        deg = 0;
    } else if (deg > 180) {
        deg = 180;
    }
    float us = 500.0f + deg * (2000.0f / 180.0f);
    uint32_t duty = (uint32_t)(us * ((1 << 14) - 1) / 20000.0f);
    ledc_set_duty(SERVO_MODE, ch, duty);
    ledc_update_duty(SERVO_MODE, ch);
}

static void servo_relax(ledc_channel_t ch)
{
    ledc_set_duty(SERVO_MODE, ch, 0);
    ledc_update_duty(SERVO_MODE, ch);
}

static float frand(float lo, float hi)
{
    return lo + (hi - lo) * (esp_random() / (float)UINT32_MAX);
}

static void servo_task(void *arg)
{
    const float beak_closed = CONFIG_CALIOS_BEAK_CLOSED_DEG;
    const float beak_open = CONFIG_CALIOS_BEAK_OPEN_DEG;
    const float center = CONFIG_CALIOS_HEAD_CENTER_DEG;
    const float tilt = CONFIG_CALIOS_HEAD_TILT_DEG;

    float beak = beak_closed, head = center;
    float last_beak_out = -1, last_head_out = -1;
    int beak_still_ms = 0, head_still_ms = 0;
    bool beak_relaxed = false, head_relaxed = false;
    int64_t next_idle_tilt = esp_timer_get_time() + 15 * 1000000LL;
    int64_t idle_tilt_until = 0;
    float idle_tilt = 0;

    for (;;) {
        int64_t now = esp_timer_get_time();
        float t = now / 1e6f;
        float level = audio_get_level();

        // Beak follows loudness.
        float beak_target = beak_closed + level * (beak_open - beak_closed);
        beak += (beak_target - beak) * 0.6f;

        // Head shows mood.
        float head_target = center;
        switch (s_state) {
        case PS_IDLE:
            if (now > next_idle_tilt) {
                idle_tilt = frand(-0.7f, 0.7f) * tilt;
                idle_tilt_until = now + 1200 * 1000;
                next_idle_tilt = now + (int64_t)frand(12, 25) * 1000000LL;
            }
            head_target = center + (now < idle_tilt_until ? idle_tilt : 0);
            break;
        case PS_LISTENING:
            head_target = center + tilt + 2.0f * sinf(t * 1.5f);
            break;
        case PS_THINKING:
            head_target = center - tilt * 0.5f + 5.0f * sinf(t * 2.2f);
            break;
        case PS_SPEAKING:
            head_target = center + 4.0f * sinf(t * 3.1f) + level * 4.0f;
            break;
        default:
            break;
        }
        head += (head_target - head) * 0.08f;

        // Write, or relax when still.
        if (fabsf(beak - last_beak_out) > 0.3f) {
            servo_write(CH_BEAK, beak);
            last_beak_out = beak;
            beak_still_ms = 0;
            beak_relaxed = false;
        } else if (RELAX_ENABLED && !beak_relaxed && (beak_still_ms += TICK_MS) > RELAX_AFTER_MS) {
            servo_relax(CH_BEAK);
            beak_relaxed = true;
        }
        if (fabsf(head - last_head_out) > 0.3f) {
            servo_write(CH_HEAD, head);
            last_head_out = head;
            head_still_ms = 0;
            head_relaxed = false;
        } else if (RELAX_ENABLED && !head_relaxed && (head_still_ms += TICK_MS) > RELAX_AFTER_MS) {
            servo_relax(CH_HEAD);
            head_relaxed = true;
        }
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

void servos_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = SERVO_MODE,
        .duty_resolution = SERVO_RES,
        .timer_num = SERVO_TIMER,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t ch = {
        .speed_mode = SERVO_MODE,
        .timer_sel = SERVO_TIMER,
        .duty = 0,
    };
    ch.channel = CH_BEAK;
    ch.gpio_num = PIN_SERVO_BEAK;
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
    ch.channel = CH_HEAD;
    ch.gpio_num = PIN_SERVO_HEAD;
    ESP_ERROR_CHECK(ledc_channel_config(&ch));

    servo_write(CH_BEAK, CONFIG_CALIOS_BEAK_CLOSED_DEG);
    servo_write(CH_HEAD, CONFIG_CALIOS_HEAD_CENTER_DEG);
    xTaskCreatePinnedToCore(servo_task, "servos", 3072, NULL, 4, NULL, 1);
}
