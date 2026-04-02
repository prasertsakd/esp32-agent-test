#pragma once

// =============================================================================
// WiFi Configuration
// =============================================================================
#define WIFI_SSID           "YOUR_WIFI_SSID"
#define WIFI_PASSWORD       "YOUR_WIFI_PASSWORD"
#define WIFI_MAX_RETRIES    10

// =============================================================================
// OpenAI API Configuration
// =============================================================================
// SECURITY: Never commit real API keys. Replace before building.
#define OPENAI_API_KEY      "sk-..."
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
#define GPIO_PIN_COUNT          8

// =============================================================================
// Web Server
// =============================================================================
#define WEB_SERVER_PORT     80
#define WS_URI              "/ws"
#define SPIFFS_BASE_PATH    "/spiffs"

// =============================================================================
// Conversation History
// =============================================================================
// Rolling window of N user+assistant turns (system prompt always kept)
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
    "Always confirm what you did after executing a command. " \
    "Respond concisely. Support both English and Thai language."
