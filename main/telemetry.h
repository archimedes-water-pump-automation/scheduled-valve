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

/* True exactly once, on the first cycle after the broker connection
 * comes up for the first time since boot.
 *
 * The valve topic is retained, so between a restart and the next
 * transition it holds whatever this controller last published — "open",
 * possibly, for a valve that power loss has since closed. The caller
 * answers by publishing the state the relay is actually in. Only the
 * first connection: a reconnect mid-hold would republish a transition
 * that has not happened. */
bool telemetry_take_first_connect(void);

/* Published on every valve transition, retained. */
void telemetry_publish_valve(bool open, const char *reason);
