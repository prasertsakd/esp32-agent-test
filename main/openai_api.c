#include "openai_api.h"
#include "config.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "openai_api";

// Internal conversation context
struct openai_ctx_t {
    conv_message_t     messages[MAX_CONVERSATION_TURNS + 1]; // +1 for system prompt
    int                count;
    SemaphoreHandle_t  lock;
};

// ─── HTTP response accumulation buffer ───────────────────────────────────────

typedef struct {
    char  *buf;
    int    len;
    int    cap;
} response_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    response_buf_t *rb = (response_buf_t *)evt->user_data;
    if (!rb) return ESP_OK;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (rb->len + evt->data_len + 1 > rb->cap) {
                int new_cap = rb->cap + evt->data_len + 2048;
                char *new_buf = realloc(rb->buf, new_cap);
                if (!new_buf) {
                    ESP_LOGE(TAG, "OOM growing response buffer");
                    return ESP_FAIL;
                }
                rb->buf = new_buf;
                rb->cap = new_cap;
            }
            memcpy(rb->buf + rb->len, evt->data, evt->data_len);
            rb->len += evt->data_len;
            rb->buf[rb->len] = '\0';
            break;
        default:
            break;
    }
    return ESP_OK;
}

// ─── Context management ───────────────────────────────────────────────────────

openai_ctx_t *openai_ctx_create(void)
{
    openai_ctx_t *ctx = calloc(1, sizeof(openai_ctx_t));
    if (!ctx) return NULL;

    ctx->lock = xSemaphoreCreateMutex();
    if (!ctx->lock) {
        free(ctx);
        return NULL;
    }

    // Insert system prompt at index 0
    ctx->messages[0].role    = ROLE_SYSTEM;
    ctx->messages[0].content = strdup(SYSTEM_PROMPT);
    ctx->count = 1;

    return ctx;
}

void openai_ctx_destroy(openai_ctx_t *ctx)
{
    if (!ctx) return;
    for (int i = 0; i < ctx->count; i++) {
        free(ctx->messages[i].content);
        free(ctx->messages[i].tool_call_id);
        free(ctx->messages[i].tool_call_json);
    }
    vSemaphoreDelete(ctx->lock);
    free(ctx);
}

static void ctx_add_message(openai_ctx_t *ctx, message_role_t role,
                             const char *content, const char *tool_call_id,
                             const char *tool_call_json)
{
    xSemaphoreTake(ctx->lock, portMAX_DELAY);

    // Rolling window: if full (beyond system prompt), remove oldest turn
    // A turn may be: user+assistant, or user+assistant(tool_calls)+tool+assistant
    // Always remove starting from index 1 until we hit the next user message or run out
    int max = MAX_CONVERSATION_TURNS + 1; // +1 for system prompt slot
    if (ctx->count >= max) {
        // Count how many to remove: at least 1, stop before next ROLE_USER or end
        int remove = 1;
        while (remove < ctx->count - 1 &&
               ctx->messages[1 + remove].role != ROLE_USER) {
            remove++;
        }
        for (int r = 0; r < remove; r++) {
            free(ctx->messages[1 + r].content);
            free(ctx->messages[1 + r].tool_call_id);
            free(ctx->messages[1 + r].tool_call_json);
        }
        int shift_count = ctx->count - 1 - remove;
        if (shift_count > 0) {
            memmove(&ctx->messages[1], &ctx->messages[1 + remove],
                    shift_count * sizeof(conv_message_t));
        }
        ctx->count -= remove;
    }

    conv_message_t *msg = &ctx->messages[ctx->count];
    memset(msg, 0, sizeof(*msg));
    msg->role           = role;
    msg->content        = content ? strdup(content) : NULL;
    msg->tool_call_id   = tool_call_id ? strdup(tool_call_id) : NULL;
    msg->tool_call_json = tool_call_json ? strdup(tool_call_json) : NULL;
    ctx->count++;

    xSemaphoreGive(ctx->lock);
}

void openai_ctx_add_user(openai_ctx_t *ctx, const char *content)
{
    ctx_add_message(ctx, ROLE_USER, content, NULL, NULL);
}

void openai_ctx_add_assistant(openai_ctx_t *ctx, const char *content,
                               const char *tool_call_json)
{
    ctx_add_message(ctx, ROLE_ASSISTANT, content, NULL, tool_call_json);
}

void openai_ctx_add_tool_result(openai_ctx_t *ctx, const char *tool_call_id,
                                 const char *content)
{
    ctx_add_message(ctx, ROLE_TOOL, content, tool_call_id, NULL);
}

void openai_ctx_reset(openai_ctx_t *ctx)
{
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    // Free all except system prompt (index 0)
    for (int i = 1; i < ctx->count; i++) {
        free(ctx->messages[i].content);
        free(ctx->messages[i].tool_call_id);
        free(ctx->messages[i].tool_call_json);
        memset(&ctx->messages[i], 0, sizeof(conv_message_t));
    }
    ctx->count = 1; // keep system prompt
    xSemaphoreGive(ctx->lock);
}

void openai_ctx_pop(openai_ctx_t *ctx, int n)
{
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    // Never pop below the system prompt (index 0)
    int to_remove = n < (ctx->count - 1) ? n : (ctx->count - 1);
    for (int i = 0; i < to_remove; i++) {
        int idx = ctx->count - 1 - i;
        free(ctx->messages[idx].content);
        free(ctx->messages[idx].tool_call_id);
        free(ctx->messages[idx].tool_call_json);
        memset(&ctx->messages[idx], 0, sizeof(conv_message_t));
    }
    ctx->count -= to_remove;
    xSemaphoreGive(ctx->lock);
}

// ─── Build OpenAI request JSON ────────────────────────────────────────────────

static char *build_request_json(openai_ctx_t *ctx)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddStringToObject(root, "model", OPENAI_MODEL);
    cJSON_AddNumberToObject(root, "max_tokens", OPENAI_MAX_TOKENS);
    cJSON_AddNumberToObject(root, "temperature", OPENAI_TEMPERATURE);

    // Messages array
    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    for (int i = 0; i < ctx->count; i++) {
        conv_message_t *m = &ctx->messages[i];
        cJSON *msg = cJSON_CreateObject();

        const char *role_str = "user";
        if (m->role == ROLE_SYSTEM)    role_str = "system";
        if (m->role == ROLE_ASSISTANT) role_str = "assistant";
        if (m->role == ROLE_TOOL)      role_str = "tool";

        cJSON_AddStringToObject(msg, "role", role_str);

        if (m->role == ROLE_TOOL) {
            // Tool result message requires tool_call_id and content
            cJSON_AddStringToObject(msg, "content", m->content ? m->content : "");
            cJSON_AddStringToObject(msg, "tool_call_id", m->tool_call_id ? m->tool_call_id : "");
        } else if (m->role == ROLE_ASSISTANT && m->tool_call_json) {
            // Assistant message with tool calls — content may be null
            if (m->content) {
                cJSON_AddStringToObject(msg, "content", m->content);
            } else {
                cJSON_AddNullToObject(msg, "content");
            }
            // Parse and attach tool_calls array
            cJSON *tc = cJSON_Parse(m->tool_call_json);
            if (tc) {
                cJSON_AddItemToObject(msg, "tool_calls", tc);
            }
        } else {
            cJSON_AddStringToObject(msg, "content", m->content ? m->content : "");
        }

        cJSON_AddItemToArray(messages, msg);
    }
    xSemaphoreGive(ctx->lock);

    // Tools array — define control_gpio function
    cJSON *tools = cJSON_AddArrayToObject(root, "tools");
    cJSON *tool  = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "type", "function");

    cJSON *func = cJSON_CreateObject();
    cJSON_AddStringToObject(func, "name", "control_gpio");
    cJSON_AddStringToObject(func, "description",
        "Control or read a GPIO pin on the ESP32-S3. "
        "Use this when the user wants to turn on/off something, toggle a pin, or read its state.");

    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "type", "object");

    cJSON *props = cJSON_CreateObject();

    // pin property
    cJSON *pin_prop = cJSON_CreateObject();
    cJSON_AddStringToObject(pin_prop, "type", "integer");
    cJSON_AddStringToObject(pin_prop, "description",
        "GPIO pin number. Valid values: 4, 5, 6, 7, 15, 16, 17, 18");
    cJSON *pin_enum = cJSON_CreateArray();
    int valid_pins[] = GPIO_CONTROLLABLE_PINS;
    for (int i = 0; i < GPIO_CONTROLLABLE_COUNT; i++) {
        cJSON_AddItemToArray(pin_enum, cJSON_CreateNumber(valid_pins[i]));
    }
    cJSON_AddItemToObject(pin_prop, "enum", pin_enum);
    cJSON_AddItemToObject(props, "pin", pin_prop);

    // action property
    cJSON *act_prop = cJSON_CreateObject();
    cJSON_AddStringToObject(act_prop, "type", "string");
    cJSON_AddStringToObject(act_prop, "description",
        "high=set 3.3V/ON, low=set 0V/OFF, toggle=flip current state, read=get current level");
    cJSON *act_enum = cJSON_CreateArray();
    cJSON_AddItemToArray(act_enum, cJSON_CreateString("high"));
    cJSON_AddItemToArray(act_enum, cJSON_CreateString("low"));
    cJSON_AddItemToArray(act_enum, cJSON_CreateString("toggle"));
    cJSON_AddItemToArray(act_enum, cJSON_CreateString("read"));
    cJSON_AddItemToObject(act_prop, "enum", act_enum);
    cJSON_AddItemToObject(props, "action", act_prop);

    cJSON_AddItemToObject(params, "properties", props);

    cJSON *required = cJSON_CreateArray();
    cJSON_AddItemToArray(required, cJSON_CreateString("pin"));
    cJSON_AddItemToArray(required, cJSON_CreateString("action"));
    cJSON_AddItemToObject(params, "required", required);

    cJSON_AddItemToObject(func, "parameters", params);
    cJSON_AddItemToObject(tool, "function", func);
    cJSON_AddItemToArray(tools, tool);

    // ── Tool 2: control_neopixel ──────────────────────────────────────────────
    cJSON *tool2 = cJSON_CreateObject();
    cJSON_AddStringToObject(tool2, "type", "function");

    cJSON *func2 = cJSON_CreateObject();
    cJSON_AddStringToObject(func2, "name", "control_neopixel");
    cJSON_AddStringToObject(func2, "description",
        "Control the NeoPixel (WS2812B) RGB LED on the ESP32-S3. "
        "Use this when the user asks to set a color, change the LED color, or turn it off.");

    cJSON *params2 = cJSON_CreateObject();
    cJSON_AddStringToObject(params2, "type", "object");

    cJSON *props2 = cJSON_CreateObject();

    // action
    cJSON *np_act = cJSON_CreateObject();
    cJSON_AddStringToObject(np_act, "type", "string");
    cJSON_AddStringToObject(np_act, "description",
        "set_color=set RGB color (requires r,g,b), off=turn LED off");
    cJSON *np_act_enum = cJSON_CreateArray();
    cJSON_AddItemToArray(np_act_enum, cJSON_CreateString("set_color"));
    cJSON_AddItemToArray(np_act_enum, cJSON_CreateString("off"));
    cJSON_AddItemToObject(np_act, "enum", np_act_enum);
    cJSON_AddItemToObject(props2, "action", np_act);

    // r, g, b channels
    const char *channels[] = {"r", "g", "b"};
    const char *ch_desc[]  = {"Red (0-255)", "Green (0-255)", "Blue (0-255)"};
    for (int i = 0; i < 3; i++) {
        cJSON *ch = cJSON_CreateObject();
        cJSON_AddStringToObject(ch, "type", "integer");
        cJSON_AddStringToObject(ch, "description", ch_desc[i]);
        cJSON_AddNumberToObject(ch, "minimum", 0);
        cJSON_AddNumberToObject(ch, "maximum", 255);
        cJSON_AddItemToObject(props2, channels[i], ch);
    }

    cJSON_AddItemToObject(params2, "properties", props2);

    cJSON *required2 = cJSON_CreateArray();
    cJSON_AddItemToArray(required2, cJSON_CreateString("action"));
    cJSON_AddItemToObject(params2, "required", required2);

    cJSON_AddItemToObject(func2, "parameters", params2);
    cJSON_AddItemToObject(tool2, "function", func2);
    cJSON_AddItemToArray(tools, tool2);

    cJSON_AddStringToObject(root, "tool_choice", "auto");

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str; // caller must free()
}

// ─── Parse OpenAI response ────────────────────────────────────────────────────

static void parse_response(const char *body, openai_response_t *response)
{
    memset(response, 0, sizeof(*response));

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        response->success = false;
        snprintf(response->error_msg, sizeof(response->error_msg),
                 "Failed to parse JSON response");
        return;
    }

    // Check for API error
    cJSON *error = cJSON_GetObjectItem(root, "error");
    if (error) {
        cJSON *msg = cJSON_GetObjectItem(error, "message");
        response->success = false;
        snprintf(response->error_msg, sizeof(response->error_msg),
                 "API error: %s", msg ? msg->valuestring : "unknown");
        cJSON_Delete(root);
        return;
    }

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (!choices || cJSON_GetArraySize(choices) == 0) {
        response->success = false;
        snprintf(response->error_msg, sizeof(response->error_msg),
                 "No choices in response");
        cJSON_Delete(root);
        return;
    }

    cJSON *choice      = cJSON_GetArrayItem(choices, 0);
    cJSON *finish_reason = cJSON_GetObjectItem(choice, "finish_reason");
    cJSON *message     = cJSON_GetObjectItem(choice, "message");

    if (!message) {
        response->success = false;
        snprintf(response->error_msg, sizeof(response->error_msg), "No message in choice");
        cJSON_Delete(root);
        return;
    }

    const char *reason = finish_reason ? finish_reason->valuestring : "";

    if (strcmp(reason, "tool_calls") == 0) {
        // Extract first tool call
        cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
        if (tool_calls && cJSON_GetArraySize(tool_calls) > 0) {
            cJSON *tc      = cJSON_GetArrayItem(tool_calls, 0);
            cJSON *tc_id   = cJSON_GetObjectItem(tc, "id");
            cJSON *tc_func = cJSON_GetObjectItem(tc, "function");

            if (tc_id)
                snprintf(response->tool_call_id, sizeof(response->tool_call_id),
                         "%s", tc_id->valuestring);

            if (tc_func) {
                cJSON *fname = cJSON_GetObjectItem(tc_func, "name");
                cJSON *fargs = cJSON_GetObjectItem(tc_func, "arguments");
                if (fname)
                    snprintf(response->function_name, sizeof(response->function_name),
                             "%s", fname->valuestring);
                if (fargs)
                    snprintf(response->function_args_json, sizeof(response->function_args_json),
                             "%s", fargs->valuestring);
            }

            // Save the full tool_calls array as JSON for the assistant message
            response->has_tool_call = true;
            // Store stringified tool_calls for conversation history
            // (attached to assistant message in the caller)
        }
        response->success = true;
    } else {
        // Normal text response
        cJSON *content = cJSON_GetObjectItem(message, "content");
        if (content && content->valuestring) {
            response->text_response = strdup(content->valuestring);
        } else {
            response->text_response = strdup("");
        }
        response->success = true;
    }

    cJSON_Delete(root);
}

// ─── Main API call ────────────────────────────────────────────────────────────

esp_err_t openai_chat_completion(openai_ctx_t *ctx, openai_response_t *response)
{
    memset(response, 0, sizeof(*response));

    char *request_body = build_request_json(ctx);
    if (!request_body) {
        response->success = false;
        snprintf(response->error_msg, sizeof(response->error_msg), "Failed to build request JSON");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGD(TAG, "Request: %.200s...", request_body);

    // Set up response buffer
    response_buf_t rb = {
        .buf = malloc(MAX_RESPONSE_LEN),
        .len = 0,
        .cap = MAX_RESPONSE_LEN,
    };
    if (!rb.buf) {
        free(request_body);
        response->success = false;
        snprintf(response->error_msg, sizeof(response->error_msg), "OOM for response buffer");
        return ESP_ERR_NO_MEM;
    }
    rb.buf[0] = '\0';

    // Configure HTTP client
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", OPENAI_API_KEY);

    esp_http_client_config_t config = {
        .url                = OPENAI_API_URL,
        .event_handler      = http_event_handler,
        .user_data          = &rb,
        .method             = HTTP_METHOD_POST,
        .timeout_ms         = 30000,
        .buffer_size        = 2048,
        .buffer_size_tx     = 4096,
        .crt_bundle_attach  = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(request_body);
        free(rb.buf);
        response->success = false;
        snprintf(response->error_msg, sizeof(response->error_msg), "Failed to init HTTP client");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_post_field(client, request_body, strlen(request_body));

    esp_err_t err = esp_http_client_perform(client);
    int http_status = esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);
    free(request_body);

    if (err != ESP_OK) {
        free(rb.buf);
        response->success = false;
        snprintf(response->error_msg, sizeof(response->error_msg),
                 "HTTP request failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "HTTP error: %s", esp_err_to_name(err));
        return err;
    }

    if (http_status != 200) {
        ESP_LOGE(TAG, "HTTP status %d: %s", http_status, rb.buf);
        free(rb.buf);
        response->success = false;
        snprintf(response->error_msg, sizeof(response->error_msg),
                 "HTTP status %d", http_status);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Response: %.300s", rb.buf);
    parse_response(rb.buf, response);
    free(rb.buf);

    return response->success ? ESP_OK : ESP_FAIL;
}

void openai_response_free(openai_response_t *response)
{
    if (!response) return;
    free(response->text_response);
    response->text_response = NULL;
}
