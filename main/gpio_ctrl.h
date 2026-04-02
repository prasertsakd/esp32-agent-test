#pragma once

#include <stdbool.h>
#include "esp_err.h"

// GPIO actions supported by the AI agent
typedef enum {
    GPIO_ACTION_HIGH   = 0,
    GPIO_ACTION_LOW    = 1,
    GPIO_ACTION_TOGGLE = 2,
    GPIO_ACTION_READ   = 3,
    GPIO_ACTION_UNKNOWN = -1,
} gpio_action_t;

// Result of a GPIO command execution
typedef struct {
    int   pin;
    gpio_action_t action;
    int   level;        // current level after operation (0 or 1)
    bool  success;
    char  error_msg[64];
    char  result_json[128]; // JSON string to return to OpenAI as tool result
} gpio_cmd_result_t;

// Initialize all controllable GPIO pins (output mode, initially LOW)
esp_err_t gpio_ctrl_init(void);

// Execute a GPIO action on a pin. Fills result struct.
void gpio_ctrl_execute(int pin, gpio_action_t action, gpio_cmd_result_t *result);

// Parse action string ("high", "low", "toggle", "read") to enum
gpio_action_t gpio_ctrl_parse_action(const char *action_str);

// Check if pin is in the allowed list
bool gpio_ctrl_is_valid_pin(int pin);

// Get current level of a pin (from internal state cache)
int gpio_ctrl_get_level(int pin);

// Returns JSON array of all pin states: [{"pin":4,"state":"LOW"}, ...]
// Caller must free() the returned string.
char *gpio_ctrl_status_json(void);
