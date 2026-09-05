#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"

#include "config.h"
#include "schedule.h"
#include "telemetry.h"

static const char *TAG = "telemetry";

static esp_mqtt_client_handle_t s_client;
static volatile bool            s_connected;

static portMUX_TYPE  s_evt_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_keep_open_pending;
static volatile bool s_turn_off_pending;

/* Delivered by the broker if this controller drops off without a clean
 * disconnect, so a dashboard cannot show "open" for a board that lost
 * power. It carries neither timestamp nor uptime: the broker publishes
 * it long after this controller wrote it, so both would be lies.
 * MQTT_CONTRACT.md makes them optional for exactly this case. */
static const char *LWT_PAYLOAD =
    "{\"event\":\"valve\",\"device\":\"" DEVICE_ID "\",\"state\":\"unknown\","
    "\"reason\":\"controller_offline\"}";

bool telemetry_online(void) { return s_connected; }

static bool take_flag(volatile bool *flag)
{
    bool taken;
    portENTER_CRITICAL(&s_evt_mux);
    taken = *flag;
    *flag = false;
    portEXIT_CRITICAL(&s_evt_mux);
    return taken;
}

bool telemetry_take_keep_open(void) { return take_flag(&s_keep_open_pending); }
bool telemetry_take_turn_off(void)  { return take_flag(&s_turn_off_pending);  }

/* ======================= publish ======================= */

/* Appends to buf at *off, tracking the length the message would have
 * needed so the caller can tell a truncated payload from a whole one. */
static void json_append(char *buf, size_t size, size_t *off,
                        const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*off < size ? buf + *off : NULL,
                      *off < size ? size - *off : 0,
                      fmt, ap);
    va_end(ap);

    if (n > 0) {
        *off += (size_t)n;
    }
}

void telemetry_publish_valve(bool open, const char *reason)
{
    char   payload[256];
    size_t off = 0;
    char   ts[SCHEDULE_ISO8601_LEN];
    char   clock[8];

    if (!s_connected || s_client == NULL) {
        return;
    }

    /* The envelope every message on every topic shares: event name,
     * this device, and the UTC timestamp when the clock has one. See
     * MQTT_CONTRACT.md. */
    json_append(payload, sizeof(payload), &off,
                "{\"event\":\"valve\",\"device\":\"%s\"", DEVICE_ID);

    if (schedule_iso8601(ts, sizeof(ts))) {
        json_append(payload, sizeof(payload), &off, ",\"timestamp\":\"%s\"", ts);
    }

    /* local_time stays alongside it for the humans reading a dashboard:
     * this module's whole behaviour is described in local hours. */
    schedule_time_string(clock, sizeof(clock));

    json_append(payload, sizeof(payload), &off,
                ",\"state\":\"%s\",\"reason\":\"%s\",\"local_time\":\"%s\""
                ",\"uptime_s\":%lu}",
                open ? "open" : "closed", reason, clock,
                (unsigned long)(esp_timer_get_time() / 1000000));

    if (off >= sizeof(payload)) {
        ESP_LOGE(TAG, "valve payload truncated at %u bytes, not published",
                 (unsigned)sizeof(payload));
        return;
    }

    /* enqueue, not publish: the blocking variant can park the caller
     * for seconds on a degraded link. Valve control cannot wait. */
    esp_mqtt_client_enqueue(s_client, TOPIC_VALVE, payload,
                            (int)off, 1, 1, true);
}

/* ======================= commands ======================= */

static void set_command(const char *cmd)
{
    if (strcmp(cmd, "keep_open") == 0) {
        portENTER_CRITICAL(&s_evt_mux);
        s_keep_open_pending = true;
        portEXIT_CRITICAL(&s_evt_mux);
        ESP_LOGI(TAG, "command: keep_open");
    } else if (strcmp(cmd, "turn_off") == 0) {
        portENTER_CRITICAL(&s_evt_mux);
        s_turn_off_pending = true;
        portEXIT_CRITICAL(&s_evt_mux);
        ESP_LOGI(TAG, "command: turn_off");
    } else {
        ESP_LOGW(TAG, "unknown command '%s'", cmd);
    }
}

/* Parses one command message. The shape it expects is fixed by
 * MQTT_CONTRACT.md and is what pump-ctl publishes:
 *
 *   {"event":"command","device":"pump-01","timestamp":"...",
 *    "command":"keep_open","reason":"flow_confirmed","uptime_s":338}
 *
 * Only the command field decides anything; device and reason are logged
 * so the valve's log says who asked and why. */
static void handle_command(const char *data, int len)
{
    /* Big enough for the full envelope with a timestamp and a reason.
     * A payload that does not fit is rejected rather than truncated
     * into a different command. */
    char buf[256];

    if (len <= 0 || len >= (int)sizeof(buf)) {
        ESP_LOGW(TAG, "command payload size %d rejected", len);
        return;
    }
    memcpy(buf, data, len);
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root != NULL) {
        const cJSON *event  = cJSON_GetObjectItemCaseSensitive(root, "event");
        const cJSON *device = cJSON_GetObjectItemCaseSensitive(root, "device");
        const cJSON *cmd    = cJSON_GetObjectItemCaseSensitive(root, "command");
        const cJSON *reason = cJSON_GetObjectItemCaseSensitive(root, "reason");

        if (cJSON_IsString(event) && strcmp(event->valuestring, "command") != 0) {
            /* Something else's event reached this topic. Acting on its
             * command field would be acting on a message not addressed
             * to this valve. */
            ESP_LOGW(TAG, "payload on command topic is not a command event");
        } else if (cJSON_IsString(cmd) && cmd->valuestring != NULL) {
            ESP_LOGI(TAG, "command from %s (%s)",
                     cJSON_IsString(device) ? device->valuestring : "unknown",
                     cJSON_IsString(reason) ? reason->valuestring : "no reason");
            set_command(cmd->valuestring);
        } else {
            ESP_LOGW(TAG, "json has no string 'command' field");
        }

        cJSON_Delete(root);
        return;
    }

    /* Bare form, convenient from mosquitto_pub during bring-up. Not
     * part of what any module publishes. */
    set_command(buf);
}

/* ======================= event handlers ======================= */

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)arg; (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            esp_mqtt_client_subscribe(s_client, TOPIC_CMD, 1);
            ESP_LOGI(TAG, "broker connected, subscribed to %s", TOPIC_CMD);
            break;

        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            ESP_LOGW(TAG, "broker disconnected");
            break;

        case MQTT_EVENT_DATA:
            /* Retained commands are rejected. keep_open belongs to one
             * trial; a retained copy would replay on every reconnect
             * and hold the valve open with no trial behind it. */
            if (event->retain) {
                ESP_LOGW(TAG, "ignoring retained command");
                break;
            }
            if (event->data_len == event->total_data_len &&
                event->current_data_offset == 0) {
                handle_command(event->data, event->data_len);
            }
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGW(TAG, "mqtt error");
            break;

        default:
            break;
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)arg; (void)event_data;

    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "wifi up");
        esp_mqtt_client_start(s_client);
    }
}

/* ======================= init ======================= */

void telemetry_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri                  = MQTT_BROKER_URI,
        .credentials.username                = MQTT_USERNAME,
        .credentials.client_id               = DEVICE_ID,
        .credentials.authentication.password = MQTT_PASSWORD,
        .session.last_will.topic             = TOPIC_VALVE,
        .session.last_will.msg               = LWT_PAYLOAD,
        .session.last_will.msg_len           = 0,
        .session.last_will.qos               = 1,
        .session.last_will.retain            = 1,
        .session.keepalive                   = 30,
        .network.reconnect_timeout_ms        = 5000,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "mqtt client init failed, running offline");
        return;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(
        s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_start());
}
