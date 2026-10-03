# zmk-split-conn-ladder

Steps the **split-link** BLE connection parameters down while the keyboard is
idle, restores them the moment activity resumes, and stops a half from staying
awake — or searching at full power — when there is nothing to work for.

ZMK's `ZMK_ACTIVITY_IDLE` state turns off the display, the backlight, the
underglow and the battery poll — and does nothing at all to the radio. Between
`ZMK_IDLE_TIMEOUT` (30 s by default) and `ZMK_IDLE_SLEEP_TIMEOUT` (15 min by
default) the split link keeps running at full active cadence while nothing is
being sent. On a 7.5 ms link that is ~133 radio wake-ups per second, around the
clock, on the half that holds the central role. This module fills that window.

What is in it — all on by default, the central half's part once the ladder is:

* **The ladder** (central half). Idle steps whose intervals and latencies are
  worked out on the keyboard from a return budget — how long coming back to
  full speed may take — and worked out again whenever the link to the
  computer, the output or the other half's power changes. On USB power the
  same steps keep full speed for the keys and save only the other half's
  battery.
  A `&ladder` key switches between AUTO, ON and OFF.
* **The low-power search** (central half). ZMK looks for a missing other half with
  half the radio's time, all night if that half is asleep. After 35 s the
  module takes over and listens for 30 ms every 1.28 s, which still catches the
  other half the moment it wakes. While the keys go to a Bluetooth host, the
  first 35 s are a middle search of the module's own, 30 ms every 240 ms, so
  the host link — and a trackball on it — keeps most of its airtime.
* **The early sleep** (both halves). A central half with no computer, and a
  peripheral half with no central, go to deep sleep about a minute after the
  last key instead of waiting for ZMK's sleep — which on a cable never comes.
* **The other half's USB power** (peripheral half). A small report that lets
  the ladder stop saving when both halves are on USB power.
* **Closing the link before sleep** (peripheral half). ZMK's sleep powers a
  half off without a word; this closes the split link first, so the central
  knows at once instead of after its supervision timeout.

And one independent option, off by default:

* **The key that wakes the computer over USB gets typed, and nothing is left
  stuck.** ZMK throws that report away, and can leave the host holding a key
  while it wakes; this re-sends the keyboard state until the host is seen
  reading again.

## Why the split link and not the host link

| | central half ↔ peripheral half | central half ↔ host |
|---|---|---|
| this half's role | **central** | **peripheral** |
| must be on air | every single event | once per (latency + 1) events |
| changing parameters | an order (`LL_CONNECTION_UPDATE_IND`) | a request the host may refuse |
| cost at 7.5 / 15 ms | ~133 events/s | ~2 events/s |

A central cannot use peripheral latency at all — it opens every connection
event itself — so on the split link the interval is the only lever. And it is
the lever worth pulling: at a 15 ms split link the two links cost this half
~67 and ~2 events per second, so the split link is ~97 % of its radio budget
(~98.5 % at 7.5 ms). The host link is already cheap thanks to its negotiated
latency, so this module does not touch it.

## What is in here

```
Kconfig                                       every option, with the reasoning in the help text
src/split_conn_ladder.c                       the ladder
src/low_power_search.c                        central: the low-power search for a missing other half
src/early_sleep.c                             both halves: the early sleep
src/absence_wait.h                            how long those two wait, and the build checks on it
src/peripheral_usb_power_report_client.c      central: reads the other half's USB power report
src/peripheral_usb_power_report.c             peripheral: sends that report
src/peripheral_usb_power_report_gatt.h        the UUIDs the two halves share
src/peripheral_close_link_before_sleep.c      peripheral: closes the split link before its sleep
src/ladder_internal.h                         between the module's files; not an API
src/behaviors/behavior_split_conn_ladder.c    the &ladder key
src/usb_resend_keys_after_wake.c              the wake-key re-send (independent)
include/dt-bindings/zmk/split_conn_ladder.h   LADDER_TOG / _CYCLE / _AUTO / _ON / _OFF
include/zmk_split_conn_ladder/ladder.h        get/set mode, for other code
dts/behaviors/split_conn_ladder.dtsi          the &ladder node, for the keymap
dts/bindings/behaviors/...yaml                its binding
examples/                                     ready-to-paste config for an Ergohaven K:04 Micro
  k04_micro_right.conf                          the central half: one line, what it gives, and the optional wake-key re-send
  k04_micro_left.conf                           the peripheral half: nothing to add, and why
  k04_micro.conf                                ZMK's own sleep, both halves
  keymap-snippet.dtsi                           the &ladder key in a keymap
  build.yaml.debug-snippet                      debug build that shows it all working
  test-no-sleep.conf                            the long diagnostic run, no sleep at all
```

## Install

`config/west.yml` already has `remotes:` and `projects:` lists. **Add entries
to them**, do not replace the file — one `remote` for the account, one
`project` for this module:

```yaml
manifest:
  remotes:
    # ... zmkfirmware and whatever else is already here ...
    - name: kjtvfvrndr-sketch
      url-base: https://github.com/kjtvfvrndr-sketch
  projects:
    # ... zmk and your other modules ...
    - name: zmk-split-conn-ladder
      remote: kjtvfvrndr-sketch
      revision: main
```

If a remote with that `url-base` is already listed under another name, reuse
that name in `remote:` instead of adding a second one — west rejects two
remotes with the same name, and two names for the same URL just clutter the
file.

`zephyr/module.yml` declares the module as a devicetree root, so the keymap
finds `<behaviors/split_conn_ladder.dtsi>` and the `dt-bindings` header with
no further setup. West builds every module in the manifest into both halves,
which is how the peripheral half gets its part without a line of its own.

Built for ZMK v0.3.0 (Zephyr 3.5) on the nRF52840.

## Configure

In the **central half's** `.conf` (the half that holds
`ZMK_SPLIT_ROLE_CENTRAL`; on the K:04 Micro,
`config/boards/shields/k04_micro/k04_micro_right.conf`):

```
CONFIG_ZMK_SPLIT_CONN_LADDER=y

# optional, off by default: the wake-key re-send, see below
# CONFIG_ZMK_USB_RESEND_KEYS_AFTER_WAKE=y
```

That is all. The steps, the low-power search and the early sleep come with
the ladder; the **peripheral half** needs no line at all — its USB power
report, its own early sleep and the close before sleep are on by default
there.

Leave ZMK's `CONFIG_ZMK_SPLIT_BLE_PREF_INT` / `_LATENCY` / `_TIMEOUT` at ZMK's
defaults (6 / 30 / 400): remove the lines if the `.conf` sets them.
The split link is created on them and moved to the computed ACTIVE a few
seconds later, so in the computed mode they decide nothing; the build prints a
warning when they differ from ZMK's. They do decide ACTIVE in the table mode (see
"The table").

Things about `.conf` files that bite (Zephyr 3.5's `scripts/kconfig/kconfig.py`):

* **Trailing comments are not comments.** Everything after `=` is the value.
  Keep comments on their own lines.
* **A repeated symbol is not an error.** For hand-written fragments Zephyr
  turns the "set more than once" warning off: the last assignment wins and
  nothing is printed. If your file already sets
  `CONFIG_ZMK_IDLE_SLEEP_TIMEOUT=...`, **edit that line** rather than pasting
  a second one — otherwise which value wins depends on paste order, silently.
* **An unknown symbol is.** Assigning a symbol that does not exist aborts the
  build ("Aborting due to Kconfig warnings"). If it names
  `ZMK_SPLIT_CONN_LADDER`, the module is not in the build — check
  `config/west.yml`.
* **So is a symbol with no prompt.** `CONFIG_ZMK_BEHAVIOR_SPLIT_CONN_LADDER`
  sets itself from the keymap; a `.conf` line assigning it stops the build.
* **The ladder's lines belong to the central half.** The ladder `depends on
  ZMK_SPLIT_ROLE_CENTRAL`; in a `.conf` both halves read, the peripheral only
  earns an "unsatisfied dependencies" warning for them.

Nothing outside the module's own symbols needs setting. A step is applied from
a work queue of the module's own — `bt_conn_le_param_update()` ends in
`bt_hci_cmd_send_sync()` and blocks, which the system work queue must not do.
That call chain measures about 800 bytes; in a build with logging (every
`LOG_*` with arguments packs them on the caller's stack, and the Bluetooth host
logs from inside the HCI call) the deepest path is printing the computed table,
about 1340 bytes, and up to 1556 in a rare case, with up to 104 more for an
interrupt. The queue's stack defaults to 1280, or 2048 when logging is on —
some 340–390 bytes spare — and costs about that much RAM. A
`CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE` line in the `.conf` has nothing to
do with the ladder: it sizes a ZMK queue that other features use.

The module supports one split peripheral —
`CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS=1`, ZMK's default. Its state is kept
for the split link as a whole; the build stops on anything else.

From here on the module's own symbols are named without their
`ZMK_SPLIT_CONN_LADDER_` prefix: `STEP1_LATENCY` is
`CONFIG_ZMK_SPLIT_CONN_LADDER_STEP1_LATENCY`.

`STEP*_AFTER_IDLE_MS` is measured from entry into `ZMK_ACTIVITY_IDLE`, not
from the last keypress: a step begins `ZMK_IDLE_TIMEOUT + STEP*_AFTER_IDLE_MS`
after the last key. Step 1 begins at once (`STEP1_AFTER_IDLE_MS=0`): the 30 s
of `ZMK_IDLE_TIMEOUT` are the hysteresis, and a pause shorter than that costs
no parameter update at all.

### Every option

`STEP2_*` through `STEP6_*` repeat `STEP1_*`; their timers are in the
`STEP1_AFTER_IDLE_MS` row. Steps numbered above `STEP_COUNT` are not compiled
in at all, so their values do not matter. In the computed mode (the default) the step symbols default to
"computed" — the value after "computed:" below — and any other value sets
that one parameter of that step by hand; see "Setting steps by hand".

| symbol (`CONFIG_` prefix) | type | default | range | what it does |
|---|---|---|---|---|
| `ZMK_SPLIT_CONN_LADDER` | bool | n | — | the ladder, and with it the low-power search and the central half's early sleep; central half only |
| `ZMK_SPLIT_CONN_LADDER_STEP_COUNT` | int | 4 | 1–6 | how many idle steps are built |
| `ZMK_SPLIT_CONN_LADDER_COMPUTED_STEPS` | bool | **y** | — | compute the steps; n: take them from the table |
| `ZMK_SPLIT_CONN_LADDER_ACTIVE_AVOIDS_HOST_INTERVAL` | bool | `ZMK_POINTING` | — | computed mode only, as the rows down to `USB_POWER_PERIPHERAL_SYNC_DELAY_MS`: choose ACTIVE against the host link's interval |
| `ZMK_SPLIT_CONN_LADDER_ACTIVE_DEFAULT_INTERVAL` | int | 13 | 6–16 | ACTIVE with no host link to keep out of the way of, 1.25 ms units |
| `ZMK_SPLIT_CONN_LADDER_PERIPHERAL_SHOWS_INDICATORS` | bool | `RGBLED_WIDGET_LAYER_PUSH` or `ZMK_SPLIT_PERIPHERAL_HID_INDICATORS` | — | the other half shows something this half sends it; sets the next default |
| `ZMK_SPLIT_CONN_LADDER_ACTIVE_PERIPHERAL_SYNC_DELAY_MS` | int | 163 with `PERIPHERAL_SHOWS_INDICATORS`, else 400 | 0–5000 | ACTIVE's latency makes the other half's attendance come nearest this |
| `ZMK_SPLIT_CONN_LADDER_RETURN_BUDGET_MS` | int | 1500 | 500–20000 | return budget of every step but the deepest |
| `ZMK_SPLIT_CONN_LADDER_DEEPEST_STEP_RETURN_BUDGET_MS` | int | 3200 | 0–20000 | the deepest step's; 0: `RETURN_BUDGET_MS`. Not below `RETURN_BUDGET_MS`, not above `MAX_RETURN_MS` |
| `ZMK_SPLIT_CONN_LADDER_STEPS_SUPERVISION_TIMEOUT` | int | 800 | 100–3200 | supervision timeout of every computed step, 10 ms units |
| `ZMK_SPLIT_CONN_LADDER_ACTIVE_SUPERVISION_TIMEOUT` | int | 400 | 100–3200 | ACTIVE's supervision timeout, 10 ms units; ACTIVE's latency is lowered to keep `MIN_SUPERVISION_WINDOWS` in it |
| `ZMK_SPLIT_CONN_LADDER_USB_POWER_PERIPHERAL_SYNC_DELAY_MS` | int | 625 | 0–10000 | the deepest USB power step's sync delay; 0 keeps the USB power steps at ACTIVE |
| `ZMK_SPLIT_CONN_LADDER_STEP1_AFTER_IDLE_MS` | int | 0 | 0–86400000 | delay after entering idle; steps 2–6: 180000, 600000, 1200000, 1800000, 2400000 |
| `ZMK_SPLIT_CONN_LADDER_STEP1_INTERVAL` | int | computed: 0; table: 25 | computed: 0–3200; table: 6–3200 | interval, 1.25 ms units |
| `ZMK_SPLIT_CONN_LADDER_STEP1_LATENCY` | int | computed: −1; table: 13 | computed: −1 to 499; table: 0–499 | peripheral latency, events |
| `ZMK_SPLIT_CONN_LADDER_STEP1_SUPERVISION_TIMEOUT` | int | computed: 0; table: 800 | computed: 0–3200; table: 10–3200 | supervision timeout, 10 ms units |
| `ZMK_SPLIT_CONN_LADDER_STEP1_USB_POWER_LATENCY` | int | computed: −1; table: 29 | computed: −1 to 499; table: 0–499 | the same step on USB power: latency |
| `ZMK_SPLIT_CONN_LADDER_STEP1_USB_POWER_SUPERVISION_TIMEOUT` | int | computed: 0; table: 800 | computed: 0–3200; table: 10–3200 | … supervision timeout |
| `ZMK_SPLIT_CONN_LADDER_STEP1_USB_POWER_INTERVAL` | int | `ZMK_SPLIT_BLE_PREF_INT` | 6–3200 | … interval; table only |
| `ZMK_SPLIT_CONN_LADDER_BOOT_IN_AUTO_MODE` | bool | **y** | — | y: boot in AUTO; n: boot in ON |
| `ZMK_SPLIT_CONN_LADDER_USB_POWER_STEPS` | bool | **y** | — | AUTO on USB power runs the USB power steps; n: holds ACTIVE |
| `ZMK_SPLIT_CONN_LADDER_MIN_SUPERVISION_WINDOWS` | int | 6 | 2–40 | floor on supervision windows: every step at build time, and the computed ones as they are worked out |
| `ZMK_SPLIT_CONN_LADDER_MAX_RETURN_MS` | int | 4000 | 500–20000 | build-time cap on leaving any step |
| `ZMK_SPLIT_CONN_LADDER_NEW_LINK_SETTLE_MS` | int | 3000 | 0–30000 | no step on a fresh link while ZMK sets it up, whatever wakes the ladder |
| `ZMK_SPLIT_CONN_LADDER_REJECTED_UPDATE_RETRY_MS` | int | 500 | 50–5000 | first retry after a refused update; doubles per refusal in a row, up to `UNCONFIRMED_UPDATE_TIMEOUT_MS` |
| `ZMK_SPLIT_CONN_LADDER_UNCONFIRMED_UPDATE_TIMEOUT_MS` | int | 15000 | 1000–60000 | how long an unconfirmed update blocks a deeper step; must exceed `MAX_RETURN_MS` |
| `ZMK_SPLIT_CONN_LADDER_BACKOFF_AFTER_LINK_LOSS` | bool | **y** | — | cap depth after a link loss |
| `ZMK_SPLIT_CONN_LADDER_BACKOFF_RESTORE_AFTER_MS` | int | 900000 | 60000–21600000 | connected time that restores one notch; doubles per failure up to 32× |
| `ZMK_SPLIT_CONN_LADDER_BACKOFF_RECONNECT_WINDOW_MS` | int | 60000 | 5000–600000 | a return inside this counts the loss against the ladder |
| `ZMK_SPLIT_CONN_LADDER_LOG_RSSI` | bool | n | — | log split RSSI; also needs `CONFIG_BT_CTLR_CONN_RSSI=y` |
| `ZMK_SPLIT_CONN_LADDER_WORK_QUEUE_STACK_SIZE` | int | 1280, 2048 with `LOG` | — | the ladder's own work queue; the build stops below those |
| `ZMK_SPLIT_CONN_LADDER_WORK_QUEUE_PRIORITY` | int | 10 | — | preemptible, like ZMK's low-priority queue |
| `ZMK_SPLIT_CONN_LADDER_LOG_LEVEL_{OFF,ERR,WRN,INF,DBG}` | choice | `_DEFAULT`: `LOG_DEFAULT_LEVEL`, INF | — | set one of them `=y`; the central half's search and sleep log here too |
| `ZMK_SPLIT_CONN_LADDER_LOW_POWER_PERIPHERAL_SEARCH` | bool | **y** | — | the low-power search; central half |
| `ZMK_SPLIT_CONN_LADDER_MIDDLE_PERIPHERAL_SEARCH_INTERVAL_MS` | int | 240 | 0, 60–1280 | the middle search in the first W while the keys go to a Bluetooth host: 30 ms every this many ms; 0: ZMK's full search instead |
| `ZMK_SPLIT_CONN_LADDER_EARLY_SLEEP` | bool | **y**; n with `ZMK_USB_LOGGING` | — | the early sleep; both halves (the central only with the ladder) |
| `ZMK_SPLIT_CONN_LADDER_ABSENCE_WAIT_MS` | int | 0: auto | 0–3600000 | W: how long a half goes without what it needs before the early sleep, and before the low-power search; auto is 35 s on a central half, 30 s on a peripheral one with nothing to save (35 s with underglow, backlight or `ZMK_EXT_POWER`) |
| `ZMK_SPLIT_CONN_LADDER_EARLY_SLEEP_BLE_PAIRING_WAIT_MS` | int | 180000 | 10000–3600000 | P: the central's early sleep while the active BLE profile waits for a computer to pair |
| `ZMK_SPLIT_CONN_LADDER_EARLY_SLEEP_WITHOUT_PERIPHERAL` | bool | n | — | the central sleeps early without the other half too, even with a computer, unless on USB power |
| `ZMK_SPLIT_CONN_LADDER_TEST_SLEEP_AND_SEARCH_IGNORE_USB` | bool | n | — | test builds only: the central's early sleep and low-power search act as on battery with no USB host, so they can be watched over a USB log |
| `ZMK_SPLIT_CONN_LADDER_EARLY_SLEEP_LOG_LEVEL_{OFF,…,DBG}` | choice | as above | — | the peripheral half's early sleep |
| `ZMK_SPLIT_CONN_LADDER_PERIPHERAL_USB_POWER_REPORT` | bool | **y** | — | peripheral half: report its USB power to the ladder |
| `ZMK_SPLIT_CONN_LADDER_PERIPHERAL_USB_POWER_REPORT_LOG_LEVEL_{OFF,…,DBG}` | choice | as above | — | as above, on that half |
| `ZMK_SPLIT_CONN_LADDER_PERIPHERAL_CLOSE_LINK_BEFORE_SLEEP` | bool | **y** | — | peripheral half: close the split link on the `SLEEP` event, before the power goes off |
| `ZMK_SPLIT_CONN_LADDER_PERIPHERAL_CLOSE_LINK_BEFORE_SLEEP_LOG_LEVEL_{OFF,…,DBG}` | choice | as above | — | as above, on that half |
| `ZMK_USB_RESEND_KEYS_AFTER_WAKE` | bool | n | — | the wake-key re-send; independent of the ladder |
| `ZMK_USB_RESEND_KEYS_AFTER_WAKE_DELAY_MS` | int | 10 | 0–500 | resume → first write |
| `ZMK_USB_RESEND_KEYS_AFTER_WAKE_GIVE_UP_MS` | int | 30000 | 1000–120000 | how long it polls for the host; after that it only re-sends on a key change |
| `ZMK_USB_RESEND_KEYS_AFTER_WAKE_LOG_LEVEL_{OFF,…,DBG}` | choice | as above | — | as above |

Set elsewhere, but part of the picture:

| symbol | default | note |
|---|---|---|
| `CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS` | 1 | must stay 1; enforced at build time |
| `CONFIG_ZMK_SPLIT_BLE_PREF_INT` / `_LATENCY` / `_TIMEOUT` | 6 / 30 / 400 | computed mode: what the link is created on; leave ZMK's defaults. Table mode: ACTIVE |
| `CONFIG_ZMK_IDLE_TIMEOUT` | 30000 | when the step timers and the early sleep's wait start counting |
| `CONFIG_ZMK_IDLE_SLEEP_TIMEOUT` | 900000 | ZMK's own sleep, on battery only; see "Step 4 instead of sleep" |
| `CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE` | 60000 | sets the floor under the early sleep's wait |
| `CONFIG_ZMK_BEHAVIOR_SPLIT_CONN_LADDER` | auto | turns itself on when the keymap uses `&ladder`; **never assign it** — it has no prompt, and Zephyr aborts on that |

## The ladder

### What the defaults do

A central half with a trackball (`ZMK_POINTING`, so
`ACTIVE_AVOIDS_HOST_INTERVAL`) whose other half shows the layer colour it is
sent (`RGBLED_WIDGET_LAYER_PUSH`, so `PERIPHERAL_SHOWS_INDICATORS`) — the Ergohaven K:04 Micro with zmk-rgbled-widget — gets, with no
line but `CONFIG_ZMK_SPLIT_CONN_LADDER=y`:

| | after idle | Mac (host link 15 ms) | PC (7.5 ms), or no computer | other half on air every | return | central half | other half |
|---|---|---|---|---|---|---|---|
| ACTIVE | — | 12.5 ms, latency 12 | 16.25 ms, latency 9 | 163 ms | 550 / 569 ms | ~900 / ~700 µA | ~58 µA |
| step 1 | 0 min | 31.25 ms, latency 13 | same | 438 ms | 1469 ms | ~380 µA | ~29 µA |
| step 2 | 3 min | 83.75 ms, latency 3 | same | 335 ms | 1424 ms | ~160 µA | ~34 µA |
| step 3 | 10 min | 136.25 ms, latency 1 | same | 273 ms | 1499 ms | ~110 µA | ~40 µA |
| step 4 | 20 min | 398.75 ms, latency 0 | same | 399 ms | 3190 ms | ~59 µA | ~31 µA |

"After idle" counts from entry into idle, 30 s after the last key. "Return"
is how long coming back to ACTIVE takes from that step — for ACTIVE, how long
leaving it for step 1 takes — by the model in "How long a switch between steps
takes"; for all that time the other half's keys still wait up to a full step
interval. Currents are modelled, not measured: the central half as
10800 µA·ms / interval for the split link plus ~32 µA for everything else
awake on it, the other half as ~12 µA plus 7466 µA·ms / its attendance. Treat
the ratios as solid and the absolute numbers as ±50 %.

On USB power, in AUTO, the same timers run the USB power steps: ACTIVE's
interval, which delays no key, and a latency that saves the other half's
battery:

| on USB power | step 1 | step 2 | step 3 | step 4 |
|---|---|---|---|---|
| Mac: latency, other half on air every | 21, 275 ms | 30, 388 ms | 39, 500 ms | 49, 625 ms |
| PC or no computer | 16, 276 ms | 23, 390 ms | 30, 504 ms | 37, 618 ms |

With the output on USB, ACTIVE is 8.75 ms at latency 18 (166 ms). When the
other half reports USB power, ACTIVE's latency is 0 — it has nothing to save —
and with both halves on USB power the ladder holds ACTIVE.

A keyboard whose other half shows nothing it is sent
(`PERIPHERAL_SHOWS_INDICATORS` off) gets the
same steps and an ACTIVE latency that makes the other half's attendance about
400 ms instead: latency 31 against a Mac, 24 against a PC or no computer, 45
with the output on USB.

The log prints the whole table whenever it is worked out:

```
steps computed for a host link at 15.00 ms: ACTIVE 12.50 ms, latency 12, timeout 4000 ms (below the host's, not dividing it; other half on air every 163 ms)
  step 1 after 0 s: 31.25 ms, latency 13, timeout 8000 ms (return ~1469 of 1500 ms, on air every 438 ms)
  step 2 after 180 s: 83.75 ms, latency 3, timeout 8000 ms (return ~1424 of 1500 ms, on air every 335 ms)
  step 3 after 600 s: 136.25 ms, latency 1, timeout 8000 ms (return ~1499 of 1500 ms, on air every 273 ms)
  step 4 after 1200 s: 398.75 ms, latency 0, timeout 8000 ms (return ~3190 of 3200 ms, on air every 399 ms)
  step 1 on USB power: latency 21, timeout 8000 ms (return ~888 ms, on air every 275 ms)
  step 2 on USB power: latency 30, timeout 8000 ms (return ~1225 ms, on air every 388 ms)
  step 3 on USB power: latency 39, timeout 8000 ms (return ~1563 ms, on air every 500 ms)
  step 4 on USB power: latency 49, timeout 8000 ms (return ~1938 ms, on air every 625 ms)
```

### ACTIVE

The interval while typing:

* **output on USB**: 8.75 ms. The central half is on a cable then, so its own
  cost does not matter, and there is no host link on the radio to keep out of
  the way of;
* **a pointer and a Bluetooth host** (`ACTIVE_AVOIDS_HOST_INTERVAL`, on with
  `ZMK_POINTING`): an interval chosen against the host link's, by the rules of
  "The interval and the computer's own link" — the longest from 12.5 to 20 ms
  that is shorter than the host's, does not divide it and shares at least
  2.5 ms with it, so that an overlap never repeats and the host wins every one;
  where there is none, the longest up to 20 ms above the host's that shares no
  factor with it;
* **otherwise** — no pointer, no Bluetooth host, nothing that fits:
  `ACTIVE_DEFAULT_INTERVAL`, 16.25 ms, which shares no factor with a PC's 7.5 ms or
  a Mac's 15 ms.

| host link | ACTIVE, `PERIPHERAL_SHOWS_INDICATORS` | … off | |
|---|---|---|---|
| none | 16.25 ms, latency 9 | latency 24 | `ACTIVE_DEFAULT_INTERVAL` |
| 7.5 ms (a PC) | 16.25 ms, latency 9 | latency 24 | above, no common factor: nothing from 12.5 ms fits below |
| 10 ms | 18.75 ms, latency 8 | latency 20 | above, no common factor |
| 11.25 ms | 20 ms, latency 7 | latency 19 | above, no common factor |
| 15 ms (a Mac) | 12.5 ms, latency 12 | latency 31 | below, sharing 2.5 ms |
| 20 ms | 17.5 ms, latency 8 | latency 22 | below, sharing 2.5 ms |
| 30, 35, 37.5, 45, 50 ms | 20 ms, latency 7 | latency 19 | below, capped at 20 ms |
| 31.25, 33.75, 48.75 (Android), 60 ms | 18.75 ms, latency 8 | latency 20 | below |
| 40, 80 ms | 17.5 ms, latency 8 | latency 22 | below |
| 36.25, 38.75, 46.25 ms | 16.25 ms, latency 9 | latency 24 | nothing fits: `ACTIVE_DEFAULT_INTERVAL` |
| output on USB | 8.75 ms, latency 18 | latency 45 | |

The latency is the other half's business: it makes that half's attendance —
how long something sent to it, an LED or layer push, may wait while typing —
come nearest `ACTIVE_PERIPHERAL_SYNC_DELAY_MS`. A key from the other half is not
delayed by latency at all: a peripheral with something to send transmits at
the next connection event whatever its latency.

Without a pointer the collisions with the host link only cost a key a few
milliseconds now and then, so ACTIVE stays `ACTIVE_DEFAULT_INTERVAL`;
`ACTIVE_AVOIDS_HOST_INTERVAL=n` is also the way to keep it there on a Mac. ACTIVE is never
longer than 20 ms, the delay the other half's keys can take, and the steps are
always longer than that.

Its timeout is `ACTIVE_SUPERVISION_TIMEOUT`, 4 s — ZMK's own default — and
shorter than the steps' 8 s on purpose. ACTIVE's attendance is short (163 ms,
or about 400 ms without `PERIPHERAL_SHOWS_INDICATORS`), so 4 s still holds
about 10 to 24 of them, and a latency that would leave fewer than
`MIN_SUPERVISION_WINDOWS` is lowered. What the shorter timeout buys: until the
link is declared lost, this half takes the other one for connected, and
whatever it sends that way — a layer colour for the other half's LED — waits
in the Bluetooth buffers it shares with the reports to the computer. A
trackball on this half stalls for as long; see "Closing the link before
sleep".

A fresh split link starts on `ZMK_SPLIT_BLE_PREF_*`, which is what ZMK creates
it with, and moves to ACTIVE after `NEW_LINK_SETTLE_MS`, like onto any step.

Not modelled: a trackball on the *peripheral* half. Its motion then crosses
the split link as well, and above a PC's 7.5 ms a shorter ACTIVE might serve it
better. The rules above protect the host link, which carries the motion either
way.

### The steps

The central half pays per connection event, so the interval is the saving,
and the latency sets what leaving a step costs. Each step takes its latency
from a fixed series and gets the **longest interval that returns to ACTIVE
within its budget** at that latency — `RETURN_BUDGET_MS`, 1.5 s, or
`DEEPEST_STEP_RETURN_BUDGET_MS`, 3.2 s, for the deepest — that also keeps
`MIN_SUPERVISION_WINDOWS` attendances inside its timeout, passes the Bluetooth rule, and
shares no factor with 2, 3 or the host link's interval. The series falls as
the steps deepen, so every step saves more than the one above and none is
slower to leave; the deepest gets the longer budget because it stands in for
sleep ("Step 4 instead of sleep").

| steps | latencies, `DEEPEST_STEP_RETURN_BUDGET_MS` set (default) | `DEEPEST_STEP_RETURN_BUDGET_MS=0` |
|---|---|---|
| 1 | 0 | 13 |
| 2 | 13, 0 | 13, 3 |
| 3 | 13, 3, 0 | 13, 3, 1 |
| 4 | 13, 3, 1, 0 | 13, 3, 1, 0 |
| 5 | 13, 3, 1, 0, 0 | 13, 5, 3, 1, 0 |
| 6 | 13, 5, 3, 1, 0, 0 | 13, 7, 5, 3, 1, 0 |

With a deepest budget of its own the deepest step is latency 0 and the others
take the series of one step fewer; with none, every step shares one series.
The first latency, 13, keeps step 1 at 31.25 ms within 1.5 s; each further one
roughly doubles the interval the budget allows. What that gives, with no host
link in the way (return in brackets):

| steps | `DEEPEST_STEP_RETURN_BUDGET_MS=3200` | `DEEPEST_STEP_RETURN_BUDGET_MS=0` |
|---|---|---|
| 1 | 398.75/0 (3190) | 31.25/13 (1469) |
| 2 | 31.25/13, 398.75/0 | 31.25/13, 83.75/3 |
| 3 | 31.25/13, 83.75/3, 398.75/0 | 31.25/13, 83.75/3, 136.25/1 |
| 4 | 31.25/13, 83.75/3, 136.25/1, 398.75/0 | …, 186.25/0 (1490) |
| 5 | 31.25/13, 83.75/3, 136.25/1, 186.25/0, 398.75/0 | 31.25/13, 61.25/5, 83.75/3, 136.25/1, 186.25/0 |
| 6 | 31.25/13, 61.25/5, 83.75/3, 136.25/1, 186.25/0, 398.75/0 | 31.25/13, 51.25/7, 61.25/5, 83.75/3, 136.25/1, 186.25/0 |

Even at latency 0 a return takes 8 events, so a budget caps the depth: 1.5 s
allows 186.25 ms at most, 3.2 s allows 398.75 ms. The other way round, a
`RETURN_BUDGET_MS` below about 1 s leaves step 1, at latency 13, nothing
longer than 20 ms, and the build asks for a lower `STEP1_LATENCY`. The timers of steps 5 and 6
default to 30 and 40 minutes of idle.

These intervals — 25, 67, 109, 319 units and the rest — share no factor with 2
or 3, so a Mac's 12-unit and a PC's 6-unit host link leave them where they
are. A computer on another interval can move a step to a shorter one; should
that leave a step nothing longer than the step above, it repeats that step
(`as the step above -- the host link's interval leaves nothing longer` in the
table, `parameters already in place` when the ladder reaches it).

### The USB power steps

On USB power the central half's own cost does not matter, so the interval —
the only thing that saves the central anything — has no reason to grow. The
USB power steps keep ACTIVE's interval and raise the peripheral latency, which is
the only thing that saves the peripheral anything.

That costs no key. A peripheral with something to send transmits at the very
next connection event whatever its latency (the controller cancels latency the
moment data is queued), so a key on the other half still waits at most one
interval. Not even the return costs anything: the interval does not change, so
there is nothing to wait out. What waits is traffic **to** the peripheral —
the LED/indicator sync — up to `interval * (latency + 1)`.

So that is what the USB power steps are built from: the latency rises in
equal steps from ACTIVE's to the one whose attendance is
`USB_POWER_PERIPHERAL_SYNC_DELAY_MS` (625 ms), reached at the deepest step.
The return budgets do not apply to a USB power step; `MIN_SUPERVISION_WINDOWS`
and `MAX_RETURN_MS` do. Past about half a second, each doubling of the delay
saves the other half only a few microamps more.
`USB_POWER_PERIPHERAL_SYNC_DELAY_MS=0` keeps every USB power step at ACTIVE.

### When it recomputes

Once when the ladder first runs; then after every change of the host link —
up, down, new parameters — of the output (USB or Bluetooth, another profile),
and of the other half's USB power. New parameters on a settled link, a move of
the output and the other half's power are followed after 0.5 s, which folds
one burst of reports into a single recompute. A fresh host link is given 6 s
from its connect instead — a computer adjusts its link in the first seconds,
and ZMK asks for its own parameters 5 s after the connect — so its transients
never cost a step change; a lost one gets 6 s to come back. Each report
restarts the wait. Never under an update in flight; that one lands first.

If the step the ladder stands on got new parameters, it is applied again — one
procedure (`step N has new parameters -- re-applying`). On a Mac the battery
steps stay put when its link comes and goes; ACTIVE moves between 16.25 and
12.5 ms, and the USB power steps with it, since they run ACTIVE's interval. On
a PC nothing moves at all.

A recompute that changes nothing prints the table all the same, its first
line ending in `steps unchanged`: a log attached after boot — plugging its
cable in moves the output to USB, which recomputes — shows it right away.

### Setting steps by hand

In the computed mode the per-step symbols default to "computed" —
`STEP*_INTERVAL` and `STEP*_SUPERVISION_TIMEOUT` to 0, `STEP*_LATENCY` to −1,
and for USB power `STEP*_USB_POWER_LATENCY` to −1 and
`STEP*_USB_POWER_SUPERVISION_TIMEOUT` to 0 — and a value the `.conf` gives one
of them sets that one parameter of that step. Everything else is
still computed.

| set in the `.conf` | that step |
|---|---|
| `STEP*_LATENCY` | runs this latency instead of the series one, with the longest interval that returns within its budget at it |
| `STEP*_INTERVAL` | runs exactly this interval, at `STEP*_LATENCY` or the series latency |
| `STEP*_SUPERVISION_TIMEOUT` | this timeout instead of `STEPS_SUPERVISION_TIMEOUT`; for a computed step it also bounds the interval through `MIN_SUPERVISION_WINDOWS` |
| `STEP*_USB_POWER_LATENCY` | this latency on USB power, on ACTIVE's interval; the computed USB power steps lie on straight lines between ACTIVE's latency, the USB power steps set, and the `USB_POWER_PERIPHERAL_SYNC_DELAY_MS` ceiling at the deepest step unless that one is set |
| `STEP*_USB_POWER_SUPERVISION_TIMEOUT` | this timeout on USB power |

The build holds every step to the same rules as a computed one and stops,
with a message naming the step, when a step set by hand:

* returns over its budget at its latency (`STEP*_INTERVAL` at 1.5 s, or 3.2 s
  for the deepest);
* breaks `MIN_SUPERVISION_WINDOWS` or the Bluetooth rule on its timeout;
* has an interval under 17 (20 ms is the longest ACTIVE), or a latency so
  high that nothing longer than 20 ms returns within its budget (above 20 at
  1.5 s, above 47 at 3.2 s);
* is not deeper than the step above: its interval must be longer, unless the
  whole step is the same.

A step that repeats the one above exactly only draws a warning. The order of
the budgets is checked too: `RETURN_BUDGET_MS ≤ DEEPEST_STEP_RETURN_BUDGET_MS ≤
MAX_RETURN_MS` (`DEEPEST_STEP_RETURN_BUDGET_MS=0` aside), and
`ACTIVE_PERIPHERAL_SYNC_DELAY_MS` not above
`USB_POWER_PERIPHERAL_SYNC_DELAY_MS`.

A set interval is taken as it is — not moved off the host's multiples, so the
log warns when it lands on one. Should the host's interval leave a computed
step no longer than a set step above it, the computed one repeats it, as
above. A USB power latency is checked at build time on
`ACTIVE_DEFAULT_INTERVAL`; where the computed ACTIVE is longer it is lowered to
what the guards allow there, and the log says so. A USB power step at or below
ACTIVE's latency is ACTIVE. `STEP*_USB_POWER_INTERVAL` is not used in this
mode: the USB power steps run ACTIVE's interval.

In the log a set interval is marked `(set)` after it, a set latency `(set)`
after the latency, and a USB power latency that could not be used as set says why:
`(set higher: the guards cap it here)` or `(set below ACTIVE's: runs ACTIVE)`.

Lines written for the table mode are not needed here and can stop the build:
`STEP1_INTERVAL=25` with `STEP1_LATENCY=14`, for one, returns in 1.56 s — over
the 1.5 s budget. Remove them; the computation gives the table's steps or
deeper ones.

### The table

`COMPUTED_STEPS=n` takes the steps from Kconfig instead: `STEP*_INTERVAL`,
`STEP*_LATENCY` and `STEP*_SUPERVISION_TIMEOUT` for the battery steps,
`STEP*_USB_POWER_INTERVAL`, `_LATENCY` and `_SUPERVISION_TIMEOUT` for the USB
power ones, and `ZMK_SPLIT_BLE_PREF_*` as ACTIVE. Nothing is worked out
again for the host link. The defaults are what the computation gives with no
computer in the way — 25/13, 67/3, 109/1 and 319/0, steps 5 and 6 repeating
step 4 — and the USB power steps run `ZMK_SPLIT_BLE_PREF_INT` at latency 29
(steps 1 and 2) and 49 (steps 3 to 6). Every step is checked at build time
against the Bluetooth rule, `MIN_SUPERVISION_WINDOWS` and `MAX_RETURN_MS`, and
the timers must increase.

The table's USB power latencies suit an ACTIVE of 12.5 ms at latency 12
(`ZMK_SPLIT_BLE_PREF_INT=10`, `_LATENCY=12`, `_TIMEOUT=600`). A USB power step
saves the other half something only above ACTIVE's latency: with ZMK's own
`PREF_LATENCY` of 30, steps 1 and 2 (latency 29) are shallower than ACTIVE and
cost the other half more. With the table, set `ZMK_SPLIT_BLE_PREF_*` as above,
or `STEP1_USB_POWER_LATENCY` and `STEP2_USB_POWER_LATENCY` above
`PREF_LATENCY`.

## On USB power: AUTO, ON, OFF and the `&ladder` key

| mode | this half on battery | this half on USB power |
|---|---|---|
| **AUTO** (default) | battery steps | USB power steps; ACTIVE if the other half is on USB power too |
| **ON** | battery steps | battery steps |
| **OFF** | held at ACTIVE | held at ACTIVE |

With `USB_POWER_STEPS=n`, AUTO on USB power is held at ACTIVE.

"USB power" is the same test that keeps ZMK itself out of deep sleep: a host,
awake or asleep, or a bare charger. A host that cuts VBUS while it sleeps
reads as absent, and the half counts as on battery again.

Plugging in keeps the depth: battery step 3 becomes USB power step 3, one
parameter update, no detour through ACTIVE. Pulling the cable out returns to
ACTIVE and restarts the idle clock — the ladder starts again from the top
rather than dropping at once to the depth the idle time would allow, because
that is usually the moment somebody picks the keyboard up. Releasing a hold
onto the battery steps (with the key) does the same.

The keyboard boots into AUTO. `BOOT_IN_AUTO_MODE=n` makes it boot into ON
instead. Whatever the key sets lives in RAM only: every
reboot starts from the Kconfig choice again.

### The other half's USB power

With both halves on USB power nothing needs saving anywhere, and the ladder
holds ACTIVE; with only the other half on USB power, ACTIVE's latency is 0.
The central learns about the other half from a small report that half
publishes — `PERIPHERAL_USB_POWER_REPORT`, on by default on a peripheral
half:

* that half reads the nRF52840's own VBUS detector once a second — the POWER
  peripheral reports VBUS whether or not a USB stack is built in — and keeps
  the result, with its own uptime, in a small GATT characteristic of its own
  service (the uptime is what lets backoff tell its sleep from a lost link,
  see Backoff);
* the central finds it two seconds after every split connection (after ZMK's
  own discovery has had its head start), subscribes to changes and reads the
  current value.

On the central the log says `other half reports USB power present/absent`
once per connection, then `other half: USB power ...` on every change; on the
other half, `USB power present/absent`. A half built without the report is
logged once per connection as `other half does not report its USB power (no
power service) -- it counts as on battery`, and nothing else changes. Until
the report arrives, and whenever the link is down, the other half counts as on
battery.

### The key

In the **central half's** keymap:

```dts
#include <behaviors/split_conn_ladder.dtsi>
#include <dt-bindings/zmk/split_conn_ladder.h>

/* ... on some layer ... */
&ladder LADDER_TOG
```

| binding | does |
|---|---|
| `&ladder LADDER_TOG` | ON ↔ AUTO (from OFF: ON) |
| `&ladder LADDER_CYCLE` | AUTO → ON → OFF → AUTO |
| `&ladder LADDER_AUTO` | AUTO |
| `&ladder LADDER_ON` | ON |
| `&ladder LADDER_OFF` | OFF |

Every mode is reachable from the keyboard: one key with `LADDER_CYCLE` walks
through all three, or give each mode its own key. `LADDER_TOG` is the
two-state shortcut for the pair you switch between while testing.

The key can sit on either half. The behavior has central locality: a key on
the peripheral half is resolved through the central's keymap anyway, so
nothing crosses the split link for it. `ZMK_BEHAVIOR_SPLIT_CONN_LADDER` turns
itself on when the keymap references `&ladder`; there is nothing to set. The
node is `/omit-if-no-ref/`, so including the `.dtsi` costs nothing without
the key.

The behavior carries ZMK Studio metadata — a display name and the five
parameter values with their own names — so once it exists it can be assigned
from Studio. Two Studio caveats, both of which look like "the key does
nothing":

* **It only exists if the keymap references it.** The node is
  `/omit-if-no-ref/` and `ZMK_BEHAVIOR_SPLIT_CONN_LADDER` follows
  `dt_compat_enabled`, so a build whose `.keymap` never writes `&ladder` does
  not compile the behavior in and Studio will not list it. Put one `&ladder`
  binding in the `.keymap` first; after that Studio can move it anywhere.
* **A keymap saved from Studio lives in flash** and overrides the `.keymap`
  file for every key it changed. If a freshly added `&ladder` key does
  nothing, either assign it in Studio too or restore the stock keymap there.

### What it looks like in the log

```
mode AUTO -> ON
ladder on battery steps (mode ON, USB power present)
...
mode ON -> AUTO
ladder on USB power steps (mode AUTO, USB power present)
step 2/4 on USB power: interval 12.50 ms, latency 30, timeout 8000 ms (... switch from level 2 ~1424 ms)
...
other half: USB power present
ladder held at ACTIVE (mode AUTO, USB power present on both halves)
step 0/4: interval 12.50 ms, latency 0, timeout 4000 ms (... switch from level 2 on USB power ~1225 ms)
```

(`0/4` is `STEP_COUNT=4`: step 0 of 4, i.e. ACTIVE.)

The gate line appears once per change, not on every USB event.

A trick for battery runs, where there is no log: unplug, wait, plug back in.
The `step K/N on USB power: ... switch from level K ...` line that follows
(`step 0/N` with `USB_POWER_STEPS=n`) says how deep the ladder had gone while
you were away.

## The low-power search

ZMK looks for a missing other half with a passive scan of 30 ms every 60 ms —
half the radio's time, about 2.5 mA — for as long as that half is missing: all
night, when it is asleep or switched off. `LOW_POWER_PERIPHERAL_SEARCH`, on by
default with the ladder, searches in stages from when the other half goes, or
from boot:

| | keys on a USB host | keys on a Bluetooth host | no host |
|---|---|---|---|
| first `ABSENCE_WAIT_MS` (35 s) | ZMK's full search | the middle search: 30 ms every 240 ms | ZMK's full search |
| after that | ZMK's full search | the low-power search: 30 ms every 1.28 s | the low-power search |

For its own two scans the module switches ZMK's Bluetooth split transport off
through ZMK's own API — which only stops its scan, since nothing is connected
— and listens itself. The low-power scan draws about 0.1 mA.

**Why the middle search.** ZMK's full search takes half the radio, and a link
to a computer over Bluetooth loses its events to it: a trackball on that link
stutters for as long as the search runs. The middle search takes an eighth,
and the search draws about 0.6 mA instead of 2.5 mA. It cannot miss the other
half, only hear it a little later: a half that has just woken or lost the link
calls at full rate for 1.28 s, which the first window catches — within 240 ms;
after that it calls every 100–150 ms, which a window catches about one time in
four, about a second on average. `MIDDLE_PERIPHERAL_SEARCH_INTERVAL_MS` sets the
interval; 0 leaves ZMK's full search in its place.

**Why not everywhere.** With the keys on USB there is no host link to take
airtime from, and on a cable no battery to save: ZMK's full search stays on
for good, and finds the other half soonest. With no host at all there is
nothing to disturb either, and the early sleep takes the central half down
soon anyway. USB power alone changes nothing — on a charger, or on a computer's
cable with the keys on Bluetooth, the search goes as on battery. Moving the
keys to USB brings the full search back; moving them off again picks up where
the count stood.

What the module's scans listen for is the other half calling:

* **directed advertising**, which a ZMK peripheral sends only to the central
  it is paired with; the radio drops any addressed elsewhere. A half waking
  from sleep sends it at full rate for 1.28 s, and one window every 1.28 s
  lands in that, so a woken half is heard at once. After that it calls every
  100–150 ms, which a window catches about one time in four — a half that
  lost the link while awake is heard within a few seconds;
* **advertising for pairing** — undirected, with ZMK's split service — which a
  half sends while it has no pairing. It counts only from the half this one is
  paired with (a half that lost its own pairing keeps its address), or from
  any half when this one has none: exactly what ZMK itself would connect to.

Either one stops the module's scan and switches ZMK's transport back on, and
ZMK's full search finds the half and connects. With the keys on a Bluetooth
host that full search lasts 2 s — ZMK hears the next call within a fraction of
that; otherwise `ABSENCE_WAIT_MS`. A call that does not end in a connection
counts as a loss at that moment: the middle (or full) search for
`ABSENCE_WAIT_MS` from the call, then the low-power one.

When it matters: the central half keeps working with a computer while the
other half is away — asleep, switched off, out of range. The trackball is the
common case on the K:04 Micro: an hour of pointing without a key on the other
half, and that half's own sleep takes it away while the central goes on. With
no computer either, the early sleep takes the central half down soon anyway.

Why the search waits `ABSENCE_WAIT_MS`, the same W as the early sleep: the
other half, once it has lost this one, waits its own W before its early sleep.
So the full or the middle search runs for as long as that half is still awake
and calling, and the low-power one takes over when it has gone to sleep — from
then on it calls again only when a key wakes it, and that call is what the
low-power scan hears. With the automatic values the central's W, 35 s, is not
shorter than the other half's, 30 or 35 s.

In the log:

```
low-power search: other half away, keys on a Bluetooth host -- listening 30 ms every 240 ms
low-power search: other half away for 35 s -- listening 30 ms every 1.28 s
low-power search: the other half called -- full search on
```

`(no paired half: any may pair)` after either of the first two means this half
has no pairing with any other half. The third line names what ended the
module's scan: `the other half called`, `a half asks to pair`, `the other half
connected`, `output on USB`, `keys not on a Bluetooth host` (the host went during
the middle search, and ZMK's full search takes over for the rest of W).
`low-power search: switching the full search back on failed (N), retry K in M
ms` and `low-power search: scan start failed (N) -- full search again` are a
busy radio refusing a scan; both are retried, the first one as long as it
takes, the second after `ABSENCE_WAIT_MS` of the full search.

## The early sleep

`ZMK_SPLIT_CONN_LADDER_EARLY_SLEEP` puts a half with nothing to work for into
deep sleep soon after its last key, instead of leaving it to ZMK's own sleep —
which comes only after `ZMK_IDLE_SLEEP_TIMEOUT`, however pointless being awake
has been, and never while the half is on USB power. On by default on both
halves: with the ladder on the central half, on its own on a peripheral half.
It works with `ZMK_SLEEP` on or off, and `ZMK_IDLE_SLEEP_TIMEOUT` does not
affect it.

| half | sleeps early | on USB power too |
|---|---|---|
| central | with no computer: none connected on the active Bluetooth profile, and none that configured it over USB | yes |
| central, `EARLY_SLEEP_WITHOUT_PERIPHERAL=y` | also without the other half, while a computer is there — unless it is on USB power | — |
| peripheral | without the central half | yes |

**What counts as a computer.** A Bluetooth host counts when it is connected on
the active profile. A USB host counts once it has configured the keyboard,
and it keeps counting while it sleeps (the bus is suspended, not gone) — so a
keyboard on the cable of a sleeping computer stays awake, and its trackball can
wake the computer. A charger or a power bank never configures anything, so on
one the central half with no Bluetooth host goes to sleep like on its battery.
A computer that is shut down but keeps its port powered looks like one asleep;
the keyboard then stays awake there, on the cable's power.

**When.** After `ABSENCE_WAIT_MS` — W — of idle with a reason to sleep. Idle is
ZMK's: W counts from entry into idle (30 s after the last key), or from the
moment the reason arose, whichever is later, and starts again whenever a
computer, the other half, USB power or the active profile comes, goes or
changes. So a central half with no computer sleeps about 65 s after its last
key, a peripheral half without the central about 60 s after its own.

W is picked automatically (`ABSENCE_WAIT_MS=0`): the settings-save formula below,
or 30 s, whichever is longer — 35 s on the central half with ZMK's defaults,
30 s on a peripheral half with nothing to save, 35 s on one with underglow,
backlight or `ZMK_EXT_POWER` (on by default in ZMK; a board's own
configuration may turn it off). The formula keeps a sleep from
cutting off a settings write. ZMK writes a changed Bluetooth profile, output,
underglow, backlight or external power state `ZMK_SETTINGS_SAVE_DEBOUNCE`
(60 s) after the change, the change being a key press, and idle starts
`ZMK_IDLE_TIMEOUT` (30 s) after the last key, so

```
W >= ZMK_SETTINGS_SAVE_DEBOUNCE - ZMK_IDLE_TIMEOUT + 5 s = 35 s
```

It applies on the central half always (with `SETTINGS`), and on a peripheral
half with `ZMK_RGB_UNDERGLOW`, `ZMK_BACKLIGHT` or `ZMK_EXT_POWER`. A value set
by hand must be at least that and at least 10 s; the build stops otherwise.
Underglow that switches itself off (`ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE`,
`_AUTO_OFF_USB`) starts that write with no key at all, at the moment it
switches, so the sleep then also waits `DEBOUNCE + 5 s` after entering idle or
after a cable goes in or out.

One case the formula does not cover: on a peripheral half with underglow,
backlight or external power, a change made from the central's keys (a global
`&rgb_ug` or `&ext_power` binding) is not a key on that half, and its idle
may have started long before. If the central then disappears within about
25 s of the change, that half sleeps W after losing it — before its own
settings write — and the change is lost there.

While the active profile is open for pairing — no computer paired to it — the
keyboard is waiting for a computer to pair, and the central half waits
`EARLY_SLEEP_BLE_PAIRING_WAIT_MS` (P, 3 min) instead of W. A computer that
connects to pair starts the count again, though ZMK says nothing of it until
the pairing is done. P must not be below W or the formula.

The same W is how long the low-power search leaves the full (or the middle)
search on — on purpose; see "The low-power search".

**How.** The sleep is ZMK's, step for step: the `SLEEP` activity event first,
so that whatever acts on that event puts itself away — the LED widget goes
dark, a WS2812 keeps its colour through System OFF unless told otherwise;
code that instead asks ZMK's state sees `IDLE`, as below — then
`zmk_pm_suspend_devices()` and `sys_poweroff()`, all on the system work queue,
as ZMK's own. The reason is logged 100 ms before, while the log still has a
chance to get out. ZMK's own activity state stays `IDLE`; if suspending the
devices fails, the devices are resumed, the listeners are told `IDLE` again
and the sleep is retried 5 s later.

**Waking.** The same as from ZMK's sleep: a key, anything else the board
leaves armed as a wake-up source — the K:04 Micro's trackball sensor, whose
motion line stays armed and keeps sampling in its rest modes — or a cable
going in. The half boots; the key that woke it is not typed.

**With a USB log.** Off by default in a `ZMK_USB_LOGGING` build, so the log
does not end when the other half goes away; `=y` turns it on there too. On a
peripheral half that is all it takes: it has no computer to wait for, and it
sleeps on USB power as well. A central half on the cable of the computer that
takes its log never sleeps early, because that computer counts as one, and
with the keys going to it the low-power search keeps ZMK's full search on. For
watching those two, a test build adds `TEST_SLEEP_AND_SEARCH_IGNORE_USB=y`: the
early sleep and the low-power search then act as on battery with no USB host —
a USB host is neither a computer nor a reason to search at full rate, USB power
is no reason to stay awake, plugging does not restart the count — while a
Bluetooth host, the other half and every timer work as always. The middle
search follows the output as ZMK selected it: `&out OUT_BLE` with a Bluetooth
host connected shows the middle search, the keys on USB show the case with no
host. The log follows each scenario up to `early sleep in 100 ms: ...`; then
the half powers off, the log ends, and the trackball or a key wakes it with the
cable still in. The ladder itself is not affected. The build warns while the
option is on, and the log says so at boot; it has no place in a daily build.

**A pointer driver that pins its sensor awake on USB power** should let go of
it when no computer is there. A sensor keeps its mode through System OFF, so a
half that goes to sleep on a charger with its sensor pinned in its fastest
mode would keep drawing that current from the battery once the cable is out.
For a PMW3610 in RUN that is 0.6 mA against 7 µA in its deepest rest mode.

In the log, on the central half with the ladder's lines, on a peripheral half
under `zmk_split_conn_ladder_early_sleep`:

```
early sleep after 35 s of idle without a computer
...
early sleep in 100 ms: no computer, idle for 35 s
```

The reasons read `no computer`, `no other half`, `no central half`.
`early sleep (...): suspending the devices failed -- retrying in 5 s` is the
retry.

## Closing the link before sleep

ZMK's sleep powers a half off without a word: the split link is not closed,
it just stops. The central half learns of it only when its supervision timeout
runs out, and until then takes the other half for connected. Whatever it sends
that way meanwhile — a layer colour for the other half's LED, an indicator —
cannot be delivered and waits in the Bluetooth buffers the central half shares
with the reports to the computer. Once those are taken, the reports wait too:
a trackball on the central half stalls until the loss is declared.

`PERIPHERAL_CLOSE_LINK_BEFORE_SLEEP`, on by default on the peripheral half,
closes the link first. ZMK raises the `SLEEP` activity event before it
suspends the devices and powers off, all on the system work queue; the
module's listener closes the link there — "remote user terminated", which the
central sees at once as reason 0x13 — and waits for the close to complete.
The controller drops the half's peripheral latency for the procedure, so the
close goes out at the next connection event and is acknowledged at the one
after: a few tens of milliseconds at ACTIVE, under a second even at the
deepest computed step. The wait allows two intervals and 0.5 s, but never less
than 1.5 s, so a step set by hand to a long interval is covered too -- and never
more than the supervision timeout and 0.5 s: by then the controller has given
the link up anyway. That is a limit, not the time it takes.

A key pressed on that half during the close is lost: ZMK handles keys on the
same work queue the close holds, and powers off after it without looking
again. At ACTIVE the window is a few tens of milliseconds, at the deepest step
up to about 0.8 s. A key still held when the power goes off wakes the half as
usual.

Other threads run while the link closes. An LED driver that paints from a
thread of its own has to keep its LEDs dark once it has seen `SLEEP`, even if
it handled the event before this listener: a colour pushed from the central
meanwhile would light it again, and a WS2812 keeps its colour through System
OFF. The zmk-rgbled-widget fork used on the K:04 Micro does.

The central half then starts its search for the missing half at once (with the
keys on a Bluetooth host, the middle search — see "The low-power search"), and
the ladder does not judge the loss: a link the other half closed on purpose
says nothing about the step it ran on (see "Backoff").

It covers ZMK's sleep and the peripheral half's early sleep, though the early
sleep comes only when there is no link to close. A reset, a pulled battery or
a half carried out of range still end the link silently; there the central's
supervision timeout decides, which is why ACTIVE's is 4 s rather than the
steps' 8 s (see "ACTIVE").

The evidence is on the central:

```
split link closed by the other half at step 0 (reason 0x13: on purpose, as before its sleep) -- not a step failure
```

The peripheral half logs `going to sleep: link to the central closed in N ms`
(or `... not closed after N ms`, `... closing the link to the central failed
(N)`: it slept without the close, and the central falls back on its timeout)
under `zmk_split_conn_ladder_close_link`, but no log shows it: ZMK sleeps only
without USB power, and after the line nothing yields before the power goes off,
so a deferred log never gets it out. Only `CONFIG_LOG_MODE_IMMEDIATE` would.

If ZMK fails to suspend the devices, it leaves its state at `SLEEP` and tries
again a second later without raising the event again, so that attempt powers
off without the close; the link has been closed once already and ZMK has
restarted advertising, so the central may have reconnected meanwhile and then
falls back on its timeout. That is ZMK's own behaviour.

## Step 4 instead of sleep

With a computer there, nothing in this module puts the keyboard to sleep:
the early sleep is for a half with nothing to work for. What a keyboard with a
computer does while nobody types is the ladder, and its fourth step, 20 minutes
into idle, is where it waits: 398.75 ms at latency 0, about 59 µA on the
central half and 31 µA on the other by the model above, against roughly
700–900 µA and 58 µA at ACTIVE. It comes back in about 3.2 s. Waking from deep
sleep instead takes a reboot, a reconnect to the computer and to the other
half, and the key that woke it.

What sleeping would save is the difference to what a sleeping half draws,
which depends on the board: the chip in System OFF draws well under a
microamp, the K:04 Micro's trackball sensor in its deepest rest mode about
7 µA, plus whatever else the board keeps powered. So every hour at step 4
costs at most about 0.06 mAh on the central half and 0.03 mAh on the other —
under 1 mAh for a 16-hour night.

On battery ZMK's own sleep, `ZMK_IDLE_SLEEP_TIMEOUT`, is the backstop; the
K:04 example sets an hour. Longer is a matter of taste at those figures;
shorter than 20.5 minutes (30 s from the last key to idle, then 20 min to
step 4) means step 4 never comes on battery. On USB power the question does not arise:
ZMK never sleeps there.

Judge step 4 by its return, not by the battery: come back after an hour and
start typing normally, with both hands. For those 3.2 s every key from the
peripheral half arrives up to 400 ms late — so it can land **after** a key the
central half typed later — and at 400 ms the peripheral can hand over only a
few key changes per connection event. Type faster than that and its 10-entry
queue fills; ZMK then discards the oldest changes (`Position state message
queue full` in the peripheral's own log). Touching the trackball first gives
the ladder a head start, since pointer motion counts as activity. If the first
words come out scrambled, `DEEPEST_STEP_RETURN_BUDGET_MS=0` gives the deepest
step the common 1.5 s budget: 186.25 ms at latency 0; with `STEP_COUNT=3` as
well the ladder ends at 136.25 ms.

Going deeper is refused at build time on purpose (`MAX_RETURN_MS`, 4 s). At
800 ms the return alone runs past six seconds, and it saves about 14 µA against
step 4 — a trade on the wrong side of every line.

## The key that wakes the computer (USB)

`CONFIG_ZMK_USB_RESEND_KEYS_AFTER_WAKE=y`, on the central half. Off by default and
independent of the ladder: without that line nothing in this section is
compiled in, and the keyboard behaves exactly as stock ZMK does.

Two things go wrong in stock ZMK when a key wakes a sleeping computer over USB.

**The waking key is thrown away.** While the bus is suspended, `usb_hid.c`
requests a remote wakeup and returns; the report itself is never written:

```
[35:40:39.689] zmk_split_conn_ladder: activity: ACTIVE
[35:40:39.690] <err> zmk: FAILED TO SEND OVER USB: -11
[35:40:39.695] <inf> usb_hid: Device resumed
[35:40:39.695] <inf> usb_hid: from suspend
```

`-11` is `-EAGAIN` from the nRF USB driver's own wakeup request; when that
request succeeds, ZMK logs nothing at all and drops the report just the same.
It is not the split link (the key arrived and was processed), and it is not
the host re-enumerating the keyboard (that would be a reset, not a resume).

**The wake takes longer than ZMK thinks.** The nRF driver reports the bus as
resumed the moment the *keyboard* starts signalling the wakeup — that is the
`Device resumed` line, 5 ms later. A computer in system sleep may not poll for
a second or more after that. ZMK's next report is parked in the USB endpoint,
and every report after it finds the endpoint busy and is dropped. When the
host finally polls, it reads whatever was parked — which can be a key-down
whose key-up was one of the dropped reports: a key that auto-repeats until the
next keystroke.

With the option on, every wake of the bus starts a short sync — one a key
caused, and one the trackball, a layer key or the host itself caused, since
the gap between "bus up" and "host reading" is the same whoever ended the
sleep. A HID keyboard report is a snapshot of what is held, not a keystroke —
the host builds key events from the differences between snapshots — so
sending a snapshot twice repeats nothing, and the sync works purely in
snapshots:

* **First, is anything owed?** Only after a wake that dropped a key report.
  The module keeps track of what the host will
  hold once it has read what is already in the endpoint. If the dropped
  report left the host believing something other than what is held now — a
  waking key still down, or a release the host never got — the current
  snapshot is written. If the host already holds the truth, nothing is
  written: an empty report parked for nothing would take the one slot the
  next real keystroke needs.
* **Then, watch.** Until the host is seen reading, every key change is written
  again after ZMK's own write, retrying while the endpoint is busy — 20 ms at
  first, doubling to 160 ms, under the quickest auto-repeat delay any host
  offers. Whatever ZMK parked in the endpoint is followed by the true state,
  so a key typed into the gap comes out once and does not stick.
* **"Seen reading"** means a write of ours went through after an earlier one
  from the same wake did. A write the host read before napping again proves
  nothing, so a new wake starts that evidence over.

Mouse reports carry movement and are never re-sent — and with them the
buttons: a trackball click whose release is refused behind a parked report
leaves the button held (a drag) until the next mouse report, and while the
sync retries it takes the one endpoint slot mouse reports need too. A
consumer key with a modifier (`LS(C_VOL_UP)`) re-sends the keyboard page too,
modifier first, as ZMK's `hid_listener` does. A key pressed while the host
resets the port on its wake is refused by ZMK as well (`-ENODEV`), and counts
as a dropped one.

```
USB suspended: the host is asleep
USB resumed after 42.7 s suspended -- a key change was dropped meanwhile
host resumed 15 ms after a dropped report -- re-sending, a key is still held and will be typed
host reading again 795 ms after the wake -- in sync; 1 key changes meanwhile, 8 writes refused
```

The first two lines come with every sleep and wake of the bus. `no key change
was dropped` is a wake by the trackball, by a layer key — which sends no report
at all — or by the host itself; the keys typed right after it are still
watched: the first of them is written again, and a `host reading` or `host
already reading` line follows. Plugging the cable in, or booting on one, is neither: until the
host configures the keyboard the bus idles, and the nRF controller reports that
idle as a suspend too (Zephyr passes on `SUSPEND`, `RESET`, `RESUME` ahead of
`CONFIGURED`). A suspend of a bus the host has not configured yet is not
counted, so a plug-in prints none of these lines. No `USB suspended` line at all means the host never suspended
the bus (a display that only went dark, for one), and a lost key then is not
the bus's doing.

Zephyr's own USB HID class is a second witness that does not depend on this
module: a logging build prints `usb_hid: Device suspended` and `usb_hid: Device
resumed` for every sleep and wake of the bus. If those are missing too, the
host did not sleep as far as USB goes — on a Mac, `pmset -g log | grep -E
"Entering Sleep|Wake from|Display is turned" | tail` shows whether it slept
or only turned the display off.

For a tap that was over before the host could read it:

```
host resumed 10 ms after a dropped report -- nothing to re-send (a tap that ended before the wake is lost)
```

The "in sync" line measures the host's own wake — how long it took to start
reading, and how many key changes happened in that time — when a write of the
module's was refused first (`N writes refused`). `host already reading at our
first write, N ms after the wake` means none was: the host was reading by the
time the module first had something to write, which after a wake that dropped
nothing is the first key change, and N is when that came, not how long the host
took. Changes that began
and ended inside that gap can reach the host only as the final state — one
report fits in the endpoint and ZMK's other writes are gone — so fast typing
into it loses keystrokes whatever the firmware does. A tap that ended before
the wake is lost the same way; a home-row mod, which ZMK only resolves on
release, always is.

If the host is still not reading after `ZMK_USB_RESEND_KEYS_AFTER_WAKE_GIVE_UP_MS`
(30 s), the module stops polling for it and says so. It does not walk away
from a report of its own still sitting in the endpoint, though: it stays
armed, and the next key change is re-sent once the host reads — otherwise that
key's release, refused behind our parked key-down, would leave it repeating.
From then on a waiting change is retried once a second rather than within
160 ms, so a host that first reads after that can repeat a key for up to a
second before the release lands.

In a build with USB logging, each refused write also prints eleven
`Failed to write endpoint buffer 0x81` warnings — Zephyr's `usb_write()` tries
`USB_NUMOF_EP_WRITE_RETRIES` (10 in ZMK) times more and warns on each. Stock
ZMK prints the same for its own refused writes. The backoff keeps a slow wake
to a few dozen lines; `-DCONFIG_USB_DEVICE_LOG_LEVEL_ERR=y` silences them.

Over **BLE** a report sent while the host link is up is queued and delivered.
One sent while the link is down is thrown away (`hog.c` pops it and returns),
so a key that has to bring the link back is lost there too — but that is the
link, not the host, and this module does not touch it.

## Units and arithmetic

| field | unit | examples |
|---|---|---|
| `_INTERVAL` | 1.25 ms steps | 10 = 12.5 ms, 13 = 16.25 ms, 25 = 31.25 ms, 67 = 83.75 ms, 109 = 136.25 ms, 149 = 186.25 ms, 319 = 398.75 ms |
| `_LATENCY` | plain count of events | — |
| `_SUPERVISION_TIMEOUT` | 10 ms steps | 400 = 4 s, 600 = 6 s, 800 = 8 s |
| `_AFTER_IDLE_MS` | milliseconds | — |

Derived, all in milliseconds:

```
LED sync delay, and how often the peripheral must be on air
                                   = INTERVAL * (LATENCY + 1) * 1.25
first peripheral-half key after a step = INTERVAL * 1.25   (worst case)
central half's own radio rate       = 1 / (INTERVAL * 1.25)
```

### How long a switch between steps takes

Not what you would guess. It is governed by the step you are **leaving**, not
the one you are moving to: the peripheral has to receive
`LL_CONNECTION_UPDATE_IND`, and while it is idle it only listens once per
`(LATENCY + 1)` events of the *current* interval.

It has two parts: getting the PDU delivered and acknowledged, which costs
about two attendance periods, and the instant itself, which Zephyr sets
`latency + 6` events ahead. Together:

```
switch time  ~=  INTERVAL_old * (3 * LATENCY_old + 8) * 1.25
```

Measured on nRF52840 / Zephyr 3.5, over a range of parameters:

| leaving | modelled | measured |
|---|---|---|
| 15 ms / lat 9 | 525 ms | 383–530 ms; 689 ms once, at −85 dBm |
| 30 ms / lat 14 | 1500 ms | 687–1484 ms |
| 60 ms / lat 5 | 1380 ms | 836–1291 ms |
| 120 ms / lat 1 | 1320 ms | 1045–1405 ms |
| 400 ms / lat 0 | 3200 ms | 3159–3251 ms |
| 15 ms / lat 29 | 1425 ms | 597–1418 ms |
| 15 ms / lat 49 | 2325 ms | 896–1587 ms |
| 30 ms / lat 30 | 2940 ms | 2769 ms |
| 60 ms / lat 30 | 5880 ms | 5348 ms |

An upper bound in nearly every case, usually by up to ~30 %, and by more when
the other half happens to be sending — it then listens at every event, and
only the instant itself is left. It ran over three times by 1–6 %, and once by
31 %: 689 ms against 525, on a weak link at −85 dBm, where lost packets add
events. Note what the formula says: **latency, not interval, is what makes
coming back slow** — it appears three times over.

**This is what the steps are tuned around**, not a power target. The
switch is the user-visible cost: for its whole duration every keystroke from
the peripheral half is still delayed by a full interval, so a 5-second return
is several badly-delayed keys in a row, not one.

At a fixed interval the central costs exactly the same whatever the latency —
it cannot use latency at all. So latency is a pure trade between the
peripheral's current and the return time:

| at 120 ms | attendance | switch | central | peripheral |
|---|---|---|---|---|
| lat 9 | 1200 ms | 4200 ms | 90 µA | 18 µA |
| lat 5 | 720 ms | 2760 ms | 90 µA | 22 µA |
| lat 2 | 360 ms | 1680 ms | 90 µA | 33 µA |
| lat 1 | 240 ms | 1320 ms | 90 µA | 43 µA |

**Buy the return time.** The peripheral is not the half that runs out first —
the central outweighs it several times over — so tens of microamps there are
the cheapest thing on the table. The computed steps turn that round: a
latency per step, and the longest interval that still returns inside 1.5 s at
it (3.2 s for the deepest, which stands in for sleep):

| | after idle | interval | latency | attendance | switch | windows | central (split link) |
|---|---|---|---|---|---|---|---|
| step 1 | 0 min | 31.25 ms | 13 | 438 ms | 1469 ms | 18.3 | 346 µA |
| step 2 | 3 min | 83.75 ms | 3 | 335 ms | 1424 ms | 23.9 | 129 µA |
| step 3 | 10 min | 136.25 ms | 1 | 273 ms | 1499 ms | 29.4 | 79 µA |
| step 4 | 20 min | 398.75 ms | 0 | 399 ms | 3190 ms | 20.1 | 27 µA |

`ZMK_SPLIT_CONN_LADDER_MAX_RETURN_MS` (4 s) caps every step at build time,
table or computed, the same way `MIN_SUPERVISION_WINDOWS` enforces robustness; the
computed steps' own budgets sit inside it.

### The interval and the computer's own link

A central half that also talks to a computer over BLE runs two links on one
radio, and nothing can keep them apart: the computer anchors its link to its
own clock, this half anchors the split link to its own, so the two schedules
slide past each other at the difference of the two clocks — hundreds of ppm on
a board without a 32 kHz crystal, tens of ppm with one. Two rules of the
controller decide what that costs:

* when two events overlap, the radio goes to whichever has waited longer since
  its own last event (`ticker_resolve_collision` in `ticker.c`); the loser is
  skipped and wins the next one;
* a running event is dropped as soon as the next one is due
  (`lll_is_abort_cb`), so a connection event carrying several reports — a
  trackball on the central half — is cut short by the next split event.

What that costs depends on the **ratio** of the intervals, not the difference:

* **A multiple of the host's interval** — 7.5 or 15 ms against a 15 ms host —
  is the bad case. The split events then sit in the same place relative to
  every host event, and that place moves only as the clocks drift: tens of
  seconds of the host link running at half rate, or of its events being cut
  where the reports queue up. With a trackball it looks like the cursor
  lagging and catching up, in stretches, minutes apart.
* **Shorter than the host's interval, with a phase step wider than the
  overlap** — 12.5 ms against 15 ms — is the good case. An overlap lasts a
  single event, and the host, which waited 15 ms against 12.5, wins every one.
  The host link loses nothing.
* **Any other non-multiple** — 13.75 or 16.25 ms — spreads the collisions
  thinly: single lost host events, never two in a row.
* **Longer than the host's interval with a coarse phase step** — 17.5 or 20 ms
  — is the worst of the non-multiples: the split is always the one that waited
  longer, so it takes every overlap.

Modelled against those two rules, with the collision window the controller
reserves, a 15 ms host loses about 8 % of its events at 16.25 ms, 11–14 % at
17.5 ms, and none at 12.5 ms.

The computed ACTIVE follows these rules ("ACTIVE"), and the step intervals
keep off the host's factors as well: 25, 67, 109 and 319 units share no factor
with the 12 units (15 ms) of a Mac link or the 6 (7.5 ms) of a PC one, and a
host on another interval moves them to ones that share none with it. Only the
shallow steps really matter — the ladder is still on one for the first second
or so after a wake, which is exactly when the cursor starts moving — but it
costs nothing to keep the deep ones clear too.

Low latency pays a second time, in robustness. The peripheral listens on
nearly every event, so it receives the update at once and has many
retransmission opportunities before the instant, instead of the single
scheduled wake-up a high-latency step gets — and that single chance is what
kills links outright.

Hard requirement, from BT Core 5.0 Vol 2 Part E 7.8.12 and enforced by
Zephyr's own `bt_le_conn_params_valid()`:

```
SUPERVISION_TIMEOUT * 4 > (1 + LATENCY) * INTERVAL
```

Every step, and ACTIVE, are checked against it — at build time with
`BUILD_ASSERT`, and at run time for whatever the computation picks — as is the
requirement that `STEP*_AFTER_IDLE_MS` increase. A bad setting fails the build instead
of dropping links on your desk.

### Picking a latency

The peripheral's cost falls as `1 / (LATENCY + 1)`, so the returns die
quickly. At a 15 ms interval, by the model used above:

| latency | LED delay | peripheral |
|---|---|---|
| 4 | 75 ms | ~112 µA |
| 9 | 150 ms | ~62 µA |
| 19 | 300 ms | ~37 µA |
| 29 | 450 ms | ~29 µA |

Raise it until the LED delay starts to annoy you, and not one step further —
which is what `ACTIVE_PERIPHERAL_SYNC_DELAY_MS` does for ACTIVE and
`USB_POWER_PERIPHERAL_SYNC_DELAY_MS` for the USB power steps.

## Why depth is dangerous, and what the module does about it

### The update handshake gets exactly one chance

Zephyr picks the Link Layer instant as

```c
/* ull_llcp_conn_upd.c */
ctx->data.cu.instant = ull_conn_event_counter(conn) + conn->lll.latency +
                       CONN_UPDATE_INSTANT_DELTA;   /* hardcoded 6 */
```

An idle peripheral wakes once every `(latency + 1)` events. With a high
latency it gets exactly **one** scheduled wake-up in which to receive and
acknowledge `LL_CONNECTION_UPDATE_IND` before the instant passes — its next
wake-up is already 5-plus events beyond it. The central keeps retransmitting
every event, but the peripheral is not listening.

Miss that single window and the two halves end up on different parameters.
Each then waits out its own supervision timeout and drops the link. The
default steps keep latency low enough that this does not arise.

### The board's clock makes that window expensive

The Ergohaven board runs the nRF52840 internal RC oscillator
(`CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC`), which selects Zephyr's **500 ppm**
accuracy bucket — the worst one. Calibration against the HF clock is already
on by default (`CLOCK_CONTROL_NRF_K32SRC_RC_CALIBRATION`, every 4 s or on a
temperature change), and 500 ppm is the calibrated figure, not a conservative
placeholder. Declaring anything better would narrow the receive windows below
what the part can actually hold. There is no 32 kHz crystal on this board, so
the number is a given and the ladder has to be designed around it.

Both halves contribute, and the mechanism is explicit in the controller
(`ull_peripheral.c`):

```c
conn->periph.sca = pdu_adv->connect_ind.sca;      /* the central's accuracy */
lll->periph.window_widening_periodic_us =
    DIV_ROUND_UP(((lll_clock_ppm_local_get() +            /* ours */
                   lll_clock_ppm_get(conn->periph.sca))   /* theirs */
                  * conn_interval_us), USEC_PER_SEC);
```

So the peripheral widens its receive window by its own accuracy **plus** the
central's, accumulated over every event it skips. Two RC halves means 1000 ppm;
a central with a 32 kHz crystal would declare 50 ppm and bring the sum to 550,
cutting the window nearly in half. That is the one real lever on this number,
and it belongs to whichever device is central — which makes it an argument for
a dongle with a proper LFXO, not for anything configurable on the halves.

#### The calibration itself is worth a look

Keeping the RC oscillator honest is not free. Zephyr re-calibrates it against
the HF clock on a schedule, and on a keyboard the schedule is the expensive
part:

| | default | what it means |
|---|---|---|
| `CLOCK_CONTROL_NRF_CALIBRATION_PERIOD` | 4000 | a calibration opportunity every 4 s |
| `CLOCK_CONTROL_NRF_CALIBRATION_MAX_SKIP` | 1 | at most one may be skipped, so it really runs every 8 s |
| `CLOCK_CONTROL_NRF_CALIBRATION_TEMP_DIFF` | 2 | skip only if the temperature moved under 0.5 °C |

Each calibration starts the 16 MHz clock for roughly 17 ms. The keyboard
community measures the whole no-crystal penalty at about 8–10 µA average, and
the calibration schedule is the larger share of it — comparable to the entire
radio budget at a deep idle step.

A keyboard on a desk is thermally boring, so most of those calibrations find
nothing to correct. Raising `MAX_SKIP` lets the driver skip more of them while
still calibrating the moment the temperature actually moves, because the
threshold is measured against the last calibration and slow drift accumulates
into it rather than being forgotten:

```
CONFIG_CLOCK_CONTROL_NRF_CALIBRATION_MAX_SKIP=10
```

That allows up to ~44 s between calibrations while nothing is changing, on both
halves.

**This is not independent of the ladder — it pulls the same rope.** Calibration
is what keeps the oscillator inside the 500 ppm that the receive windows are
sized for. Skip too many and the real error can exceed the declared figure, the
windows stop covering the drift, and the peripheral misses the wake-ups this
whole module is built to protect. Nordic's calibrated LFRC is nominally around
250 ppm against a declared 500, so there is roughly 2x of headroom to spend —
which is why a modest `MAX_SKIP` is defensible and a large one is not.

Treat it as a second-order change and make it **after** the ladder has been
stable for a while, on its own. Changing both at once means a new link drop
tells you nothing about which one caused it.

#### Testing that theory without new hardware

`CLOCK_CONTROL_NRF_K32SRC_SYNTH` derives the 32 kHz clock from the HF crystal,
which gives crystal-grade accuracy — Zephyr's accuracy choice then defaults to
50 ppm rather than 500. It keeps HFXO running, so it costs hundreds of
microamps to a milliamp and is unusable on a battery. On a central that is
tethered to USB it costs nothing.

This is optional and low priority: it needs many hours per arm to say anything
about a sporadic failure, and the case for a dongle rests on moving the central
role off battery rather than on its clock. `ZMK_SPLIT_CONN_LADDER_LOG_RSSI` answers
the more actionable question — signal or congestion — from a single build. Do
that first.

If you still want the clock isolated, build the central with
`-DCONFIG_CLOCK_CONTROL_NRF_K32SRC_SYNTH=y`, and the sum drops from 1000 ppm to
550 without touching anything else. **Give that image its own
`artifact-name` and never run it on battery** — an unplugged half on SYNTH is
flat within hours. It also needs the ladder in mode ON: on the very cable that
makes the experiment safe, AUTO runs the USB power steps, not the deep ones
under test.

There is no runtime version of this. The clock source is a build-time choice,
and `lll_clock_sca_local_get()` returns the compile-time constant, so even the
Bluetooth 5.0 Sleep Clock Accuracy Update procedure (`BT_CTLR_SCA_UPDATE`,
which has no host API in any case) would announce the same number.

### The shape of the ladder

The two costs pull apart cleanly:

* the **central** must open every connection event, so its cost is
  `1 / interval` and latency does nothing for it;
* the **peripheral's** cost, its robustness, and how fast a parameter change
  can be handed to it are all set by `attendance = interval × (latency + 1)`.

So: raise the interval to save the central, and keep the latency low enough
that coming back stays quick. The computed steps hold the return time near
1.5 s at every depth down to step 3, which means a deeper step is **no slower
to leave and no more fragile** than a shallow one — only cheaper for the
central. With the defaults:

| | interval | latency | attendance | drift window | supervision windows | central (split link) | peripheral |
|---|---|---|---|---|---|---|---|
| ACTIVE, Mac | 12.5 ms | 12 | 163 ms | ±0.16 ms | 24.6 | 864 µA | 58 µA |
| ACTIVE, PC or none | 16.25 ms | 9 | 163 ms | ±0.16 ms | 24.6 | 665 µA | 58 µA |
| step 1 | 31.25 ms | 13 | 438 ms | ±0.44 ms | 18.3 | 346 µA | 29 µA |
| step 2 | 83.75 ms | 3 | 335 ms | ±0.34 ms | 23.9 | 129 µA | 34 µA |
| step 3 | 136.25 ms | 1 | 273 ms | ±0.27 ms | 29.4 | 79 µA | 40 µA |
| step 4 | 398.75 ms | 0 | 399 ms | ±0.40 ms | 20.1 | 27 µA | 31 µA |

Currents are modelled, not measured: a fixed charge per connection event on
the central (the split link's share alone here; the rest of the half adds
about 32 µA), a fixed charge per attendance over a floor of about 12 µA on the
peripheral. Treat the ratios as solid and the absolute numbers as ±50 %.

Note that the peripheral does **not** keep getting cheaper as attendance grows —
the drift window widens in step with it, so the receiver stays on longer per
wake. Past roughly a second of attendance the two effects all but cancel,
while the supervision windows keep shrinking. That is the argument for keeping
attendance short at every depth rather than letting it grow.

**Supervision windows** is `supervision timeout / attendance` — how many of the
peripheral's scheduled wake-ups may be missed in a row before the link is
declared dead. It is the single number that says how much interference a step
tolerates, and `ZMK_SPLIT_CONN_LADDER_MIN_SUPERVISION_WINDOWS` enforces a floor on it at
build time for every step and for ACTIVE. Bluetooth itself permits as few as
two; two does not survive on this board.

### Backoff

`ZMK_SPLIT_CONN_LADDER_BACKOFF_AFTER_LINK_LOSS` (on by default) treats a dropped link as
evidence that the depth it dropped at does not hold on this hardware, in this
room. The ladder caps itself one step shallower and hands the notch back after
a period of continuous connection.

Without it, a link that cannot survive the deepest step reconnects, gets pushed
straight back to that step, and dies again — a loop that looks from the outside
like a half that never comes back.

The battery and the USB power steps keep separate caps: a depth that fails on one
parameter set says nothing about the other. The stability clock is shared —
it measures the link, not a column — and hands a notch back to each capped
column at once; the widening resets when both are at full depth again.

Neighbouring steps with the same parameters count as one depth, because to the
link they are one. The table's USB power steps come in pairs — 1 and 2 at latency
29, 3 and 4 at 49 — and a computed step repeats the one above when the host
link's interval leaves it nothing deeper. A loss at the second of such a pair
caps the column below the pair, not at its first half, which would run the
very parameters that just failed; and a notch handed back reopens both
together.

Two refinements keep it from doing harm of its own:

**A link the other half closed is not judged at all.** With
`PERIPHERAL_CLOSE_LINK_BEFORE_SLEEP` (on by default there) the other half
closes the link before its sleep, the central sees reason 0x13, "remote user
terminated", and nothing is charged — however soon the half comes back. Nor
does it restart the stability clock below: the link that comes back keeps the
time since the last real failure, so a half that sleeps every hour does not
hold a capped depth back.

**Any other loss is judged by how fast the link returns — and by whether the
other half restarted.** A half built without that option still drops the link
when it enters its own deep sleep, and so does a reset or a pulled battery;
none of that is a parameter problem, and from the central it is
indistinguishable from a supervision timeout. A half that lost the link over parameters is awake
and advertising, so it comes back in seconds; a half that powered itself off
stays away until a key is pressed on it. Only a return inside
`BACKOFF_RECONNECT_WINDOW_MS` counts against the ladder.

That rule alone is fooled by a key pressed on the sleeping half soon after it
fell asleep: the half is back within seconds, and its sleep reads as a link
loss. So the other half's report (`PERIPHERAL_USB_POWER_REPORT`) carries its
uptime, and a loss waits for the report of the new link (up to 10 s), with or
without `USB_POWER_STEPS`. Up for less time than the central has not heard from
it: it restarted — its sleep, a reset, a battery pulled — and the loss is not
counted, however quick the return. Without a report in time, the return alone
decides. While it waits, the ladder does not step back onto the parameters
the link died at, nor deeper: the report comes about 2 s after the connect, and
a step applied before the verdict would be the failing one again. The absence is measured
from the disconnect to the reconnect, both stamped in the Bluetooth callbacks
where they happen — not from when the work
handler got round to them, which after a reconnect is `NEW_LINK_SETTLE_MS` later. When
both are waiting for the same run of the handler, the stamps also say which
came first: a fresh link that drops before the handler has even seen it come
up died on ACTIVE parameters, and is not charged to any step.

**The retry interval doubles on each failure**, up to 32×. Otherwise a depth
that simply does not work costs one dropped link every
`BACKOFF_RESTORE_AFTER_MS`, forever.
With the default 15 minutes the retries land at 15 min, 30 min, 1 h, 2 h, 4 h,
then 8 h — so a bad depth stops being retried within a working day, while a
one-off drop from someone walking past with a microwave is forgiven quickly.

### Restarting the idle clock

A reconnect restarts the idle clock, so a brand-new link starts from the top:
ACTIVE for `NEW_LINK_SETTLE_MS` while ZMK sets it up, then step 1, and the deeper steps
on their timers from there. That is what keeps a reconnect landing twenty
minutes into an idle period from dropping the fresh link straight to the
deepest step. Moving onto the battery steps from anything else —
unplugging in AUTO, or a hold released with the key — does the same, for the
reason given above. Moving onto the USB power steps does not: plugging in
keeps the depth, and so does the other half unplugging while this one stays on
USB power. A USB power step delays no key, so there is nothing to protect.

### Not stacking updates

A descent waits for the previous update to be confirmed
(`UNCONFIRMED_UPDATE_TIMEOUT_MS` bounds that wait), so the peripheral never
has to process two instants back to back. So does a change between battery and
USB power steps:
plugging in during a descent waits for the descent to land. Restoring ACTIVE
does not wait — coming back up is urgent, and the controller queues that
procedure rather than refusing it.

A keypress or a cable arriving in the middle of a descent is handled in one of
two ways, because of how Zephyr's host checks a request: against the
parameters of the last *completed* update. If the ascent asks for exactly those
— the descent from ACTIVE is still in flight — the host answers `-EALREADY`
and sends nothing, so the module waits for the descent to land and then asks
again. Otherwise both procedures run in order and the descent's parameters are
reported first; the module recognises that as the overtaken request landing,
not as unexpected parameters, and waits for its own. Either way the link is
back at ACTIVE about one descent plus one return later.

## Reading the log

The ladder registers its own log module — the central half's low-power search
and early sleep log there too — and so do the re-send, the other half's report,
the other half's early sleep and its close before sleep, and each is tuned
independently of the rest of ZMK with Zephyr's usual choice: one of
`_LOG_LEVEL_OFF`, `_ERR`, `_WRN`, `_INF`, `_DBG` set `=y` after
`CONFIG_ZMK_SPLIT_CONN_LADDER`, `CONFIG_ZMK_USB_RESEND_KEYS_AFTER_WAKE`,
`CONFIG_ZMK_SPLIT_CONN_LADDER_PERIPHERAL_USB_POWER_REPORT`,
`CONFIG_ZMK_SPLIT_CONN_LADDER_EARLY_SLEEP` or
`CONFIG_ZMK_SPLIT_CONN_LADDER_PERIPHERAL_CLOSE_LINK_BEFORE_SLEEP` (the plain `..._LOG_LEVEL` numbers
have no prompt, and a `.conf` cannot set them). All default to
`CONFIG_LOG_DEFAULT_LEVEL`, which Zephyr defaults to 3 (INFO), so **everything
below compiles in with no extra flag**.

* `CONFIG_ZMK_LOGGING_MINIMAL` does **not** silence these modules. It only
  stops `CONFIG_ZMK_LOG_LEVEL` being defaulted to 4, which affects ZMK's own
  modules.
* A few lines are at `LOG_DBG` and are compiled out at the default level:
  `link re-established, level -1, idle clock restarted` (level 0 in the
  table mode, where the link comes up on ACTIVE itself), `level N skipped, no
  split link up`, `step N landed first, step M still queued`. The first two
  disambiguate "the ladder is broken" from "there is no split link to apply
  to", so for a first bring-up add
  `-DCONFIG_ZMK_SPLIT_CONN_LADDER_LOG_LEVEL_DBG=y`.

One idle period with the defaults against a Mac, RSSI on, reads:

```
activity: IDLE
step 1/4: interval 31.25 ms, latency 13, timeout 8000 ms (peripheral on air every 438 ms; switch from level 0 ~550 ms)
split link RSSI: -63 dBm
split link applied: interval 31.25 ms, latency 13, timeout 8000 ms
...
activity: ACTIVE
step 0/4: interval 12.50 ms, latency 12, timeout 4000 ms (peripheral on air every 163 ms; switch from level 4 ~3190 ms)
split link applied: interval 12.50 ms, latency 12, timeout 4000 ms
```

* `step N/M` is what was **requested**; `split link applied` is what the
  controller actually **applied**. Compare them — a mismatch means the
  controller trimmed something, and from then on the real numbers are the
  applied ones.
* The delay between those two lines is the real switch time. Compare it
  against the `switch from level N` estimate in the `step` line; if a deep
  step takes much longer, cut that step's latency (see the arithmetic
  section) or drop the step. `switch from level -1` means the level was
  unknown at the time, after a lost confirmation.
* `ladder on battery steps (...)` / `ladder on USB power steps (...)` /
  `ladder held at ACTIVE (...)` is the gate: the mode and whether USB power is
  present — `present on both halves` when the other half reports it too.
  `mode X -> Y` is the key.
* `step N/M on USB power` is a USB power step; `level N on USB power` in the
  `switch from` part means the link was on one. `step N/M: parameters already
  in place` (`step N/M on USB power: ...` for a USB power step) is a step with
  the same parameters as the one before it, so nothing was sent — a step that
  repeats the one above, or the table's paired USB power steps.
* `other half reports USB power present|absent`, then `other half: USB power
  ...` on changes, is the other half's report; `other half does not report its
  USB power (no power service)` means that half was built without
  `PERIPHERAL_USB_POWER_REPORT`. `other half's USB power: ... failed`
  lines are the discovery, subscription or read failing — the other half then
  counts as on battery.
* `host link applied:` lines are logged too. That is where the interval and
  latency the host granted become visible — useful on macOS, which insists
  on 15 ms and will refuse a 7.5 ms request.
* `param update rejected: N -- retry K in M ms` is the controller or the host
  refusing a request. In Zephyr 3.5 that is rare: the controller queues Link
  Layer procedures instead of refusing a second one, so what is left is -105
  (`-ENOBUFS`, no HCI command buffer) or -5 (`-EIO`, the controller had no
  free procedure context; Zephyr's own `opcode 0x2013 status 0x0c` warning
  comes with it). The retry starts at `REJECTED_UPDATE_RETRY_MS` and doubles
  with every refusal in a row up to `UNCONFIRMED_UPDATE_TIMEOUT_MS`; an
  accepted update resets it.
  A long run of these is worth a report, not a setting.
* `activity: IDLE` with no `step` line following means `bt_conn_foreach`
  found no central-role connection — check whether the split link is up.
  Or the ladder is held: look for the gate line.
* `update to step N never confirmed -- re-applying` means the controller never
  reported the change. The module stops pretending to know the level and asks
  again.
* `split link lost at step N (reason 0x08)` is a supervision timeout: the two
  halves stopped hearing each other. If N is the deepest step every time, that
  step is too deep for this hardware. Backoff will settle there on its own, and
  you can make it permanent with a shorter interval for that step
  (`STEP*_INTERVAL`) or a lower `STEP_COUNT`.
  If it also happens at step 0, the link itself is weak — look at the RSSI and
  at what stands between the halves before blaming any step.
* `split link closed by the other half at step N (reason 0x13: on purpose, as
  before its sleep) -- not a step failure` is the other half closing the link
  on purpose: before its sleep (`PERIPHERAL_CLOSE_LINK_BEFORE_SLEEP`), or on a
  settings reset there. Nothing is judged, and the search for it starts at
  once.
* `split link lost at step N: the other half restarted while it was down (up
  X.Y s) -- its sleep or a reset, not a step failure` is backoff reading the
  other half's uptime after a reconnect: that half had powered off without
  closing the link — a reset, a pulled battery, or ZMK's sleep on a half
  without the close — and was woken, so nothing is charged.
* `split link: reconnect attempt failed (reason 0x3e), ZMK retries` is a
  connection that came up and died in its first events — a failed reconnect,
  not a second loss. It does prove the other half was awake and advertising,
  so backoff judges the loss before it on that attempt.
* `split link dropped at step N and returned in X.Y s -- limiting depth to M,
  retrying it in Z min` is backoff engaging (`at step N on USB power ...
  limiting depth on USB power to M` for a USB power step), and `split link
  stable for Z min, allowing depth M on battery, K on USB power again` is it
  handing the notch back.
* `split link was away X.Y s -- treating as the other half sleeping, not a step
  failure` is backoff deciding the drop was the peripheral's own deep sleep —
  one that did not close the link first. If that line appears at roughly
  `ZMK_IDLE_SLEEP_TIMEOUT` after your last keypress on that half, it is
  exactly that: check that the half is built with
  `PERIPHERAL_CLOSE_LINK_BEFORE_SLEEP`; with it, its sleep reads as the 0x13
  line above. (The other half's early sleep cannot cause it: that half sleeps
  early only without the central.)
* `host link up: interval N ms, latency L, timeout T ms` is the link to the
  computer as the computer opened it. About five seconds later this half
  asks for its own preferred parameters (`BT_PERIPHERAL_PREF_*`), and if the
  computer agrees, `host link applied: ...` follows. Between them, the last
  one is the interval the computer really runs — the one the split interval
  is best chosen against.
* `split interval A ms and the host's B ms are multiples: the two links will
  collide in long stretches` — a warning, with every host line that shows
  such a pair; see "The interval and the computer's own link". With computed
  steps it comes with the `steps computed` line instead, and only a keys-only
  ACTIVE can land there — or a step interval set by hand, whose warning
  follows that step's line.
* `host link down (reason 0xNN)` is the link to the computer closing.
* `steps computed ...: ACTIVE X ms, latency L, timeout T ms (why; ...)` and the step lines
  under it are the computed table, printed at the first run and after every
  change of the host link, the output or the other half's power; `steps
  unchanged` at the end of the first line means nothing moved. `step N has new
  parameters -- re-applying` follows when the step the ladder stands on moved.
  A parameter set by hand is marked `(set)` after it. See "The ladder".
* `split link RSSI: N dBm` appears only with `ZMK_SPLIT_CONN_LADDER_LOG_RSSI=y`
  plus `CONFIG_BT_CTLR_CONN_RSSI=y`. Across a desk, expect −40..−65 dBm. If the
  level is healthy and steps still fail, the losses are congestion rather than
  signal, and channel classification (`bt_le_set_chan_map`) is the lever.
* `USB suspended: the host is asleep`, `USB resumed after N s suspended --
  ...`, `host resumed N ms after a dropped report -- ...`, `host reading
  again N ms after the wake -- in sync; ...` and `host already reading at our
  first write, N ms after the wake -- in sync; ...` are the wake-key re-send;
  see that section.

* `low-power search: ...` lines are the low-power search, `early sleep ...`
  lines the early sleep (on a peripheral half under
  `zmk_split_conn_ladder_early_sleep`), `going to sleep: ...` lines on a
  peripheral half the close before sleep; see those sections.

The log reaches you over a USB cable, so in AUTO it shows the USB power steps. To
watch the battery steps, switch to ON with the key. A `ZMK_USB_LOGGING` build
also leaves the early sleep off unless it is turned on, so that the log does
not end when the other half goes away.

## Design notes

* The level has exactly one writer, the ladder's work handler.
  `on_connected` only raises an atomic flag, because the compare-then-set in
  `apply_level()` is not atomic and a reconnect landing mid-update would
  otherwise leave our idea of the level permanently out of step with the
  link — after which the early return freezes the wrong parameters in place.
  The `&ladder` key and USB events likewise only write the mode atomically and
  wake the handler.
* The activity state is read inside the work handler, not in the event
  listener, so an already-running handler cannot clobber a just-applied
  ACTIVE. Cancelling a delayable work does not wait for a handler that has
  already started.
* All BLE calls run on the module's own work queue (thread
  `split_conn_ladder`). For a central, `bt_conn_le_param_update()` goes into
  `bt_hci_cmd_send_sync()` and blocks until the controller answers; the
  system work queue must not carry that, and a queue of our own neither
  waits behind nor blocks other modules' low-priority work. Applying a step
  measures 792 bytes (`-fstack-usage` and the call graph of a real build:
  thread entry 8, `work_queue_main` 56, `ladder_work_cb` 56, `apply_level`
  48, `bt_conn_foreach` 32, `apply_to_conn` 64, `bt_conn_le_param_update`
  528), 1252 with logging. The deepest path of a logging build is printing
  the computed table, 1340 bytes, and 1556 should a connection be released
  inside its walk (Zephyr then resumes advertising with an HCI command of
  its own); an interrupt stacks up to 104 more. The queue's stack defaults to
  1280, or 2048 with logging, and a `BUILD_ASSERT` stops a build that sets it
  lower. The low-power search (1284 bytes with logging) and the other half's
  report (1012) need less; on the system work queue the early sleep goes
  deeper than ZMK's own sleep by one frame, 112 bytes with logging, and in
  the Bluetooth thread the module's callbacks stay below ZMK's own. The queue starts at
  `POST_KERNEL`, before anything can raise an event that reaches the
  listener.
* `bt_conn_foreach()` hands out conn objects that are still being
  established — `bt_conn_le_create()` allocates one long before `connected`
  fires — so every walk the ladder makes filters on `bt_conn_info.state ==
  BT_CONN_STATE_CONNECTED`. Without that, "is the split link up" answers yes
  during a reconnect and the module writes parameters into a link that does
  not exist yet. The low-power search's walk is the one exception, on purpose: a
  connection being set up means ZMK found the half and is connecting, which is
  no time to switch its transport off.
* A parameter report with nothing of ours outstanding means something else
  moved the link (Zephyr's own automatic update, or the peripheral asking).
  The module drops to an unknown level and re-applies, rather than believing
  a level whose `apply_level()` would then early-return forever.
* A link lost mid-switch is charged to the level that was **in force**, not
  to the one on its way in. Blaming the target would let a failed 0→1 descent
  cap the ladder at 0 and switch it off for good.
* `ZMK_ACTIVITY_SLEEP` cancels the work, and the handler returns before any
  Bluetooth call while the state is SLEEP. Otherwise a pending handler fires
  during `zmk_pm_suspend_devices()` and blocks on HCI against a radio that is
  already going down.
* Uptime is tracked with `k_uptime_get_32()`: the read is atomic on a 32-bit
  target, and unsigned subtraction stays correct across the ~49-day wrap.
* A trackball keeps the ladder at ACTIVE. ZMK's `activity.c` has an input
  listener under `CONFIG_ZMK_POINTING`, so pointer motion counts as activity.
* The re-send runs on the system work queue, the same one ZMK's HID listener
  sends from, so it cannot interleave with a report ZMK is writing. It never
  writes into a suspended bus: that would take ZMK's suspend branch and wake a
  host that may have gone back to sleep on purpose. ZMK's `usb_status` is
  written from the USB stack's own context and can change at any instruction,
  so the bus is checked on both sides of every write, and a write that met a
  suspended bus counts as refused — never as delivered, although
  `usb_wakeup_request()` returns 0. It writes through
  `zmk_usb_hid_send_*_report()` directly, so a busy endpoint costs no ZMK
  error line per attempt (Zephyr's own `usb_write()` warnings remain; see that
  section).
* A refused parameter update is retried after `REJECTED_UPDATE_RETRY_MS`,
  doubling with each refusal in a row up to `UNCONFIRMED_UPDATE_TIMEOUT_MS`, so
  a refusal that does not clear
  costs a log line every few seconds rather than an HCI round trip twice a
  second.
* The SLEEP listener cancels a scheduled or queued run of the handler but does
  not wait for one already running: waiting (`k_work_cancel_delayable_sync()`)
  would stall ZMK's system work queue behind a Bluetooth command. A run that
  started finishes on its own; any later one stops at the handler's SLEEP
  check before touching the radio.
* An `-EALREADY` from `bt_conn_le_param_update()` means nothing was sent. With
  no update in flight it confirms the level; with one in flight it is ignored,
  and the in-flight update's report brings the request back.
* ZMK keeps its headers private to its own `app` target. The module library
  asks for them explicitly, rather than relying on some other module in the
  build exporting them globally.
* A level is a parameter set, not a depth: 0 is ACTIVE, 1..N the battery
  steps, N+1..2N the USB power steps. Everything that tracks what the link runs —
  confirmed, pending, overtaken — is therefore column-blind, and only the
  handler knows about columns: it picks the depth from the idle time, caps it
  with that column's backoff limit, and maps it to a level.
* The other half's report is a GATT characteristic of the module's own
  (random 128-bit UUIDs in `src/peripheral_usb_power_report_gatt.h`), readable and
  notified, with no encryption demanded: one bit of "is it charging" is
  nothing to protect, and this way it cannot fail on a link that is still
  securing. The central subscribes with a volatile subscription — dropped with
  the link, re-made on every connection, so a reflash that moves the handles
  cannot leave a stale one — and at value handle + 1, the CCC's fixed place in
  that service. Discovery starts from the ladder's queue, because a GATT
  request may block for a buffer (1012 bytes of stack with logging, inside
  the queue's 2048); everything after runs in the GATT callbacks, which only
  set an atomic and wake the handler.
* The low-power search switches ZMK's split transport through ZMK's own API,
  `set_enabled()` of `bt_central` (`app/src/split/bluetooth/central.c`). Off,
  it only stops ZMK's scan when nothing is connected — and it would disconnect
  a connected half, which is why the module calls it only when no split
  connection exists in any state. ZMK's split core does not switch the
  transport back by itself: it selects a transport again only when the one it
  has is no longer available, and this one stays available throughout. ZMK
  sets its `is_scanning` flag before starting a scan and leaves it set when
  the start fails, so a failed switch-on is followed by a switch-off, which
  clears it, and retried — soon at first, then every `ABSENCE_WAIT_MS`, never
  given up: without it the other half could not connect at all.
* The low-power search's scan starts and stops on the ladder's queue, since
  both block on HCI. Its callback runs on the Bluetooth RX thread and only raises a
  flag; the advertising data is parsed there from a saved and restored buffer
  state, so the report stays intact for anyone else.
* Those HCI calls take real time, and the low-power search reads the clock
  only after them: switching ZMK's search back on after a call stamps the
  call's time and the end of its full search when the switch is done, and a
  clock read before it would lag behind those stamps.
* The early sleep runs on the system work queue, as ZMK's own sleep does. That
  queue is cooperative, so nothing else runs between the `SLEEP` event and the
  power going off unless a listener or a device's suspend blocks (an SPI write
  to an LED, say; on a peripheral half, the close before sleep) — the same as
  with ZMK's own sleep. Its listener ignores the activity events it raises itself;
  ZMK's own state stays `IDLE` throughout, and the ladder, told `IDLE` again
  after a failed attempt, keeps the idle period it was in rather than starting
  over.
* The close before sleep waits on the system work queue, inside ZMK's own
  `SLEEP` event, so it cannot wait for the `disconnected` callback, which
  Zephyr delivers on that same queue: it polls the connection's state. The
  state is set when the controller reports the disconnection, on its priority
  receive thread, whatever `BT_RECV_*` says. Its deepest path with logging is
  about 900 bytes, some 1060 from the activity handler down, on a system work
  queue of 2048.
* The early sleep's timestamps — entry into idle, the last change of a
  computer, the other half or USB power — are atomics written where those
  things happen, in listeners and Bluetooth callbacks, and read by the one
  handler; stamps first, the clock second, so none lies in its future.

## Status

On hardware — an Ergohaven K:04 Micro, nRF52840, ZMK v0.3.0 / Zephyr 3.5,
the right half central with a PMW3610 trackball:

* the computed steps come out as the tables above show — against a Mac, with
  the output on USB and with no computer, with the other half showing the
  layer colour and without — and are applied as computed; the stepping
  schedule is correct to the millisecond, and the switch-time model matches
  every transition measured (the 400 ms step: 3196 ms against 3200 modelled);
* no unconfirmed or unexpected updates; RSSI −63..−65 dBm across a desk;
* the central half ran more than 1.5 days on battery with deep sleep disabled
  entirely, without a worrying drop;
* the modes and the key, the USB power steps, the other half's report on every
  connection, the hold with both halves on USB and the return to the same
  depth on USB power when one is unplugged, backoff and its restoration; the
  ladder's own stack peaked at 616 bytes in a logging build (the static
  figures above are the bound);
* backoff's verdict: a restart of the other half while the link was down read
  as `restarted while it was down`, and nothing was charged;
* the trackball's cursor on BLE went into long laggy stretches with the split
  link at 15 ms and at 7.5 ms — both multiples of the 15 ms macOS gives its
  own link — and stopped when the split moved to 16.25 ms;
* the middle search with the keys on a Bluetooth host, the low-power search
  after 35 s, and the other half's call ending both with a reconnect;
* the early sleep: the central half with no computer goes down; on the cable of
  a computer with the keys on USB it stays awake without the other half;
* the wake-key re-send: a key held while the Mac slept delivered once on its
  wake, and a waking host already reading at the first write recognised as in
  sync; a cable plugged into an awake Mac read as a waking host, since the nRF
  controller reports the idle bus before the host's first reset as a
  suspend — a suspend of a bus the host has not configured is not counted;
* the trackball runs without a stall with the other half switched off and its
  loss not yet declared: the layer-colour push of the zmk-rgbled-widget fork
  holds one write at most in the Bluetooth buffers the reports to the
  computer need, and the close before sleep and ACTIVE's 4 s timeout keep
  that window short;
* the close before sleep: the central half logs reason 0x13 and charges
  nothing, at step 0 and at the 400 ms step 4, on the battery steps and on the
  USB power steps; the woken half is back in 2–3 s, through a few `0x3e`
  attempts while it boots;
* ACTIVE's 4 s timeout: applied as asked, and the other half switched off is
  declared lost sooner than at the steps' 8 s;
* the pairing wait: the central half on a profile open for pairing, with no
  computer, stays awake 3 minutes and then sleeps.

Verified off the board:

* real firmware builds against ZMK v0.3.0, with no compiler warning from the
  module: the central half computed and from the table, with and without
  logging, with and without USB logging, with ZMK's own sleep on and off, with
  LED sync, with steps set by hand (four and six steps), with
  `EARLY_SLEEP_WITHOUT_PERIPHERAL`, with underglow switching itself off, with
  the low-power search or the early sleep or both off; the peripheral half plain, logging,
  with ZMK's sleep, with underglow, without its report; the K:04 Micro itself,
  both halves on its own board with zmk-rgbled-widget and the PMW3610 driver;
  and the build stops — steps outside the guards or the budgets, a wait below
  the settings-save formula, a pairing wait below the wait — each with its
  message;
* Kconfig checks: every default, the computed mode's "computed" values, the
  early sleep off in a USB-logging build and back on when asked, the options
  that exist only on one half, and every symbol in the examples;
* the real sources run through simulated scenarios. The ladder: descent,
  plug and unplug at depth, forced ON on a cable, OFF, failed and refused
  updates, a descent overtaken by a cable or a keypress, a foreign parameter
  change, link loss mid-descent and while held, a loss and a reconnect seen in
  one run, SLEEP (ZMK's and the early sleep's), backoff judged by the return
  and by the other half's uptime — with and without `USB_POWER_STEPS`, the
  failed step held until the verdict, a report 25 days old not taken for the
  new link's — a link the other half closed (0x13) left unjudged, the report's
  discovery, subscription and failures. The close before sleep: nothing on
  IDLE or ACTIVE or with no link up, the close waited for at ACTIVE, at the
  deepest step and at a 4 s step set by hand, the wait capped by the
  supervision timeout, a close that never completes, a refused one, the
  references balanced. The computed steps:
  every table the firmware computes — one to six steps, both deepest budgets,
  PC, Mac and other hosts, no host, output on USB, a keys-only keyboard, LED
  sync on and off, ACTIVE's timeout short enough to cut its latency, the other half on USB power, steps set by hand of every
  kind — against an independent reference; recomputes on host, output and
  power changes, held back under an update in flight. The early sleep: no
  computer, a Bluetooth host coming and going, a USB host awake and asleep, a
  charger, a key restarting the count, a computer arriving inside the
  announcement, the pairing wait and a computer connecting to pair, 25 days
  of idle with a computer before it goes, a failed suspend and its retry, ZMK's own
  sleep under way, `EARLY_SLEEP_WITHOUT_PERIPHERAL`, the underglow guard, the
  peripheral half with and without USB. The low-power search, with HCI calls
  that take time as they do on the chip: boot, a directed call, pairing calls
  from its own half and from a stranger, no paired half, a connection, a
  loss, a connection attempt that fails while the search waits on it, ZMK
  finding the half just as the middle search takes over, USB
  power, the middle search with the keys on a Bluetooth host — a
  loss, a call and its 2 s of full search, a connection from it, the host
  going and coming back, the keys moving to USB and back, at intervals 0, 60,
  240, 241 and 1280 ms — a failed switch-on retried for as long as it takes, a
  failed middle and low-power scan, a transport ZMK itself switched off;
* about 200 random configurations of the computed mode — step counts,
  budgets, the steps' and ACTIVE's timeouts, ACTIVE and sync targets, steps set by hand at random —
  compiled and held both to the reference and to rules that do not depend on
  it: every step within the guards; every computed one within its budget, on
  a clean interval, deeper than the one above and the longest that fits at
  its latency, checked by brute force; every repeat checked the same way for
  a deeper interval that would have fitted; the USB power steps on ACTIVE's
  interval. Twice as many more were refused by the build, each by one of its
  checks;
* deliberate breaks: 64 of the early sleep and the low-power search, and 22
  of the close before sleep, the handling of a closed link and ACTIVE's
  timeout, were tried, and every one makes a scenario fail;
* the wake-key re-send, against a USB endpoint that holds one unread report:
  a slow and a fast host, a tap, a tap followed by a key before the host
  reads, the stuck-key sequence, a host that goes back to sleep, resets the
  port, suspends under a write or never reads within the window, a key
  pressed while the port is reset, the output leaving USB with a key held and
  coming back, unplugging, BLE output, the boot protocol, a consumer key with
  a modifier.

The RSSI option is off by default and diagnostic only.

`bt_conn_cb_register()` returns `void` in Zephyr 3.5 and `int` from 3.7 on —
revisit when the ZMK base moves.

## License

MIT
