#include "gpio_ctrl.h"
#include "config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "gpio_ctrl";

static const int allowed_pins[GPIO_PIN_COUNT] = GPIO_CONTROLLABLE_PINS;
static int pin_levels[GPIO_PIN_COUNT] = {0}; // cached state: 0=LOW, 1=HIGH

// Returns index of pin in allowed_pins[], or -1 if not found
static int pin_index(int pin)
{
    for (int i = 0; i < GPIO_PIN_COUNT; i++) {
        if (allowed_pins[i] == pin) return i;
    }
    return -1;
}

esp_err_t gpio_ctrl_init(void)
{
    ESP_LOGI(TAG, "Initializing %d GPIO pins", GPIO_PIN_COUNT);
    for (int i = 0; i < GPIO_PIN_COUNT; i++) {
        int pin = allowed_pins[i];
        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << pin),
            .mode         = GPIO_MODE_INPUT_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure GPIO %d: %s", pin, esp_err_to_name(err));
            return err;
        }
        gpio_set_level(pin, 0);
        pin_levels[i] = 0;
        ESP_LOGI(TAG, "GPIO %d configured (LOW)", pin);
    }
    return ESP_OK;
}

bool gpio_ctrl_is_valid_pin(int pin)
{
    return pin_index(pin) >= 0;
}

int gpio_ctrl_get_level(int pin)
{
    int idx = pin_index(pin);
    if (idx < 0) return -1;
    return pin_levels[idx];
}

gpio_action_t gpio_ctrl_parse_action(const char *action_str)
{
    if (!action_str) return GPIO_ACTION_UNKNOWN;
    if (strcmp(action_str, "high")   == 0) return GPIO_ACTION_HIGH;
    if (strcmp(action_str, "low")    == 0) return GPIO_ACTION_LOW;
    if (strcmp(action_str, "toggle") == 0) return GPIO_ACTION_TOGGLE;
    if (strcmp(action_str, "read")   == 0) return GPIO_ACTION_READ;
    return GPIO_ACTION_UNKNOWN;
}

void gpio_ctrl_execute(int pin, gpio_action_t action, gpio_cmd_result_t *result)
{
    memset(result, 0, sizeof(*result));
    result->pin    = pin;
    result->action = action;

    int idx = pin_index(pin);
    if (idx < 0) {
        result->success = false;
        snprintf(result->error_msg, sizeof(result->error_msg),
                 "Pin %d is not in the allowed list", pin);
        snprintf(result->result_json, sizeof(result->result_json),
                 "{\"error\":\"Pin %d not allowed\"}", pin);
        ESP_LOGW(TAG, "Rejected: %s", result->error_msg);
        return;
    }

    if (action == GPIO_ACTION_UNKNOWN) {
        result->success = false;
        snprintf(result->error_msg, sizeof(result->error_msg), "Unknown action");
        snprintf(result->result_json, sizeof(result->result_json),
                 "{\"error\":\"Unknown action\"}");
        return;
    }

    int new_level = pin_levels[idx];

    switch (action) {
        case GPIO_ACTION_HIGH:
            new_level = 1;
            break;
        case GPIO_ACTION_LOW:
            new_level = 0;
            break;
        case GPIO_ACTION_TOGGLE:
            new_level = pin_levels[idx] ? 0 : 1;
            break;
        case GPIO_ACTION_READ:
            new_level = gpio_get_level(pin);
            pin_levels[idx] = new_level; // sync cache
            result->level   = new_level;
            result->success = true;
            snprintf(result->result_json, sizeof(result->result_json),
                     "{\"pin\":%d,\"state\":\"%s\",\"level\":%d}",
                     pin, new_level ? "HIGH" : "LOW", new_level);
            ESP_LOGI(TAG, "Read GPIO %d: %s", pin, new_level ? "HIGH" : "LOW");
            return;
        default:
            break;
    }

    gpio_set_level(pin, new_level);
    pin_levels[idx] = new_level;
    result->level   = new_level;
    result->success = true;
    snprintf(result->result_json, sizeof(result->result_json),
             "{\"pin\":%d,\"action\":\"%s\",\"state\":\"%s\"}",
             pin,
             action == GPIO_ACTION_HIGH   ? "high" :
             action == GPIO_ACTION_LOW    ? "low"  : "toggle",
             new_level ? "HIGH" : "LOW");

    ESP_LOGI(TAG, "GPIO %d -> %s", pin, new_level ? "HIGH" : "LOW");
}

char *gpio_ctrl_status_json(void)
{
    // Build: [{"pin":4,"state":"LOW"},{"pin":5,"state":"LOW"},...]
    // Each entry is ~25 chars. Total ~25*GPIO_PIN_COUNT + brackets + commas
    int buf_size = GPIO_PIN_COUNT * 30 + 8;
    char *buf = malloc(buf_size);
    if (!buf) return NULL;

    int offset = 0;
    offset += snprintf(buf + offset, buf_size - offset, "[");
    for (int i = 0; i < GPIO_PIN_COUNT; i++) {
        offset += snprintf(buf + offset, buf_size - offset,
                           "%s{\"pin\":%d,\"state\":\"%s\"}",
                           i > 0 ? "," : "",
                           allowed_pins[i],
                           pin_levels[i] ? "HIGH" : "LOW");
    }
    snprintf(buf + offset, buf_size - offset, "]");
    return buf;
}
