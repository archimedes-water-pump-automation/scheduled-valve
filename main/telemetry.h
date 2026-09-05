#pragma once

#include <stdbool.h>

void telemetry_start(void);
bool telemetry_online(void);

/* Event accessors. Each returns true once per received event and
 * clears it. Commands here are events, not desired state: a keep_open
 * belongs to one specific trial and must not survive to influence a
 * later one. */
bool telemetry_take_keep_open(void);
bool telemetry_take_turn_off(void);

/* Published on every valve transition, retained. */
void telemetry_publish_valve(bool open, const char *reason);
