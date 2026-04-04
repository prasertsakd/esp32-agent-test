#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "config.h"

// Message roles
typedef enum {
    ROLE_SYSTEM    = 0,
    ROLE_USER      = 1,
    ROLE_ASSISTANT = 2,
    ROLE_TOOL      = 3,
} message_role_t;

// A single conversation message (heap-allocated content)
typedef struct {
    message_role_t role;
    char *content;        // heap allocated, must be freed
    char *tool_call_id;   // only for ROLE_TOOL, heap allocated
    char *tool_call_json; // only for ROLE_ASSISTANT with tool calls (serialized)
} conv_message_t;

// Conversation context (opaque)
typedef struct openai_ctx_t openai_ctx_t;

// Result of a single chat completion call
typedef struct {
    bool  has_tool_call;
    char  tool_call_id[64];
    char  function_name[64];
    char  function_args_json[256]; // raw JSON string of function arguments
    char *text_response;           // heap allocated, NULL if tool_call
    bool  success;
    char  error_msg[128];
} openai_response_t;

// Create a new conversation context (includes system prompt)
openai_ctx_t *openai_ctx_create(void);

// Destroy conversation context and free all memory
void openai_ctx_destroy(openai_ctx_t *ctx);

// Add a message to the conversation history
void openai_ctx_add_user(openai_ctx_t *ctx, const char *content);
void openai_ctx_add_assistant(openai_ctx_t *ctx, const char *content,
                               const char *tool_call_json);
void openai_ctx_add_tool_result(openai_ctx_t *ctx, const char *tool_call_id,
                                 const char *content);

// Clear all messages except the system prompt
void openai_ctx_reset(openai_ctx_t *ctx);

// Remove the last N messages from the conversation history
void openai_ctx_pop(openai_ctx_t *ctx, int n);

// Send conversation to OpenAI API and get response.
// BLOCKING — must be called from a FreeRTOS task (not httpd handler).
esp_err_t openai_chat_completion(openai_ctx_t *ctx, openai_response_t *response);

// Free heap-allocated fields inside a response struct
void openai_response_free(openai_response_t *response);
