#pragma once

#include "esp_err.h"
#include "openai_api.h"

// Initialize and start the HTTP server + WebSocket
// ctx: shared OpenAI conversation context
esp_err_t web_server_start(openai_ctx_t *ctx);

// Stop the HTTP server
void web_server_stop(void);
