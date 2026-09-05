#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "config.h"
#include "control.h"
#include "schedule.h"
#include "valve.h"
#include "telemetry.h"

static const char *TAG = "control";

typedef enum {
    VS_IDLE,     /* closed, waiting for the next scheduled slot     */
    VS_TRIAL,    /* open, waiting for keep_open before the deadline */
    VS_HOLDING   /* keep_open received; open until turn_off         */
} state_t;

static const char *state_name(state_t s)
{
    switch (s) {
        case VS_IDLE:  return "idle";
        case VS_TRIAL: return "trial";
        default:       return "holding";
    }
}

static void control_task(void *arg)
{
    (void)arg;

    state_t state = VS_IDLE;

    int64_t state_since_ms = esp_timer_get_time() / 1000;
    const char *reason     = "boot";

    for (;;) {
        int64_t now_ms   = esp_timer_get_time() / 1000;
        int64_t in_state = now_ms - state_since_ms;

        /* Called unconditionally: slots must advance whether or not
         * this state can act on one, so a long hold does not leave a
         * stale slot that fires the instant we return to idle. */
        bool slot_due = schedule_slot_due();

        bool keep_open = telemetry_take_keep_open();
        bool turn_off  = telemetry_take_turn_off();

        state_t next = state;

        /* turn_off is a safety command and is honoured in any state,
         * including a trial in progress. */
        if (turn_off && state != VS_IDLE) {
            next   = VS_IDLE;
            reason = "turn_off";
        } else {
            switch (state) {

            case VS_IDLE:
                if (keep_open) {
                    /* No trial is running, so nothing to keep open.
                     * Discarding it is the point: a stray or replayed
                     * event must not open a mains valve on its own. */
                    ESP_LOGW(TAG, "keep_open outside a trial, ignored");
                }
                if (in_state < MIN_CLOSED_MS) {
                    break;
                }
                if (slot_due) {
                    next   = VS_TRIAL;
                    reason = "scheduled_trial";
                }
                break;

            case VS_TRIAL:
                if (keep_open) {
                    next   = VS_HOLDING;
                    reason = "keep_open";
                } else if (in_state >= KEEP_OPEN_WINDOW_MS) {
                    next   = VS_IDLE;
                    reason = "trial_timeout";
                }
                break;

            case VS_HOLDING:
                if (in_state >= MAX_HOLD_MS) {
                    ESP_LOGW(TAG, "max hold reached with no turn_off");
                    next   = VS_IDLE;
                    reason = "max_hold";
                }
                break;
            }
        }

        if (next != state) {
            bool was_open = valve_is_open();

            state          = next;
            state_since_ms = now_ms;

            bool want_open = (state == VS_TRIAL || state == VS_HOLDING);

            /* The relay moves first. Telemetry reports what happened;
             * it is never a precondition for it. */
            if (want_open != was_open) {
                if (want_open) {
                    valve_open();
                } else {
                    valve_close();
                }
            }
            telemetry_publish_valve(want_open, reason);

            char clock[8];
            schedule_time_string(clock, sizeof(clock));
            ESP_LOGI(TAG, "-> %s (%s) at %s",
                     state_name(state), reason, clock);
        }

        vTaskDelay(pdMS_TO_TICKS(CONTROL_PERIOD_MS));
    }
}

void control_start(void)
{
    valve_init();   /* relay driven closed before anything else */

    xTaskCreate(control_task, "control", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "scheduled activator up, valve closed");
}
