# scheduled-valve

ESP32 firmware for the first-stage activator. On a configured schedule it opens
a 110 V AC solenoid valve to admit water to the pipeline, then waits for a
`keep_open` event. If one arrives the valve stays open until `turn_off`;
if not, it closes and waits for the next slot.

The valve sits before the water pump. Replaces manual operation of that valve.

Standalone in its logic: it opens on its own schedule and closes on its own
guards, needing nothing from the rest of the system to stay safe. The one
thing it shares is the command topic `pump-ctl` publishes `keep_open` and
`turn_off` on — see [MQTT_CONTRACT.md](MQTT_CONTRACT.md).

## Behaviour

| State | Valve | Leaves when |
|---|---|---|
| `idle` | shut | A scheduled slot arrives |
| `trial` | open | `keep_open` (→ holding), or `KEEP_OPEN_WINDOW_MS` elapses (→ idle) |
| `holding` | open | `turn_off`, or `MAX_HOLD_MS` elapses |

`turn_off` is honoured in any state, including mid-trial.

A `keep_open` received while idle is logged and discarded. That is the point:
the event authorises continuing a trial already in progress, so a stray or
replayed one cannot open a mains valve on its own.

## Configuring the schedule

Three constants in `main/config.h` describe the window and cadence. Slots align
to the window start, so 03:00 with a 10-minute interval gives 03:00, 03:10,
03:20, and so on.

```c
/* every 10 min, 03:00–05:00  (default) */
#define SCHED_START_HOUR    3
#define SCHED_START_MIN     0
#define SCHED_END_HOUR      5
#define SCHED_END_MIN       0
#define SCHED_INTERVAL_MIN 10
```

| Desired | START | END | INTERVAL |
|---|---|---|---|
| Every 10 min, 03:00–05:00 | 3, 0 | 5, 0 | 10 |
| Every 30 min, all day | 0, 0 | 23, 59 | 30 |
| Every 15 min, 22:00–02:00 | 22, 0 | 2, 0 | 15 |
| Once at 06:00 | 6, 0 | 6, 0 | 1 |

A window where the end is earlier than the start crosses midnight and is
handled.

### The clock is a hard dependency

The ESP32 has no battery-backed RTC, so the schedule needs SNTP and a
timezone. **No scheduled opening happens until the clock is valid** — a board
that has not synced sits at 1970 and would otherwise fire at an arbitrary hour.

`TZ_STRING` defaults to `"<-03>3"` (UTC−3, no DST), inferred from the 110 V
mains. Check it. Getting it wrong makes the valve open at the wrong hour with
no error anywhere.

```c
#define TZ_STRING "UTC0"                        /* UTC             */
#define TZ_STRING "EST5EDT,M3.2.0,M11.1.0"      /* US Eastern      */
#define TZ_STRING "CET-1CEST,M3.5.0,M10.5.0/3"  /* Central Europe  */
```

The board resyncs hourly (`CONFIG_LWIP_SNTP_UPDATE_DELAY`), because drift over
a long uptime shifts the schedule.

### Reboot behaviour

If the first valid clock read lands mid-window, the module adopts the current
slot rather than firing. A board that reboots repeatedly cannot trigger trials
outside the normal cadence — the cost is waiting up to one interval after a
restart.

A reboot while holding leaves the valve closed (the relay de-energises on power
loss) and the module returns to idle. It does not resume the hold: nothing has
confirmed that water is still wanted. The next scheduled slot picks it back up.

### Sizing the trial window

`KEEP_OPEN_WINDOW_MS` must exceed the time water needs to travel the pipeline
*plus* the time whatever is downstream needs to notice and decide. Too short
and every trial times out before the answer can arrive, and the valve never
holds open. Measure it once on the real plumbing rather than guessing.

`MAX_HOLD_MS` is a runaway guard — a hold that never receives `turn_off` would
otherwise leave a mains valve open indefinitely. Four hours by default.

## Hardware

| Signal | GPIO | Notes |
|---|---|---|
| Relay IN | 33 | Active low, 10 kΩ pull-up to 3V3 |

No sensors. The module is driven entirely by the clock and MQTT.

Low-voltage side runs from a single 5 V supply: ESP32 VIN and the relay coil,
common ground.

The 10 kΩ pull-up on GPIO33 is not optional. The pin floats through reset and
the bootloader window, and a floating active-low input can close the relay —
opening a mains valve — before firmware exists.

### The 110 V side

Line → relay COM. Relay NO → valve. Valve → neutral. Earth to the valve body.

- **Normally-open contacts and a normally-closed valve.** Loss of power, a
  crash, or a held reset all leave the valve shut.
- **Contact rating must exceed the valve's inrush**, not its holding current.
  An SRD-05VDC-SL-C at 10 A / 250 VAC covers a typical solenoid comfortably.
- **Fit an RC snubber across the contacts.** An AC solenoid is an inductive
  load; switching it arcs across the contacts and erodes them. A 100 Ω +
  0.1 µF X2-rated snubber, or a ready-made module, extends contact life
  substantially. Do **not** fit a flyback diode — that is for DC coils and
  will short an AC supply.
- **None of this goes on a breadboard.** Enclosure, rated terminals, mains
  separated from the low-voltage side. Have it inspected by a qualified
  electrician before energising.

## Build

```sh
cp main/secrets.h.example main/secrets.h   # then edit it
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Requires ESP-IDF v5.x.

## MQTT

Defined in [MQTT_CONTRACT.md](MQTT_CONTRACT.md), which is mirrored in every
repository of this system. The command topic is shared with
[`pump-ctl`](https://github.com/archimedes-water-pump-automation/pump-ctl);
changing a field there means changing it on both sides.

**Subscribes** to `watertank/activator-01/cmd` — QoS 1:

```json
{"event":"command","device":"pump-01","timestamp":"2026-09-05T03:10:12Z",
 "command":"keep_open","reason":"flow_confirmed","uptime_s":338}
```

Only `command` (`keep_open` or `turn_off`) decides anything. `device` and
`reason` are logged, so the valve's log says who asked and why, and a payload
whose `event` is something other than `command` is rejected rather than
mined for a command field.

A bare command word is also accepted, which is a convenience for `mosquitto_pub`
during bring-up rather than something any module publishes:

```sh
mosquitto_pub -h broker -t watertank/activator-01/cmd -m keep_open -q 1
```

**Do not publish these retained.** Unlike a desired-state topic, `keep_open` is
an event bound to one specific trial. A retained copy replays on every
reconnect and would hold the valve open with no trial behind it. The firmware
rejects retained messages on this topic, but publishing without `-r` is the
correct habit.

**Publishes** to `watertank/activator-01/valve` — QoS 1, retained, one message
per transition:

```json
{"event":"valve","device":"activator-01","timestamp":"2026-09-05T06:10:00Z",
 "state":"open","reason":"scheduled_trial","local_time":"03:10","uptime_s":1840}
```

Reasons: `scheduled_trial`, `keep_open`, `trial_timeout`, `turn_off`,
`max_hold`, and `boot` for the announcement above.

`timestamp` is UTC and appears once SNTP has landed; `local_time` is the same
moment on this board's own clock, kept because everything about this module —
its window, its interval, its log lines — is described in local hours. No
module in this system subscribes to this topic today; it exists for dashboards
and for diagnosing what the activator did.

On its first connection after a restart the module publishes the state the
relay is actually in, with `reason: "boot"`. The topic is retained, so without
it a restart leaves the last pre-restart message standing — `"open"`, for a
valve that power loss has since closed. Only the first connection, so a
reconnect never reports a transition that has not happened.

Last will sets `"state":"unknown"` so a dashboard cannot show `open`
indefinitely for a controller that has lost power. It carries neither
`timestamp` nor `uptime_s`: the broker publishes it long after this board
wrote it.

## Bring-up

1. Flash with the relay IN wire disconnected and mains off. Watch the log for
   `clock synced` and confirm the local time is right.
2. Temporarily set a wide window and a 1-minute interval to see trials fire
   without waiting for 03:00.
3. Send `keep_open` during a trial and confirm the state goes to `holding`;
   send `turn_off` and confirm it closes.
4. Connect relay IN, mains still off, and listen for the click.
5. Restore the real schedule. Energise the mains side only after an
   electrician has checked it.

## Known gaps

- The schedule is compile-time. Making it runtime-configurable over MQTT with
  NVS persistence is the obvious next step.
- Credentials are compiled in. Move to NVS before deployment.
- Plaintext MQTT. Switch to `mqtts://` with a CA certificate — an
  unauthenticated broker is an unauthenticated switch for a mains valve.
- No valve position feedback. The firmware knows what it commanded, not what
  the valve did. A stuck valve is undetectable here.
- No OTA update path.

## License

MIT — see [LICENSE](LICENSE).
