#pragma once

#include <stdint.h>
#include "esp_err.h"

// Initialize NeoPixel strip (RMT-based, uses espressif/led_strip component)
esp_err_t neopixel_init(void);

// Set all LEDs to a single RGB color
esp_err_t neopixel_set_color(uint8_t r, uint8_t g, uint8_t b);

// Turn off all LEDs
esp_err_t neopixel_off(void);

// Get last-set color
void neopixel_get_color(uint8_t *r, uint8_t *g, uint8_t *b);

// Returns heap-allocated JSON string: {"r":255,"g":0,"b":0,"hex":"#ff0000"}
// Caller must free().
char *neopixel_status_json(void);
