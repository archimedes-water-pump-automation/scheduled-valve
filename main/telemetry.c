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

void telemetry_publish_valve(bool open, const char *reason)
{
    if (!s_connected || s_client == NULL) {
        return;
    }

    char clock[8];
    schedule_time_string(clock, sizeof(clock));

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"event\":\"valve\",\"device\":\"%s\",\"state\":\"%s\","
             "\"reason\":\"%s\",\"local_time\":\"%s\",\"uptime_s\":%lu}",
             DEVICE_ID, open ? "open" : "closed", reason, clock,
             (unsigned long)(esp_timer_get_time() / 1000000));

    /* enqueue, not publish: the blocking variant can park the caller
     * for seconds on a degraded link. Valve control cannot wait. */
    esp_mqtt_client_enqueue(s_client, TOPIC_VALVE, payload,
                            (int)strlen(payload), 1, 1, true);
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

static void handle_command(const char *data, int len)
{
    char buf[128];

    if (len <= 0 || len >= (int)sizeof(buf)) {
        ESP_LOGW(TAG, "command payload size %d rejected", len);
        return;
    }
    memcpy(buf, data, len);
    buf[len] = '\0';

    /* JSON form: {"command":"keep_open"} / {"command":"turn_off"} */
    cJSON *root = cJSON_Parse(buf);
    if (root != NULL) {
        const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "command");
        if (cJSON_IsString(cmd) && cmd->valuestring != NULL) {
            set_command(cmd->valuestring);
        } else {
            ESP_LOGW(TAG, "json has no string 'command' field");
        }
        cJSON_Delete(root);
        return;
    }

    /* Bare form, convenient from mosquitto_pub. */
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
