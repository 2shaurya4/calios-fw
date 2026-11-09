// Audio in and out.
//
//  Mic:     external INMP441 (I2S0 standard RX, 32-bit slots) or onboard PDM mic
//           (I2S0 PDM RX) @ 24 kHz -> high-pass + gain
//           -> mic stream buffer -> sender task -> mic callback (realtime) in 100 ms chunks
//  Speaker: replies/chirps -> playback stream buffer (45 s, PSRAM) -> playback task
//           -> volume -> I2S1 standard TX -> MAX98357A -> speaker
//
// The playback task also measures loudness, which the servo task turns into
// beak movement.

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2s_pdm.h"  // onboard mic
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "sdkconfig.h"

#include "app.h"
#include "audio.h"
#include "board.h"

static const char *TAG = "audio";

#define PLAY_SECONDS     45
#define PLAY_BUF_BYTES   (AUDIO_SAMPLE_RATE * 2 * PLAY_SECONDS)
#define PLAY_FRAME       480                    // 20 ms
#define MIC_FRAME        480                    // 20 ms
#define MIC_CHUNK        (AUDIO_SAMPLE_RATE / 10)   // 100 ms = 2400 samples
#define MIC_BUF_BYTES    (AUDIO_SAMPLE_RATE * 2 * 2) // 2 s
#define AMP_IDLE_OFF_MS  2000

static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;

static StreamBufferHandle_t s_play;
static StaticStreamBuffer_t s_play_struct;
static StreamBufferHandle_t s_mic;
static StaticStreamBuffer_t s_mic_struct;
static SemaphoreHandle_t s_play_mutex;

static volatile uint32_t s_flush_req;
static volatile uint32_t s_flush_ack;
static volatile bool s_eos;
static volatile bool s_mic_on;
static volatile float s_level;
static audio_mic_cb_t s_mic_cb;

static void amp_enable(bool on)
{
    gpio_set_level(PIN_AMP_SD, on ? 1 : 0);
}

// ---- Playback -----------------------------------------------------------------

static void drain_play_buffer(void)
{
    uint8_t scratch[512];
    while (xStreamBufferReceive(s_play, scratch, sizeof(scratch), 0) > 0) {
    }
}

static void playback_task(void *arg)
{
    static int16_t frame[PLAY_FRAME];
    const int32_t volume = CONFIG_CALIOS_VOLUME;  // 0..100
    bool amp_on = false;
    uint32_t idle_ms = 0;
    float level = 0;

    for (;;) {
        if (s_flush_ack != s_flush_req) {
            drain_play_buffer();
            s_flush_ack = s_flush_req;
        }

        size_t got = xStreamBufferReceive(s_play, frame, sizeof(frame), pdMS_TO_TICKS(30));
        if (s_flush_ack != s_flush_req) {
            continue;  // a flush arrived while we waited; drop this frame
        }

        if (got >= 2) {
            size_t n = got / 2;
            if (!amp_on) {
                amp_enable(true);
                amp_on = true;
            }
            idle_ms = 0;

            // Loudness (before volume, so the beak doesn't depend on volume).
            float acc = 0;
            for (size_t i = 0; i < n; i++) {
                float s = frame[i] / 32768.0f;
                acc += s * s;
            }
            float rms = sqrtf(acc / n);
            float target = fminf(1.0f, rms * 5.0f);
            level += (target - level) * (target > level ? 0.6f : 0.25f);
            s_level = level;

            for (size_t i = 0; i < n; i++) {
                frame[i] = (int16_t)((int32_t)frame[i] * volume / 100);
            }
            size_t written = 0;
            i2s_channel_write(s_tx, frame, n * 2, &written, portMAX_DELAY);
        } else {
            level *= 0.6f;
            s_level = level < 0.01f ? 0 : level;
            idle_ms += 30;
            if (s_eos && xStreamBufferIsEmpty(s_play)) {
                s_eos = false;
                vTaskDelay(pdMS_TO_TICKS(60));  // let the I2S DMA play out
                app_post(EV_PLAYBACK_DRAINED, 0, NULL);
            }
            if (amp_on && idle_ms > AMP_IDLE_OFF_MS) {
                amp_enable(false);  // silences hiss and saves power
                amp_on = false;
            }
        }
    }
}

void audio_play_pcm(const int16_t *pcm, size_t samples)
{
    uint32_t gen = s_flush_req;
    const uint8_t *p = (const uint8_t *)pcm;
    size_t left = samples * 2;
    int stalls = 0;

    xSemaphoreTake(s_play_mutex, portMAX_DELAY);
    while (left > 0 && s_flush_req == gen) {
        size_t chunk = left > 4096 ? 4096 : left;
        size_t w = xStreamBufferSend(s_play, p, chunk, pdMS_TO_TICKS(200));
        p += w;
        left -= w;
        if (w == 0 && ++stalls > 50) {
            ESP_LOGW(TAG, "playback buffer stuck, dropping %u bytes", (unsigned)left);
            break;
        }
    }
    xSemaphoreGive(s_play_mutex);
}

void audio_flush(void)
{
    s_eos = false;
    s_flush_req++;
    for (int i = 0; i < 20 && s_flush_ack != s_flush_req; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void audio_end_of_stream(void)
{
    s_eos = true;
}

float audio_get_level(void)
{
    return s_level;
}

// ---- Chirps (synthesized, so there are no sound files to manage) -------------

static size_t synth_sweep(int16_t *out, float f0, float f1, int ms, float amp)
{
    const int n = AUDIO_SAMPLE_RATE * ms / 1000;
    const int attack = AUDIO_SAMPLE_RATE / 200;  // 5 ms
    const int release = AUDIO_SAMPLE_RATE / 50;  // 20 ms
    float phase = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / n;
        float f = f0 * powf(f1 / f0, t);
        f *= 1.0f + 0.04f * sinf(2.0f * (float)M_PI * 35.0f * i / AUDIO_SAMPLE_RATE);  // warble
        phase += 2.0f * (float)M_PI * f / AUDIO_SAMPLE_RATE;
        float env = fminf(1.0f, (float)i / attack) * fminf(1.0f, (float)(n - i) / release);
        out[i] = (int16_t)(amp * 32767.0f * env * sinf(phase));
    }
    return n;
}

static size_t synth_silence(int16_t *out, int ms)
{
    const int n = AUDIO_SAMPLE_RATE * ms / 1000;
    memset(out, 0, n * sizeof(int16_t));
    return n;
}

static size_t synth_click(int16_t *out, int ms, float amp)
{
    const int n = AUDIO_SAMPLE_RATE * ms / 1000;
    for (int i = 0; i < n; i++) {
        float env = expf(-6.0f * i / n);
        float noise = ((int32_t)(esp_random() & 0xffff) - 32768) / 32768.0f;
        out[i] = (int16_t)(amp * 32767.0f * env * noise);
    }
    return n;
}

void audio_play_chirp(chirp_t chirp)
{
    const size_t max_samples = AUDIO_SAMPLE_RATE * 6 / 10;  // 600 ms
    int16_t *buf = heap_caps_malloc(max_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) {
        return;
    }
    size_t n = 0;
    switch (chirp) {
    case CHIRP_HELLO:
        n += synth_sweep(buf + n, 1800, 3000, 90, 0.35f);
        n += synth_silence(buf + n, 40);
        n += synth_sweep(buf + n, 2200, 3600, 110, 0.35f);
        break;
    case CHIRP_WAKE:
        n += synth_sweep(buf + n, 2000, 3400, CHIRP_WAKE_MS, 0.35f);
        break;
    case CHIRP_CANCEL:
        n += synth_sweep(buf + n, 3200, 1800, 120, 0.3f);
        break;
    case CHIRP_ERROR:
        n += synth_sweep(buf + n, 900, 700, 120, 0.35f);
        n += synth_silence(buf + n, 60);
        n += synth_sweep(buf + n, 900, 700, 120, 0.35f);
        break;
    case CHIRP_SHUTTER:
        n += synth_click(buf + n, 25, 0.4f);
        break;
    }
    audio_play_pcm(buf, n);
    free(buf);
}

// ---- Microphone ---------------------------------------------------------------

#if CONFIG_CALIOS_MIC_EXTERNAL
#define MIC_GAIN CONFIG_CALIOS_EXT_MIC_GAIN
#else
#define MIC_GAIN CONFIG_CALIOS_MIC_GAIN
#endif

// Reads one 20 ms frame as 16-bit samples, whichever mic is fitted.
static size_t mic_read_frame(int16_t *frame)
{
    size_t bytes = 0;
#if CONFIG_CALIOS_MIC_EXTERNAL
    // The INMP441 sends 24-bit samples left-aligned in 32-bit slots. Keep the top
    // 16 bits; MIC_GAIN makes up the level afterwards.
    static int32_t raw[MIC_FRAME];
    if (i2s_channel_read(s_rx, raw, sizeof(raw), &bytes, portMAX_DELAY) != ESP_OK) {
        return 0;
    }
    size_t n = bytes / sizeof(int32_t);
    for (size_t i = 0; i < n; i++) {
        frame[i] = (int16_t)(raw[i] >> 16);
    }
    return n;
#else
    if (i2s_channel_read(s_rx, frame, MIC_FRAME * sizeof(int16_t), &bytes, portMAX_DELAY) != ESP_OK) {
        return 0;
    }
    return bytes / sizeof(int16_t);
#endif
}

static void mic_task(void *arg)
{
    static int16_t frame[MIC_FRAME];
    const float gain = (float)MIC_GAIN;
    float hp_x = 0, hp_y = 0;

    for (;;) {
        size_t n = mic_read_frame(frame);
        for (size_t i = 0; i < n; i++) {
            // One-pole high-pass (~20 Hz) removes the mic's DC offset.
            float x = frame[i];
            float y = x - hp_x + 0.995f * hp_y;
            hp_x = x;
            hp_y = y;
            float v = y * gain;
            if (v > 32767.0f) {
                v = 32767.0f;
            } else if (v < -32768.0f) {
                v = -32768.0f;
            }
            frame[i] = (int16_t)v;
        }
        if (s_mic_on) {
            xStreamBufferSend(s_mic, frame, n * 2, 0);
        }
    }
}

static void mic_sender_task(void *arg)
{
    static int16_t chunk[MIC_CHUNK];
    for (;;) {
        // Blocks until a full 100 ms chunk is ready, or returns a partial one
        // after the timeout (so the tail of an utterance still gets sent).
        size_t got = xStreamBufferReceive(s_mic, chunk, sizeof(chunk), pdMS_TO_TICKS(150));
        if (got >= 2 && s_mic_cb) {
            s_mic_cb(chunk, got / 2);
        }
    }
}

void audio_mic_stream(bool on)
{
    if (on == s_mic_on) {
        return;
    }
    s_mic_on = on;
    if (!on) {
        for (int i = 0; i < 30 && !xStreamBufferIsEmpty(s_mic); i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

// ---- Init -----------------------------------------------------------------------

esp_err_t audio_init(audio_mic_cb_t mic_cb)
{
    s_mic_cb = mic_cb;

    gpio_config_t sd = {
        .pin_bit_mask = 1ULL << PIN_AMP_SD,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&sd));
    amp_enable(false);

    // Speaker: I2S1, standard (Philips) mode, mono data sent on both slots.
    i2s_chan_config_t tx_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    tx_chan.auto_clear = true;  // send silence when we run out of data
    tx_chan.dma_desc_num = 6;
    tx_chan.dma_frame_num = 240;
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan, &s_tx, NULL));
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_LRCLK,
            .dout = PIN_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));

    // Microphone on I2S0.
    i2s_chan_config_t rx_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    rx_chan.dma_desc_num = 8;
    rx_chan.dma_frame_num = MIC_FRAME;
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan, NULL, &s_rx));
#if CONFIG_CALIOS_MIC_EXTERNAL
    // INMP441: standard I2S, 64 clocks per frame (32-bit slots), L/R tied to GND
    // so it talks in the left slot. 24 kHz -> 1.536 MHz SCK, within its range.
    i2s_std_config_t mic_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_EXT_MIC_SCK,
            .ws = PIN_EXT_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = PIN_EXT_MIC_SD,
        },
    };
    mic_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &mic_cfg));
    ESP_LOGI(TAG, "mic: external INMP441 on GPIO%d/%d/%d, gain %dx",
             PIN_EXT_MIC_SCK, PIN_EXT_MIC_WS, PIN_EXT_MIC_SD, MIC_GAIN);
#else
    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = PIN_PDM_CLK,
            .din = PIN_PDM_DATA,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(s_rx, &pdm_cfg));
    ESP_LOGI(TAG, "mic: onboard PDM, gain %dx", MIC_GAIN);
#endif
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));

    // Big buffers live in PSRAM.
    uint8_t *play_storage = heap_caps_malloc(PLAY_BUF_BYTES + 1, MALLOC_CAP_SPIRAM);
    uint8_t *mic_storage = heap_caps_malloc(MIC_BUF_BYTES + 1, MALLOC_CAP_SPIRAM);
    if (!play_storage || !mic_storage) {
        ESP_LOGE(TAG, "not enough PSRAM for audio buffers");
        return ESP_ERR_NO_MEM;
    }
    s_play = xStreamBufferCreateStatic(PLAY_BUF_BYTES, 1, play_storage, &s_play_struct);
    s_mic = xStreamBufferCreateStatic(MIC_BUF_BYTES, MIC_CHUNK * 2, mic_storage, &s_mic_struct);
    s_play_mutex = xSemaphoreCreateMutex();

    xTaskCreatePinnedToCore(playback_task, "playback", 4096, NULL, 8, NULL, 1);
    xTaskCreatePinnedToCore(mic_task, "mic", 4096, NULL, 7, NULL, 1);
    xTaskCreatePinnedToCore(mic_sender_task, "mic_send", 4096, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "audio ready (%d Hz, playback buffer %d s)", AUDIO_SAMPLE_RATE, PLAY_SECONDS);
    return ESP_OK;
}
