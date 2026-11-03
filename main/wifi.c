#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#include "app.h"
#include "wifi.h"

static const char *TAG = "wifi";
#define WIFI_UP_BIT BIT0

static EventGroupHandle_t s_events;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = data;
        bool was_up = xEventGroupGetBits(s_events) & WIFI_UP_BIT;
        xEventGroupClearBits(s_events, WIFI_UP_BIT);
        if (was_up) {
            ESP_LOGW(TAG, "lost Wi-Fi (reason %d)", e->reason);
            app_post(EV_WIFI_DOWN, 0, NULL);
        }
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "connected, IP " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_events, WIFI_UP_BIT);
        app_post(EV_WIFI_UP, 0, NULL);
    }
}

void wifi_start(void)
{
    s_events = xEventGroupCreate();
    if (strlen(CONFIG_CALIOS_WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "No Wi-Fi network set. Run `idf.py menuconfig` -> Calios -> Wi-Fi network name.");
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, CONFIG_CALIOS_WIFI_SSID, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, CONFIG_CALIOS_WIFI_PASSWORD, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = strlen(CONFIG_CALIOS_WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;  // phone hotspots often use WPA3

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);  // lower latency for live audio
    ESP_LOGI(TAG, "connecting to \"%s\"", CONFIG_CALIOS_WIFI_SSID);
}

bool wifi_wait_connected(TickType_t timeout)
{
    return xEventGroupWaitBits(s_events, WIFI_UP_BIT, pdFALSE, pdTRUE, timeout) & WIFI_UP_BIT;
}
