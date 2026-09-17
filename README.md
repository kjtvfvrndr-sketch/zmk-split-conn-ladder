# zmk-split-conn-ladder

Steps the **split-link** BLE connection parameters down while the keyboard is
idle, and restores them the moment activity resumes.

ZMK's `ZMK_ACTIVITY_IDLE` state turns off the display, the backlight, the
underglow and the battery poll — and does nothing at all to the radio. Between
`ZMK_IDLE_TIMEOUT` (30 s by default) and `ZMK_IDLE_SLEEP_TIMEOUT` (15 min by
default) the split link keeps running at full active cadence while nothing is
being sent. On a 7.5 ms link that is ~133 radio wake-ups per second, around the
clock, on the half that holds the central role. This module fills that window.

## Why the split link and not the host link

| | central half ↔ peripheral half | central half ↔ host |
|---|---|---|
| this half's role | **central** | **peripheral** |
| must be on air | every single event | once per (latency + 1) events |
| changing parameters | an order (`LL_CONNECTION_UPDATE_IND`) | a request the host may refuse |
| cost at 7.5 / 15 ms | ~133 events/s | ~2 events/s |

A central cannot use peripheral latency at all — it opens every connection
event itself — so on the split link the interval is the only lever, and it
moves ~98 % of this half's radio budget. The host link is already cheap
thanks to its negotiated latency, so this module does not touch it.

## Install

Add to `config/west.yml`:

```yaml
manifest:
  remotes:
    - name: kjtvfvrndr-sketch
      url-base: https://github.com/kjtvfvrndr-sketch
  projects:
    - name: zmk-split-conn-ladder
      remote: kjtvfvrndr-sketch
      revision: main
```

## Configure

Everything goes in the **central half's** `.conf` (the half that holds
`ZMK_SPLIT_ROLE_CENTRAL`). The module `depends on ZMK_SPLIT_ROLE_CENTRAL`, so
on the peripheral half it is not built at all and the settings do nothing.

```
# ── ACTIVE parameters. NOT part of this module: the link is created with
#    these, so the ladder reads the same symbols and cannot drift from them.
CONFIG_ZMK_SPLIT_BLE_PREF_INT=12
CONFIG_ZMK_SPLIT_BLE_PREF_LATENCY=9
CONFIG_ZMK_SPLIT_BLE_PREF_TIMEOUT=600

# ── the ladder
CONFIG_ZMK_SPLIT_CONN_LADDER=y
CONFIG_ZMK_SPLIT_CONN_LADDER_STEPS=3

CONFIG_ZMK_SPLIT_CONN_LADDER_S1_AFTER_MS=60000
CONFIG_ZMK_SPLIT_CONN_LADDER_S1_INT=24
CONFIG_ZMK_SPLIT_CONN_LADDER_S1_LATENCY=30
CONFIG_ZMK_SPLIT_CONN_LADDER_S1_TIMEOUT=600

CONFIG_ZMK_SPLIT_CONN_LADDER_S2_AFTER_MS=150000
CONFIG_ZMK_SPLIT_CONN_LADDER_S2_INT=48
CONFIG_ZMK_SPLIT_CONN_LADDER_S2_LATENCY=30
CONFIG_ZMK_SPLIT_CONN_LADDER_S2_TIMEOUT=600

CONFIG_ZMK_SPLIT_CONN_LADDER_S3_AFTER_MS=600000
CONFIG_ZMK_SPLIT_CONN_LADDER_S3_INT=96
CONFIG_ZMK_SPLIT_CONN_LADDER_S3_LATENCY=20
CONFIG_ZMK_SPLIT_CONN_LADDER_S3_TIMEOUT=800
```

Kconfig `.conf` files do not understand trailing comments — everything after
`=` is the value. Keep comments on their own lines.

`S*_AFTER_MS` is measured from entry into `ZMK_ACTIVITY_IDLE`, not from the
last keypress. Total idle before a step fires is `ZMK_IDLE_TIMEOUT +
S*_AFTER_MS`.

`S1_AFTER_MS` is the hysteresis that makes bursty work free: with 60 s, any
pause shorter than ~90 s costs **zero** parameter updates, so reading-then-
typing never pays a ramp-up.

## Units and arithmetic

| field | unit | examples |
|---|---|---|
| `_INT` | 1.25 ms steps | 12 = 15 ms, 24 = 30 ms, 48 = 60 ms, 96 = 120 ms, 160 = 200 ms |
| `_LATENCY` | plain count of events | — |
| `_TIMEOUT` | 10 ms steps | 400 = 4 s, 600 = 6 s, 800 = 8 s |
| `_AFTER_MS` | milliseconds | — |

Derived, all in milliseconds:

```
LED sync delay, and how often the peripheral must be on air
                                   = INT * (LATENCY + 1) * 1.25
ramp-up back to ACTIVE             = INT * 6 * 1.25   (6 events is the
                                     Link Layer instant minimum)
first peripheral-half key after a step = INT * 1.25   (worst case)
central half's own radio rate       = 1 / (INT * 1.25)
```

Hard requirement, from BT Core 5.0 Vol 2 Part E 7.8.12 and enforced by
Zephyr's own `bt_le_conn_params_valid()`:

```
TIMEOUT * 4 > (1 + LATENCY) * INT
```

Every step, and the ACTIVE values, are checked against it with
`BUILD_ASSERT`, as is the requirement that `S*_AFTER_MS` increase. A bad
table fails the build instead of dropping links on your desk.

### Picking a latency

The peripheral's cost falls as `1 / (LATENCY + 1)`, so the returns die
quickly. At a 15 ms interval:

| latency | LED delay | peripheral |
|---|---|---|
| 4 | 75 ms | ~107 µA |
| 9 | 150 ms | ~53 µA |
| 19 | 300 ms | ~27 µA |
| 29 | 450 ms | ~18 µA |

Raise it until the LED delay starts to annoy you, and not one step further.

### Picking an interval for a deep step

Bounded by the ramp-up, not by the first-key delay: at 500 ms the link needs
~3 s to come back up to speed, and you type the whole first sentence with
hand-to-hand skew. Around 120 ms (720 ms ramp-up) is the practical floor.

## Reading the log

The module registers its own log module, so
`CONFIG_ZMK_SPLIT_CONN_LADDER_LOG_LEVEL` tunes it independently of the rest of
ZMK. Its level resolves to `CONFIG_LOG_DEFAULT_LEVEL`, which Zephyr defaults to
3 (INFO), so **everything below compiles in with no extra flag**.

Two things worth knowing:

* `CONFIG_ZMK_LOGGING_MINIMAL` does **not** silence this module. It only stops
  `CONFIG_ZMK_LOG_LEVEL` being defaulted to 4, which affects ZMK's own modules.
* Two lines are at `LOG_DBG` and are therefore compiled out at the default
  level — `link re-established, level forced to 0` and `level N recorded, no
  split link up yet`. They are exactly what disambiguates "the ladder is
  broken" from "there is no split link to apply to", so for the first bring-up
  add:

```
-DCONFIG_ZMK_SPLIT_CONN_LADDER_LOG_LEVEL_DBG=y
```

If you ever set `CONFIG_LOG_DEFAULT_LEVEL` below 3 globally, pin this module
back up with `-DCONFIG_ZMK_SPLIT_CONN_LADDER_LOG_LEVEL_INF=y`.

```
activity: IDLE
step 1/3: interval 30.00 ms, latency 30, timeout 6000 ms (peripheral on air every 930 ms, ramp-up 180 ms)
split link applied: interval 30.00 ms, latency 30, timeout 6000 ms
```

* `step N/M` is what was **requested**; `split link applied` is what the
  controller actually **applied**. Compare them — a mismatch means the
  controller trimmed something, and from then on the real numbers are the
  applied ones.
* The delay between those two lines is the real Link Layer instant. Six
  connection events is the spec minimum, not a promise; if a deep step takes
  much longer than `INT * 6 * 1.25` to land, make it shallower.
* `host link applied:` lines are logged too. That is where the interval and
  latency the host granted become visible — useful on macOS, which insists
  on 15 ms and will refuse a 7.5 ms request.
* `param update rejected: -16` (`-EBUSY`) right after a connect means
  `SETTLE_MS` was too short and we collided with Zephyr's own automatic
  parameter update. The module retries after `RETRY_MS`; if the line repeats,
  raise `SETTLE_MS`.
* `activity: IDLE` with no `step` line following means `bt_conn_foreach`
  found no central-role connection — check whether the split link is up.

## Design notes

* `current_level` has exactly one writer, the low-priority work handler.
  `on_connected` only raises an atomic flag, because the compare-then-set in
  `apply_level()` is not atomic and a reconnect landing mid-update would
  otherwise leave our idea of the level permanently out of step with the
  link — after which the early return freezes the wrong parameters in place.
* The activity state is read inside the work handler, not in the event
  listener, so an already-running handler cannot clobber a just-applied
  ACTIVE. Cancelling a delayable work does not wait for a handler that has
  already started.
* All BLE calls run on ZMK's low-priority work queue. For a central,
  `bt_conn_le_param_update()` goes into `bt_hci_cmd_send_sync()` and blocks
  until the controller answers; the system work queue must not carry that.
* `ZMK_ACTIVITY_SLEEP` cancels the work. Otherwise a pending handler fires
  during `zmk_pm_suspend_devices()` and blocks on HCI against a radio that is
  already going down.
* Uptime is tracked with `k_uptime_get_32()`: the read is atomic on a 32-bit
  target, and unsigned subtraction stays correct across the ~49-day wrap.
* A trackball keeps the ladder at ACTIVE. ZMK's `activity.c` has an input
  listener under `CONFIG_ZMK_POINTING`, so pointer motion counts as activity.

## Interaction with deep sleep

Waking from `ZMK_ACTIVITY_SLEEP` is a `sys_poweroff()` recovery: full reset,
host reconnect, split re-establish — one to three seconds, and the keystroke
that woke it is consumed by the GPIO wake and never reaches the host. Every
ladder step wakes in less than one connection interval instead.

So once the ladder works, push deep sleep **further out**, not closer. At a
120 ms step the central half costs ~67 µA; an extra hour before sleeping is
~67 µAh, which is nothing next to a reboot in the middle of reading something.

```
CONFIG_ZMK_IDLE_SLEEP_TIMEOUT=3600000
```

## Status

Syntax-checked against stub headers for `STEPS` 1 through 4, with the
build-time guards verified to fire on a too-low timeout, on out-of-order
steps, and on bad ACTIVE values. **Not built for hardware and not run on
hardware.** Expect to iterate; the log lines above are there for exactly that.

Verified against ZMK v0.3.0 / Zephyr v3.5 API signatures. `bt_conn_cb_register()`
returns `void` in Zephyr 3.5 and `int` from 3.7 on — revisit when the ZMK base
moves.

## License

MIT
