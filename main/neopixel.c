#include "neopixel.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "neopixel";

static led_strip_handle_t s_strip = NULL;
static uint8_t s_r = 0, s_g = 0, s_b = 0;

esp_err_t neopixel_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = NEOPIXEL_GPIO,
        .max_leds       = NEOPIXEL_COUNT,
        .led_model      = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_cfg = {
        .clk_src       = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10 MHz
        .flags.with_dma = false,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(err));
        return err;
    }

    // Start with LED off
    led_strip_clear(s_strip);
    ESP_LOGI(TAG, "NeoPixel ready on GPIO%d (%d LED)", NEOPIXEL_GPIO, NEOPIXEL_COUNT);
    return ESP_OK;
}

esp_err_t neopixel_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) return ESP_ERR_INVALID_STATE;

    for (int i = 0; i < NEOPIXEL_COUNT; i++) {
        led_strip_set_pixel(s_strip, i, r, g, b);
    }
    esp_err_t err = led_strip_refresh(s_strip);
    if (err == ESP_OK) {
        s_r = r; s_g = g; s_b = b;
        ESP_LOGI(TAG, "Color set to #%02x%02x%02x", r, g, b);
    }
    return err;
}

esp_err_t neopixel_off(void)
{
    if (!s_strip) return ESP_ERR_INVALID_STATE;
    esp_err_t err = led_strip_clear(s_strip);
    if (err == ESP_OK) {
        s_r = 0; s_g = 0; s_b = 0;
        ESP_LOGI(TAG, "NeoPixel off");
    }
    return err;
}

void neopixel_get_color(uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (r) *r = s_r;
    if (g) *g = s_g;
    if (b) *b = s_b;
}

char *neopixel_status_json(void)
{
    char *buf = malloc(64);
    if (!buf) return NULL;
    snprintf(buf, 64,
             "{\"r\":%d,\"g\":%d,\"b\":%d,\"hex\":\"#%02x%02x%02x\"}",
             s_r, s_g, s_b, s_r, s_g, s_b);
    return buf;
}
