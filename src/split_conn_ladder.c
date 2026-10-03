/*
 * Copyright (c) 2026 eXidno
 *
 * SPDX-License-Identifier: MIT
 *
 * Steps the SPLIT-link connection parameters down while the keyboard is idle
 * and restores them the moment activity resumes.
 *
 * Why the split link and not the host link: on the split link this half is
 * central, so bt_conn_le_param_update() starts a Link Layer procedure that
 * ends in LL_CONNECTION_UPDATE_IND -- applied at an instant the central
 * picks. The peripheral's host could in principle object to a parameter
 * request, but only through a le_param_req callback, which ZMK does not
 * register, so what the central asks for is what the link runs. Toward the
 * host this half is peripheral and could only ask; and there is nothing to
 * win there anyway, because the negotiated peripheral latency already keeps
 * that link at roughly two connection events per second, against ~80 for a
 * 12.5 ms split link and ~67 for a 15 ms one.
 *
 * The shape of the ladder. A central cannot use peripheral latency at all --
 * it must open every single connection event -- so its cost is set by the
 * INTERVAL alone, and the interval is where essentially all of the saving is:
 * the split link is ~97 % of this half's radio events at 12.5 ms.
 *
 * The interval also decides whether the two links collide, when this half
 * has one of its own to a computer: both run on one radio, and an interval
 * that is a multiple of the host's parks their schedules on top of each
 * other for as long as the clocks take to drift apart. Keep it off the
 * host's multiples and, if possible, just below it -- README, "The interval
 * and the computer's own link".
 *
 * Latency is a separate knob, and it buys only the peripheral, which is not
 * the half that runs out first. What it sells is time:
 *
 *     attendance = interval * (latency + 1)      how often it must listen
 *     switch     = interval * (3 * latency + 8)  how long a change takes
 *
 * The second one is the user-visible cost. Zephyr sets the Link Layer instant
 * at event_counter + latency + 6 (ull_llcp_conn_upd.c), and the PDU still has
 * to reach a peripheral that is deliberately not listening, so both terms scale
 * with latency. Coming back from a deep, high-latency step therefore takes
 * seconds, during which every keystroke from that half is still delayed by a
 * full interval.
 *
 * So the steps are tuned to a SWITCH BUDGET rather than to a power target:
 * each takes a latency, and the longest interval that still returns inside
 * about 1.5 s at it. The peripheral ends up costing more than it strictly has
 * to, which is the right way round -- it outlives the central several times
 * over either way.
 *
 * ZMK_SPLIT_CONN_LADDER_COMPUTED_STEPS, the default, does that tuning on the keyboard
 * -- a latency series and a return budget per step -- and does it again
 * whenever the link to the computer, the output or the other half's power
 * changes, moving ACTIVE out of the host link's way too (compute_table()).
 * Any parameter of any step can still be set by hand. Off, the steps are a
 * table.
 *
 * Low latency pays a second time. The peripheral listens on nearly every
 * event, so it receives the update immediately and has many retransmission
 * opportunities before the instant, instead of the single scheduled wake-up a
 * high-latency step gets. That single-chance failure is what kills a link
 * outright, and it is worst on this board: it runs the nRF52840 internal RC
 * oscillator (CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC), whose calibrated accuracy
 * sits in Zephyr's 500 ppm bucket, doubled across the two halves, so the
 * receive window has to be widened in proportion to attendance.
 *
 * On USB power this half has nothing to save, but the other half is still on
 * its battery, and only latency saves it anything. So in mode AUTO on USB
 * power the same steps, on the same timers, run a second parameter set -- the
 * USB-power column, STEP*_USB_POWER_*: the ACTIVE interval, which delays no
 * key, and a higher latency. If the other half reports USB power as well
 * (peripheral_usb_power_report_client.c), or
 * ZMK_SPLIT_CONN_LADDER_USB_POWER_STEPS is off, AUTO holds ACTIVE on USB
 * power instead. The &ladder behavior can force the battery column ON
 * whatever the power -- which is how it gets watched over a USB log -- or
 * switch the ladder OFF.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/logging/log.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_LOG_RSSI)
#include <zephyr/bluetooth/hci.h>
#include <zephyr/net/buf.h>
#include <zephyr/sys/byteorder.h>
#endif

#include <zmk/event_manager.h>
#include <zmk/activity.h>
#include <zmk/events/activity_state_changed.h>

/* The same guard activity.c uses for "USB power present": usb.c and its event
 * are built with the USB device stack, which a USB-logging build also has. */
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
#include <zmk/usb.h>
#include <zmk/events/usb_conn_state_changed.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_COMPUTED_STEPS)
#include <zmk/endpoints.h>
#include <zmk/events/endpoint_changed.h>
#endif

#include <dt-bindings/zmk/split_conn_ladder.h>
#include <zmk_split_conn_ladder/ladder.h>

#include "ladder_internal.h"

LOG_MODULE_REGISTER(zmk_split_conn_ladder, CONFIG_ZMK_SPLIT_CONN_LADDER_LOG_LEVEL);

#define NEW_LINK_SETTLE_MS            CONFIG_ZMK_SPLIT_CONN_LADDER_NEW_LINK_SETTLE_MS
#define REJECTED_UPDATE_RETRY_MS      CONFIG_ZMK_SPLIT_CONN_LADDER_REJECTED_UPDATE_RETRY_MS
#define UNCONFIRMED_UPDATE_TIMEOUT_MS CONFIG_ZMK_SPLIT_CONN_LADDER_UNCONFIRMED_UPDATE_TIMEOUT_MS
#define MIN_SUPERVISION_WINDOWS       CONFIG_ZMK_SPLIT_CONN_LADDER_MIN_SUPERVISION_WINDOWS
#define MAX_RETURN_MS                 CONFIG_ZMK_SPLIT_CONN_LADDER_MAX_RETURN_MS

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_BACKOFF_AFTER_LINK_LOSS)
#define BACKOFF_RESTORE_AFTER_MS    CONFIG_ZMK_SPLIT_CONN_LADDER_BACKOFF_RESTORE_AFTER_MS
#define BACKOFF_RECONNECT_WINDOW_MS CONFIG_ZMK_SPLIT_CONN_LADDER_BACKOFF_RECONNECT_WINDOW_MS
/* One week: keeps the doubling bounded. */
#define BACKOFF_RESTORE_AFTER_MAX_MS 604800000U
/* How long a loss waits for the other half's report before it is judged on
 * the return alone. The report comes ~2 s after the connect. */
#define JUDGE_WAIT_MS 10000U
#endif

/* BT_HCI_ERR_CONN_FAIL_TO_ESTAB (hci_types.h): `connected` fired, and the link
 * died in its first connection events. */
#define HCI_ERR_CONN_FAIL_TO_ESTAB 0x3e
/* BT_HCI_ERR_REMOTE_USER_TERM_CONN: the other half closed the link on purpose
 * -- before its sleep (PERIPHERAL_CLOSE_LINK_BEFORE_SLEEP there), on a settings
 * reset, or ZMK switching its transport off. Never parameters that failed. A
 * disconnect this half starts reports 0x16 instead. */
#define HCI_ERR_REMOTE_USER_TERM_CONN 0x13

/* Bluetooth interval units (1.25 ms) to milliseconds, integer only. */
#define INT_MS_W(i) ((i) * 125 / 100)
#define INT_MS_F(i) ((i) * 125 % 100)
/* n connection events of interval i, in whole ms, rounded: 13 units (16.25 ms)
 * times 10 is 163, not the 160 that rounding the interval first would give. */
#define EVENTS_MS(i, n) (((uint32_t)(i) * 125U * (uint32_t)(n) + 50U) / 100U)

/* What happened to the link to the computer (host_link_changed()). */
enum host_event {
	HOST_UP,
	HOST_CHANGED,
	HOST_DOWN,
};

struct step {
	uint32_t after_ms;
	uint16_t interval;
	uint16_t latency;
	uint16_t timeout;
};

#define STEP(n)                                                                          \
	{                                                                                \
		.after_ms = CONFIG_ZMK_SPLIT_CONN_LADDER_STEP##n##_AFTER_IDLE_MS,        \
		.interval = CONFIG_ZMK_SPLIT_CONN_LADDER_STEP##n##_INTERVAL,             \
		.latency = CONFIG_ZMK_SPLIT_CONN_LADDER_STEP##n##_LATENCY,               \
		.timeout = CONFIG_ZMK_SPLIT_CONN_LADDER_STEP##n##_SUPERVISION_TIMEOUT,   \
	}

/*
 * Two build-time guards per step.
 *
 * CHECK_SPEC is Zephyr's own bt_le_conn_params_valid() rule, from BT Core 5.0
 * Vol 2 Part E 7.8.12: a parameter set that fails it is refused at runtime.
 *
 * CHECK_WINDOWS is the one that actually keeps links alive. It demands that
 * the supervision timeout cover at least MIN_SUPERVISION_WINDOWS peripheral
 * attendances, which is how many of them may be missed before the link is
 * declared dead.
 * The spec rule alone permits as few as two, and two is not survivable on a
 * 500 ppm board. It is also what stops a deep step from silently becoming
 * more fragile than a shallow one: raise the interval without dropping the
 * latency to match and the build fails.
 *
 *     windows = timeout * 10 / (interval * 1.25 * (latency + 1))
 *   ->  timeout * 8 >= MIN_SUPERVISION_WINDOWS * interval * (latency + 1)
 */
#define CHECK_SPEC(name, to, lat, in)                                                \
	BUILD_ASSERT((to) * 4 > (1 + (lat)) * (in),                                  \
		     name ": supervision timeout below the Bluetooth minimum")

#define CHECK_WINDOWS(name, to, lat, in)                                                 \
	BUILD_ASSERT((to) * 8 >= MIN_SUPERVISION_WINDOWS * (in) * (1 + (lat)),           \
		     name ": fewer supervision windows than "                            \
			  "ZMK_SPLIT_CONN_LADDER_MIN_SUPERVISION_WINDOWS -- "            \
			  "raise the timeout or lower the latency")

/*
 * The third guard is the user-visible one. Leaving a step costs
 *     interval * (3 * latency + 8)
 * and for that whole time every keystroke from the peripheral half is still
 * delayed by a full interval -- several bad keys in a row, not one. Capping it
 * at build time is what stops a step from being tuned for power alone.
 *
 *     interval * 1.25 * (3 * latency + 8) <= MAX_RETURN_MS
 *   ->  interval * 125 * (3 * latency + 8) <= MAX_RETURN_MS * 100
 */
#define CHECK_SWITCH(name, lat, in)                                                      \
	BUILD_ASSERT((in) * 125 * (3 * (lat) + 8) <= MAX_RETURN_MS * 100,                \
		     name ": leaving this step takes longer than "                       \
			  "ZMK_SPLIT_CONN_LADDER_MAX_RETURN_MS -- "                      \
			  "lower the latency or the interval")

#define CHECK(name, to, lat, in)                                                     \
	CHECK_SPEC(name, to, lat, in);                                               \
	CHECK_WINDOWS(name, to, lat, in)

#define CHECK_STEP(n)                                                                    \
	CHECK("step " #n, CONFIG_ZMK_SPLIT_CONN_LADDER_STEP##n##_SUPERVISION_TIMEOUT,    \
	      CONFIG_ZMK_SPLIT_CONN_LADDER_STEP##n##_LATENCY,                            \
	      CONFIG_ZMK_SPLIT_CONN_LADDER_STEP##n##_INTERVAL);                          \
	CHECK_SWITCH("step " #n, CONFIG_ZMK_SPLIT_CONN_LADDER_STEP##n##_LATENCY,         \
		     CONFIG_ZMK_SPLIT_CONN_LADDER_STEP##n##_INTERVAL)

/*
 * An update that is still running must not be declared lost: the deepest step
 * is allowed to take MAX_RETURN_MS, so the timeout has to be longer than that
 * or the ladder would re-issue on top of a procedure in flight -- exactly the
 * back-to-back instants that kill links.
 */
BUILD_ASSERT(UNCONFIRMED_UPDATE_TIMEOUT_MS > MAX_RETURN_MS,
	     "ZMK_SPLIT_CONN_LADDER_UNCONFIRMED_UPDATE_TIMEOUT_MS must exceed "
	     "ZMK_SPLIT_CONN_LADDER_MAX_RETURN_MS");

/*
 * The ladder runs on a work queue of its own. bt_conn_le_param_update() ends
 * in bt_hci_cmd_send_sync() and blocks until the controller answers, which
 * must not happen on the system work queue, where ZMK builds and sends HID
 * reports.
 *
 * Not ZMK's low-priority queue: the chain below needs more than ZMK's
 * 768-byte default for that thread, and a module cannot raise it -- `select`
 * does not work for an int, and ZMK's own `default 768` is parsed before any
 * module Kconfig, so it always wins -- which would force a line on every user.
 * A queue of our own sizes itself (ZMK_SPLIT_CONN_LADDER_WORK_QUEUE_STACK_SIZE: 1280, or
 * 2048 in a logging build), costs about that much RAM, and neither queues
 * behind nor blocks other modules' low-priority work.
 *
 * Measured with -fstack-usage and the call graph of real Ergohaven K:04 builds
 * of ZMK 0.3.0, central role, each path including the thread entry and
 * work_queue_main (64 bytes):
 *
 *                                         without logging   with USB logging
 *   applying a step: ladder_work_cb,             792               1252
 *     apply_level, bt_conn_foreach,
 *     apply_to_conn, bt_conn_le_param_update
 *   the computed table: refresh_table and       ~300               1340
 *     its log lines (464 bytes of its own)
 *   ... a connection released inside its         832               1556
 *     walk, which resumes advertising with
 *     an HCI command of its own
 *   the low-power search switching ZMK's scan    816               1284
 *   the other half's report: GATT discovery      680               1012
 *
 * An interrupt stacks up to 104 bytes more on top (FPU context). So the
 * worst is about 940 and 1660 bytes, and the defaults leave 340-390 spare.
 *
 * Logging costs that much because every LOG_* with arguments packages them
 * on the caller's stack, and the Bluetooth host logs from inside the HCI
 * call. The guard only fires if someone lowers the symbol by hand: an
 * overflow here would land minutes into an idle period, with nothing in the
 * log to explain it.
 */
#if IS_ENABLED(CONFIG_LOG)
#define LADDER_STACK_MIN 2048
#else
#define LADDER_STACK_MIN 1280
#endif

BUILD_ASSERT(CONFIG_ZMK_SPLIT_CONN_LADDER_WORK_QUEUE_STACK_SIZE >= LADDER_STACK_MIN,
	     "CONFIG_ZMK_SPLIT_CONN_LADDER_WORK_QUEUE_STACK_SIZE must be at least "
	     STRINGIFY(LADDER_STACK_MIN) " (1280 without logging, 2048 with it): "
	     "this queue needs up to ~940 bytes of stack, ~1660 in a logging "
	     "build. Its default is right; remove the override.");

K_THREAD_STACK_DEFINE(ladder_stack, CONFIG_ZMK_SPLIT_CONN_LADDER_WORK_QUEUE_STACK_SIZE);
static struct k_work_q ladder_q;

/*
 * One peripheral. Every piece of state here -- the confirmed level, the one in
 * flight, the loss accounting -- is kept for the split link as a whole, and a
 * second peripheral would make it lie: a request one of them refused would be
 * re-sent to the other on top of a procedure already running there, which is
 * the back-to-back instant this module exists to avoid.
 */
#if defined(CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS)
BUILD_ASSERT(CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS == 1,
	     "zmk-split-conn-ladder supports one split peripheral "
	     "(CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS=1)");
#endif

/*
 * The link is created on ZMK's PREF_* symbols (bt_conn_le_create()), and in
 * the table mode ACTIVE is exactly that, so our idea of "fast" and the link's
 * actual state cannot diverge. With the computed steps ACTIVE is worked out
 * like the steps, and a fresh link starts on PREF and is moved there like onto
 * any step. Either way PREF has to be a valid parameter set.
 */
static const struct step step_pref = {
	.interval = CONFIG_ZMK_SPLIT_BLE_PREF_INT,
	.latency = CONFIG_ZMK_SPLIT_BLE_PREF_LATENCY,
	.timeout = CONFIG_ZMK_SPLIT_BLE_PREF_TIMEOUT,
};

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_COMPUTED_STEPS)
/* Computed: PREF only opens the link, which is then moved to the computed
 * ACTIVE, so it has to be valid Bluetooth, not meet MIN_SUPERVISION_WINDOWS. */
CHECK_SPEC("active", CONFIG_ZMK_SPLIT_BLE_PREF_TIMEOUT, CONFIG_ZMK_SPLIT_BLE_PREF_LATENCY,
	   CONFIG_ZMK_SPLIT_BLE_PREF_INT);
#else
/* The table: PREF is ACTIVE, and held to everything a step is. */
CHECK("active", CONFIG_ZMK_SPLIT_BLE_PREF_TIMEOUT, CONFIG_ZMK_SPLIT_BLE_PREF_LATENCY,
      CONFIG_ZMK_SPLIT_BLE_PREF_INT);
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_COMPUTED_STEPS)
/* Worked out by compute_table() on the ladder's queue, from the targets below,
 * the link to the computer and any step set by hand (fixes[]); only the timers
 * come from Kconfig as they are. */
#define TABLE
#define AFTER(n) {.after_ms = CONFIG_ZMK_SPLIT_CONN_LADDER_STEP##n##_AFTER_IDLE_MS}
#else
#define TABLE const
#endif

static TABLE struct step step_active = {
	.interval = CONFIG_ZMK_SPLIT_BLE_PREF_INT,
	.latency = CONFIG_ZMK_SPLIT_BLE_PREF_LATENCY,
	.timeout = CONFIG_ZMK_SPLIT_BLE_PREF_TIMEOUT,
};

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_COMPUTED_STEPS)
static struct step idle_steps[] = {
	AFTER(1),
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 2
	AFTER(2),
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 3
	AFTER(3),
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 4
	AFTER(4),
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 5
	AFTER(5),
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 6
	AFTER(6),
#endif
};
#else
static const struct step idle_steps[] = {
	STEP(1),
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 2
	STEP(2),
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 3
	STEP(3),
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 4
	STEP(4),
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 5
	STEP(5),
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 6
	STEP(6),
#endif
};

CHECK_STEP(1);
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 2
CHECK_STEP(2);
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 3
CHECK_STEP(3);
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 4
CHECK_STEP(4);
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 5
CHECK_STEP(5);
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 6
CHECK_STEP(6);
#endif
#endif /* CONFIG_ZMK_SPLIT_CONN_LADDER_COMPUTED_STEPS */

#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 2
BUILD_ASSERT(CONFIG_ZMK_SPLIT_CONN_LADDER_STEP2_AFTER_IDLE_MS >
		     CONFIG_ZMK_SPLIT_CONN_LADDER_STEP1_AFTER_IDLE_MS,
	     "step 2 must come after step 1");
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 3
BUILD_ASSERT(CONFIG_ZMK_SPLIT_CONN_LADDER_STEP3_AFTER_IDLE_MS >
		     CONFIG_ZMK_SPLIT_CONN_LADDER_STEP2_AFTER_IDLE_MS,
	     "step 3 must come after step 2");
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 4
BUILD_ASSERT(CONFIG_ZMK_SPLIT_CONN_LADDER_STEP4_AFTER_IDLE_MS >
		     CONFIG_ZMK_SPLIT_CONN_LADDER_STEP3_AFTER_IDLE_MS,
	     "step 4 must come after step 3");
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 5
BUILD_ASSERT(CONFIG_ZMK_SPLIT_CONN_LADDER_STEP5_AFTER_IDLE_MS >
		     CONFIG_ZMK_SPLIT_CONN_LADDER_STEP4_AFTER_IDLE_MS,
	     "step 5 must come after step 4");
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 6
BUILD_ASSERT(CONFIG_ZMK_SPLIT_CONN_LADDER_STEP6_AFTER_IDLE_MS >
		     CONFIG_ZMK_SPLIT_CONN_LADDER_STEP5_AFTER_IDLE_MS,
	     "step 6 must come after step 5");
#endif

#define N_IDLE ((int)ARRAY_SIZE(idle_steps))

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_USB_POWER_STEPS) &&                                   \
	IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_COMPUTED_STEPS)
/* The USB-power column, computed with the battery one. Only idle_steps[]
 * carries the timers: one timer drives both columns. */
static struct step usb_steps[ARRAY_SIZE(idle_steps)];
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_USB_POWER_STEPS)
/*
 * The USB-power column: the same steps on the same timers, with the
 * parameters used in AUTO mode on USB power. Checked exactly like the battery
 * column.
 */
#define USB_STEP(n)                                                                          \
	{                                                                                    \
		.after_ms = CONFIG_ZMK_SPLIT_CONN_LADDER_STEP##n##_AFTER_IDLE_MS,            \
		.interval = CONFIG_ZMK_SPLIT_CONN_LADDER_STEP##n##_USB_POWER_INTERVAL,       \
		.latency = CONFIG_ZMK_SPLIT_CONN_LADDER_STEP##n##_USB_POWER_LATENCY,         \
		.timeout = CONFIG_ZMK_SPLIT_CONN_LADDER_STEP##n##_USB_POWER_SUPERVISION_TIMEOUT, \
	}

#define CHECK_USB_STEP(n)                                                                    \
	CHECK("step " #n " on USB power",                                                    \
	      CONFIG_ZMK_SPLIT_CONN_LADDER_STEP##n##_USB_POWER_SUPERVISION_TIMEOUT,          \
	      CONFIG_ZMK_SPLIT_CONN_LADDER_STEP##n##_USB_POWER_LATENCY,                      \
	      CONFIG_ZMK_SPLIT_CONN_LADDER_STEP##n##_USB_POWER_INTERVAL);                    \
	CHECK_SWITCH("step " #n " on USB power",                                             \
		     CONFIG_ZMK_SPLIT_CONN_LADDER_STEP##n##_USB_POWER_LATENCY,               \
		     CONFIG_ZMK_SPLIT_CONN_LADDER_STEP##n##_USB_POWER_INTERVAL)

static const struct step usb_steps[] = {
	USB_STEP(1),
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 2
	USB_STEP(2),
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 3
	USB_STEP(3),
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 4
	USB_STEP(4),
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 5
	USB_STEP(5),
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 6
	USB_STEP(6),
#endif
};

CHECK_USB_STEP(1);
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 2
CHECK_USB_STEP(2);
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 3
CHECK_USB_STEP(3);
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 4
CHECK_USB_STEP(4);
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 5
CHECK_USB_STEP(5);
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 6
CHECK_USB_STEP(6);
#endif
#endif /* CONFIG_ZMK_SPLIT_CONN_LADDER_USB_POWER_STEPS */

/*
 * Level 0 is ACTIVE, 1..N_IDLE index idle_steps[level - 1] -- the battery
 * column -- and N_IDLE+1..2*N_IDLE the USB-power column, usb_steps[] at the
 * same depths. One number per parameter set keeps everything below column-blind:
 * a level is simply what the link runs.
 *
 * current_level is what the link is CONFIRMED to be running, -1 when that is
 * unknown. pending_level is what was asked for and not yet seen confirmed.
 *
 * Splitting the two matters: committing the level when the request is issued
 * is a lie whenever the peripheral misses the PDU. We would then believe the
 * link is at ACTIVE, every later apply_level() would early-return because the
 * level already matches, and nothing would ever put it right -- the central
 * sitting there waiting for a confirmation that is never coming while the link
 * is still down at the deep step. Confirmation comes from le_param_updated()
 * and is only accepted when the reported parameters are the ones we asked for;
 * if it does not come in time the level goes back to unknown so the next pass
 * re-issues.
 *
 * superseded_level is a request that was still pending when a newer one was
 * issued -- in practice a descent overtaken by a keypress, or by plugging in.
 * The controller does not cancel it: it runs both procedures in order, so the
 * older parameters are reported first. Recognising them keeps that ordinary
 * sequence from reading as "unexpected parameters" and costing a third
 * procedure.
 *
 * All of this is written ONLY by ladder_work_cb, on the ladder's own work
 * queue. The Bluetooth callbacks -- on the RX thread, a disconnect on the
 * system work queue -- do nothing but raise atomic flags, because the
 * compare-then-set inside apply_level() is not atomic: a reconnect landing
 * mid-update would otherwise leave our belief and the link's reality
 * permanently out of step.
 */
static int current_level;
static int pending_level = -1;
static int superseded_level = -1;
/* Depth caps (0..N_IDLE) set by link losses, one per column: a depth that
 * fails on battery parameters says nothing about the USB-power ones. */
static int max_level = N_IDLE;
static int max_level_usb = N_IDLE;

/* Consecutive refused updates; widens the retry. Handler-only. */
static uint8_t reject_streak;

/* 32-bit uptime: the read is atomic on a 32-bit target and unsigned
 * subtraction stays correct across the ~49-day wrap. */
static uint32_t idle_since;
static uint32_t update_issued_at;

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_BACKOFF_AFTER_LINK_LOSS)
static uint32_t stable_since;
static uint32_t stable_required = BACKOFF_RESTORE_AFTER_MS;
static uint8_t backoff_count;
static uint32_t lost_at;
static int lost_level;
static bool loss_armed;
/* The latest loss was the other half closing the link on purpose: the link
 * that comes back keeps the stability clock it had. */
static bool last_loss_was_peer_close;
/* When the other half was last heard before the loss: the supervision
 * timeout runs from its last packet. */
static uint32_t last_heard;
/* A loss that came back and waits for the other half's report. */
static bool judge_pending;
static uint32_t judge_back_at;
static uint32_t judge_until;
#endif

/* The other half's uptime, from its power report, and when that arrived.
 * Written from the Bluetooth RX thread; stamp last, the handler reads it
 * first. */
static atomic_t peer_uptime;
static atomic_t peer_report_at;
/* A report has come since the split link last went down: cleared in the
 * disconnect callback, set by the report. Whether the report is the new
 * link's is this flag, not a comparison of stamps -- one from a link that
 * lasted weeks would pass that after 2^31 ms. */
static atomic_t peer_report_fresh;

static atomic_t link_reset;
static atomic_t link_lost;
/* ... and the latest loss was the other half closing the link. */
static atomic_t loss_was_peer_close;
/* k_uptime_get_32() at the latest split connect / disconnect, taken in the
 * callbacks, where it happened -- not when the handler got round to it. */
static atomic_t reset_stamp;
static atomic_t lost_stamp;
static atomic_t update_in_flight;
static atomic_t update_confirmed;
/* What le_param_updated() last reported for the split link. Checked against
 * the step we asked for: le_param_updated also fires for updates nobody here
 * requested, and for a stale one that lands after we have already moved on. */
static atomic_t applied_interval;
static atomic_t applied_latency;
static atomic_t applied_timeout;
/* Bumped after each store, so the handler can tell that the three values it
 * just read came from one report. The RX thread is cooperative and the handler
 * is not, so without this a second report landing between two of the reads
 * could make an unrelated pair look like a match. */
static atomic_t applied_gen;

/*
 * Whether the ladder may step down at all. Written from the &ladder behavior
 * on the system work queue, read by the handler. Every write reschedules the
 * handler and the handler re-reads it on every run, so a plain atomic is all
 * the synchronisation this needs.
 */
#define BOOT_MODE                                                                  \
	(IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_BOOT_IN_AUTO_MODE) ? LADDER_AUTO : LADDER_ON)

static atomic_t ladder_mode = ATOMIC_INIT(BOOT_MODE);

/* What the mode and the power source add up to. Handler-only state. */
enum gate {
	GATE_UNKNOWN = -1,
	/* ON, or AUTO on battery: the battery column */
	GATE_BATTERY_STEPS,
	/* AUTO on USB power: the USB-power column */
	GATE_USB_POWER_STEPS,
	/* AUTO on USB power, held at ACTIVE: USB_POWER_STEPS off, or the other
	 * half on USB power too */
	GATE_USB_POWER_ACTIVE,
	/* OFF: held at ACTIVE */
	GATE_OFF,
};

static enum gate last_gate = GATE_UNKNOWN;

static bool is_usb_power_level(int level)
{
	return level > N_IDLE;
}

/* 0 for ACTIVE, 1..N_IDLE for a step of either column; -1 stays -1. */
static int depth_of(int level)
{
	return is_usb_power_level(level) ? level - N_IDLE : level;
}

static int level_for(bool usb_power, int depth)
{
	return usb_power && depth > 0 ? depth + N_IDLE : depth;
}

static const char *column_tag(int level)
{
	return is_usb_power_level(level) ? " on USB power" : "";
}

static const struct step *step_at(int level)
{
	if (level <= 0) {
		return &step_active;
	}
#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_USB_POWER_STEPS)
	if (is_usb_power_level(level)) {
		return &usb_steps[level - N_IDLE - 1];
	}
#endif
	return &idle_steps[level - 1];
}

static bool same_params(const struct step *a, const struct step *b)
{
	return a->interval == b->interval && a->latency == b->latency && a->timeout == b->timeout;
}

/* What the ladder is aiming at, whether or not it has landed yet. */
static int effective_level(void)
{
	return pending_level >= 0 ? pending_level : current_level;
}

/* Whether le_param_updated() last reported exactly this step. False rather
 * than a torn answer if a report lands in the middle of the read. */
static bool applied_is(const struct step *s)
{
	for (int try = 0; try < 3; try++) {
		atomic_val_t gen = atomic_get(&applied_gen);
		bool match = (uint16_t)atomic_get(&applied_interval) == s->interval &&
			     (uint16_t)atomic_get(&applied_latency) == s->latency &&
			     (uint16_t)atomic_get(&applied_timeout) == s->timeout;

		if (gen == atomic_get(&applied_gen)) {
			return match;
		}
	}
	return false;
}

static void count_central(struct bt_conn *conn, void *data)
{
	struct bt_conn_info info;

	if (bt_conn_get_info(conn, &info) || info.type != BT_CONN_TYPE_LE ||
	    info.state != BT_CONN_STATE_CONNECTED) {
		return;
	}
	if (info.role == BT_CONN_ROLE_CENTRAL) {
		(*(int *)data)++;
	}
}

/* Asked rather than remembered: a cached up/down flag can go stale when a
 * disconnect and a reconnect both land between two runs of the handler. */
static bool split_link_up(void)
{
	int n = 0;

	bt_conn_foreach(BT_CONN_TYPE_LE, count_central, &n);
	return n > 0;
}

/*
 * The link to the computer shares this half's radio with the split link, and
 * an ACTIVE split interval that is a multiple of the host's -- or the other
 * way round -- keeps the two links' events in one place relative to each
 * other for as long as the clocks take to drift apart: README, "The interval
 * and the computer's own link". Checked wherever the host's interval shows
 * up, so a host that picks a new one later is caught too.
 */
static void check_host_interval(uint16_t split, uint16_t host)
{
	if (host && (split % host == 0 || host % split == 0)) {
		LOG_WRN("split interval %u.%02u ms and the host's %u.%02u ms are multiples: "
			"the two links will collide in long stretches",
			INT_MS_W(split), INT_MS_F(split), INT_MS_W(host), INT_MS_F(host));
	}
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_COMPUTED_STEPS)
/*
 * The computed steps (ZMK_SPLIT_CONN_LADDER_COMPUTED_STEPS, the default): the
 * same ladder, its parameters worked out here from a few budgets instead of
 * read from a table, and worked out again whenever the link to the computer,
 * the output or the other half's power changes.
 *
 * ACTIVE. The interval while typing:
 *
 *   the output on USB: USB_OUT_INT, 8.75 ms. The central half is on a cable
 *   then, so its own cost does not count, and there is no host link on the
 *   radio to keep out of the way of;
 *
 *   a pointer whose motion goes to the computer over Bluetooth, and a host
 *   link: an interval chosen against it (README, "The interval and the
 *   computer's own link") --
 *
 *     below it, not shorter than ACTIVE_FLOOR (12.5 ms), sharing a factor of
 *     at least W_UNITS with it and not dividing it: the two schedules then
 *     meet only in single, spread overlaps, and the host link wins every
 *     one -- 10 units (12.5 ms) against a Mac's 12;
 *
 *     otherwise above it, sharing no factor with it: overlaps are spread
 *     evenly and each costs the host link one event -- 13 (16.25 ms)
 *     against a PC's 6 (7.5 ms);
 *
 *   otherwise, and when nothing fits, ACTIVE_DEFAULT_INTERVAL (16.25 ms).
 *
 * Never longer than KEY_CAP, the delay the other half's keys can take. The
 * latency makes the other half's attendance -- the delay of an LED or layer
 * push while typing -- come nearest ACTIVE_PERIPHERAL_SYNC_DELAY_MS, and is 0
 * while the other half reports USB power: then it has nothing to save.
 *
 * Steps. The central pays per connection event, so the interval is the
 * saving, and the latency sets what leaving the step costs. Each step takes
 * its latency from a fixed series (or STEP*_LATENCY) and gets the longest
 * interval that returns within its budget at that latency --
 * RETURN_BUDGET_MS, or DEEPEST_STEP_RETURN_BUDGET_MS for the deepest -- keeps
 * MIN_SUPERVISION_WINDOWS attendances inside its timeout and passes the
 * Bluetooth rule, on a number that shares no factor with 2, 3 or the host's
 * interval: a Mac's 12 units, a PC's 6, whatever this computer uses. The
 * series falls as the steps deepen, so every step saves more than the one
 * above and none is slower to leave. Should the host leave a step nothing
 * longer than the step above, it repeats that one: the link has nothing to
 * switch to ("parameters already in place").
 *
 * A step set by hand (struct step_fix) runs its STEP*_INTERVAL as it is, at
 * its STEP*_LATENCY or the series one; the build holds it to the same budget,
 * MIN_SUPERVISION_WINDOWS and Bluetooth rules, to being longer than any
 * ACTIVE, and to going deeper than the step above.
 *
 * The USB-power column keeps ACTIVE's interval, which delays no key. All a
 * USB-power step changes is the latency, and all that costs is the other
 * half's sync delay, which is its attendance. The latency rises in equal
 * steps from ACTIVE's to the one whose attendance is
 * USB_POWER_PERIPHERAL_SYNC_DELAY_MS, reached at the deepest step. The return
 * budgets do not apply -- leaving a USB-power step delays no key either --
 * only the guards.
 *
 * Integer arithmetic only, rule for rule the reference the tests hold it to.
 */
#define RETURN_BUDGET_MS           CONFIG_ZMK_SPLIT_CONN_LADDER_RETURN_BUDGET_MS
/* 0: the deepest step has no budget of its own and takes RETURN_BUDGET_MS. */
#define DEEPEST_STEP_OWN_BUDGET_MS CONFIG_ZMK_SPLIT_CONN_LADDER_DEEPEST_STEP_RETURN_BUDGET_MS
#define DEEPEST_STEP_BUDGET_MS                                                                     \
	(DEEPEST_STEP_OWN_BUDGET_MS ? DEEPEST_STEP_OWN_BUDGET_MS : RETURN_BUDGET_MS)
#define STEPS_SUPERVISION_TIMEOUT             CONFIG_ZMK_SPLIT_CONN_LADDER_STEPS_SUPERVISION_TIMEOUT
#define ACTIVE_SUPERVISION_TIMEOUT      CONFIG_ZMK_SPLIT_CONN_LADDER_ACTIVE_SUPERVISION_TIMEOUT
#define ACTIVE_DEFAULT_INTERVAL         CONFIG_ZMK_SPLIT_CONN_LADDER_ACTIVE_DEFAULT_INTERVAL
#define ACTIVE_PERIPHERAL_SYNC_DELAY_MS CONFIG_ZMK_SPLIT_CONN_LADDER_ACTIVE_PERIPHERAL_SYNC_DELAY_MS
#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_USB_POWER_STEPS)
#define USB_POWER_PERIPHERAL_SYNC_DELAY_MS                                                         \
	CONFIG_ZMK_SPLIT_CONN_LADDER_USB_POWER_PERIPHERAL_SYNC_DELAY_MS
#endif
#define USB_OUT_INT  7    /* 8.75 ms: ACTIVE with the output on USB */
#define W_UNITS      2    /* one overlap: two reservations, ~2.5 ms */
#define KEY_CAP      16   /* 20 ms: the longest ACTIVE */
#define ACTIVE_FLOOR 10   /* 12.5 ms: the shortest ACTIVE below a host's interval */
#define STEP_MIN_INT (KEY_CAP + 1)
#define LONGEST_INT  3200 /* 4 s, Bluetooth's limit */
#define LONGEST_LAT  499  /* ditto */
#define N_STEPS      CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT
/* A fresh link to the computer is left this long before the steps follow it:
 * the computer adjusts it in the first seconds, and ZMK asks for its own
 * parameters 5 s after the connect. A lost one gets as long to come back. */
#define HOST_SETTLE_MS 6000
/* Anything else -- new parameters on a settled link, the output moving, the
 * other half's power -- is followed this soon: long enough to fold one burst
 * of events into a single recompute. */
#define HOST_QUICK_MS  500

BUILD_ASSERT(RETURN_BUDGET_MS <= MAX_RETURN_MS,
	     "ZMK_SPLIT_CONN_LADDER_RETURN_BUDGET_MS must not exceed "
	     "ZMK_SPLIT_CONN_LADDER_MAX_RETURN_MS");
BUILD_ASSERT(DEEPEST_STEP_OWN_BUDGET_MS == 0 || DEEPEST_STEP_OWN_BUDGET_MS >= RETURN_BUDGET_MS,
	     "ZMK_SPLIT_CONN_LADDER_DEEPEST_STEP_RETURN_BUDGET_MS must be 0 or not below "
	     "ZMK_SPLIT_CONN_LADDER_RETURN_BUDGET_MS");
BUILD_ASSERT(DEEPEST_STEP_OWN_BUDGET_MS <= MAX_RETURN_MS,
	     "ZMK_SPLIT_CONN_LADDER_DEEPEST_STEP_RETURN_BUDGET_MS must not exceed "
	     "ZMK_SPLIT_CONN_LADDER_MAX_RETURN_MS");
#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_USB_POWER_STEPS)
BUILD_ASSERT(USB_POWER_PERIPHERAL_SYNC_DELAY_MS == 0 ||
		     ACTIVE_PERIPHERAL_SYNC_DELAY_MS <= USB_POWER_PERIPHERAL_SYNC_DELAY_MS,
	     "ZMK_SPLIT_CONN_LADDER_ACTIVE_PERIPHERAL_SYNC_DELAY_MS must not exceed "
	     "ZMK_SPLIT_CONN_LADDER_USB_POWER_PERIPHERAL_SYNC_DELAY_MS: the USB power steps end "
	     "there");
#endif

/*
 * The link is created on ZMK_SPLIT_BLE_PREF_* and moved to the computed
 * ACTIVE after NEW_LINK_SETTLE_MS; nothing else uses them in this mode. Values
 * other than ZMK's own defaults are left over from the table mode.
 */
#if CONFIG_ZMK_SPLIT_BLE_PREF_INT != 6 || CONFIG_ZMK_SPLIT_BLE_PREF_LATENCY != 30 ||             \
	CONFIG_ZMK_SPLIT_BLE_PREF_TIMEOUT != 400
#warning "zmk-split-conn-ladder: ZMK_SPLIT_BLE_PREF_* differ from ZMK's defaults, but the computed steps do not use them -- the link is only created on them and moved to the computed ACTIVE a few seconds later. Remove those lines."
#endif

/*
 * The latency series, m long: 13, 3, 1, 0 up to four, 13, 5, 3, 1, 0 for five
 * and 13, 7, 5, 3, 1, 0 for six. With a budget of its own for the deepest
 * step (DEEPEST_STEP_RETURN_BUDGET_MS), that step is latency 0 and the others
 * take the series one step shorter. The first latency keeps step 1 at
 * 31.25 ms within 1.5 s; each further one roughly doubles the interval the
 * budget allows.
 */
#define SERIES_OF(m, k)                                                                        \
	((m) <= 4   ? ((k) == 1 ? 13 : (k) == 2 ? 3 : (k) == 3 ? 1 : 0)                           \
	 : (m) == 5 ? ((k) == 1 ? 13 : (k) == 2 ? 5 : (k) == 3 ? 3 : (k) == 4 ? 1 : 0)             \
		    : ((k) == 1 ? 13 : (k) == 2 ? 7 : (k) == 3 ? 5 : (k) == 4 ? 3 : (k) == 5 ? 1 : 0))
#define SERIES_LAT(k)                                                                          \
	(DEEPEST_STEP_OWN_BUDGET_MS && (k) == N_STEPS                                          \
		 ? 0                                                                           \
		 : SERIES_OF(DEEPEST_STEP_OWN_BUDGET_MS ? N_STEPS - 1 : N_STEPS, (k)))

/* Step k's return budget: the deepest may have one of its own. */
#define BUDGET(k) ((k) == N_STEPS ? DEEPEST_STEP_BUDGET_MS : RETURN_BUDGET_MS)

/*
 * The longest interval at latency `lat` that returns within `r` ms, keeps
 * MIN_SUPERVISION_WINDOWS attendances inside timeout `to`, and passes the
 * Bluetooth rule timeout * 4 > (1 + latency) * interval.
 */
#define LONGEST_FOR(lat, r, to)                                                                \
	MIN(MIN((r) * 100 / (125 * (3 * (lat) + 8)),                                           \
		(to) * 8 / (MIN_SUPERVISION_WINDOWS * (1 + (lat)))),                           \
	    MIN(((to) * 4 - 1) / (1 + (lat)), LONGEST_INT))

/* The largest number at most t that shares no factor with 2 or 3. */
#define CLEAN6(t)                                                                              \
	((t) - ((t) % 6 == 0 ? 1 : (t) % 6 == 2 ? 1 : (t) % 6 == 3 ? 2 : (t) % 6 == 4 ? 3 : 0))

/*
 * Steps set by hand. In this mode the per-step symbols default to "computed"
 * -- STEP*_INTERVAL and STEP*_SUPERVISION_TIMEOUT to 0, STEP*_LATENCY to -1,
 * and for USB power STEP*_USB_POWER_LATENCY to -1 and
 * STEP*_USB_POWER_SUPERVISION_TIMEOUT to 0 -- so any other value is one the
 * .conf gave, and it overrides the computation for that one parameter of that
 * step.
 */
struct step_fix {
	int16_t in;  /* 0: computed */
	int16_t lat; /* -1: the series */
	int16_t to;  /* 0: STEPS_SUPERVISION_TIMEOUT */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_USB_POWER_STEPS)
	int16_t usb_lat; /* -1: on the ramp */
	int16_t usb_to;  /* 0: STEPS_SUPERVISION_TIMEOUT */
#endif
};

/* STEP_CFG(2, LATENCY) is CONFIG_ZMK_SPLIT_CONN_LADDER_STEP2_LATENCY. */
#define STEP_CFG(n, p) CONFIG_ZMK_SPLIT_CONN_LADDER_STEP##n##_##p

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_USB_POWER_STEPS)
#define FIX_USB(n)                                                                             \
	, .usb_lat = STEP_CFG(n, USB_POWER_LATENCY),                                           \
	  .usb_to = STEP_CFG(n, USB_POWER_SUPERVISION_TIMEOUT)
#else
#define FIX_USB(n)
#endif
#define FIX(n)                                                                                 \
	{.in = STEP_CFG(n, INTERVAL),                                                          \
	 .lat = STEP_CFG(n, LATENCY),                                                          \
	 .to = STEP_CFG(n, SUPERVISION_TIMEOUT) FIX_USB(n)}

/* A step's timeout: its own when set, STEPS_SUPERVISION_TIMEOUT otherwise. */
#define FIX_TO(to) ((to) > 0 ? (to) : STEPS_SUPERVISION_TIMEOUT)

/* What step n runs with no host link in the way -- the build checks it, and
 * a host link can only make a computed interval shorter. */
#define FIX_LAT(n)  (STEP_CFG(n, LATENCY) >= 0 ? STEP_CFG(n, LATENCY) : SERIES_LAT(n))
#define FIX_TO_N(n) FIX_TO(STEP_CFG(n, SUPERVISION_TIMEOUT))
#define NOHOST_INT(n)                                                                          \
	(STEP_CFG(n, INTERVAL) > 0 ? STEP_CFG(n, INTERVAL)                                     \
				   : CLEAN6(LONGEST_FOR(FIX_LAT(n), BUDGET(n), FIX_TO_N(n))))

/*
 * Every step, set or computed, at build time. A computed interval passes the
 * guards by construction; what can fail is the latency (so high that nothing
 * longer than ACTIVE returns in time) and a set interval.
 */
#define CHECK_FIX(n)                                                                           \
	BUILD_ASSERT(STEP_CFG(n, INTERVAL) == 0 || STEP_CFG(n, INTERVAL) >= 6,                 \
		     "ZMK_SPLIT_CONN_LADDER_STEP" #n "_INTERVAL: 0 (computed) or 6..3200");    \
	BUILD_ASSERT(STEP_CFG(n, SUPERVISION_TIMEOUT) == 0 ||                                  \
			     STEP_CFG(n, SUPERVISION_TIMEOUT) >= 10,                           \
		     "ZMK_SPLIT_CONN_LADDER_STEP" #n "_SUPERVISION_TIMEOUT: 0 (the common "    \
		     "ZMK_SPLIT_CONN_LADDER_STEPS_SUPERVISION_TIMEOUT) or 10..3200");                \
	BUILD_ASSERT(STEP_CFG(n, INTERVAL) == 0 || STEP_CFG(n, INTERVAL) >= STEP_MIN_INT,      \
		     "step " #n ": ZMK_SPLIT_CONN_LADDER_STEP" #n "_INTERVAL must be at "      \
		     "least 17 -- a step shorter than 20 ms is not deeper than every ACTIVE"); \
	BUILD_ASSERT(STEP_CFG(n, INTERVAL) > 0 || NOHOST_INT(n) >= STEP_MIN_INT,               \
		     "step " #n ": at this latency nothing longer than 20 ms returns "         \
		     "within the step's budget and keeps MIN_SUPERVISION_WINDOWS in its "      \
		     "supervision timeout -- lower ZMK_SPLIT_CONN_LADDER_STEP" #n              \
		     "_LATENCY, raise the budget, or raise the step's supervision timeout "    \
		     "(or lower ZMK_SPLIT_CONN_LADDER_MIN_SUPERVISION_WINDOWS)");              \
	BUILD_ASSERT(STEP_CFG(n, INTERVAL) == 0 ||                                             \
			     STEP_CFG(n, INTERVAL) * 125 * (3 * FIX_LAT(n) + 8) <= BUDGET(n) * 100, \
		     "step " #n ": the set interval returns over the step's budget -- "        \
		     "lower ZMK_SPLIT_CONN_LADDER_STEP" #n "_INTERVAL or its latency");        \
	CHECK("step " #n, FIX_TO_N(n), FIX_LAT(n), NOHOST_INT(n))

/* Deeper than the step above: a longer interval, or the very same step. */
#define SAME_STEP(n, m)                                                                  \
	(NOHOST_INT(n) == NOHOST_INT(m) && FIX_LAT(n) == FIX_LAT(m) && FIX_TO_N(n) == FIX_TO_N(m))
#define CHECK_DEEPER(n, m)                                                               \
	BUILD_ASSERT(NOHOST_INT(n) > NOHOST_INT(m) || SAME_STEP(n, m),                   \
		     "step " #n " is not deeper than step " #m ": its interval must be "  \
		     "longer, or the whole step the same")

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_USB_POWER_STEPS)
/* A USB-power latency set by hand is checked on ACTIVE_DEFAULT_INTERVAL, the
 * ACTIVE with no host link to avoid; where ACTIVE is longer, compute_table()
 * holds it to what the guards allow. */
#define FIX_USB_IN(n) (STEP_CFG(n, USB_POWER_LATENCY) >= 0 ? ACTIVE_DEFAULT_INTERVAL : 0)
#define CHECK_FIX_USB(n)                                                                       \
	BUILD_ASSERT(STEP_CFG(n, USB_POWER_SUPERVISION_TIMEOUT) == 0 ||                        \
			     STEP_CFG(n, USB_POWER_SUPERVISION_TIMEOUT) >= 10,                 \
		     "ZMK_SPLIT_CONN_LADDER_STEP" #n "_USB_POWER_SUPERVISION_TIMEOUT: 0 (the " \
		     "common ZMK_SPLIT_CONN_LADDER_STEPS_SUPERVISION_TIMEOUT) or 10..3200");         \
	CHECK("step " #n " on USB power", FIX_TO(STEP_CFG(n, USB_POWER_SUPERVISION_TIMEOUT)),  \
	      MAX(STEP_CFG(n, USB_POWER_LATENCY), 0), FIX_USB_IN(n));                          \
	CHECK_SWITCH("step " #n " on USB power", MAX(STEP_CFG(n, USB_POWER_LATENCY), 0),       \
		     FIX_USB_IN(n))
#else
#define CHECK_FIX_USB(n) BUILD_ASSERT(1, "")
#endif

static const struct step_fix fixes[] = {
	FIX(1),
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 2
	FIX(2),
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 3
	FIX(3),
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 4
	FIX(4),
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 5
	FIX(5),
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 6
	FIX(6),
#endif
};

BUILD_ASSERT(ARRAY_SIZE(fixes) == ARRAY_SIZE(idle_steps), "one entry per step");

CHECK_FIX(1);
CHECK_FIX_USB(1);
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 2
CHECK_FIX(2);
CHECK_FIX_USB(2);
CHECK_DEEPER(2, 1);
#if SAME_STEP(2, 1)
#warning "zmk-split-conn-ladder: step 2 repeats step 1"
#endif
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 3
CHECK_FIX(3);
CHECK_FIX_USB(3);
CHECK_DEEPER(3, 2);
#if SAME_STEP(3, 2)
#warning "zmk-split-conn-ladder: step 3 repeats step 2"
#endif
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 4
CHECK_FIX(4);
CHECK_FIX_USB(4);
CHECK_DEEPER(4, 3);
#if SAME_STEP(4, 3)
#warning "zmk-split-conn-ladder: step 4 repeats step 3"
#endif
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 5
CHECK_FIX(5);
CHECK_FIX_USB(5);
CHECK_DEEPER(5, 4);
#if SAME_STEP(5, 4)
#warning "zmk-split-conn-ladder: step 5 repeats step 4"
#endif
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEP_COUNT >= 6
CHECK_FIX(6);
CHECK_FIX_USB(6);
CHECK_DEEPER(6, 5);
#if SAME_STEP(6, 5)
#warning "zmk-split-conn-ladder: step 6 repeats step 5"
#endif
#endif

enum active_why {
	WHY_KEYS,
	WHY_NO_HOST,
	WHY_USB,
	WHY_BELOW,
	WHY_ABOVE,
	WHY_NONE_FITS,
};

static const char *const why_text[] = {
	[WHY_KEYS] = "no pointer",
	[WHY_NO_HOST] = "no host link",
	[WHY_USB] = "output on USB",
	[WHY_BELOW] = "below the host's, not dividing it",
	[WHY_ABOVE] = "above the host's, no common factor",
	[WHY_NONE_FITS] = "nothing fits the host's",
};

/* The greatest common divisor. */
static int common_factor(int a, int b)
{
	while (b) {
		const int r = a % b;

		a = b;
		b = r;
	}
	return a;
}

static bool step_int_ok(int n, int host)
{
	return n >= 6 && n <= LONGEST_INT && common_factor(n, 6) == 1 &&
	       (host == 0 || common_factor(n, host) == 1);
}

/* The largest interval at most t that shares no factor with 2, 3 or the
 * host's; below 6 when there is none. */
static int snap_down(int t, int host)
{
	t = MIN(t, LONGEST_INT);
	while (t >= 6 && !step_int_ok(t, host)) {
		t--;
	}
	return t;
}

static int pick_active(int host, bool ble_out, enum active_why *why)
{
	if (!ble_out) {
		*why = WHY_USB;
		return USB_OUT_INT;
	}
	if (!IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_ACTIVE_AVOIDS_HOST_INTERVAL)) {
		*why = WHY_KEYS;
		return ACTIVE_DEFAULT_INTERVAL;
	}
	if (host == 0) {
		*why = WHY_NO_HOST;
		return ACTIVE_DEFAULT_INTERVAL;
	}
	for (int n = MIN(host - 1, KEY_CAP); n >= ACTIVE_FLOOR; n--) {
		if (host % n && common_factor(n, host) >= W_UNITS) {
			*why = WHY_BELOW;
			return n;
		}
	}
	for (int n = KEY_CAP; n > host; n--) {
		if (common_factor(n, host) == 1) {
			*why = WHY_ABOVE;
			return n;
		}
	}
	*why = WHY_NONE_FITS;
	return ACTIVE_DEFAULT_INTERVAL;
}

/* The build-time guards -- CHECK_WINDOWS, CHECK_SWITCH, CHECK_SPEC -- at run
 * time. */
static bool params_fit(int n, int lat, int to)
{
	return to * 8 >= MIN_SUPERVISION_WINDOWS * n * (1 + lat) &&
	       n * 125 * (3 * lat + 8) <= MAX_RETURN_MS * 100 && to * 4 > (1 + lat) * n;
}

static bool set_params(struct step *s, int in, int lat, int to)
{
	if (s->interval == in && s->latency == lat && s->timeout == to) {
		return false;
	}
	s->interval = (uint16_t)in;
	s->latency = (uint16_t)lat;
	s->timeout = (uint16_t)to;
	return true;
}

/* ACTIVE's latency at interval n: the attendance nearest ACTIVE_PERIPHERAL_SYNC_DELAY_MS -- n
 * units of 1.25 ms, rounded to the nearest whole event -- within the guards;
 * 0 when the other half is on USB power. */
static int active_lat(int n, bool peer_usb)
{
	if (peer_usb) {
		return 0;
	}

	int lat = MIN(MAX((ACTIVE_PERIPHERAL_SYNC_DELAY_MS * 8 + 5 * n) / (10 * n) - 1, 0), LONGEST_LAT);

	while (lat > 0 && !params_fit(n, lat, ACTIVE_SUPERVISION_TIMEOUT)) {
		lat--;
	}
	return lat;
}

static bool set_active(int n, bool peer_usb)
{
	return set_params(&step_active, n, active_lat(n, peer_usb), ACTIVE_SUPERVISION_TIMEOUT);
}

/* The return budget of step k, 1-based. */
static int budget_ms(int k)
{
	return BUDGET(k);
}

/* Step k's latency: set, or the series. */
static int step_lat(int k)
{
	const int lat = fixes[k - 1].lat;

	return lat >= 0 ? lat : SERIES_LAT(k);
}

static int longest_for(int lat, int r, int to)
{
	return LONGEST_FOR(lat, r, to);
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_USB_POWER_STEPS)
/* The largest latency at interval t that returns inside r ms and keeps
 * MIN_SUPERVISION_WINDOWS attendances inside timeout `to`; -1 when not even 0 does. */
static int lat_for(int t, int r, int to)
{
	const int x = r * 100 / (125 * t);

	if (x < 8) {
		return -1;
	}

	int lat = MIN(MIN((x - 8) / 3, to * 8 / (MIN_SUPERVISION_WINDOWS * t) - 1), LONGEST_LAT);

	while (lat >= 0 && to * 4 <= (1 + lat) * t) {
		lat--;
	}
	return lat;
}

static int usb_power_timeout(int k)
{
	return FIX_TO(fixes[k - 1].usb_to);
}

/* The longest latency USB-power step k can have on ACTIVE's interval: the
 * guards alone, since leaving a USB-power step delays no key. */
static int usb_power_max_latency(int k)
{
	return lat_for(step_active.interval, MAX_RETURN_MS, usb_power_timeout(k));
}

/* What USB-power step k, set by hand, runs: its latency as far as the
 * guards allow, and never below ACTIVE's -- there it is ACTIVE. */
static int usb_power_set_latency(int k)
{
	return MAX(MIN(fixes[k - 1].usb_lat, usb_power_max_latency(k)), step_active.latency);
}

/* l0 moved num/den of the way to l1, rounded toward l0. */
static int ramp(int l0, int l1, int num, int den)
{
	return l1 >= l0 ? l0 + (l1 - l0) * num / den : l0 - (l0 - l1) * num / den;
}
#endif

/* Fills step_active, idle_steps[] and usb_steps[]; true if anything moved. */
static bool compute_table(int host, bool ble_out, bool peer_usb, enum active_why *why)
{
	bool changed = set_active(pick_active(host, ble_out, why), peer_usb);
	const struct step *prev = &step_active;

	for (int k = 1; k <= N_IDLE; k++) {
		const struct step_fix *f = &fixes[k - 1];
		const int to = FIX_TO(f->to);
		const int lat = step_lat(k);
		struct step *s = &idle_steps[k - 1];

		if (f->in > 0) {
			/* Set by hand, and checked at build time. */
			changed |= set_params(s, f->in, lat, to);
		} else {
			const int t = snap_down(longest_for(lat, budget_ms(k), to), host);

			if (t <= prev->interval) {
				/* The host's interval leaves nothing deeper. */
				changed |= set_params(s, prev->interval, prev->latency, prev->timeout);
			} else {
				changed |= set_params(s, t, lat, to);
			}
		}
		prev = s;
	}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_USB_POWER_STEPS)
	/* The USB-power column: ACTIVE's interval, the latency on straight
	 * lines between anchors -- ACTIVE's at depth 0, every step set by hand,
	 * and `top` at the deepest unless that one is set: the latency whose
	 * attendance, the other half's sync delay, is
	 * USB_POWER_PERIPHERAL_SYNC_DELAY_MS. Without a step set, equal steps
	 * from ACTIVE's to `top`. */
	const int act_lat = step_active.latency;
	const int top = USB_POWER_PERIPHERAL_SYNC_DELAY_MS * 4 / 5 / step_active.interval - 1;
	int a0 = 0;
	int l0 = act_lat;

	for (int k = 1; k <= N_IDLE; k++) {
		int lat;

		if (fixes[k - 1].usb_lat >= 0) {
			lat = usb_power_set_latency(k);
			a0 = k;
			l0 = lat;
		} else {
			int a1 = N_IDLE;
			int l1 = top;

			for (int j = k + 1; j <= N_IDLE; j++) {
				if (fixes[j - 1].usb_lat >= 0) {
					a1 = j;
					l1 = usb_power_set_latency(j);
					break;
				}
			}
			lat = MIN(ramp(l0, l1, k - a0, a1 - a0), usb_power_max_latency(k));
		}

		if (lat <= act_lat) {
			changed |= set_params(&usb_steps[k - 1], step_active.interval, act_lat,
					      step_active.timeout);
		} else {
			changed |= set_params(&usb_steps[k - 1], step_active.interval, lat,
					      usb_power_timeout(k));
		}
	}
#endif
	return changed;
}

static void log_table(int host, enum active_why why, bool peer_usb, bool changed)
{
	const struct step *a = &step_active;
	const char *tail = changed ? "" : ", steps unchanged";

	if (host) {
		LOG_INF("steps computed for a host link at %u.%02u ms: ACTIVE %u.%02u ms, "
			"latency %u, timeout %u ms (%s; other half on air every %u ms%s)%s",
			INT_MS_W(host), INT_MS_F(host), INT_MS_W(a->interval), INT_MS_F(a->interval),
			a->latency, a->timeout * 10U, why_text[why],
			EVENTS_MS(a->interval, a->latency + 1), peer_usb ? ", on USB power" : "", tail);
	} else {
		LOG_INF("steps computed: ACTIVE %u.%02u ms, latency %u, timeout %u ms (%s; other "
			"half on air every %u ms%s)%s",
			INT_MS_W(a->interval), INT_MS_F(a->interval), a->latency, a->timeout * 10U,
			why_text[why], EVENTS_MS(a->interval, a->latency + 1),
			peer_usb ? ", on USB power" : "", tail);
	}

	if (why != WHY_USB) {
		check_host_interval(a->interval, (uint16_t)host);
	}

	/* The whole table every time, changed or not: a log attached after
	 * boot -- over the very cable whose output change runs this -- would
	 * otherwise never show it. */
	for (int k = 1; k <= N_IDLE; k++) {
		const struct step *s = &idle_steps[k - 1];
		const struct step *up = k > 1 ? &idle_steps[k - 2] : a;
		const struct step_fix *f = &fixes[k - 1];

		if (f->in == 0 && same_params(s, up)) {
			LOG_INF("  step %d after %u s: as %s -- %s leaves nothing longer", k,
				s->after_ms / 1000U, k > 1 ? "the step above" : "ACTIVE",
				host ? "the host link's interval" : "its budget");
			continue;
		}
		LOG_INF("  step %d after %u s: %u.%02u ms%s, latency %u%s, timeout %u ms (return "
			"~%u of %d ms, on air every %u ms)",
			k, s->after_ms / 1000U, INT_MS_W(s->interval), INT_MS_F(s->interval),
			f->in > 0 ? " (set)" : "", s->latency, f->lat >= 0 ? " (set)" : "",
			s->timeout * 10U, EVENTS_MS(s->interval, 3 * s->latency + 8), budget_ms(k),
			EVENTS_MS(s->interval, s->latency + 1));
		/* A computed interval keeps off the host's multiples; one set by
		 * hand is where it was put. */
		if (f->in > 0 && why != WHY_USB) {
			check_host_interval(s->interval, (uint16_t)host);
		}
	}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_USB_POWER_STEPS)
	for (int k = 1; k <= N_IDLE; k++) {
		const struct step *c = &usb_steps[k - 1];
		const int want = fixes[k - 1].usb_lat;
		/* A set latency the guards cap to ACTIVE's or below runs ACTIVE,
		 * timeout and all. */
		const bool runs_active = same_params(c, &step_active);
		const char *tag = want < 0		 ? ""
				  : want == c->latency ? " (set)"
				  : want < c->latency  ? " (set below ACTIVE's: runs ACTIVE)"
				  : runs_active	       ? " (set, but the guards leave nothing above "
							 "ACTIVE's: runs ACTIVE)"
						       : " (set higher: the guards cap it here)";

		LOG_INF("  step %d on USB power: latency %u%s, timeout %u ms (return ~%u ms, on air every "
			"%u ms)",
			k, c->latency, tag, c->timeout * 10U, EVENTS_MS(c->interval, 3 * c->latency + 8),
			EVENTS_MS(c->interval, c->latency + 1));
	}
#endif
}

/* Handler-only, like the levels. */
static bool table_ready;
/* The link to the computer, the output or the other half's power changed,
 * and the wait host_link_changed() gave it is over: set by table_work_cb(),
 * which runs on this queue too. */
static bool table_dirty;

static void find_host(struct bt_conn *conn, void *data)
{
	int *host = data;
	struct bt_conn_info info;

	if (bt_conn_get_info(conn, &info) || info.type != BT_CONN_TYPE_LE ||
	    info.state != BT_CONN_STATE_CONNECTED || info.role == BT_CONN_ROLE_CENTRAL) {
		return;
	}
	/* ZMK keeps one computer connected at a time; were there more, the
	 * busiest link is the one to keep clear of. */
	if (*host == 0 || info.le.interval < *host) {
		*host = info.le.interval;
	}
}

/*
 * Works the steps out again for the link to the computer, the output and the
 * other half's power as they are now. A level whose parameters moved is no
 * longer what the link runs, so it goes to unknown and the handler re-applies
 * it. Kept out of line: its locals must not sit in the handler's frame under
 * the HCI call that applying a step makes.
 */
static __noinline void refresh_table(void)
{
	int host = 0;
	const bool ble_out = zmk_endpoints_selected().transport == ZMK_TRANSPORT_BLE;
	const bool peer_usb = split_conn_ladder_peripheral_on_usb();
	const struct step before = *step_at(MAX(current_level, 0));
	enum active_why why;

	bt_conn_foreach(BT_CONN_TYPE_LE, find_host, &host);

	const bool changed = compute_table(host, ble_out, peer_usb, &why) || !table_ready;

	log_table(host, why, peer_usb, changed);
	table_ready = true;

	if (current_level >= 0 && !same_params(&before, step_at(current_level))) {
		LOG_INF("step %d%s has new parameters -- re-applying", depth_of(current_level),
			column_tag(current_level));
		current_level = -1;
	}
}
#endif /* CONFIG_ZMK_SPLIT_CONN_LADDER_COMPUTED_STEPS */

/*
 * Any USB power at all: a host, asleep or awake, or a bare charger. This is
 * the same test that keeps ZMK itself out of deep sleep on a cable. A host
 * that cuts VBUS while it sleeps reads as absent, and the ladder resumes --
 * which is right, the half is on battery again.
 */
static bool usb_powered(void)
{
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
	return zmk_usb_is_powered();
#else
	return false;
#endif
}

static const char *mode_name(int mode)
{
	switch (mode) {
	case LADDER_AUTO:
		return "AUTO";
	case LADDER_ON:
		return "ON";
	case LADDER_OFF:
		return "OFF";
	default:
		return "?";
	}
}

static enum gate gate_for(int mode, bool usb, bool both)
{
	if (mode == LADDER_OFF) {
		return GATE_OFF;
	}
	if (mode == LADDER_AUTO && usb) {
		return IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_USB_POWER_STEPS) && !both ? GATE_USB_POWER_STEPS
										     : GATE_USB_POWER_ACTIVE;
	}
	return GATE_BATTERY_STEPS;
}

static const char *gate_name(enum gate gate)
{
	switch (gate) {
	case GATE_BATTERY_STEPS:
		return "on battery steps";
	case GATE_USB_POWER_STEPS:
		return "on USB power steps";
	default:
		return "held at ACTIVE";
	}
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_LOG_RSSI)
/*
 * Optional, off by default, and it needs CONFIG_BT_CTLR_CONN_RSSI=y -- which
 * ZMK leaves off -- or the controller answers "unknown command".
 *
 * It answers the question the rest of this module cannot: when a step fails,
 * is the signal weak, or is the band simply busy? A healthy split link across
 * a desk should sit around -40..-60 dBm. If it does and steps still fail, the
 * losses are congestion, and the fix is channel classification rather than
 * anything to do with connection parameters.
 */
static void log_rssi(struct bt_conn *conn, void *data)
{
	struct bt_conn_info info;
	uint16_t handle;
	struct net_buf *buf, *rsp = NULL;
	struct bt_hci_cp_read_rssi *cp;
	struct bt_hci_rp_read_rssi *rp;

	ARG_UNUSED(data);

	if (bt_conn_get_info(conn, &info) || info.type != BT_CONN_TYPE_LE ||
	    info.state != BT_CONN_STATE_CONNECTED || info.role != BT_CONN_ROLE_CENTRAL) {
		return;
	}
	if (bt_hci_get_conn_handle(conn, &handle)) {
		return;
	}

	buf = bt_hci_cmd_create(BT_HCI_OP_READ_RSSI, sizeof(*cp));
	if (!buf) {
		return;
	}
	cp = net_buf_add(buf, sizeof(*cp));
	cp->handle = sys_cpu_to_le16(handle);

	if (bt_hci_cmd_send_sync(BT_HCI_OP_READ_RSSI, buf, &rsp) || !rsp) {
		LOG_DBG("RSSI read failed -- is CONFIG_BT_CTLR_CONN_RSSI enabled?");
		return;
	}

	rp = (void *)rsp->data;
	LOG_INF("split link RSSI: %d dBm", rp->rssi);
	net_buf_unref(rsp);
}

static void report_rssi(void)
{
	bt_conn_foreach(BT_CONN_TYPE_LE, log_rssi, NULL);
}
#else
static inline void report_rssi(void)
{
}
#endif

struct apply_ctx {
	const struct step *s;
	int err;
	int matched;
	bool already;
	/* What the link runs now: the host's record of the last update that
	 * completed, known even when the level is not. */
	uint16_t now_in;
	uint16_t now_lat;
};

static void apply_to_conn(struct bt_conn *conn, void *data)
{
	struct apply_ctx *ctx = data;
	struct bt_conn_info info;

	/* bt_conn_foreach() also hands out connections that are still being
	 * established; those have no handle yet, and an update against one is
	 * only good for an error in the log. */
	if (bt_conn_get_info(conn, &info) || info.type != BT_CONN_TYPE_LE ||
	    info.state != BT_CONN_STATE_CONNECTED) {
		return;
	}

	/* Central role means the other half. Peripheral role means the host;
	 * that link is not ours to command and not worth optimising. */
	if (info.role != BT_CONN_ROLE_CENTRAL) {
		return;
	}

	ctx->matched++;
	ctx->now_in = info.le.interval;
	ctx->now_lat = info.le.latency;

	struct bt_le_conn_param p = BT_LE_CONN_PARAM_INIT(ctx->s->interval, ctx->s->interval,
							  ctx->s->latency, ctx->s->timeout);

	int err = bt_conn_le_param_update(conn, &p);

	if (err == -EALREADY) {
		/* Nothing was sent; apply_level() decides what that means. */
		ctx->already = true;
	} else if (err) {
		/* Logged, with the retry it earns, by retry_after_reject(). */
		ctx->err = err;
	}
}

/* Returns 0 when the level was applied, otherwise the errno of the failure. */
static int apply_level(int level)
{
	uint32_t now = k_uptime_get_32();

	if (pending_level >= 0) {
		/* Already on its way there; asking twice only makes the
		 * peripheral chew through two instants back to back. */
		if (level == pending_level) {
			return 0;
		}
	} else if (level == current_level) {
		return 0;
	}

	const struct step *s = step_at(level);
	/* An overtaken request still runs first, so ours starts from there. */
	const bool queued = pending_level >= 0;
	const bool from_known = queued || current_level >= 0;
	const int from_level = queued ? pending_level : MAX(current_level, 0);
	const struct step *from = step_at(from_level);
	struct apply_ctx ctx = {.s = s, .err = 0, .matched = 0, .already = false};

	bt_conn_foreach(BT_CONN_TYPE_LE, apply_to_conn, &ctx);

	if (ctx.err) {
		return ctx.err;
	}

	if (ctx.matched == 0) {
		/* Nothing to command. Leave current_level alone: the next link
		 * starts on PREF_*, and link_came_up() records what that is, so
		 * there is nothing to record here. */
		LOG_DBG("level %d skipped, no split link up", level);
		return 0;
	}

	if (ctx.already) {
		/*
		 * -EALREADY: the host compares against the parameters of the last
		 * COMPLETED update and sends nothing when they match
		 * (bt_conn_le_param_update(), host/conn.c).
		 *
		 * With nothing in flight that record is the link itself, so the
		 * level is simply confirmed -- which also ends any "never
		 * confirmed" retry for parameters the link already has.
		 *
		 * With an update in flight the record is about to go stale: this
		 * request was dropped, not queued. Keep waiting for the one in
		 * flight; its report runs the handler again, the record has moved
		 * by then, and the request goes out for real. Recording it as
		 * pending instead would wait for a procedure that does not exist.
		 */
		if (pending_level < 0) {
			current_level = level;
			/* INF, not DBG: a step that repeats the one above -- the
			 * host link's interval left nothing deeper, or the guards
			 * capped a USB-power step -- shows up only this way: it
			 * has the same parameters, so nothing is sent. */
			LOG_INF("step %d/%d%s: parameters already in place", depth_of(level), N_IDLE,
				column_tag(level));
		}
		return 0;
	}

	superseded_level = pending_level; /* -1 when nothing was outstanding */
	pending_level = level;
	/* update_confirmed is deliberately NOT cleared here: the report for the
	 * request we just overtook can land during the HCI call above, and it is
	 * the superseded branch's only evidence. A stale report cannot be
	 * mistaken for ours, because the match is on exact parameters. */
	atomic_set(&update_in_flight, 1);
	update_issued_at = now;

	/*
	 * How long the switch takes is governed by the parameters we are
	 * LEAVING, not the ones we are moving to, and it has two parts:
	 *
	 *   delivery + acknowledgement ~ 2 attendance periods of that step,
	 *   the instant itself          = (latency + 6) connection events
	 *                                 (ull_llcp_conn_upd.c)
	 *
	 *   switch ~ interval * (3 * latency + 8)
	 *
	 * Checked against hardware, modelled vs measured: 15 ms/lat9 525 vs
	 * 399-466, 30 ms/lat14 1500 vs 1216-1361, 60 ms/lat5 1380 vs 1156-1278,
	 * 120 ms/lat1 1320 vs 1276, 400 ms/lat0 3200 vs 3196; and 2940 vs 2769
	 * at 30 ms/lat30. An upper bound, usually by up to ~30 %; seen over it
	 * by a few percent, and once by 31 % (689 vs 525) on a -85 dBm link.
	 *
	 * Note what this says: at a fixed interval the central costs the same
	 * whatever the latency, while the latency alone sets both the
	 * peripheral's cost and how long coming back takes. That is the trade
	 * the default steps are tuned around.
	 *
	 * Behind a request still running, the switch starts where that one
	 * lands; otherwise from what the link runs now -- known even when the
	 * level is not: after a recompute moved the step, a lost confirmation,
	 * a fresh link that ACTIVE has moved away from.
	 */
	const uint32_t from_air = queued ? EVENTS_MS(from->interval, 3 * from->latency + 8)
					 : EVENTS_MS(ctx.now_in, 3 * ctx.now_lat + 8);

	LOG_INF("step %d/%d%s: interval %u.%02u ms, latency %u, timeout %u ms "
		"(peripheral on air every %u ms; switch from level %d%s ~%u ms)",
		depth_of(level), N_IDLE, column_tag(level), INT_MS_W(s->interval),
		INT_MS_F(s->interval), s->latency, s->timeout * 10,
		EVENTS_MS(s->interval, s->latency + 1), from_known ? depth_of(from_level) : -1,
		from_known ? column_tag(from_level) : "", from_air);

	report_rssi();

	return 0;
}

static void ladder_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(ladder_work, ladder_work_cb);

static void reschedule(k_timeout_t delay)
{
	/*
	 * Never the system work queue: for a central,
	 * bt_conn_le_param_update() goes into bt_hci_cmd_send_sync() and
	 * blocks until the controller answers.
	 *
	 * The result is ignored on purpose. The one refusal this can meet is
	 * -EBUSY for an immediate submit while a SLEEP cancel is still waiting
	 * for a running handler to return (work.c: K_WORK_CANCELING). The board
	 * is powering off then; and if it does not, it stays in SLEEP until a
	 * key raises ACTIVE, which reschedules long after the flag cleared.
	 */
	(void)k_work_reschedule_for_queue(&ladder_q, &ladder_work, delay);
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_COMPUTED_STEPS)
static void table_work_cb(struct k_work *work)
{
	ARG_UNUSED(work);
	table_dirty = true;
	reschedule(K_NO_WAIT);
}

static K_WORK_DELAYABLE_DEFINE(table_work, table_work_cb);
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_COMPUTED_STEPS)
/* k_uptime_get_32() at the latest connect of a link to the computer. Written
 * from the Bluetooth RX thread, read wherever a change is reported. */
static atomic_t host_up_at;
#endif

/*
 * The link to the computer came up, changed or went, or the output moved: the
 * computed steps follow. A link in its first HOST_SETTLE_MS is still being
 * adjusted, and a lost one may be back in a moment, so those wait -- one
 * recompute for the lot, not one per transient. Everything else is followed
 * after HOST_QUICK_MS. Each report restarts the wait.
 */
static void host_link_changed(enum host_event ev)
{
#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_COMPUTED_STEPS)
	const uint32_t now = k_uptime_get_32();

	if (ev == HOST_UP) {
		atomic_set(&host_up_at, (atomic_val_t)now);
	}

	const uint32_t since = now - (uint32_t)atomic_get(&host_up_at);
	uint32_t delay = HOST_QUICK_MS;

	if (ev == HOST_DOWN) {
		delay = HOST_SETTLE_MS;
	} else if (since < HOST_SETTLE_MS) {
		delay = MAX(HOST_SETTLE_MS - since, HOST_QUICK_MS);
	}
	(void)k_work_reschedule_for_queue(&ladder_q, &table_work, K_MSEC(delay));
#else
	ARG_UNUSED(ev);
#endif
}

struct k_work_q *split_conn_ladder_work_q(void)
{
	return &ladder_q;
}

/* The other half's USB power changed: let the handler re-read the gate, and
 * the computed ACTIVE follow -- its latency is 0 while that half is on USB. */
void split_conn_ladder_peripheral_usb_power_changed(void)
{
	host_link_changed(HOST_CHANGED);
	reschedule(K_NO_WAIT);
}

/* The other half's report carried its uptime: a loss waiting on it can be
 * judged now. */
void split_conn_ladder_peripheral_uptime(uint32_t uptime_ms)
{
	atomic_set(&peer_uptime, (atomic_val_t)uptime_ms);
	atomic_set(&peer_report_at, (atomic_val_t)k_uptime_get_32());
	atomic_set(&peer_report_fresh, 1);
	reschedule(K_NO_WAIT);
}

int split_conn_ladder_get_mode(void)
{
	return (int)atomic_get(&ladder_mode);
}

int split_conn_ladder_set_mode(int mode)
{
	if (mode != LADDER_AUTO && mode != LADDER_ON && mode != LADDER_OFF) {
		return -EINVAL;
	}

	int old = (int)atomic_set(&ladder_mode, mode);

	LOG_INF("mode %s -> %s", mode_name(old), mode_name(mode));

	/* The handler decides what the new mode means for the link, on its
	 * own queue, as the one writer of the level. */
	reschedule(K_NO_WAIT);
	return 0;
}

/*
 * One place decides when the handler runs again.
 *
 * `have_deadline` says whether the ladder itself wants to be woken. On its own
 * the answer is often no -- sitting at the deepest step there is nothing left
 * to do. But an unconfirmed update must never be left without a timer: the
 * confirmation may simply never arrive, and the only other thing that would
 * run the handler is an activity transition, which does not happen while the
 * user keeps typing. That is how a stuck update survives to become a half that
 * "hangs waiting for a confirmation".
 */
static void schedule_next(uint32_t delay_ms, bool have_deadline)
{
	if (pending_level >= 0) {
		uint32_t cap = UNCONFIRMED_UPDATE_TIMEOUT_MS + REJECTED_UPDATE_RETRY_MS;

		if (!have_deadline || delay_ms > cap) {
			delay_ms = cap;
			have_deadline = true;
		}
	}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_BACKOFF_AFTER_LINK_LOSS)
	if (judge_pending) {
		/* No report may come at all; the fallback judgment has a time. */
		const uint32_t t = k_uptime_get_32();
		const uint32_t wait = (int32_t)(judge_until - t) > 0 ? judge_until - t : 0;

		if (!have_deadline || delay_ms > wait) {
			delay_ms = wait;
			have_deadline = true;
		}
	}
#endif

	if (have_deadline) {
		reschedule(K_MSEC(delay_ms));
	}
}

/*
 * A refused update. What Zephyr 3.5 can refuse one with here: -ENOBUFS (no HCI
 * command buffer), -EIO (the controller answered Command Disallowed, which in
 * its LLCP means no free procedure context -- it queues procedures otherwise),
 * -ENOMEM, -ECONNREFUSED. The ordinary ones clear within milliseconds, so the
 * first retry comes after REJECTED_UPDATE_RETRY_MS; each further refusal in a row doubles the
 * wait, up to UNCONFIRMED_UPDATE_TIMEOUT_MS, so that something which does not clear costs
 * a log line every few seconds instead of an HCI round trip twice a second.
 * Any accepted update resets the streak.
 */
#define REJECTED_UPDATE_RETRY_CAP_MS MAX(UNCONFIRMED_UPDATE_TIMEOUT_MS, REJECTED_UPDATE_RETRY_MS)

static void retry_after_reject(int err)
{
	const uint32_t delay = MIN((uint32_t)REJECTED_UPDATE_RETRY_MS << MIN(reject_streak, 6),
				   (uint32_t)REJECTED_UPDATE_RETRY_CAP_MS);

	if (reject_streak < UINT8_MAX) {
		reject_streak++;
	}
	LOG_WRN("param update rejected: %d -- retry %u in %u ms", err, reject_streak, delay);
	reschedule(K_MSEC(delay));
}

/* True while a previously issued update has not been confirmed by the
 * controller. The timeout keeps a lost confirmation from wedging the ladder. */
static bool update_pending(uint32_t now)
{
	if (atomic_get(&update_confirmed)) {
		/* A report is waiting to be read by the handler; the update is
		 * settled, not lost. */
		return true;
	}
	if (!atomic_get(&update_in_flight)) {
		return false;
	}
	if (now - update_issued_at >= UNCONFIRMED_UPDATE_TIMEOUT_MS) {
		atomic_set(&update_in_flight, 0);
		return false;
	}
	return true;
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_BACKOFF_AFTER_LINK_LOSS)
/*
 * A disconnect on its own does not prove the step was too deep. The other half
 * also drops the link when it enters its own deep sleep, and that is not a
 * parameter problem -- capping the ladder for it would slowly disable depth
 * for no reason.
 *
 * The two are told apart by what happens next. A half that lost the link over
 * parameters is awake and advertising, so it comes back in seconds. A half
 * that powered itself off stays away until someone presses a key on it. So the
 * loss is armed here and only judged once the link returns.
 */

/*
 * Neighbouring steps with the same parameters are one depth as far as the link
 * can tell -- a step repeats the one above when the host link's interval
 * leaves nothing deeper, or when the guards cap a USB-power latency -- so a
 * cap never falls between two of them. Otherwise a loss at step 4 would cap
 * the ladder at a step 3 that runs the very parameters that just failed, and
 * it would take a second loss to get anywhere.
 */
static int group_first(bool usb_power, int depth)
{
	while (depth > 1 && same_params(step_at(level_for(usb_power, depth - 1)),
					step_at(level_for(usb_power, depth)))) {
		depth--;
	}
	return depth;
}

static int group_last(bool usb_power, int depth)
{
	while (depth < N_IDLE && same_params(step_at(level_for(usb_power, depth + 1)),
					     step_at(level_for(usb_power, depth)))) {
		depth++;
	}
	return depth;
}

static void judge_loss(uint32_t back_at)
{
	uint32_t away = back_at - lost_at;

	loss_armed = false;

	if (lost_level < 1) {
		return; /* died on ACTIVE parameters; not the ladder's doing */
	}
	if (away > BACKOFF_RECONNECT_WINDOW_MS) {
		LOG_INF("split link was away %u.%u s -- treating as the other half "
			"sleeping, not a step failure",
			away / 1000U, away % 1000U / 100U);
		return;
	}

	/* Charged to the column it failed in; the other one keeps its depth.
	 * Below every step that runs the parameters that failed. */
	const bool usb_power = is_usb_power_level(lost_level);
	const int depth = depth_of(lost_level);
	const int limit = group_first(usb_power, depth) - 1;
	int *cap = usb_power ? &max_level_usb : &max_level;

	if (*cap > limit) {
		*cap = limit;
	}

	if (backoff_count < 255) {
		backoff_count++;
	}

	uint64_t widened = (uint64_t)BACKOFF_RESTORE_AFTER_MS << MIN(backoff_count - 1, 5);

	stable_required = (uint32_t)MIN(widened, (uint64_t)BACKOFF_RESTORE_AFTER_MAX_MS);

	LOG_WRN("split link dropped at step %d%s and returned in %u.%u s -- limiting "
		"depth%s to %d, retrying it in %u min",
		depth, column_tag(lost_level), away / 1000U, away % 1000U / 100U,
		column_tag(lost_level), *cap, stable_required / 60000U);
}

/*
 * A loss that came back waits here for the other half's report, which carries
 * its uptime. If that half restarted while the link was down -- its own deep
 * sleep, a reset, a battery pulled -- the link was never the problem, however
 * fast it came back: a half that powers off without closing the link (ZMK's
 * sleep without PERIPHERAL_CLOSE_LINK_BEFORE_SLEEP there) looks from this side
 * exactly like a supervision timeout, and a key pressed on it a few seconds
 * later looks exactly like a quick return. With no
 * report in time -- a half built without it -- the return alone decides.
 */
static void settle_judgment(uint32_t now)
{
	if (atomic_get(&peer_report_fresh)) {
		const uint32_t at = (uint32_t)atomic_get(&peer_report_at);
		const uint32_t up = (uint32_t)atomic_get(&peer_uptime);

		judge_pending = false;
		/* Up for less than the link was down: it booted after its last
		 * packet reached us. */
		if (up < at - last_heard) {
			LOG_INF("split link lost at step %d%s: the other half restarted while "
				"it was down (up %u.%u s) -- its sleep or a reset, not a step "
				"failure",
				depth_of(lost_level), column_tag(lost_level), up / 1000U,
				up % 1000U / 100U);
			return;
		}
		judge_loss(judge_back_at);
		return;
	}
	if ((int32_t)(now - judge_until) >= 0) {
		judge_pending = false;
		judge_loss(judge_back_at);
	}
}
#endif

/*
 * The split link dropped at `at`. The level it died at is the CONFIRMED one,
 * not the one on its way in: a link that dies mid-switch died on the
 * parameters it was still running, and blaming the target would cap the
 * ladder for a depth that was never in force -- for a 0->1 descent, that
 * would switch the ladder off entirely.
 */
static void link_went_down(uint32_t at, bool closed)
{
	atomic_set(&update_in_flight, 0);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_BACKOFF_AFTER_LINK_LOSS)
	if (judge_pending) {
		/* Back and gone again before the other half reported: judge the
		 * earlier loss on its return alone, before this one overwrites it. */
		judge_pending = false;
		judge_loss(judge_back_at);
	}
	lost_at = at;
	lost_level = MAX(current_level, 0);
	last_heard = at - step_at(lost_level)->timeout * 10U;
	/* Closed by the other half on purpose: nothing to judge. */
	loss_armed = !closed;
	last_loss_was_peer_close = closed;
#else
	ARG_UNUSED(at);
	ARG_UNUSED(closed);
#endif
	pending_level = -1;
	superseded_level = -1;
}

/*
 * A split link came up at `at`. It comes up on PREF_*, which is ACTIVE --
 * unless the steps are computed and the link to the computer has moved ACTIVE
 * elsewhere; then the level is unknown until the handler moves it there.
 * Restart the idle clock with it: without this, a reconnect deep into an idle
 * period drops the brand-new link straight to the deepest step a few seconds
 * later. If that depth is what broke the link in the first place, it breaks
 * again, reconnects, breaks again -- a loop that looks from the outside like a
 * half that never comes back.
 */
static void link_came_up(uint32_t at, uint32_t now)
{
	current_level = same_params(&step_active, &step_pref) ? 0 : -1;
	pending_level = -1;
	superseded_level = -1;
	idle_since = now;
	atomic_set(&update_in_flight, 0);
	atomic_set(&update_confirmed, 0);
	reject_streak = 0;
#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_BACKOFF_AFTER_LINK_LOSS)
	if (loss_armed) {
		/* Judged on how long the link was really gone: from the
		 * disconnect to this connect, both stamped where they happened,
		 * not on when this handler got round to running. A loss that
		 * could count against a step first waits for the other half's
		 * report, which says whether it restarted meanwhile. */
		if (lost_level >= 1 && at - lost_at <= BACKOFF_RECONNECT_WINDOW_MS) {
			loss_armed = false;
			judge_pending = true;
			judge_back_at = at;
			judge_until = now + JUDGE_WAIT_MS;
		} else {
			judge_loss(at);
		}
	}
	/* The stability clock measures the link since its last failure. A close
	 * by the other half -- its sleep -- is none, so it keeps running across
	 * one: an hourly sleep of the other half would otherwise hold a capped
	 * depth back for as long as the restore period exceeds an hour. */
	if (!last_loss_was_peer_close) {
		stable_since = now;
	}
#else
	ARG_UNUSED(at);
#endif
	LOG_DBG("link re-established, level %d, idle clock restarted", current_level);
}

/* How much of NEW_LINK_SETTLE_MS a fresh split link still has to wait; 0 once it has
 * settled. The stamp is read before the clock, as with idle_since, so a
 * connect that lands in between cannot wrap the difference. */
static uint32_t settle_left(void)
{
	const uint32_t up_at = (uint32_t)atomic_get(&reset_stamp);
	const uint32_t up_for = k_uptime_get_32() - up_at;

	return up_for < NEW_LINK_SETTLE_MS ? NEW_LINK_SETTLE_MS - up_for : 0;
}

/* A SLEEP event came and nothing since: the board is powering off. ZMK's
 * own sleep also sets its state to SLEEP; the early sleep leaves it at IDLE,
 * and this is how the handler knows then. */
static atomic_t sleep_announced;

static void ladder_work_cb(struct k_work *work)
{
	ARG_UNUSED(work);

	uint32_t now = k_uptime_get_32();

	const bool confirmed = atomic_cas(&update_confirmed, 1, 0);

	if (confirmed && pending_level >= 0) {
		if (applied_is(step_at(pending_level))) {
			current_level = pending_level;
			pending_level = -1;
			superseded_level = -1;
			/* The superseded branch below may have re-armed this after
			 * the final report already cleared it. */
			atomic_set(&update_in_flight, 0);
		} else if (superseded_level >= 0 && applied_is(step_at(superseded_level))) {
			/* The overtaken request landed first; ours is queued right
			 * behind it and still counts as in flight. */
			LOG_DBG("step %d%s landed first, step %d%s still queued",
				depth_of(superseded_level), column_tag(superseded_level),
				depth_of(pending_level), column_tag(pending_level));
			current_level = superseded_level;
			superseded_level = -1;
			atomic_set(&update_in_flight, 1);
		} else {
			/* Something else moved the link. Do not pretend the
			 * request landed -- go to unknown and re-issue. */
			LOG_WRN("unexpected parameters on the split link -- re-applying");
			pending_level = -1;
			superseded_level = -1;
			current_level = -1;
		}
	} else if (confirmed && current_level >= 0 && !applied_is(step_at(current_level))) {
		/*
		 * A report with nothing outstanding: the parameters were changed
		 * by something other than this module. Believing the old level
		 * would freeze the link there, because every later apply_level()
		 * for that level returns early.
		 */
		LOG_WRN("split link parameters changed from outside -- re-applying");
		current_level = -1;
	}

	if (pending_level >= 0 && !update_pending(now)) {
		/* Issued, never confirmed. The link may be at either set of
		 * parameters, so stop pretending to know: -1 makes the next
		 * apply_level() re-issue instead of early-returning. */
		LOG_WRN("update to step %d%s never confirmed -- re-applying",
			depth_of(pending_level), column_tag(pending_level));
		pending_level = -1;
		superseded_level = -1;
		current_level = -1;
	}

	/*
	 * Link losses and re-connections. The callbacks only raise flags and
	 * stamp the time, so a disconnect and a connect can both be waiting
	 * here -- on_connected() even pushes this run NEW_LINK_SETTLE_MS out, over the
	 * immediate one on_disconnected() asked for. The flags cannot say which
	 * came first; the stamps can, and the order decides what the loss is
	 * charged to.
	 */
	const bool lost = atomic_cas(&link_lost, 1, 0);
	const bool reset = atomic_cas(&link_reset, 1, 0);
	const bool closed = atomic_get(&loss_was_peer_close) != 0;
	const uint32_t lost_t = (uint32_t)atomic_get(&lost_stamp);
	const uint32_t reset_t = (uint32_t)atomic_get(&reset_stamp);

	if (lost && reset && (int32_t)(lost_t - reset_t) > 0) {
		/* Came back and dropped again before we ran: the link that
		 * dropped was the fresh one, on ACTIVE parameters. */
		link_came_up(reset_t, now);
		link_went_down(lost_t, closed);
	} else {
		if (lost) {
			link_went_down(lost_t, closed);
		}
		if (reset) {
			link_came_up(reset_t, now);
		}
	}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_BACKOFF_AFTER_LINK_LOSS)
	/* Before anything can step down: a cap it produces applies at once. */
	if (judge_pending) {
		settle_judgment(now);
	}
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_COMPUTED_STEPS)
	/* Before anything applies a step. Never under an update in flight: its
	 * report is matched against the parameters it was issued with, and it
	 * runs this handler again when it lands. */
	if (!table_ready || (table_dirty && pending_level < 0)) {
		table_dirty = false;
		refresh_table();
	}
#endif

	/*
	 * The gate: may the ladder step down at all, and on which parameter
	 * set -- battery or USB power? Evaluated before the link check, so that
	 * plugging and unplugging show up in the log even while the other half
	 * is away.
	 */
	const int mode = (int)atomic_get(&ladder_mode);
	const bool usb = usb_powered();
	/* The other half's report; false while unknown. Without
	 * USB_POWER_STEPS the gate does not ask. */
	const bool both = usb && split_conn_ladder_peripheral_on_usb();
	const enum gate gate = gate_for(mode, usb, both);

	if (gate != last_gate) {
		if (gate == GATE_BATTERY_STEPS && last_gate != GATE_UNKNOWN) {
			/*
			 * Onto the battery column from anything else -- a hold,
			 * or the USB-power column: start from the top, as if the
			 * keyboard had only now gone idle. Pulling the cable is
			 * usually the moment before somebody picks the keyboard
			 * up, and dropping straight to a deep battery step would
			 * make the first words from the other half pay the slowest
			 * return there is. Onto the USB-power column the clock
			 * keeps running: a USB-power step delays no key, so
			 * plugging in keeps the depth and only swaps the
			 * parameters.
			 */
			idle_since = now;
		}
		LOG_INF("ladder %s (mode %s, USB power %s)", gate_name(gate), mode_name(mode),
			both ? "present on both halves" : usb ? "present" : "absent");
		last_gate = gate;
	}

	if (!split_link_up()) {
		/* on_connected() will wake us; nothing to schedule meanwhile. */
		return;
	}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_BACKOFF_AFTER_LINK_LOSS)
	if ((max_level < N_IDLE || max_level_usb < N_IDLE) &&
	    (now - stable_since) >= stable_required) {
		/* One clock for both columns: stability is the link's, not a
		 * column's. Each capped column gets a notch back. */
		if (max_level < N_IDLE) {
			max_level = group_last(false, max_level + 1);
		}
		if (max_level_usb < N_IDLE) {
			max_level_usb = group_last(true, max_level_usb + 1);
		}
		stable_since = now;
		if (IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_USB_POWER_STEPS)) {
			LOG_INF("split link stable for %u min, allowing depth %d on battery, "
				"%d on USB power again",
				stable_required / 60000U, max_level, max_level_usb);
		} else {
			LOG_INF("split link stable for %u min, allowing depth %d again",
				stable_required / 60000U, max_level);
		}
		if (max_level == N_IDLE && max_level_usb == N_IDLE) {
			/* Back to full depth: start the next accusation from the
			 * short retry again instead of the widened one. */
			backoff_count = 0;
			stable_required = BACKOFF_RESTORE_AFTER_MS;
		}
	}
#endif

	/* The activity state is read here rather than in the listener, so that
	 * an already-running instance cannot clobber a just-applied ACTIVE. */
	enum zmk_activity_state st = zmk_activity_get_state();

	if (st == ZMK_ACTIVITY_SLEEP || atomic_get(&sleep_announced)) {
		/* The board is powering off; there is nothing to restore. */
		return;
	}

	if (st == ZMK_ACTIVITY_ACTIVE || gate == GATE_USB_POWER_ACTIVE || gate == GATE_OFF) {
		/*
		 * The one wait on the way up: a fresh link whose level is not
		 * known. That link runs PREF_* -- a perfectly good ACTIVE -- or
		 * whatever moved it in its first seconds; either way the move to
		 * the ACTIVE the steps want is not worth landing in the middle of
		 * ZMK's set-up, and waits for NEW_LINK_SETTLE_MS like any step.
		 */
		if (current_level < 0 && pending_level < 0) {
			const uint32_t left = settle_left();

			if (left) {
				schedule_next(left, true);
				return;
			}
		}

		/* Otherwise deliberately not gated on update_pending(): coming
		 * back up is urgent, and the controller queues a second procedure
		 * behind the first rather than refusing it. A hold takes the same
		 * road: the other half plugging in too brings the link straight
		 * back. */
		int err = apply_level(0);

		if (err) {
			retry_after_reject(err);
		} else {
			reject_streak = 0;
			schedule_next(0, false);
		}
		return;
	}

	/*
	 * Read the idle start BEFORE the clock. The activity listener runs on
	 * the cooperative system queue and can preempt this handler after `now`
	 * was sampled; against `now`, its fresh timestamp would lie in the
	 * future, and the unsigned difference would wrap to "idle for weeks".
	 */
	const uint32_t since = idle_since;
	uint32_t elapsed = k_uptime_get_32() - since;
	const bool usb_power = gate == GATE_USB_POWER_STEPS;
	const int cap = usb_power ? max_level_usb : max_level;
	int depth = 0;

	/* One timer for both columns: the depth depends on idle time only. */
	for (int i = N_IDLE - 1; i >= 0; i--) {
		if (elapsed >= idle_steps[i].after_ms) {
			depth = i + 1;
			break;
		}
	}

	depth = MIN(depth, cap);

	/* A loss still waiting for its verdict: not back onto the step it died
	 * at, nor deeper, before the other half's report says whether that step
	 * was to blame -- the report comes about 2 s after the connect, and
	 * NEW_LINK_SETTLE_MS alone would let the step in a second later. The
	 * verdict comes with the report or at judge_until. */
	bool held_for_verdict = false;

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_BACKOFF_AFTER_LINK_LOSS)
	if (judge_pending && is_usb_power_level(lost_level) == usb_power) {
		/* The step above the failed parameters, not above the failed
		 * step: steps that repeat it run the same parameters. */
		const int failed = group_first(usb_power, depth_of(lost_level));

		if (depth >= failed) {
			depth = failed - 1;
			held_for_verdict = true;
		}
	}
#endif

	const int level = level_for(usb_power, depth);

	/*
	 * A fresh link is left alone for NEW_LINK_SETTLE_MS, whatever woke us.
	 * The delay on_connected() puts on this run is not enough on its own: a
	 * key, a cable, the &ladder key or the other half's power report pulls
	 * the run back in, and with STEP1_AFTER_IDLE_MS below NEW_LINK_SETTLE_MS
	 * a step would then land in the middle of ZMK's set-up. ACTIVE waits
	 * only when the level is not known, as above -- otherwise a new link is
	 * at ACTIVE anyway.
	 */
	if (level != 0 || current_level < 0) {
		const uint32_t left = settle_left();

		if (left) {
			schedule_next(left, true);
			return;
		}
	}

	/* Anything but ACTIVE waits for the update in flight -- a deeper step,
	 * and a change of column too. Stacking a second procedure behind one
	 * that has not landed makes the peripheral process two instants back
	 * to back, which is exactly when it drops one; and neither is urgent:
	 * a USB-power step delays no key, and a key press goes to ACTIVE,
	 * which never waits. */
	if (level != 0 && level != effective_level() && update_pending(now)) {
		reschedule(K_MSEC(REJECTED_UPDATE_RETRY_MS));
		return;
	}

	int err = apply_level(level);

	if (err) {
		retry_after_reject(err);
		return;
	}
	reject_streak = 0;

	if (held_for_verdict) {
		schedule_next(0, false); /* until the verdict: the report or judge_until */
		return;
	}
	if (depth < cap) {
		/* Uncapped, so the loop did not select idle_steps[depth]: elapsed
		 * is strictly below its after_ms and the difference is positive. */
		schedule_next(idle_steps[depth].after_ms - elapsed, true);
		return;
	}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_BACKOFF_AFTER_LINK_LOSS)
	if (max_level < N_IDLE || max_level_usb < N_IDLE) {
		/* A column held at a reduced depth by an earlier link loss -- this
		 * one, or the other. Keep a timer alive so the stability check
		 * above can hand the next notch back without waiting for the
		 * user to touch the keyboard. */
		uint32_t waited = k_uptime_get_32() - stable_since;

		schedule_next(waited >= stable_required ? REJECTED_UPDATE_RETRY_MS : stable_required - waited,
			      true);
		return;
	}
#endif

	/* At the bottom with nothing scheduled: only an unconfirmed update
	 * still needs a wake-up. */
	schedule_next(0, false);
}

static const char *state_name(enum zmk_activity_state st)
{
	switch (st) {
	case ZMK_ACTIVITY_ACTIVE:
		return "ACTIVE";
	case ZMK_ACTIVITY_IDLE:
		return "IDLE";
	case ZMK_ACTIVITY_SLEEP:
		return "SLEEP";
	default:
		return "?";
	}
}

/* The state the last event announced. Listener-only. */
static enum zmk_activity_state last_state = ZMK_ACTIVITY_ACTIVE;

static void on_activity(enum zmk_activity_state state)
{
	const enum zmk_activity_state was = last_state;

	last_state = state;
	atomic_set(&sleep_announced, state == ZMK_ACTIVITY_SLEEP);
	LOG_INF("activity: %s", state_name(state));

	switch (state) {
	case ZMK_ACTIVITY_IDLE:
		/* IDLE after SLEEP is a deep sleep that did not happen -- the
		 * early sleep puts the real state back when suspending the
		 * devices fails. The idle period it interrupted goes on. */
		if (was != ZMK_ACTIVITY_SLEEP) {
			idle_since = k_uptime_get_32();
		}
		reschedule(K_NO_WAIT);
		break;
	case ZMK_ACTIVITY_ACTIVE:
		reschedule(K_NO_WAIT);
		break;
	case ZMK_ACTIVITY_SLEEP:
		/*
		 * Drop a run that is scheduled or queued, so it does not fire
		 * during zmk_pm_suspend_devices() and block on HCI against a
		 * radio that is already going down. A run that has ALREADY
		 * started is not waited for: the _sync variant would stall this
		 * queue -- ZMK's system queue -- behind a Bluetooth command.
		 * That run finishes on its own, and any later one stops at the
		 * handler's SLEEP check before touching the radio.
		 */
		(void)k_work_cancel_delayable(&ladder_work);
		break;
	default:
		break;
	}
}

static int on_event(const zmk_event_t *eh)
{
	const struct zmk_activity_state_changed *act = as_zmk_activity_state_changed(eh);

	if (act) {
		on_activity(act->state);
		return ZMK_EV_EVENT_BUBBLE;
	}

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
	/* Plugged, unplugged, host asleep, host awake: ZMK raises this for
	 * every one of them. Whether any of it moves the gate is the
	 * handler's call -- it costs one wasted run when it does not. */
	if (as_zmk_usb_conn_state_changed(eh)) {
		reschedule(K_NO_WAIT);
	}
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_COMPUTED_STEPS)
	/* The output moved between USB and Bluetooth, or to another profile. */
	if (as_zmk_endpoint_changed(eh)) {
		host_link_changed(HOST_CHANGED);
	}
#endif

	return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zmk_split_conn_ladder, on_event);
ZMK_SUBSCRIPTION(zmk_split_conn_ladder, zmk_activity_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(zmk_split_conn_ladder, zmk_usb_conn_state_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_COMPUTED_STEPS)
ZMK_SUBSCRIPTION(zmk_split_conn_ladder, zmk_endpoint_changed);
#endif

/*
 * The link to the computer showed its interval. In the table mode ACTIVE is
 * fixed, and a warning is all there is to give; computed steps follow the
 * link, once it holds still.
 */
static void host_interval_seen(uint16_t interval, enum host_event ev)
{
	if (IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_COMPUTED_STEPS)) {
		host_link_changed(ev);
	} else {
		check_host_interval(CONFIG_ZMK_SPLIT_BLE_PREF_INT, interval);
	}
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	struct bt_conn_info info;

	/* A failed create-connection reports here with err set, never through
	 * `disconnected`, so it is neither a link nor a loss. */
	if (err || bt_conn_get_info(conn, &info) || info.type != BT_CONN_TYPE_LE) {
		return;
	}

	if (info.role != BT_CONN_ROLE_CENTRAL) {
		/* The link to the computer, as the computer opened it. A host
		 * that never runs an update procedure shows its interval
		 * nowhere else -- and this half asks for its own preferred
		 * parameters only a few seconds later, which then shows up as
		 * "host link applied" if the host agrees. */
		LOG_INF("host link up: interval %u.%02u ms, latency %u, timeout %u ms",
			INT_MS_W(info.le.interval), INT_MS_F(info.le.interval), info.le.latency,
			info.le.timeout * 10);
		host_interval_seen(info.le.interval, HOST_UP);
		return;
	}

	/* Stamp first, flag second: the handler reads them the other way. */
	atomic_set(&reset_stamp, (atomic_val_t)k_uptime_get_32());
	atomic_set(&link_reset, 1);
	/*
	 * Right after `connected`, ZMK sets up the link -- encryption, GATT
	 * discovery, subscriptions -- and those are the procedures a step
	 * would have to queue behind. The handler holds every step back until
	 * NEW_LINK_SETTLE_MS after the stamp above, whatever else runs it earlier; this
	 * is simply when it next has something to decide.
	 */
	reschedule(K_MSEC(NEW_LINK_SETTLE_MS));
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	struct bt_conn_info info;

	if (bt_conn_get_info(conn, &info) || info.type != BT_CONN_TYPE_LE) {
		return;
	}

	if (info.role != BT_CONN_ROLE_CENTRAL) {
		LOG_INF("host link down (reason 0x%02x)", reason);
		host_link_changed(HOST_DOWN);
		return;
	}

	atomic_set(&peer_report_fresh, 0);
	atomic_set(&lost_stamp, (atomic_val_t)k_uptime_get_32());
	atomic_set(&loss_was_peer_close, reason == HCI_ERR_REMOTE_USER_TERM_CONN);
	atomic_set(&link_lost, 1);
	if (reason == HCI_ERR_CONN_FAIL_TO_ESTAB) {
		/* `connected` fired, but the first packets never came back: a
		 * reconnect attempt that failed, not a link lost at some step.
		 * current_level still names the step of the previous link. */
		LOG_WRN("split link: reconnect attempt failed (reason 0x3e), ZMK retries");
	} else if (reason == HCI_ERR_REMOTE_USER_TERM_CONN) {
		LOG_INF("split link closed by the other half at step %d%s (reason 0x13: on "
			"purpose, as before its sleep) -- not a step failure",
			depth_of(current_level), column_tag(current_level));
	} else {
		/* 0x08 is a supervision timeout: the halves stopped hearing each
		 * other, which is what a missed parameter update looks like from
		 * here. The level is read without the handler's say-so, for the
		 * log only. */
		LOG_WRN("split link lost at step %d%s (reason 0x%02x)", depth_of(current_level),
			column_tag(current_level), reason);
	}
	reschedule(K_NO_WAIT);
}

/*
 * What the controller actually applied. This is the line the whole exercise
 * is measured by: the delay between a "step N" line and this one is the real
 * switch time, and comparing the values shows whether the request survived
 * intact. Host-link updates are logged too -- that is where the interval and
 * latency the host granted become visible.
 */
static void on_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
			     uint16_t timeout)
{
	struct bt_conn_info info;

	if (bt_conn_get_info(conn, &info) || info.type != BT_CONN_TYPE_LE) {
		return;
	}

	if (info.role == BT_CONN_ROLE_CENTRAL) {
		atomic_set(&applied_interval, interval);
		atomic_set(&applied_latency, latency);
		atomic_set(&applied_timeout, timeout);
		atomic_inc(&applied_gen);
		atomic_set(&update_in_flight, 0);
		atomic_set(&update_confirmed, 1);
		/* The commit itself happens in the work handler, the only
		 * writer of the level, so it cannot race a request in flight. */
		reschedule(K_NO_WAIT);
	}

	LOG_INF("%s link applied: interval %u.%02u ms, latency %u, timeout %u ms",
		info.role == BT_CONN_ROLE_CENTRAL ? "split" : "host", INT_MS_W(interval),
		INT_MS_F(interval), latency, timeout * 10);
	if (info.role != BT_CONN_ROLE_CENTRAL) {
		host_interval_seen(interval, HOST_CHANGED);
	}
}

static struct bt_conn_cb conn_cb = {
	.connected = on_connected,
	.disconnected = on_disconnected,
	.le_param_updated = on_param_updated,
};

static int zmk_split_conn_ladder_init(void)
{
	/* void in Zephyr 3.5, returns int from 3.7 on -- revisit when the
	 * ZMK base moves. */
	bt_conn_cb_register(&conn_cb);
	current_level = 0;
	pending_level = -1;
	superseded_level = -1;
	max_level = N_IDLE;
	max_level_usb = N_IDLE;
	return 0;
}

/*
 * The queue starts at POST_KERNEL, where ZMK starts its own: before any
 * APPLICATION-level init can raise an event that reaches our listener. A
 * submit to a queue not yet started is refused, and would be lost.
 */
static int zmk_split_conn_ladder_queue_init(void)
{
	static const struct k_work_queue_config cfg = {.name = "split_conn_ladder"};

	k_work_queue_start(&ladder_q, ladder_stack, K_THREAD_STACK_SIZEOF(ladder_stack),
			   CONFIG_ZMK_SPLIT_CONN_LADDER_WORK_QUEUE_PRIORITY, &cfg);
	return 0;
}

SYS_INIT(zmk_split_conn_ladder_queue_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
SYS_INIT(zmk_split_conn_ladder_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
