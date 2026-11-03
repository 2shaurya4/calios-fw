#include <string.h>
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "board.h"
#include "camera.h"

static const char *TAG = "camera";
static bool s_ok;

esp_err_t camera_init(void)
{
    camera_config_t cfg = {
        .pin_pwdn = -1,
        .pin_reset = -1,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_1,      // servos use LEDC_TIMER_0
        .ledc_channel = LEDC_CHANNEL_2,  // servos use channels 0 and 1
        .pixel_format = PIXFORMAT_JPEG,
#if CONFIG_CALIOS_CAMERA_VGA
        .frame_size = FRAMESIZE_VGA,     // 640x480
#else
        .frame_size = FRAMESIZE_QVGA,    // 320x240
#endif
        .jpeg_quality = 12,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };
    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed: %s (is the camera board clipped on?)", esp_err_to_name(err));
        return err;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
#if CONFIG_CALIOS_CAMERA_VFLIP
        s->set_vflip(s, 1);
#endif
#if CONFIG_CALIOS_CAMERA_HMIRROR
        s->set_hmirror(s, 1);
#endif
        ESP_LOGI(TAG, "camera ready (sensor PID 0x%x)", s->id.PID);
    }
    s_ok = true;
    return ESP_OK;
}

bool camera_available(void)
{
    return s_ok;
}

esp_err_t camera_capture(uint8_t **jpg, size_t *len)
{
    if (!s_ok) {
        return ESP_ERR_INVALID_STATE;
    }
    camera_fb_t *fb = esp_camera_fb_get();  // may be a stale frame; drop it
    if (fb) {
        esp_camera_fb_return(fb);
    }
    fb = esp_camera_fb_get();
    if (!fb) {
        return ESP_FAIL;
    }
    if (fb->format != PIXFORMAT_JPEG || fb->len == 0) {
        esp_camera_fb_return(fb);
        return ESP_FAIL;
    }
    uint8_t *copy = heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM);
    if (!copy) {
        esp_camera_fb_return(fb);
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, fb->buf, fb->len);
    *jpg = copy;
    *len = fb->len;
    esp_camera_fb_return(fb);
    return ESP_OK;
}
