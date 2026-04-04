#pragma once

#include "secrets.h"   // WIFI_SSID, WIFI_PASSWORD, OPENAI_API_KEY

// =============================================================================
// WiFi
// =============================================================================
#define WIFI_MAX_RETRIES    10

// =============================================================================
// OpenAI API
// =============================================================================
#define OPENAI_API_URL      "https://api.openai.com/v1/chat/completions"
#define OPENAI_MODEL        "gpt-4o-mini"
#define OPENAI_MAX_TOKENS   512
#define OPENAI_TEMPERATURE  0.7f

// =============================================================================
// GPIO Configuration
// =============================================================================
// Pins available for AI control.
// Avoids ESP32-S3 strapping pins: GPIO0, GPIO3, GPIO45, GPIO46
#define GPIO_CONTROLLABLE_PINS  {4, 5, 6, 7, 15, 16, 17, 18}
#define GPIO_CONTROLLABLE_COUNT 8

// =============================================================================
// NeoPixel (WS2812B) Configuration
// =============================================================================
// GPIO48 = onboard RGB LED on ESP32-S3-DevKitC-1
// Change to match your board / external strip wiring
#define NEOPIXEL_GPIO       48
#define NEOPIXEL_COUNT      1     // number of LEDs in the strip

// =============================================================================
// Web Server
// =============================================================================
#define WEB_SERVER_PORT     80
#define WS_URI              "/ws"
#define SPIFFS_BASE_PATH    "/spiffs"

// =============================================================================
// Conversation History
// =============================================================================
#define MAX_CONVERSATION_TURNS  20
#define MAX_MESSAGE_LEN         1024
#define MAX_RESPONSE_LEN        4096

// =============================================================================
// Chat Worker Task
// =============================================================================
#define CHAT_TASK_STACK_SIZE    (12 * 1024)
#define CHAT_TASK_PRIORITY      5

// =============================================================================
// System Prompt
// =============================================================================
#define SYSTEM_PROMPT \
    "You are a helpful assistant embedded in an ESP32-S3 microcontroller. " \
    "You can control GPIO pins using the control_gpio function. " \
    "Available GPIO pins: 4, 5, 6, 7, 15, 16, 17, 18. " \
    "Actions: 'high' sets pin to 3.3V (ON), 'low' sets pin to 0V (OFF), " \
    "'toggle' flips the current state, 'read' returns the current level. " \
    "You can also control the NeoPixel RGB LED using the control_neopixel function. " \
    "Actions: 'set_color' requires r, g, b values (0-255); 'off' turns the LED off. " \
    "Always confirm what you did after executing a command. " \
    "You are also a general-purpose assistant. If the user asks questions unrelated " \
    "to hardware control (such as facts, explanations, calculations, or general conversation), " \
    "answer them helpfully and directly without invoking any tools. " \
    "Respond concisely. Support both English and Thai language."
