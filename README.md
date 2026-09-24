# zmk-split-conn-ladder

Steps the **split-link** BLE connection parameters down while the keyboard is
idle, and restores them the moment activity resumes.

ZMK's `ZMK_ACTIVITY_IDLE` state turns off the display, the backlight, the
underglow and the battery poll — and does nothing at all to the radio. Between
`ZMK_IDLE_TIMEOUT` (30 s by default) and `ZMK_IDLE_SLEEP_TIMEOUT` (15 min by
default) the split link keeps running at full active cadence while nothing is
being sent. On a 7.5 ms link that is ~133 radio wake-ups per second, around the
clock, on the half that holds the central role. This module fills that window.

Two smaller things live here as well:

* **On a cable the ladder switches to cable steps.** This half has nothing to
  save on USB power, but the other half is still on its battery, and only
  latency saves it anything. So on a cable the same steps, on the same timers,
  keep the ACTIVE interval — no key waits for them — and raise only the
  latency. When the other half is on USB power too (it says so with one line
  in its own `.conf`), the link stays at ACTIVE. A `&ladder` key switches
  between AUTO, ON and OFF.
* **The key that wakes the computer over USB gets typed, and nothing is left
  stuck** (optional, independent of the ladder). ZMK throws that report away,
  and can leave the host holding a key while it wakes; this re-sends the
  keyboard state until the host is seen reading again.

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
Kconfig                                   every option, with the reasoning in the help text
src/split_conn_ladder.c                   the ladder
src/peripheral_power_client.c             central: reads the other half's USB power report
src/peripheral_power.c                    peripheral: sends that report (optional)
src/peripheral_power_gatt.h               the UUIDs the two halves share
src/ladder_internal.h                     between the ladder and the client; not an API
src/behaviors/behavior_split_conn_ladder.c  the &ladder key
src/usb_resume_resend.c                   the wake-key re-send (independent)
include/dt-bindings/zmk/split_conn_ladder.h  LADDER_TOG / _CYCLE / _AUTO / _ON / _OFF
include/zmk_split_conn_ladder/ladder.h    get/set mode, for other code
dts/behaviors/split_conn_ladder.dtsi      the &ladder node, for the keymap
dts/bindings/behaviors/...yaml            its binding
examples/                                 ready-to-paste config for an Ergohaven K:04 Micro
  k04_micro_right.conf                      the ladder, central half
  k04_micro_left.conf                       the one optional line for the peripheral half
  k04_micro.conf                            sleep timeout, both halves
  keymap-snippet.dtsi                       the &ladder key in a keymap
  build.yaml.debug-snippet                  debug build that shows it all working
  test-no-sleep.conf                        the long diagnostic run, sleep disabled
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
no further setup.

## Configure

Everything goes in the **central half's** `.conf` (the half that holds
`ZMK_SPLIT_ROLE_CENTRAL`), except one optional line for the other half, shown
after the block. The ladder `depends on ZMK_SPLIT_ROLE_CENTRAL`, so on the
peripheral half it is not built at all. A `.conf` both halves read works too —
on the peripheral Kconfig prints an "unsatisfied dependencies" warning and
ignores the lines — but the central's own file keeps that build log clean.

```
# ── ACTIVE parameters. NOT part of this module: the link is created with
#    these, so the ladder reads the same symbols and cannot drift from them.
#    12.5 ms, not 15: shorter than the 15 ms a Mac gives its own link and not
#    a multiple of it -- see "The interval and the computer's own link".
CONFIG_ZMK_SPLIT_BLE_PREF_INT=10
CONFIG_ZMK_SPLIT_BLE_PREF_LATENCY=12
CONFIG_ZMK_SPLIT_BLE_PREF_TIMEOUT=600

# ── the ladder (these are also the defaults)
CONFIG_ZMK_SPLIT_CONN_LADDER=y
CONFIG_ZMK_SPLIT_CONN_LADDER_STEPS=3

CONFIG_ZMK_SPLIT_CONN_LADDER_S1_AFTER_MS=60000
CONFIG_ZMK_SPLIT_CONN_LADDER_S1_INT=25
CONFIG_ZMK_SPLIT_CONN_LADDER_S1_LATENCY=14
CONFIG_ZMK_SPLIT_CONN_LADDER_S1_TIMEOUT=800

CONFIG_ZMK_SPLIT_CONN_LADDER_S2_AFTER_MS=150000
CONFIG_ZMK_SPLIT_CONN_LADDER_S2_INT=49
CONFIG_ZMK_SPLIT_CONN_LADDER_S2_LATENCY=5
CONFIG_ZMK_SPLIT_CONN_LADDER_S2_TIMEOUT=800

CONFIG_ZMK_SPLIT_CONN_LADDER_S3_AFTER_MS=600000
CONFIG_ZMK_SPLIT_CONN_LADDER_S3_INT=97
CONFIG_ZMK_SPLIT_CONN_LADDER_S3_LATENCY=1
CONFIG_ZMK_SPLIT_CONN_LADDER_S3_TIMEOUT=800

# ── the same steps on USB power (mode AUTO): same timers, the ACTIVE
#    interval, more latency. Defaults: S*_USB_INT = CONFIG_ZMK_SPLIT_BLE_PREF_INT,
#    S*_USB_TIMEOUT = 800, latencies below.
CONFIG_ZMK_SPLIT_CONN_LADDER_S1_USB_LATENCY=29
CONFIG_ZMK_SPLIT_CONN_LADDER_S2_USB_LATENCY=29
CONFIG_ZMK_SPLIT_CONN_LADDER_S3_USB_LATENCY=49

# ── optional, off by default: the wake-key re-send, see below
# CONFIG_ZMK_USB_RESUME_RESEND=y
```

And in the **peripheral half's** `.conf` (on the K:04 Micro,
`config/boards/shields/k04_micro/k04_micro_left.conf`), optionally:

```
CONFIG_ZMK_SPLIT_CONN_LADDER_PERIPHERAL_POWER=y
```

With it, that half tells the central whether it is on USB power, and with both
halves plugged in the ladder stays at ACTIVE. Without it the other half counts
as on battery, which costs nothing but the cable steps' slower LED sync while
both charge. Nothing else is needed on that side: west builds every module in
the manifest into both halves.

Things about `.conf` files that bite (Zephyr 3.5's `scripts/kconfig/kconfig.py`):

* **Trailing comments are not comments.** Everything after `=` is the value.
  Keep comments on their own lines.
* **A repeated symbol is not an error.** For hand-written fragments Zephyr
  turns the "set more than once" warning off: the last assignment wins and
  nothing is printed. If your file already sets
  `CONFIG_ZMK_SPLIT_BLE_PREF_LATENCY=10` or
  `CONFIG_ZMK_IDLE_SLEEP_TIMEOUT=...`, **edit that line** rather than pasting
  a second one — otherwise which value wins depends on paste order, silently.
* **An unknown symbol is.** Assigning a symbol that does not exist aborts the
  build ("Aborting due to Kconfig warnings"). If it names
  `ZMK_SPLIT_CONN_LADDER`, the module is not in the build — check
  `config/west.yml`.
* **So is a symbol with no prompt.** `CONFIG_ZMK_BEHAVIOR_SPLIT_CONN_LADDER`
  sets itself from the keymap; a `.conf` line assigning it stops the build.
* **These lines belong to the central half.** See the paragraph above. On the
  peripheral they only earn an "unsatisfied dependencies" warning.

Nothing outside the ladder's own symbols needs setting. A step is applied from
a work queue of the module's own — `bt_conn_le_param_update()` ends in
`bt_hci_cmd_send_sync()` and blocks, which the system work queue must not do.
That call chain measures about 770 bytes, and 1200 in a build with logging
(every `LOG_*` with arguments packs them on the caller's stack, and the
Bluetooth host logs from inside the HCI call). The queue's stack defaults to
1024, or 1536 when logging is on, and costs about that much RAM. It used to
share ZMK's low-priority queue, whose 768-byte default a module cannot raise:
`select` does not work on an int, and ZMK's own `default 768` is parsed before
any module's Kconfig, so it always wins.

The module supports one split peripheral —
`CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS=1`, ZMK's default. Its state is kept
for the split link as a whole; the build stops on anything else.

`S*_AFTER_MS` is measured from entry into `ZMK_ACTIVITY_IDLE`, not from the
last keypress. Total idle before a step fires is `ZMK_IDLE_TIMEOUT +
S*_AFTER_MS`.

`S1_AFTER_MS` is the hysteresis that makes bursty work free: with 60 s, any
pause shorter than ~90 s costs **zero** parameter updates, so reading-then-
typing never pays a ramp-up.

### Every option

`S2_*` through `S4_*` repeat `S1_*` with the defaults shown in the step table
further down, and `S2_USB_*` through `S4_USB_*` repeat `S1_USB_*` (latency 29,
29, 49, 49); steps numbered above `STEPS` are not compiled in at all, so their
values do not matter.

| symbol (`CONFIG_` prefix) | type | default | range | what it does |
|---|---|---|---|---|
| `ZMK_SPLIT_CONN_LADDER` | bool | n | — | the ladder; central half only |
| `ZMK_SPLIT_CONN_LADDER_STEPS` | int | 3 | 1–4 | how many idle steps are built |
| `ZMK_SPLIT_CONN_LADDER_STACK_SIZE` | int | 1024, 1536 with `LOG` | — | the ladder's own work queue; the build stops below those |
| `ZMK_SPLIT_CONN_LADDER_THREAD_PRIORITY` | int | 10 | — | preemptible, like ZMK's low-priority queue |
| `ZMK_SPLIT_CONN_LADDER_PAUSE_ON_USB` | bool | **y** | — | y: boot in AUTO; n: boot in ON |
| `ZMK_SPLIT_CONN_LADDER_USB_STEPS` | bool | **y** | — | AUTO on USB power runs the cable steps; n: holds ACTIVE, as before |
| `ZMK_SPLIT_CONN_LADDER_S1_AFTER_MS` | int | 60000 | 0–3600000 | delay after entering idle |
| `ZMK_SPLIT_CONN_LADDER_S1_INT` | int | 25 | 6–3200 | interval, 1.25 ms units |
| `ZMK_SPLIT_CONN_LADDER_S1_LATENCY` | int | 14 | 0–499 | peripheral latency, events |
| `ZMK_SPLIT_CONN_LADDER_S1_TIMEOUT` | int | 800 | 10–3200 | supervision timeout, 10 ms units |
| `ZMK_SPLIT_CONN_LADDER_S1_USB_INT` | int | `ZMK_SPLIT_BLE_PREF_INT` | 6–3200 | the same step on USB power: interval |
| `ZMK_SPLIT_CONN_LADDER_S1_USB_LATENCY` | int | 29 | 0–499 | … latency |
| `ZMK_SPLIT_CONN_LADDER_S1_USB_TIMEOUT` | int | 800 | 10–3200 | … supervision timeout |
| `ZMK_SPLIT_CONN_LADDER_MIN_WINDOWS` | int | 6 | 2–40 | build-time floor on supervision windows |
| `ZMK_SPLIT_CONN_LADDER_MAX_SWITCH_MS` | int | 4000 | 500–20000 | build-time cap on leaving a step |
| `ZMK_SPLIT_CONN_LADDER_SETTLE_MS` | int | 3000 | 0–30000 | no step on a fresh link while ZMK sets it up, whatever wakes the ladder; matters only when `S1_AFTER_MS` is shorter |
| `ZMK_SPLIT_CONN_LADDER_RETRY_MS` | int | 500 | 50–5000 | first retry after a refused update; doubles per refusal in a row, up to `UPDATE_TIMEOUT_MS` |
| `ZMK_SPLIT_CONN_LADDER_UPDATE_TIMEOUT_MS` | int | 15000 | 1000–60000 | how long an unconfirmed update blocks a deeper step; must exceed `MAX_SWITCH_MS` |
| `ZMK_SPLIT_CONN_LADDER_BACKOFF` | bool | **y** | — | cap depth after a link loss |
| `ZMK_SPLIT_CONN_LADDER_STABLE_MS` | int | 900000 | 60000–21600000 | connected time that restores one notch; doubles per failure up to 32× |
| `ZMK_SPLIT_CONN_LADDER_BACKOFF_CONFIRM_MS` | int | 60000 | 5000–600000 | a return inside this counts the loss against the ladder |
| `ZMK_SPLIT_CONN_LADDER_RSSI` | bool | n | — | log split RSSI; also needs `CONFIG_BT_CTLR_CONN_RSSI=y` |
| `ZMK_SPLIT_CONN_LADDER_LOG_LEVEL` | int | 3 | 0–4 | or the usual `..._LOG_LEVEL_DBG=y` form |
| `ZMK_SPLIT_CONN_LADDER_PERIPHERAL_POWER` | bool | n | — | **peripheral half's** `.conf`: report its USB power to the ladder |
| `ZMK_SPLIT_CONN_LADDER_PERIPHERAL_POWER_LOG_LEVEL` | int | 3 | 0–4 | as above, on that half |
| `ZMK_USB_RESUME_RESEND` | bool | n | — | the wake-key re-send; independent of the ladder |
| `ZMK_USB_RESUME_RESEND_DELAY_MS` | int | 10 | 0–500 | resume → first write |
| `ZMK_USB_RESUME_RESEND_WINDOW_MS` | int | 30000 | 1000–120000 | how long it polls for the host; after that it only re-sends on a key change |
| `ZMK_USB_RESUME_RESEND_LOG_LEVEL` | int | 3 | 0–4 | as above |

Set elsewhere, but part of the picture:

| symbol | default | note |
|---|---|---|
| `CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS` | 1 | must stay 1; enforced at build time |
| `CONFIG_ZMK_SPLIT_BLE_PREF_INT` / `_LATENCY` / `_TIMEOUT` | ZMK's | these *are* the ladder's ACTIVE step |
| `CONFIG_ZMK_IDLE_TIMEOUT` | 30000 | when `S*_AFTER_MS` starts counting |
| `CONFIG_ZMK_IDLE_SLEEP_TIMEOUT` | 900000 | push it out once the ladder works |
| `CONFIG_ZMK_BEHAVIOR_SPLIT_CONN_LADDER` | auto | turns itself on when the keymap uses `&ladder`; **never assign it** — it has no prompt, and Zephyr aborts on that |

## On a cable: AUTO, ON, OFF and the `&ladder` key

| mode | this half on battery | this half on USB power |
|---|---|---|
| **AUTO** (default) | battery steps | cable steps; ACTIVE if the other half is on USB power too |
| **ON** | battery steps | battery steps |
| **OFF** | held at ACTIVE | held at ACTIVE |

With `CONFIG_ZMK_SPLIT_CONN_LADDER_USB_STEPS=n`, AUTO on USB power is held at
ACTIVE, as it was before the cable steps existed.

"USB power" is the same test that keeps ZMK itself out of deep sleep: a host,
awake or asleep, or a bare charger. A host that cuts VBUS while it sleeps
reads as absent, and the half counts as on battery again.

Plugging in keeps the depth: battery step 3 becomes cable step 3, one
parameter update, no detour through ACTIVE. Pulling the cable out returns to
ACTIVE and restarts the idle clock — the ladder starts again from the top
rather than dropping at once to the depth the idle time would allow, because
that is usually the moment somebody picks the keyboard up. Releasing a hold
onto the battery steps (with the key) does the same.

The keyboard boots into AUTO. `CONFIG_ZMK_SPLIT_CONN_LADDER_PAUSE_ON_USB=n`
makes it boot into ON instead. Whatever the key sets lives in RAM only: every
reboot starts from the Kconfig choice again.

### The cable steps

On USB power the central's own consumption does not matter, so the interval —
the only thing that saves the central anything — has no reason to grow. The
cable steps keep the ACTIVE interval and raise the peripheral latency, which is
the only thing that saves the peripheral anything.

That costs no key. A peripheral with something to send transmits at the very
next connection event whatever its latency (the controller cancels latency the
moment data is queued), so a key on the other half still waits at most one
interval, 12.5 ms. Not even the return costs anything: the interval does not
change, so there is nothing to wait out. What waits is traffic **to** the
peripheral — the LED/indicator sync — up to `interval * (latency + 1)`.

| at 12.5 ms | peripheral on air every | peripheral, by the model used for the step table |
|---|---|---|
| ACTIVE, latency 12 | 163 ms | ~58 µA |
| S1/S2 on cable, latency 29 | 375 ms | ~32 µA |
| S3/S4 on cable, latency 49 | 625 ms | ~24 µA |

S1 and S2 share their parameters by default, so step 2 on a cable sends
nothing and logs `step 2/3 on cable: parameters already in place`. With
ZMK's own default `PREF_LATENCY` of 30 the cable latencies above would save
nothing — they have to be above the ACTIVE latency to be worth anything.

Every build-time check (Bluetooth minimum, `MIN_WINDOWS`, `MAX_SWITCH_MS`)
applies to the cable steps as to the battery ones, and a link loss on a cable
step caps the cable steps only (see Backoff).

### The other half's USB power

With both halves on USB power nothing needs saving anywhere, and the ladder
holds ACTIVE. The central learns about the other half from a small report the
other half publishes when built with
`CONFIG_ZMK_SPLIT_CONN_LADDER_PERIPHERAL_POWER=y`:

* that half reads the nRF52840's own VBUS detector once a second — a
  peripheral half has no USB stack, and the detector does not need one — and
  keeps the result, with its own uptime, in a small GATT characteristic of its
  own service (the uptime is what lets backoff tell its sleep from a lost
  link, see Backoff);
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
ladder on cable steps (mode AUTO, USB power present)
step 2/3 on cable: interval 12.50 ms, latency 29, timeout 8000 ms (... switch from level 2 ~1409 ms)
...
other half: USB power present
ladder held at ACTIVE (mode AUTO, USB power present on both halves)
step 0/3: interval 12.50 ms, latency 12, timeout 6000 ms (... switch from level 2 on cable ~1188 ms)
```

(`0/3` is `STEPS=3`: step 0 of 3, i.e. ACTIVE.)

The gate line appears once per change, not on every USB event.

A trick for battery runs, where there is no log: unplug, wait, plug back in.
The `step K/N on cable ... switch from level K` line that follows (`step 0/N`
with `USB_STEPS=n`) says how deep the ladder had gone while you were away.

## The key that wakes the computer (USB)

`CONFIG_ZMK_USB_RESUME_RESEND=y`, on the central half. Off by default and
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

Mouse reports carry movement and are never re-sent. A consumer key with a
modifier (`LS(C_VOL_UP)`) re-sends the keyboard page too, modifier first, as
ZMK's `hid_listener` does.

```
USB suspended: the host is asleep
USB resumed after 42.7 s suspended -- a key change was dropped meanwhile
host resumed 15 ms after a dropped report -- re-sending, a key is still held and will be typed
host reading again 795 ms after the wake -- in sync; 1 key changes meanwhile, 8 writes refused
```

The first two lines come with every sleep and wake of the bus. `no key change
was dropped` is a wake by the trackball, by a layer key — which sends no report
at all — or by the host itself; the keys typed right after it are still
watched, and the `host reading again` line follows only if one of them had to
be written again. No `USB suspended` line at all means the host never suspended
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

The "in sync" line measures the host's own wake: how long it took to start
reading, and how many key changes happened in that time. Changes that began
and ended inside that gap can reach the host only as the final state — one
report fits in the endpoint and ZMK's other writes are gone — so fast typing
into it loses keystrokes whatever the firmware does. A tap that ended before
the wake is lost the same way; a home-row mod, which ZMK only resolves on
release, always is.

If the host is still not reading after `ZMK_USB_RESUME_RESEND_WINDOW_MS`
(30 s), the module stops polling for it and says so. It does not walk away
from a report of its own still sitting in the endpoint, though: it stays
armed, and the next key change is re-sent once the host reads — otherwise that
key's release, refused behind our parked key-down, would leave it repeating.

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
| `_INT` | 1.25 ms steps | 10 = 12.5 ms, 25 = 31.25 ms, 49 = 61.25 ms, 97 = 121.25 ms, 159 = 198.75 ms, 319 = 398.75 ms |
| `_LATENCY` | plain count of events | — |
| `_TIMEOUT` | 10 ms steps | 400 = 4 s, 600 = 6 s, 800 = 8 s |
| `_AFTER_MS` | milliseconds | — |

Derived, all in milliseconds:

```
LED sync delay, and how often the peripheral must be on air
                                   = INT * (LATENCY + 1) * 1.25
first peripheral-half key after a step = INT * 1.25   (worst case)
central half's own radio rate       = 1 / (INT * 1.25)
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
switch time  ~=  INT_old * (3 * LATENCY_old + 8) * 1.25
```

Measured on nRF52840 / Zephyr 3.5, with the defaults and on older tables:

| leaving | modelled | measured |
|---|---|---|
| 15 ms / lat 9 (ACTIVE) | 525 ms | 383–530 ms; 689 ms once, at −85 dBm |
| 30 ms / lat 14 (step 1) | 1500 ms | 687–1484 ms |
| 60 ms / lat 5 (step 2) | 1380 ms | 836–1291 ms |
| 120 ms / lat 1 (step 3) | 1320 ms | 1045–1405 ms |
| 400 ms / lat 0 (step 4) | 3200 ms | 3159–3251 ms |
| 15 ms / lat 29 (cable steps 1–2) | 1425 ms | 597–1418 ms |
| 15 ms / lat 49 (cable steps 3–4) | 2325 ms | 896–1587 ms |
| 30 ms / lat 30 | 2940 ms | 2769 ms |
| 60 ms / lat 30 | 5880 ms | 5348 ms |

An upper bound in nearly every case, usually by up to ~30 %, and by more when
the other half happens to be sending — it then listens at every event, and
only the instant itself is left. It ran over three times by 1–6 %, and once by
31 %: 689 ms against 525, on a weak link at −85 dBm, where lost packets add
events. Note what the formula says: **latency, not interval, is what makes
coming back slow** — it appears three times over.

**This is what the default steps are tuned around**, not a power target. The
switch is the user-visible cost: for its whole duration every keystroke from
the peripheral half is still delayed by a full interval, so a 5-second return
is several badly-delayed keys in a row, not one.

At a fixed interval the central costs exactly the same whatever the latency —
it cannot use latency at all. So latency is a pure trade between the
peripheral's current and the return time:

| at 120 ms | attendance | switch | central | peripheral |
|---|---|---|---|---|
| lat 9 | 1200 ms | 4200 ms | 92 µA | 18 µA |
| lat 5 | 720 ms | 2760 ms | 92 µA | 23 µA |
| lat 2 | 360 ms | 1680 ms | 92 µA | 34 µA |
| lat 1 | 240 ms | 1320 ms | 92 µA | 44 µA |

**Buy the return time.** The peripheral is not the half that runs out first —
the central outweighs it several times over — so tens of microamps there are
the cheapest thing on the table. The defaults pick, for each interval, the
largest latency that still returns inside about 1.5 s:

| | after idle | interval | latency | attendance | switch | windows | central |
|---|---|---|---|---|---|---|---|
| step 1 | 1 min | 31.25 ms | 14 | 469 ms | 1563 ms | 17.1 | 346 µA |
| step 2 | 2.5 min | 61.25 ms | 5 | 368 ms | 1409 ms | 21.8 | 177 µA |
| step 3 | 10 min | 121.25 ms | 1 | 243 ms | 1334 ms | 33.0 | 91 µA |
| step 4 | 30 min | 398.75 ms | 0 | 399 ms | 3190 ms | 20.1 | 29 µA |

`ZMK_SPLIT_CONN_LADDER_MAX_SWITCH_MS` enforces the budget at build time, the
same way `MIN_WINDOWS` enforces robustness. Step 4 is the deliberate exception,
which the 4 s default is sized to allow — see below.

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

The step intervals are picked the same way: 25, 49, 97 and 319 units share no
factor with the 12 units (15 ms) of a Mac link or the 6 (7.5 ms) of a PC one.
Only the shallow steps really matter — the ladder is still on one for the
first second or so after a wake, which is exactly when the cursor starts
moving — but it costs nothing to keep the deep ones clear too.

Low latency pays a second time, in robustness. The peripheral listens on
nearly every event, so it receives the update at once and has many
retransmission opportunities before the instant, instead of the single
scheduled wake-up a high-latency step gets — and that single chance is what
kills links outright.

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

Bounded by the switch time, not by the first-key delay — and the switch time
is set by latency at least as much as by the interval. See the switch-time
section above before going deeper than ~120 ms.

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
role off battery rather than on its clock. `ZMK_SPLIT_CONN_LADDER_RSSI` answers
the more actionable question — signal or congestion — from a single build. Do
that first.

If you still want the clock isolated, build the central with
`-DCONFIG_CLOCK_CONTROL_NRF_K32SRC_SYNTH=y`, and the sum drops from 1000 ppm to
550 without touching anything else. **Give that image its own
`artifact-name` and never run it on battery** — an unplugged half on SYNTH is
flat within hours. It also needs the ladder in mode ON: on the very cable that
makes the experiment safe, AUTO runs the cable steps, not the deep ones under
test.

There is no runtime version of this. The clock source is a build-time choice,
and `lll_clock_sca_local_get()` returns the compile-time constant, so even the
Bluetooth 5.0 Sleep Clock Accuracy Update procedure (`BT_CTLR_SCA_UPDATE`,
which has no host API in any case) would announce the same number.

### The shape of the default ladder

The two costs pull apart cleanly:

* the **central** must open every connection event, so its cost is
  `1 / interval` and latency does nothing for it;
* the **peripheral's** cost, its robustness, and how fast a parameter change
  can be handed to it are all set by `attendance = interval × (latency + 1)`.

So: raise the interval to save the central, and keep the latency low enough
that coming back stays quick. The defaults hold the return time near 1.5 s at
every depth down to step 3, which means a deeper step is **no slower to leave
and no more fragile** than a shallow one — only cheaper for the central.

| | interval | latency | attendance | drift window | supervision windows | central | peripheral |
|---|---|---|---|---|---|---|---|
| ACTIVE | 12.5 ms | 12 | 163 ms | ±0.16 ms | 36.9 | 640 µA | 58 µA |
| step 1 | 31.25 ms | 14 | 469 ms | ±0.47 ms | 17.1 | 346 µA | 28 µA |
| step 2 | 61.25 ms | 5 | 368 ms | ±0.37 ms | 21.8 | 177 µA | 33 µA |
| step 3 | 121.25 ms | 1 | 243 ms | ±0.24 ms | 33.0 | 91 µA | 43 µA |
| step 4 | 398.75 ms | 0 | 399 ms | ±0.40 ms | 20.1 | 29 µA | 31 µA |

Currents are modelled, not measured: a fixed per-wake charge for the HFXO ramp
and CPU, plus receiver current across the RX window. Treat the ratios as solid
and the absolute numbers as ±50 %.

Note that the peripheral does **not** keep getting cheaper as attendance grows —
the drift window widens in step with it, so the receiver stays on longer per
wake. Past roughly a second of attendance the two effects all but cancel,
while the supervision windows keep shrinking. That is the argument for keeping
attendance short at every depth rather than letting it grow.

**Supervision windows** is `supervision timeout / attendance` — how many of the
peripheral's scheduled wake-ups may be missed in a row before the link is
declared dead. It is the single number that says how much interference a step
tolerates, and `ZMK_SPLIT_CONN_LADDER_MIN_WINDOWS` enforces a floor on it at
build time for every step and for ACTIVE. Bluetooth itself permits as few as
two; two does not survive on this board.

### Backoff

`ZMK_SPLIT_CONN_LADDER_BACKOFF` (on by default) treats a dropped link as
evidence that the depth it dropped at does not hold on this hardware, in this
room. The ladder caps itself one step shallower and hands the notch back after
a period of continuous connection.

Without it, a link that cannot survive the deepest step reconnects, gets pushed
straight back to that step, and dies again — a loop that looks from the outside
like a half that never comes back.

The battery and the cable steps keep separate caps: a depth that fails on one
parameter set says nothing about the other. The stability clock is shared —
it measures the link, not a column — and hands a notch back to each capped
column at once; the widening resets when both are at full depth again.

Neighbouring steps with the same parameters count as one depth, because to the
link they are one. The default cable steps come in pairs — 1 and 2 at latency
29, 3 and 4 at 49 — so a loss at cable step 4 caps the cable steps at 2, not
at 3, which would run the very parameters that just failed; and a notch handed
back reopens 3 and 4 together.

Two refinements keep it from doing harm of its own:

**A loss is judged by how fast the link returns — and by whether the other half
restarted.** The other half also drops the link when it enters its own deep
sleep, and that is not a parameter problem. ZMK powers a sleeping half off
without closing the link, so from the central its sleep is indistinguishable
from a supervision timeout. A half that lost the link over parameters is awake
and advertising, so it comes back in seconds; a half that powered itself off
stays away until a key is pressed on it. Only a return inside
`BACKOFF_CONFIRM_MS` counts against the ladder.

That rule alone is fooled by a key pressed on the sleeping half soon after it
fell asleep — which is exactly what the first hardware run did, with sleep at
2.5 minutes: three sleeps of the other half, each followed by a key within
seconds, read as three link losses. So when the other half sends its report
(`ZMK_SPLIT_CONN_LADDER_PERIPHERAL_POWER`), the report carries its uptime, and
a loss waits for it after the reconnect (up to 10 s). Up for less time than
the central has not heard from it: it restarted — its sleep, a reset, a
battery pulled — and the loss is not counted, however quick the return.
Without a report in time, the return alone decides, as before. The absence is measured from the disconnect to the reconnect, both
stamped in the Bluetooth callbacks where they happen — not from when the work
handler got round to them, which after a reconnect is `SETTLE_MS` later. When
both are waiting for the same run of the handler, the stamps also say which
came first: a fresh link that drops before the handler has even seen it come
up died on ACTIVE parameters, and is not charged to any step.

**The retry interval doubles on each failure**, up to 32×. Otherwise a depth
that simply does not work costs one dropped link every `STABLE_MS`, forever.
With the default 15 minutes the retries land at 15 min, 30 min, 1 h, 2 h, 4 h,
then 8 h — so a bad depth stops being retried within a working day, while a
one-off drop from someone walking past with a microwave is forgiven quickly.

### Restarting the idle clock

A reconnect restarts the idle clock, so a brand-new link always gets the full
`S1_AFTER_MS` at ACTIVE parameters before the first step. That is what keeps a
reconnect landing twenty minutes into an idle period from dropping the fresh
link to the deepest step three seconds later, while ZMK is still doing service
discovery on it. Moving onto the battery steps from anything else —
unplugging in AUTO, or a hold released with the key — does the same, for the
reason given above. Moving onto the cable steps does not: plugging in keeps
the depth, and so does the other half unplugging while this one stays on the
cable. A cable step delays no key, so there is nothing to protect.

### Not stacking updates

A descent waits for the previous update to be confirmed
(`UPDATE_TIMEOUT_MS` bounds that wait), so the peripheral never has to process
two instants back to back. So does a change between battery and cable steps:
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

The ladder registers its own log module, and so does the re-send, so
`CONFIG_ZMK_SPLIT_CONN_LADDER_LOG_LEVEL` and
`CONFIG_ZMK_USB_RESUME_RESEND_LOG_LEVEL` tune them independently of the rest
of ZMK. Both resolve to `CONFIG_LOG_DEFAULT_LEVEL`, which Zephyr defaults to 3
(INFO), so **everything below compiles in with no extra flag**.

* `CONFIG_ZMK_LOGGING_MINIMAL` does **not** silence these modules. It only
  stops `CONFIG_ZMK_LOG_LEVEL` being defaulted to 4, which affects ZMK's own
  modules.
* A few lines are at `LOG_DBG` and are compiled out at the default level:
  `link re-established, level 0, idle clock restarted`, `level N skipped, no
  split link up`, `step N landed first, step M still queued`. The first two
  disambiguate "the ladder is broken" from "there is no split link to apply
  to", so for a first bring-up add
  `-DCONFIG_ZMK_SPLIT_CONN_LADDER_LOG_LEVEL_DBG=y`.

From a real run (compressed schedule, `STEPS=4`, RSSI on):

```
activity: IDLE
step 1/4: interval 31.25 ms, latency 14, timeout 8000 ms (peripheral on air every 469 ms; switch from level 0 ~551 ms)
split link RSSI: -63 dBm
split link applied: interval 31.25 ms, latency 14, timeout 8000 ms
...
activity: ACTIVE
step 0/4: interval 12.50 ms, latency 12, timeout 6000 ms (peripheral on air every 163 ms; switch from level 4 ~3190 ms)
split link applied: interval 12.50 ms, latency 12, timeout 6000 ms
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
* `ladder on battery steps (...)` / `ladder on cable steps (...)` /
  `ladder held at ACTIVE (...)` is the gate: the mode and whether USB power is
  present — `present on both halves` when the other half reports it too.
  `mode X -> Y` is the key.
* `step N/M on cable` is a cable step; `level N on cable` in the `switch from`
  part means the link was on one. `step N/M on cable: parameters already in
  place` is a cable step with the same parameters as the one before it, so
  nothing was sent (step 2 with the defaults).
* `other half reports USB power present|absent`, then `other half: USB power
  ...` on changes, is the other half's report; `other half does not report its
  USB power (no power service)` means that half was built without
  `ZMK_SPLIT_CONN_LADDER_PERIPHERAL_POWER`. `other half's USB power: ... failed`
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
  comes with it). The retry starts at `RETRY_MS` and doubles with every
  refusal in a row up to `UPDATE_TIMEOUT_MS`; an accepted update resets it.
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
  you can make it permanent by lowering that step's latency or dropping `STEPS`.
  If it also happens at step 0, the link itself is weak — look at the RSSI and
  at what stands between the halves before blaming any step.
* `split link lost at step N: the other half restarted while it was down (up
  X.Y s) -- its sleep or a reset, not a step failure` is backoff reading the
  other half's uptime after a reconnect: that half had powered off (ZMK's deep
  sleep does that without a word) and was woken, so nothing is charged.
* `split link: reconnect attempt failed (reason 0x3e), ZMK retries` is a
  connection that came up and died in its first events — a failed reconnect,
  not a second loss. It does prove the other half was awake and advertising,
  so backoff judges the loss before it on that attempt.
* `split link dropped at step N and returned in X.Y s -- limiting depth to M,
  retrying it in Z min` is backoff engaging (`at step N on cable ... limiting
  depth on cable to M` for a cable step), and `split link stable for Z min,
  allowing depth M on battery, K on cable again` is it handing the notch back.
* `split link was away X.Y s -- treating as the other half sleeping, not a step
  failure` is backoff deciding the drop was the peripheral's own deep sleep.
  If that line appears at roughly `ZMK_IDLE_SLEEP_TIMEOUT` after your last
  keypress on that half, it is exactly that, and raising the sleep timeout on
  both halves is the fix.
* `host link up: interval N ms, latency L, timeout T ms` is the link to the
  computer as the computer opened it. About five seconds later this half
  asks for its own preferred parameters (`BT_PERIPHERAL_PREF_*`), and if the
  computer agrees, `host link applied: ...` follows. Between them, the last
  one is the interval the computer really runs — the one the split interval
  is best chosen against.
* `split interval A ms and the host's B ms are multiples: the two links will
  collide in long stretches` — a warning, with every host line that shows
  such a pair; see "The interval and the computer's own link".
* `split link RSSI: N dBm` appears only with `ZMK_SPLIT_CONN_LADDER_RSSI=y`
  plus `CONFIG_BT_CTLR_CONN_RSSI=y`. Across a desk, expect −40..−65 dBm. If the
  level is healthy and steps still fail, the losses are congestion rather than
  signal, and channel classification (`bt_le_set_chan_map`) is the lever.
* `USB suspended: the host is asleep`, `USB resumed after N s suspended --
  ...`, `host resumed N ms after a dropped report -- ...` and `host reading
  again N ms after the wake -- in sync; ...` are the wake-key re-send; see
  that section.

The log reaches you over a USB cable, so in AUTO it shows the cable steps. To
watch the battery steps, switch to ON with the key.

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
  waits behind nor blocks other modules' low-priority work. The chain
  measures 768 bytes (`-fstack-usage` on a real build: thread entry 8,
  `work_queue_main` 56, `ladder_work_cb` 48, `apply_level` 32,
  `bt_conn_foreach` 32, `apply_to_conn` 64, `bt_conn_le_param_update` 528),
  1180 with logging; the queue's stack defaults to 1024 or 1536 accordingly,
  and a `BUILD_ASSERT` stops a build that sets it lower. The queue starts at
  `POST_KERNEL`, before anything can raise an event that reaches the
  listener.
* `bt_conn_foreach()` hands out conn objects that are still being
  established — `bt_conn_le_create()` allocates one long before `connected`
  fires — so every walk filters on `bt_conn_info.state ==
  BT_CONN_STATE_CONNECTED`. Without that, "is the split link up" answers yes
  during a reconnect and the module writes parameters into a link that does
  not exist yet.
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
* A refused parameter update is retried after `RETRY_MS`, doubling with each
  refusal in a row up to `UPDATE_TIMEOUT_MS`, so a refusal that does not clear
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
  steps, N+1..2N the cable steps. Everything that tracks what the link runs —
  confirmed, pending, overtaken — is therefore column-blind, and only the
  handler knows about columns: it picks the depth from the idle time, caps it
  with that column's backoff limit, and maps it to a level.
* The other half's report is a GATT characteristic of the module's own
  (random 128-bit UUIDs in `src/peripheral_power_gatt.h`), readable and
  notified, with no encryption demanded: one bit of "is it charging" is
  nothing to protect, and this way it cannot fail on a link that is still
  securing. The central subscribes with a volatile subscription — dropped with
  the link, re-made on every connection, so a reflash that moves the handles
  cannot leave a stale one — and at value handle + 1, the CCC's fixed place in
  that service. Discovery starts from the ladder's queue, because a GATT
  request may block for a buffer (1012 bytes of stack with logging, inside
  the queue's 1536); everything after runs in the GATT callbacks, which only
  set an atomic and wake the handler.

## Step 4: trading power for availability

Steps 1–3 are free in the sense that matters: they return inside ~1.5 s, which
is shorter than the pause between coming back to a keyboard and typing a full
word with the off hand. Step 4 is not free — 3.2 s — and it exists for one
purpose: to make deep sleep unnecessary for hours at a time.

The arithmetic, for the central half over a day of 8 h use and 16 h away:

| | µA·h per day |
|---|---|
| active hours alone | 5744 |
| + ladder to step 3, sleep after 1 h | ~5870 |
| + ladder to step 4, sleep after 4 h | ~6460 |
| + ladder to step 3, no sleep at all | ~7220 |

So sleep is still the cheapest thing available — a step 4 that postpones it
costs roughly 10 % more per day. What it buys is that the keyboard is *alive*:
no reboot, no lost keystroke, no host re-pairing across a lunch break. Waking
the central from deep sleep also means reconnecting to the host, which is the
slow half of the whole affair.

To try it:

```
CONFIG_ZMK_SPLIT_CONN_LADDER_STEPS=4
CONFIG_ZMK_IDLE_SLEEP_TIMEOUT=14400000
```

Then judge it by the return, not by the battery: come back after an hour and
start typing normally, with both hands. For those 3.2 s every key from the
peripheral half arrives up to 400 ms late — so it can land **after** a key the
central half typed later — and at 400 ms the peripheral can hand over only a
few key changes per connection event. Type faster than that and its 10-entry
queue fills; ZMK then discards the oldest changes (`Position state message
queue full` in the peripheral's own log). Touching the trackball first gives
the ladder a head start, since pointer motion counts as activity. If the first
words come out scrambled, `S4_INT=159` (198.75 ms, 1.6 s return) or `STEPS=3` is
the fix.

Going deeper than this is refused at build time on purpose. At 800 ms the
return alone runs past six seconds, and it saves 14 µA against step 4 — a
trade that is on the wrong side of every line.

## Interaction with deep sleep

Waking from `ZMK_ACTIVITY_SLEEP` is a `sys_poweroff()` recovery: full reset,
host reconnect, split re-establish — one to three seconds, and the keystroke
that woke it is consumed by the GPIO wake and never reaches the host. Every
ladder step wakes in less than one connection interval instead.

So once the ladder works, push deep sleep **further out**, not closer. At a
120 ms step the central half costs ~92 µA; an extra hour before sleeping is
~92 µAh, which is nothing next to a reboot in the middle of reading something.

```
CONFIG_ZMK_IDLE_SLEEP_TIMEOUT=3600000
```

## Status

On hardware (Ergohaven K:04 Micro, nRF52840, ZMK v0.3.0 / Zephyr 3.5):

* the stepping schedule and the log lines are correct to the millisecond;
* the switch-time model matches every transition of the default ladder, step 4
  included (3196 ms measured against 3200 modelled);
* no link losses, no unconfirmed or unexpected updates across the whole
  ladder; RSSI −63..−65 dBm across a desk;
* the central half ran more than 1.5 days on battery with deep sleep disabled
  entirely, without a worrying drop;
* the three "losses" of the first run were the other half's deep sleep, not
  the link: it powers off without disconnecting, and the uptime it now
  reports on reconnecting tells the two apart;
* the trackball's cursor on BLE went into long laggy stretches with the split
  link at 15 ms and at 7.5 ms — both multiples of the 15 ms macOS gives its
  own link — and stopped when the split moved to 16.25 ms. 12.5 ms, which the
  model puts ahead of both, is the example config's value now but has not yet
  had a day of use behind it.

This revision adds the AUTO/ON/OFF modes, the `&ladder` behavior, the handling
of an overtaken update, the wake-key re-send, and the fixes from three rounds
of independent review. Those are verified off the board:

* real firmware builds against ZMK v0.3.0 — central with Studio and USB
  logging, central without logging, central ladder-only, peripheral with
  `&ladder` in its keymap, a peripheral handed the central's `.conf`, re-send
  only (with boot protocol), RSSI — all without a single compiler warning in
  the module; and the two build-time stops (stack too small, with and without
  logging) checked to fire with their instruction;
* the stack figures above, measured with `-fstack-usage` on those builds and
  walked through the call graph of the linked image;
* Kconfig and devicetree checks (the binding resolves, the keymap cells decode
  to the header values, `/omit-if-no-ref/` drops an unreferenced node);
* the real sources run through simulated scenarios, against a host that
  answers `-EALREADY` the way Zephyr 3.5 does, a controller that can refuse,
  a connection that can still be connecting, and a USB endpoint that holds one
  unread report: descent, plug and unplug at depth, forced ON on a cable, OFF,
  failed and refused updates, a descent overtaken by a cable or a keypress, a
  foreign parameter change, link loss mid-descent and while held, a loss and a
  reconnect seen in one run, SLEEP; and for the re-send, a slow and a fast
  host, a tap, a tap followed by a key before the host reads, the stuck-key
  sequence, a host that goes back to sleep (before and after reading), resets
  the port, suspends under a write, or never reads within the window,
  unplugging, BLE output, the boot protocol, and a consumer key with a
  modifier;
* mutation checks: every fix above was reverted once on purpose, and each
  revert makes a scenario fail — except one defensive line against a
  cross-thread race the single-threaded simulation cannot produce.

**Not yet run on hardware.** (Since run, in part: see the end of this section.)

The latest revision adds the cable steps and the other half's USB power report.
Verified the same way, off the board: real builds of the central with the
cable steps (with and without logging, with them switched off) and of a
peripheral with the report (with and without logging), no module warnings; the
stack of the discovery start measured on the ladder's queue; Kconfig checks
for the new symbols (the cable interval follows `PREF_INT`, the report exists
only on a peripheral half); simulated scenarios for the cable steps — descent,
plug and unplug at depth, a key on a cable step, both halves on USB and one
unplugging again, mode ON on a cable, plugging in during a descent, a loss on a
cable step capping only the cable steps and the notch coming back on its timer
— and for the report's client: discovery order and handles, subscription and
first read, notifications, a half without the service, a failed read, a link
gone before discovery, refused discovery retried and given up.

It also closes a gap in `SETTLE_MS`: the delay `on_connected()` put on the
handler was undone by anything that ran it earlier — a key, a cable, the
`&ladder` key, now also the other half's power report — so with `S1_AFTER_MS`
below `SETTLE_MS` a step could land in the middle of ZMK's set-up. The handler
now holds every step back until `SETTLE_MS` after the connect itself, and a
scenario per wake source checks it. With the default `S1_AFTER_MS` the gap
never opened.

Fourteen deliberate breaks of the new logic were tried; thirteen make a
scenario fail, and the survivor is a defensive cancel of a discovery start
whose own lookup would find no link anyway.

First hardware run of this revision (2026-09-22, about 30 minutes on a
compressed schedule, `STEPS=4`, sleep at 2.5 minutes, the central on USB): the
modes and the key, the cable steps (`parameters already in place` for the
paired ones), the other half's report on every connection, the hold with both
halves on USB and the return to the same cable depth when one is unplugged,
backoff and its 15-minute restoration all behaved as designed; the ladder's own
stack peaked at 616 of 1536 bytes. The switch times went into the table above.

The three "losses" of that run — at cable step 4, at battery step 4, and one at
ACTIVE — were no losses: each came exactly 150 s after the last key on the
other half plus the supervision timeout, which is that half going into its deep
sleep. Two of them were charged to steps because a key woke it within seconds;
that is what the uptime in its report now prevents (see Backoff). No real link
loss has been seen yet, at latency 49 or at 400 ms. The run also showed that
backoff could cap a pair of identical steps at its lower half, fixed above.

The RSSI option is off by default and diagnostic only.

`bt_conn_cb_register()` returns `void` in Zephyr 3.5 and `int` from 3.7 on —
revisit when the ZMK base moves.

## License

MIT
