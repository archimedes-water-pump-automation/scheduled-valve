#pragma once

#define DEVICE_ID "activator-01"

/* ======================= pin map ======================= */

#define PIN_RELAY  GPIO_NUM_33   /* relay IN, 10k pull-up to 3V3 */

#define RELAY_ACTIVE_LOW 1

/* ======================= clock ======================= */

/* POSIX TZ string. The schedule below is in LOCAL time, so this is
 * not optional. "<-03>3" is UTC-3 with no DST (Brazil). Examples:
 *   UTC              "UTC0"
 *   US Eastern       "EST5EDT,M3.2.0,M11.1.0"
 *   Central Europe   "CET-1CEST,M3.5.0,M10.5.0/3"
 * Get it wrong and the valve opens at the wrong hour, silently. */
#define TZ_STRING   "<-03>3"
#define SNTP_SERVER "pool.ntp.org"

/* ======================= schedule ======================= */

/* Open the valve every SCHED_INTERVAL_MIN minutes, between the start
 * and end times, local clock. Slots are aligned to the window start,
 * so 03:00 + 10 min gives 03:00, 03:10, 03:20 ...
 *
 * A window where end < start crosses midnight and is handled.
 *
 *   every 10 min, 03:00-05:00  ->  3, 0, 5, 0, 10   (below)
 *   every 30 min, all day      ->  0, 0, 23, 59, 30
 *   every 15 min, 22:00-02:00  ->  22, 0, 2, 0, 15  (wraps)
 */
#define SCHED_START_HOUR    3
#define SCHED_START_MIN     0
#define SCHED_END_HOUR      5
#define SCHED_END_MIN       0
#define SCHED_INTERVAL_MIN 10

/* ======================= trial and hold ======================= */

/* How long the valve stays open waiting for a keep_open event.
 * Must exceed the time water needs to travel the pipeline and for
 * whatever is downstream to notice and decide. Too short and every
 * trial times out before the answer can arrive. */
#define KEEP_OPEN_WINDOW_MS  120000        /* 2 minutes */

/* Runaway guard. A hold that never receives turn_off would otherwise
 * leave a mains valve open indefinitely. */
#define MAX_HOLD_MS   (4LL * 60 * 60 * 1000)   /* 4 hours */

#define MIN_CLOSED_MS   30000   /* cooldown; limits contact wear */
#define CONTROL_PERIOD_MS 500

/* ======================= network ======================= */

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy main/secrets.h.example to main/secrets.h and fill in credentials"
#endif

#define TOPIC_CMD   "watertank/" DEVICE_ID "/cmd"    /* subscribed */
#define TOPIC_VALVE "watertank/" DEVICE_ID "/valve"  /* published  */
