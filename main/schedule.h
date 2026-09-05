#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Starts SNTP and applies TZ_STRING. Returns immediately; the clock
 * becomes valid asynchronously. */
void schedule_init(void);

/* False until SNTP has delivered a plausible wall-clock time. No
 * scheduled opening may happen while this is false. */
bool schedule_time_valid(void);

/* True exactly once per scheduled slot. Has side effects: call it once
 * per control cycle regardless of state, so slots advance whether or
 * not the caller can act on them. */
bool schedule_slot_due(void);

/* Local time as "HH:MM" for logging. Writes "--:--" if unsynced. */
void schedule_time_string(char *buf, size_t len);

/* Buffer size for schedule_iso8601(): "2026-09-05T03:10:12Z" plus NUL. */
#define SCHEDULE_ISO8601_LEN 21

/* Writes the current UTC time as RFC 3339, the form MQTT_CONTRACT.md
 * requires of the envelope's timestamp field. Returns false while the
 * clock is unsynced, which is why that field is optional: a board with
 * no battery-backed RTC would otherwise stamp every event 1970. */
bool schedule_iso8601(char *buf, size_t len);
