// Camera preview web page, for testing.
//
//   http://<parrot IP>/         page with the live view and the last photo
//   http://<parrot IP>/photo    one snapshot (JPEG)
//   http://<parrot IP>/last     the last photo the parrot sent to OpenAI (JPEG)
//   http://<parrot IP>:81/stream  live MJPEG stream (on its own port so the
//                                  page stays responsive while it runs)
//
// The IP address is printed in the log once Wi-Fi connects. The red privacy LED
// stays on while anyone is watching the live view.

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"

#include "camera.h"
#include "leds.h"
#include "preview.h"

static const char *TAG = "preview";

#define BOUNDARY "calioscamframe"
#define STREAM_FRAME_MS 100  // ~10 frames per second at most

static const char INDEX_HTML[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Calios camera</title><style>"
    "body{font-family:system-ui,sans-serif;margin:16px;background:#111;color:#eee}"
    "h1{font-size:20px}h2{font-size:16px;margin:0 0 8px}"
    ".row{display:flex;gap:20px;flex-wrap:wrap}.col{flex:1;min-width:280px}"
    "img{width:100%;max-width:640px;aspect-ratio:4/3;object-fit:contain;background:#222;border-radius:8px;display:block}"
    "button,a{font:inherit;margin-top:8px;margin-right:8px;color:#9cf}"
    "button{background:#2a2f36;border:1px solid #4a515c;border-radius:6px;padding:4px 12px;cursor:pointer}"
    "p{color:#aaa;font-size:14px}"
    "</style></head><body><h1>Calios camera</h1><div class=row>"
    "<div class=col><h2>Live</h2><img id=live alt='live view'>"
    "<button id=toggle>Stop</button><a href='/photo' download='calios.jpg'>Save a snapshot</a></div>"
    "<div class=col><h2>Last photo the parrot sent</h2><img id=last alt='No photo yet'>"
    "<button id=refresh>Refresh</button></div></div>"
    "<p>The red privacy LED on the parrot is on while the live view is running.</p>"
    "<script>"
    "var live=document.getElementById('live'),on=true;"
    "var url=location.protocol+'//'+location.hostname+':81/stream';"
    "function start(){live.src=url+'?t='+Date.now();}"
    "document.getElementById('toggle').onclick=function(){on=!on;"
    "if(on){start();this.textContent='Stop';}else{live.removeAttribute('src');this.textContent='Start';}};"
    "function last(){document.getElementById('last').src='/last?t='+Date.now();}"
    "document.getElementById('refresh').onclick=last;start();last();"
    "</script></body></html>";

static esp_err_t send_jpeg(httpd_req_t *req, uint8_t *jpg, size_t len)
{
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_send(req, (const char *)jpg, len);
    free(jpg);
    return r;
}

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t photo_get(httpd_req_t *req)
{
    uint8_t *jpg = NULL;
    size_t len = 0;
    leds_show_privacy(1500);
    if (camera_grab_frame(&jpg, &len) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera not ready");
        return ESP_OK;
    }
    return send_jpeg(req, jpg, len);
}

static esp_err_t last_get(httpd_req_t *req)
{
    uint8_t *jpg = NULL;
    size_t len = 0;
    if (camera_copy_last_photo(&jpg, &len) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "The parrot hasn't taken a photo yet");
        return ESP_OK;
    }
    return send_jpeg(req, jpg, len);
}

static esp_err_t stream_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" BOUNDARY);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    ESP_LOGI(TAG, "live view started");

    char part[128];
    int failures = 0;
    for (;;) {
        leds_show_privacy(600);
        uint8_t *jpg = NULL;
        size_t len = 0;
        if (camera_grab_frame(&jpg, &len) != ESP_OK) {
            if (++failures > 20) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        failures = 0;
        int hl = snprintf(part, sizeof(part),
                          "\r\n--" BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                          (unsigned)len);
        esp_err_t r = httpd_resp_send_chunk(req, part, hl);
        if (r == ESP_OK) {
            r = httpd_resp_send_chunk(req, (const char *)jpg, len);
        }
        free(jpg);
        if (r != ESP_OK) {
            break;  // the browser closed the page or pressed Stop
        }
        vTaskDelay(pdMS_TO_TICKS(STREAM_FRAME_MS));
    }
    ESP_LOGI(TAG, "live view stopped");
    return ESP_OK;
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *e = data;
    ESP_LOGI(TAG, "Camera preview: http://" IPSTR "/", IP2STR(&e->ip_info.ip));
}

void preview_start(void)
{
    httpd_handle_t page = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.ctrl_port = 32768;
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;
    if (httpd_start(&page, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "couldn't start the preview page");
        return;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/photo", .method = HTTP_GET, .handler = photo_get},
        {.uri = "/last", .method = HTTP_GET, .handler = last_get},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(page, &uris[i]);
    }

    httpd_handle_t stream = NULL;
    httpd_config_t scfg = HTTPD_DEFAULT_CONFIG();
    scfg.server_port = 81;
    scfg.ctrl_port = 32769;
    scfg.stack_size = 6144;
    scfg.max_open_sockets = 2;
    scfg.lru_purge_enable = true;
    if (httpd_start(&stream, &scfg) == ESP_OK) {
        const httpd_uri_t s = {.uri = "/stream", .method = HTTP_GET, .handler = stream_get};
        httpd_register_uri_handler(stream, &s);
    } else {
        ESP_LOGE(TAG, "couldn't start the live view");
    }

    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL);
}
