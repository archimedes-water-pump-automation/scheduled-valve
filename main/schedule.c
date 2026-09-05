#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_sntp.h"
#include "esp_log.h"

#include "config.h"
#include "schedule.h"

static const char *TAG = "schedule";

static int  s_last_slot = -1;
static bool s_primed    = false;
static bool s_announced = false;

void schedule_init(void)
{
    setenv("TZ", TZ_STRING, 1);
    tzset();

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, SNTP_SERVER);
    esp_sntp_init();

    ESP_LOGI(TAG, "sntp started (%s), tz=%s", SNTP_SERVER, TZ_STRING);
    ESP_LOGI(TAG, "window %02d:%02d-%02d:%02d every %d min",
             SCHED_START_HOUR, SCHED_START_MIN,
             SCHED_END_HOUR, SCHED_END_MIN, SCHED_INTERVAL_MIN);
}

static bool local_now(struct tm *out)
{
    time_t now;
    time(&now);
    localtime_r(&now, out);
    /* Unsynced boards come up in 1970. Anything past 2020 means SNTP
     * has landed. */
    return out->tm_year > (2020 - 1900);
}

bool schedule_time_valid(void)
{
    struct tm ti;
    return local_now(&ti);
}

void schedule_time_string(char *buf, size_t len)
{
    struct tm ti;
    if (local_now(&ti)) {
        snprintf(buf, len, "%02d:%02d", ti.tm_hour, ti.tm_min);
    } else {
        snprintf(buf, len, "--:--");
    }
}

/* Windows may cross midnight, in which case end < start. */
static bool in_window(int now_min, int start_min, int end_min)
{
    if (start_min <= end_min) {
        return now_min >= start_min && now_min <= end_min;
    }
    return now_min >= start_min || now_min <= end_min;
}

bool schedule_slot_due(void)
{
    struct tm ti;

    if (!local_now(&ti)) {
        s_primed    = false;
        s_last_slot = -1;
        return false;
    }

    if (!s_announced) {
        s_announced = true;
        ESP_LOGI(TAG, "clock synced: %02d:%02d", ti.tm_hour, ti.tm_min);
    }

    int now_min   = ti.tm_hour * 60 + ti.tm_min;
    int start_min = SCHED_START_HOUR * 60 + SCHED_START_MIN;
    int end_min   = SCHED_END_HOUR   * 60 + SCHED_END_MIN;

    if (!in_window(now_min, start_min, end_min)) {
        /* Reset so the first slot after the window opens always fires,
         * but stay primed: priming is only about the first clock read. */
        s_last_slot = -1;
        s_primed    = true;
        return false;
    }

    int since = (now_min - start_min + 1440) % 1440;
    int slot  = since / SCHED_INTERVAL_MIN;

    if (!s_primed) {
        /* First valid clock read landed mid-window. Adopt the current
         * slot rather than firing, so a reboot cannot trigger a trial
         * outside the normal cadence. */
        s_primed    = true;
        s_last_slot = slot;
        ESP_LOGI(TAG, "started mid-window, waiting for next slot");
        return false;
    }

    if (slot != s_last_slot) {
        s_last_slot = slot;
        return true;
    }
    return false;
}
