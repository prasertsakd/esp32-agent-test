#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"

#include "config.h"
#include "gpio_ctrl.h"
#include "neopixel.h"
#include "openai_api.h"
#include "web_server.h"

static const char *TAG = "main";

// ─── WiFi ─────────────────────────────────────────────────────────────────────

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_count = 0;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < WIFI_MAX_RETRIES) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGI(TAG, "Retrying WiFi connection (%d/%d)...", s_retry_count, WIFI_MAX_RETRIES);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "WiFi connection failed after %d retries", WIFI_MAX_RETRIES);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", WIFI_SSID);

    // Wait for connection or failure
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY
    );

    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip);
    vEventGroupDelete(s_wifi_event_group);

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

// ─── App entry ────────────────────────────────────────────────────────────────

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 ChatBot starting...");

    // Initialize NVS (required for WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize GPIO
    ESP_ERROR_CHECK(gpio_ctrl_init());

    // Initialize NeoPixel
    ESP_ERROR_CHECK(neopixel_init());

    // Connect to WiFi
    ret = wifi_init_sta();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi failed — restarting in 5s");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }

    // Create OpenAI conversation context (shared across all WebSocket clients)
    openai_ctx_t *ctx = openai_ctx_create();
    if (!ctx) {
        ESP_LOGE(TAG, "Failed to create OpenAI context");
        esp_restart();
    }

    // Start web server
    ret = web_server_start(ctx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        openai_ctx_destroy(ctx);
        esp_restart();
    }

    ESP_LOGI(TAG, "==============================================");
    ESP_LOGI(TAG, "  ChatBot ready! Open http://<device-ip>/");
    ESP_LOGI(TAG, "==============================================");

    // Main loop — nothing to do, everything handled by httpd + FreeRTOS tasks
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGD(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());
    }
}
