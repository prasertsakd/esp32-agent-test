#include "web_server.h"
#include "config.h"
#include "gpio_ctrl.h"
#include "neopixel.h"
#include "openai_api.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "cJSON.h"

static const char *TAG = "web_server";

static httpd_handle_t s_server = NULL;
static openai_ctx_t  *s_ctx   = NULL;

// ─── WebSocket send helpers ───────────────────────────────────────────────────

// Context passed to the chat worker task
typedef struct {
    int              client_fd;
    httpd_handle_t   server;
    char            *user_message; // heap allocated
} chat_task_arg_t;

static void ws_send_text(httpd_handle_t server, int fd, const char *json_str)
{
    if (!json_str) return;
    httpd_ws_frame_t frame = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json_str,
        .len     = strlen(json_str),
    };
    esp_err_t err = httpd_ws_send_frame_async(server, fd, &frame);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ws_send_frame_async fd=%d err=%s", fd, esp_err_to_name(err));
    }
}

static void ws_send_json_type(httpd_handle_t server, int fd,
                               const char *type, const char *key, const char *value)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "type", type);
    if (key && value) {
        cJSON_AddStringToObject(obj, key, value);
    }
    char *str = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (str) {
        ws_send_text(server, fd, str);
        free(str);
    }
}

static void ws_send_gpio_status(httpd_handle_t server, int fd)
{
    char *pins_json = gpio_ctrl_status_json();
    if (!pins_json) return;

    cJSON *obj  = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "type", "gpio");
    cJSON *arr = cJSON_Parse(pins_json);
    if (arr) {
        cJSON_AddItemToObject(obj, "pins", arr);
    }
    free(pins_json);

    char *str = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (str) {
        ws_send_text(server, fd, str);
        free(str);
    }
}

static void ws_send_neopixel_status(httpd_handle_t server, int fd)
{
    uint8_t r, g, b;
    neopixel_get_color(&r, &g, &b);

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "type", "neopixel");
    cJSON_AddNumberToObject(obj, "r", r);
    cJSON_AddNumberToObject(obj, "g", g);
    cJSON_AddNumberToObject(obj, "b", b);
    char hex[8];
    snprintf(hex, sizeof(hex), "#%02x%02x%02x", r, g, b);
    cJSON_AddStringToObject(obj, "hex", hex);

    char *str = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (str) {
        ws_send_text(server, fd, str);
        free(str);
    }
}

// ─── Chat worker task ─────────────────────────────────────────────────────────
// Runs in a separate FreeRTOS task to avoid blocking httpd

static void chat_worker_task(void *arg)
{
    chat_task_arg_t *task_arg = (chat_task_arg_t *)arg;
    httpd_handle_t server    = task_arg->server;
    int fd                   = task_arg->client_fd;
    char *user_msg           = task_arg->user_message;
    free(task_arg);

    ESP_LOGI(TAG, "chat_worker: user='%s'", user_msg);

    // Add user message to conversation
    openai_ctx_add_user(s_ctx, user_msg);
    free(user_msg);

    // Send thinking indicator
    ws_send_json_type(server, fd, "thinking", NULL, NULL);

    // First OpenAI call
    openai_response_t resp;
    esp_err_t err = openai_chat_completion(s_ctx, &resp);

    if (err != ESP_OK || !resp.success) {
        ESP_LOGE(TAG, "OpenAI error: %s", resp.error_msg);
        ws_send_json_type(server, fd, "error", "text", resp.error_msg);
        openai_response_free(&resp);
        vTaskDelete(NULL);
        return;
    }

    if (resp.has_tool_call) {
        // ── Tool call handling ────────────────────────────────────────────────
        ESP_LOGI(TAG, "Tool call: %s args=%s", resp.function_name, resp.function_args_json);

        ws_send_json_type(server, fd, "thinking", NULL, NULL);

        // Execute the right tool based on function name
        char tool_result_json[256] = "{\"error\":\"unknown tool\"}";

        if (strcmp(resp.function_name, "control_gpio") == 0) {
            int pin = -1;
            gpio_action_t action = GPIO_ACTION_UNKNOWN;
            cJSON *args = cJSON_Parse(resp.function_args_json);
            if (args) {
                cJSON *j_pin    = cJSON_GetObjectItem(args, "pin");
                cJSON *j_action = cJSON_GetObjectItem(args, "action");
                if (j_pin)    pin = j_pin->valueint;
                if (j_action) action = gpio_ctrl_parse_action(j_action->valuestring);
                cJSON_Delete(args);
            }
            gpio_cmd_result_t gpio_result;
            gpio_ctrl_execute(pin, action, &gpio_result);
            snprintf(tool_result_json, sizeof(tool_result_json),
                     "%s", gpio_result.result_json);
            ESP_LOGI(TAG, "GPIO result: %s", tool_result_json);

        } else if (strcmp(resp.function_name, "control_neopixel") == 0) {
            cJSON *args = cJSON_Parse(resp.function_args_json);
            if (args) {
                cJSON *j_action = cJSON_GetObjectItem(args, "action");
                const char *action_str = j_action ? j_action->valuestring : "off";

                if (strcmp(action_str, "set_color") == 0) {
                    int r = 0, g = 0, b = 0;
                    cJSON *jr = cJSON_GetObjectItem(args, "r");
                    cJSON *jg = cJSON_GetObjectItem(args, "g");
                    cJSON *jb = cJSON_GetObjectItem(args, "b");
                    if (jr) r = jr->valueint;
                    if (jg) g = jg->valueint;
                    if (jb) b = jb->valueint;
                    // Clamp to 0-255
                    r = r < 0 ? 0 : r > 255 ? 255 : r;
                    g = g < 0 ? 0 : g > 255 ? 255 : g;
                    b = b < 0 ? 0 : b > 255 ? 255 : b;
                    neopixel_set_color((uint8_t)r, (uint8_t)g, (uint8_t)b);
                    snprintf(tool_result_json, sizeof(tool_result_json),
                             "{\"action\":\"set_color\",\"r\":%d,\"g\":%d,\"b\":%d,"
                             "\"hex\":\"#%02x%02x%02x\"}",
                             r, g, b, r, g, b);
                } else {
                    neopixel_off();
                    snprintf(tool_result_json, sizeof(tool_result_json),
                             "{\"action\":\"off\"}");
                }
                cJSON_Delete(args);
            }
            ESP_LOGI(TAG, "NeoPixel result: %s", tool_result_json);
        }

        // Build tool_calls JSON for conversation history (assistant message)
        cJSON *tc_array = cJSON_CreateArray();
        cJSON *tc_item  = cJSON_CreateObject();
        cJSON_AddStringToObject(tc_item, "id", resp.tool_call_id);
        cJSON_AddStringToObject(tc_item, "type", "function");
        cJSON *tc_func = cJSON_CreateObject();
        cJSON_AddStringToObject(tc_func, "name", resp.function_name);
        cJSON_AddStringToObject(tc_func, "arguments", resp.function_args_json);
        cJSON_AddItemToObject(tc_item, "function", tc_func);
        cJSON_AddItemToArray(tc_array, tc_item);
        char *tc_json = cJSON_PrintUnformatted(tc_array);
        cJSON_Delete(tc_array);

        // Save tool_call_id before freeing resp
        char saved_tc_id[64];
        snprintf(saved_tc_id, sizeof(saved_tc_id), "%s", resp.tool_call_id);

        openai_ctx_add_assistant(s_ctx, NULL, tc_json);
        free(tc_json);
        openai_response_free(&resp);
        openai_ctx_add_tool_result(s_ctx, saved_tc_id, tool_result_json);

        // Second OpenAI call to get final text response
        openai_response_t resp2;
        err = openai_chat_completion(s_ctx, &resp2);

        if (err != ESP_OK || !resp2.success) {
            ESP_LOGE(TAG, "OpenAI error (2nd): %s", resp2.error_msg);
            ws_send_json_type(server, fd, "error", "text", resp2.error_msg);
            openai_response_free(&resp2);
            // Roll back assistant(tool_calls) + tool_result to keep history consistent
            openai_ctx_pop(s_ctx, 2);
            vTaskDelete(NULL);
            return;
        }

        const char *final_text = resp2.text_response ? resp2.text_response : "";
        openai_ctx_add_assistant(s_ctx, final_text, NULL);

        // Send text reply to UI
        cJSON *reply = cJSON_CreateObject();
        cJSON_AddStringToObject(reply, "type", "message");
        cJSON_AddStringToObject(reply, "role", "assistant");
        cJSON_AddStringToObject(reply, "text", final_text);
        char *reply_str = cJSON_PrintUnformatted(reply);
        cJSON_Delete(reply);
        if (reply_str) {
            ws_send_text(server, fd, reply_str);
            free(reply_str);
        }

        openai_response_free(&resp2);

        // Send updated hardware status
        ws_send_gpio_status(server, fd);
        ws_send_neopixel_status(server, fd);

    } else {
        // ── Normal text response ──────────────────────────────────────────────
        const char *text = resp.text_response ? resp.text_response : "";
        openai_ctx_add_assistant(s_ctx, text, NULL);

        cJSON *reply = cJSON_CreateObject();
        cJSON_AddStringToObject(reply, "type", "message");
        cJSON_AddStringToObject(reply, "role", "assistant");
        cJSON_AddStringToObject(reply, "text", text);
        char *reply_str = cJSON_PrintUnformatted(reply);
        cJSON_Delete(reply);
        if (reply_str) {
            ws_send_text(server, fd, reply_str);
            free(reply_str);
        }

        openai_response_free(&resp);
    }

    vTaskDelete(NULL);
}

// ─── WebSocket handler ────────────────────────────────────────────────────────

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        // WebSocket handshake — just return OK
        ESP_LOGI(TAG, "WebSocket client connected, fd=%d", httpd_req_to_sockfd(req));
        ws_send_gpio_status(req->handle, httpd_req_to_sockfd(req));
        ws_send_neopixel_status(req->handle, httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    // Receive WebSocket frame
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(ws_pkt));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    // Get frame length first
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame (len probe) failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (ws_pkt.len == 0 || ws_pkt.type != HTTPD_WS_TYPE_TEXT) {
        return ESP_OK;
    }

    uint8_t *buf = calloc(ws_pkt.len + 1, 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    ws_pkt.payload = buf;

    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed: %s", esp_err_to_name(ret));
        free(buf);
        return ret;
    }
    buf[ws_pkt.len] = '\0';

    // Parse incoming JSON
    cJSON *msg = cJSON_Parse((char *)buf);
    free(buf);

    if (!msg) {
        ESP_LOGW(TAG, "Invalid JSON from client");
        return ESP_OK;
    }

    cJSON *type_item = cJSON_GetObjectItem(msg, "type");
    if (!type_item || !type_item->valuestring) {
        cJSON_Delete(msg);
        return ESP_OK;
    }

    if (strcmp(type_item->valuestring, "message") == 0) {
        cJSON *text_item = cJSON_GetObjectItem(msg, "text");
        if (!text_item || !text_item->valuestring) {
            cJSON_Delete(msg);
            return ESP_OK;
        }

        // Spawn chat worker task (non-blocking)
        chat_task_arg_t *task_arg = malloc(sizeof(chat_task_arg_t));
        if (!task_arg) {
            cJSON_Delete(msg);
            return ESP_ERR_NO_MEM;
        }
        task_arg->client_fd    = httpd_req_to_sockfd(req);
        task_arg->server       = req->handle;
        task_arg->user_message = strdup(text_item->valuestring);

        BaseType_t created = xTaskCreate(
            chat_worker_task,
            "chat_worker",
            CHAT_TASK_STACK_SIZE,
            task_arg,
            CHAT_TASK_PRIORITY,
            NULL
        );
        if (created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create chat_worker task");
            free(task_arg->user_message);
            free(task_arg);
        }
    }

    cJSON_Delete(msg);
    return ESP_OK;
}

// ─── HTTP route handlers ──────────────────────────────────────────────────────

static esp_err_t index_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /");
    FILE *f = fopen(SPIFFS_BASE_PATH "/index.html", "r");
    if (!f) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");

    char chunk[512];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        httpd_resp_send_chunk(req, chunk, n);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0); // end chunked response
    return ESP_OK;
}

static esp_err_t api_gpio_handler(httpd_req_t *req)
{
    char *json = gpio_ctrl_status_json();
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t api_reset_handler(httpd_req_t *req)
{
    openai_ctx_reset(s_ctx);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    ESP_LOGI(TAG, "Conversation reset");
    return ESP_OK;
}

// ─── SPIFFS init ──────────────────────────────────────────────────────────────

static esp_err_t init_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = SPIFFS_BASE_PATH,
        .partition_label        = NULL,
        .max_files              = 5,
        .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount SPIFFS");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "SPIFFS partition not found");
        } else {
            ESP_LOGE(TAG, "SPIFFS init error: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info(NULL, &total, &used);
    ESP_LOGI(TAG, "SPIFFS: total=%d used=%d", total, used);
    return ESP_OK;
}

// ─── Server start / stop ──────────────────────────────────────────────────────

esp_err_t web_server_start(openai_ctx_t *ctx)
{
    s_ctx = ctx;

    esp_err_t ret = init_spiffs();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS not available — index.html won't be served");
        // Continue anyway; GPIO API still works
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port      = WEB_SERVER_PORT;
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;

    ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start httpd: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register URI handlers
    httpd_uri_t uri_index = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = index_handler,
    };
    httpd_register_uri_handler(s_server, &uri_index);

    httpd_uri_t uri_gpio = {
        .uri     = "/api/gpio",
        .method  = HTTP_GET,
        .handler = api_gpio_handler,
    };
    httpd_register_uri_handler(s_server, &uri_gpio);

    httpd_uri_t uri_reset = {
        .uri     = "/api/reset",
        .method  = HTTP_POST,
        .handler = api_reset_handler,
    };
    httpd_register_uri_handler(s_server, &uri_reset);

    // WebSocket handler
    httpd_uri_t uri_ws = {
        .uri          = WS_URI,
        .method       = HTTP_GET,
        .handler      = ws_handler,
        .is_websocket = true,
    };
    httpd_register_uri_handler(s_server, &uri_ws);

    ESP_LOGI(TAG, "HTTP server started on port %d", WEB_SERVER_PORT);
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
