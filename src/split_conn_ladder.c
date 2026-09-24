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
 * So the steps below are tuned to a SWITCH BUDGET rather than to a power
 * target: pick the interval for the central, then the largest latency that
 * still returns inside about 1.5 s. The peripheral ends up costing more than
 * it strictly has to, which is the right way round -- it outlives the central
 * several times over either way.
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
 * cable column, S*_USB_*: the ACTIVE interval, which delays no key, and a
 * higher latency. If the other half reports USB power as well
 * (peripheral_power_client.c), or ZMK_SPLIT_CONN_LADDER_USB_STEPS is off, AUTO
 * holds ACTIVE on USB instead. The &ladder behavior can force the battery
 * column ON whatever the power -- which is how it gets watched over a USB log
 * -- or switch the ladder OFF.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/logging/log.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_RSSI)
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

#include <dt-bindings/zmk/split_conn_ladder.h>
#include <zmk_split_conn_ladder/ladder.h>

#include "ladder_internal.h"

LOG_MODULE_REGISTER(zmk_split_conn_ladder, CONFIG_ZMK_SPLIT_CONN_LADDER_LOG_LEVEL);

#define SETTLE_MS         CONFIG_ZMK_SPLIT_CONN_LADDER_SETTLE_MS
#define RETRY_MS          CONFIG_ZMK_SPLIT_CONN_LADDER_RETRY_MS
#define UPDATE_TIMEOUT_MS CONFIG_ZMK_SPLIT_CONN_LADDER_UPDATE_TIMEOUT_MS
#define MIN_WINDOWS       CONFIG_ZMK_SPLIT_CONN_LADDER_MIN_WINDOWS
#define MAX_SWITCH_MS     CONFIG_ZMK_SPLIT_CONN_LADDER_MAX_SWITCH_MS

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_BACKOFF)
#define STABLE_MS     CONFIG_ZMK_SPLIT_CONN_LADDER_STABLE_MS
#define CONFIRM_MS    CONFIG_ZMK_SPLIT_CONN_LADDER_BACKOFF_CONFIRM_MS
#define STABLE_MAX_MS 604800000U /* one week; keeps the doubling bounded */
/* How long a loss waits for the other half's report before it is judged on
 * the return alone. The report comes ~2 s after the connect. */
#define JUDGE_WAIT_MS 10000U
#endif

/* BT_HCI_ERR_CONN_FAIL_TO_ESTAB (hci_types.h): `connected` fired, and the link
 * died in its first connection events. */
#define HCI_ERR_CONN_FAIL_TO_ESTAB 0x3e

/* Bluetooth interval units (1.25 ms) to milliseconds, integer only. */
#define INT_MS_W(i) ((i) * 125 / 100)
#define INT_MS_F(i) ((i) * 125 % 100)
/* n connection events of interval i, in whole ms, rounded: 13 units (16.25 ms)
 * times 10 is 163, not the 160 that rounding the interval first would give. */
#define EVENTS_MS(i, n) (((uint32_t)(i) * 125U * (uint32_t)(n) + 50U) / 100U)

struct step {
	uint32_t after_ms;
	uint16_t interval;
	uint16_t latency;
	uint16_t timeout;
};

#define STEP(n)                                                      \
	{                                                            \
		.after_ms = CONFIG_ZMK_SPLIT_CONN_LADDER_S##n##_AFTER_MS,    \
		.interval = CONFIG_ZMK_SPLIT_CONN_LADDER_S##n##_INT,         \
		.latency = CONFIG_ZMK_SPLIT_CONN_LADDER_S##n##_LATENCY,      \
		.timeout = CONFIG_ZMK_SPLIT_CONN_LADDER_S##n##_TIMEOUT,      \
	}

/*
 * Two build-time guards per step.
 *
 * CHECK_SPEC is Zephyr's own bt_le_conn_params_valid() rule, from BT Core 5.0
 * Vol 2 Part E 7.8.12: a parameter set that fails it is refused at runtime.
 *
 * CHECK_WINDOWS is the one that actually keeps links alive. It demands that
 * the supervision timeout cover at least MIN_WINDOWS peripheral attendances,
 * which is how many of them may be missed before the link is declared dead.
 * The spec rule alone permits as few as two, and two is not survivable on a
 * 500 ppm board. It is also what stops a deep step from silently becoming
 * more fragile than a shallow one: raise the interval without dropping the
 * latency to match and the build fails.
 *
 *     windows = timeout * 10 / (interval * 1.25 * (latency + 1))
 *   ->  timeout * 8 >= MIN_WINDOWS * interval * (latency + 1)
 */
#define CHECK_SPEC(name, to, lat, in)                                                \
	BUILD_ASSERT((to) * 4 > (1 + (lat)) * (in),                                  \
		     name ": supervision timeout below the Bluetooth minimum")

#define CHECK_WINDOWS(name, to, lat, in)                                             \
	BUILD_ASSERT((to) * 8 >= MIN_WINDOWS * (in) * (1 + (lat)),                   \
		     name ": fewer supervision windows than MIN_WINDOWS -- "         \
			  "raise the timeout or lower the latency")

/*
 * The third guard is the user-visible one. Leaving a step costs
 *     interval * (3 * latency + 8)
 * and for that whole time every keystroke from the peripheral half is still
 * delayed by a full interval -- several bad keys in a row, not one. Capping it
 * at build time is what stops a step from being tuned for power alone.
 *
 *     interval * 1.25 * (3 * latency + 8) <= MAX_SWITCH_MS
 *   ->  interval * 125 * (3 * latency + 8) <= MAX_SWITCH_MS * 100
 */
#define CHECK_SWITCH(name, lat, in)                                                  \
	BUILD_ASSERT((in) * 125 * (3 * (lat) + 8) <= MAX_SWITCH_MS * 100,            \
		     name ": leaving this step takes longer than "                   \
			  "MAX_SWITCH_MS -- lower the latency or the interval")

#define CHECK(name, to, lat, in)                                                     \
	CHECK_SPEC(name, to, lat, in);                                               \
	CHECK_WINDOWS(name, to, lat, in)

#define CHECK_STEP(n)                                            \
	CHECK("step " #n, CONFIG_ZMK_SPLIT_CONN_LADDER_S##n##_TIMEOUT,   \
	      CONFIG_ZMK_SPLIT_CONN_LADDER_S##n##_LATENCY,               \
	      CONFIG_ZMK_SPLIT_CONN_LADDER_S##n##_INT);                  \
	CHECK_SWITCH("step " #n,                                         \
		     CONFIG_ZMK_SPLIT_CONN_LADDER_S##n##_LATENCY,        \
		     CONFIG_ZMK_SPLIT_CONN_LADDER_S##n##_INT)

/*
 * An update that is still running must not be declared lost: the deepest step
 * is allowed to take MAX_SWITCH_MS, so the timeout has to be longer than that
 * or the ladder would re-issue on top of a procedure in flight -- exactly the
 * back-to-back instants that kill links.
 */
BUILD_ASSERT(UPDATE_TIMEOUT_MS > MAX_SWITCH_MS,
	     "ZMK_SPLIT_CONN_LADDER_UPDATE_TIMEOUT_MS must exceed MAX_SWITCH_MS");

/*
 * The ladder runs on a work queue of its own. bt_conn_le_param_update() ends
 * in bt_hci_cmd_send_sync() and blocks until the controller answers, which
 * must not happen on the system work queue, where ZMK builds and sends HID
 * reports.
 *
 * It used to share ZMK's low-priority queue, and that forced a line on every
 * user: the chain below needs more than ZMK's 768-byte default for that
 * thread, and a module cannot raise it -- `select` does not work for an int,
 * and ZMK's own `default 768` is parsed before any module Kconfig, so it
 * always wins. A queue of our own sizes itself (ZMK_SPLIT_CONN_LADDER_STACK_
 * SIZE: 1024, or 1536 in a logging build), costs about that much RAM, and no
 * longer queues behind -- or blocks -- other modules' low-priority work.
 *
 * Measured with -fstack-usage on real nice_nano_v2 builds of ZMK 0.3.0,
 * central role:
 *
 *                                   without logging   with USB logging
 *   thread entry + work_queue_main         64                 64
 *   ladder_work_cb                         48                200
 *   apply_level                            32                168
 *   bt_conn_foreach                        32                 32
 *   apply_to_conn                          64                 64
 *   bt_conn_le_param_update and below     528                652
 *   total                                 768               1180
 *
 * The other thing on this queue, peripheral_power_client.c starting its GATT
 * discovery, is shallower: 680 and 1012 bytes including the entry.
 *
 * Logging costs that much because every LOG_* with arguments packages them
 * on the caller's stack, and the Bluetooth host logs from inside the HCI
 * call. The guard only fires if someone lowers the symbol by hand: an
 * overflow here would land minutes into an idle period, with nothing in the
 * log to explain it.
 */
#if IS_ENABLED(CONFIG_LOG)
#define LADDER_STACK_MIN 1536
#else
#define LADDER_STACK_MIN 1024
#endif

BUILD_ASSERT(CONFIG_ZMK_SPLIT_CONN_LADDER_STACK_SIZE >= LADDER_STACK_MIN,
	     "CONFIG_ZMK_SPLIT_CONN_LADDER_STACK_SIZE must be at least "
	     STRINGIFY(LADDER_STACK_MIN) " (1024 without logging, 1536 with it): "
	     "applying a step needs ~770 bytes of stack, ~1200 in a logging "
	     "build. Its default is right; remove the override.");

K_THREAD_STACK_DEFINE(ladder_stack, CONFIG_ZMK_SPLIT_CONN_LADDER_STACK_SIZE);
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
 * ACTIVE reads the very symbols the link is created with in
 * bt_conn_le_create(), so our idea of "fast" and the link's actual state
 * cannot diverge. Do not turn these into separate Kconfig options.
 */
static const struct step step_active = {
	.interval = CONFIG_ZMK_SPLIT_BLE_PREF_INT,
	.latency = CONFIG_ZMK_SPLIT_BLE_PREF_LATENCY,
	.timeout = CONFIG_ZMK_SPLIT_BLE_PREF_TIMEOUT,
};

CHECK("active", CONFIG_ZMK_SPLIT_BLE_PREF_TIMEOUT, CONFIG_ZMK_SPLIT_BLE_PREF_LATENCY,
      CONFIG_ZMK_SPLIT_BLE_PREF_INT);

static const struct step idle_steps[] = {
	STEP(1),
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEPS >= 2
	STEP(2),
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEPS >= 3
	STEP(3),
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEPS >= 4
	STEP(4),
#endif
};

CHECK_STEP(1);
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEPS >= 2
CHECK_STEP(2);
BUILD_ASSERT(CONFIG_ZMK_SPLIT_CONN_LADDER_S2_AFTER_MS >
		     CONFIG_ZMK_SPLIT_CONN_LADDER_S1_AFTER_MS,
	     "step 2 must come after step 1");
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEPS >= 3
CHECK_STEP(3);
BUILD_ASSERT(CONFIG_ZMK_SPLIT_CONN_LADDER_S3_AFTER_MS >
		     CONFIG_ZMK_SPLIT_CONN_LADDER_S2_AFTER_MS,
	     "step 3 must come after step 2");
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEPS >= 4
CHECK_STEP(4);
BUILD_ASSERT(CONFIG_ZMK_SPLIT_CONN_LADDER_S4_AFTER_MS >
		     CONFIG_ZMK_SPLIT_CONN_LADDER_S3_AFTER_MS,
	     "step 4 must come after step 3");
#endif

#define N_IDLE ((int)ARRAY_SIZE(idle_steps))

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_USB_STEPS)
/*
 * The cable column: the same steps on the same timers, with the parameters
 * used in AUTO mode on USB power. Checked exactly like the battery column.
 */
#define USB_STEP(n)                                                      \
	{                                                                \
		.after_ms = CONFIG_ZMK_SPLIT_CONN_LADDER_S##n##_AFTER_MS,        \
		.interval = CONFIG_ZMK_SPLIT_CONN_LADDER_S##n##_USB_INT,         \
		.latency = CONFIG_ZMK_SPLIT_CONN_LADDER_S##n##_USB_LATENCY,      \
		.timeout = CONFIG_ZMK_SPLIT_CONN_LADDER_S##n##_USB_TIMEOUT,      \
	}

#define CHECK_USB_STEP(n)                                                \
	CHECK("step " #n " on USB", CONFIG_ZMK_SPLIT_CONN_LADDER_S##n##_USB_TIMEOUT, \
	      CONFIG_ZMK_SPLIT_CONN_LADDER_S##n##_USB_LATENCY,                   \
	      CONFIG_ZMK_SPLIT_CONN_LADDER_S##n##_USB_INT);                      \
	CHECK_SWITCH("step " #n " on USB",                                       \
		     CONFIG_ZMK_SPLIT_CONN_LADDER_S##n##_USB_LATENCY,            \
		     CONFIG_ZMK_SPLIT_CONN_LADDER_S##n##_USB_INT)

static const struct step usb_steps[] = {
	USB_STEP(1),
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEPS >= 2
	USB_STEP(2),
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEPS >= 3
	USB_STEP(3),
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEPS >= 4
	USB_STEP(4),
#endif
};

CHECK_USB_STEP(1);
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEPS >= 2
CHECK_USB_STEP(2);
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEPS >= 3
CHECK_USB_STEP(3);
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEPS >= 4
CHECK_USB_STEP(4);
#endif
#endif /* CONFIG_ZMK_SPLIT_CONN_LADDER_USB_STEPS */

/*
 * Level 0 is ACTIVE, 1..N_IDLE index idle_steps[level - 1] -- the battery
 * column -- and N_IDLE+1..2*N_IDLE the cable column, usb_steps[] at the same
 * depths. One number per parameter set keeps everything below column-blind:
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
 * queue. The Bluetooth callbacks run on the RX thread and do nothing but raise
 * atomic flags, because the compare-then-set inside apply_level() is not
 * atomic: a reconnect landing mid-update would otherwise leave our belief and
 * the link's reality permanently out of step.
 */
static int current_level;
static int pending_level = -1;
static int superseded_level = -1;
/* Depth caps (0..N_IDLE) set by link losses, one per column: a depth that
 * fails on battery parameters says nothing about the cable ones. */
static int max_level = N_IDLE;
static int max_level_usb = N_IDLE;

/* Consecutive refused updates; widens the retry. Handler-only. */
static uint8_t reject_streak;

/* 32-bit uptime: the read is atomic on a 32-bit target and unsigned
 * subtraction stays correct across the ~49-day wrap. */
static uint32_t idle_since;
static uint32_t update_issued_at;

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_BACKOFF)
static uint32_t stable_since;
static uint32_t stable_required = STABLE_MS;
static uint8_t backoff_count;
static uint32_t lost_at;
static int lost_level;
static bool loss_armed;
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

static atomic_t link_reset;
static atomic_t link_lost;
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
	(IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_PAUSE_ON_USB) ? LADDER_AUTO : LADDER_ON)

static atomic_t ladder_mode = ATOMIC_INIT(BOOT_MODE);

/* What the mode and the power source add up to. Handler-only state. */
enum gate {
	GATE_UNKNOWN = -1,
	GATE_RUN,   /* the battery column */
	GATE_CABLE, /* AUTO with USB power: the cable column */
	GATE_USB,   /* AUTO with USB power, held at ACTIVE: USB_STEPS off, or
		     * the other half on USB power too */
	GATE_OFF,   /* OFF: held at ACTIVE */
};

static enum gate last_gate = GATE_UNKNOWN;

static bool is_cable(int level)
{
	return level > N_IDLE;
}

/* 0 for ACTIVE, 1..N_IDLE for a step of either column; -1 stays -1. */
static int depth_of(int level)
{
	return is_cable(level) ? level - N_IDLE : level;
}

static int level_for(bool cable, int depth)
{
	return cable && depth > 0 ? depth + N_IDLE : depth;
}

static const char *col_tag(int level)
{
	return is_cable(level) ? " on cable" : "";
}

static const struct step *step_at(int level)
{
	if (level <= 0) {
		return &step_active;
	}
#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_USB_STEPS)
	if (is_cable(level)) {
		return &usb_steps[level - N_IDLE - 1];
	}
#endif
	return &idle_steps[level - 1];
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
		return IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_USB_STEPS) && !both ? GATE_CABLE
										     : GATE_USB;
	}
	return GATE_RUN;
}

static const char *gate_name(enum gate gate)
{
	switch (gate) {
	case GATE_RUN:
		return "on battery steps";
	case GATE_CABLE:
		return "on cable steps";
	default:
		return "held at ACTIVE";
	}
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_RSSI)
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
	const bool from_known = pending_level >= 0 || current_level >= 0;
	const int from_level = pending_level >= 0 ? pending_level : MAX(current_level, 0);
	const struct step *from = step_at(from_level);
	struct apply_ctx ctx = {.s = s, .err = 0, .matched = 0, .already = false};

	bt_conn_foreach(BT_CONN_TYPE_LE, apply_to_conn, &ctx);

	if (ctx.err) {
		return ctx.err;
	}

	if (ctx.matched == 0) {
		/* Nothing to command. Leave current_level alone: on_connected()
		 * resets it to 0, which is where a freshly created link really
		 * is, so there is nothing to record here. */
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
			/* INF, not DBG: with the default cable steps this is how
			 * step 2 on cable shows up at all -- it has step 1's
			 * parameters, so nothing is sent. */
			LOG_INF("step %d/%d%s: parameters already in place", depth_of(level), N_IDLE,
				col_tag(level));
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
	 */
	uint32_t from_air = EVENTS_MS(from->interval, 3 * from->latency + 8);

	LOG_INF("step %d/%d%s: interval %u.%02u ms, latency %u, timeout %u ms "
		"(peripheral on air every %u ms; switch from level %d%s ~%u ms)",
		depth_of(level), N_IDLE, col_tag(level), INT_MS_W(s->interval),
		INT_MS_F(s->interval), s->latency, s->timeout * 10,
		EVENTS_MS(s->interval, s->latency + 1), from_known ? depth_of(from_level) : -1,
		from_known ? col_tag(from_level) : "", from_air);

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

struct k_work_q *split_conn_ladder_work_q(void)
{
	return &ladder_q;
}

/* The other half's USB power changed: let the handler re-read the gate. */
void split_conn_ladder_peripheral_power_changed(void)
{
	reschedule(K_NO_WAIT);
}

/* The other half's report carried its uptime: a loss waiting on it can be
 * judged now. */
void split_conn_ladder_peripheral_uptime(uint32_t uptime_ms)
{
	atomic_set(&peer_uptime, (atomic_val_t)uptime_ms);
	atomic_set(&peer_report_at, (atomic_val_t)k_uptime_get_32());
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
		uint32_t cap = UPDATE_TIMEOUT_MS + RETRY_MS;

		if (!have_deadline || delay_ms > cap) {
			delay_ms = cap;
			have_deadline = true;
		}
	}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_BACKOFF)
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
 * first retry comes after RETRY_MS; each further refusal in a row doubles the
 * wait, up to UPDATE_TIMEOUT_MS, so that something which does not clear costs
 * a log line every few seconds instead of an HCI round trip twice a second.
 * Any accepted update resets the streak.
 */
#define RETRY_CAP_MS MAX(UPDATE_TIMEOUT_MS, RETRY_MS)

static void retry_after_reject(int err)
{
	const uint32_t delay =
		MIN((uint32_t)RETRY_MS << MIN(reject_streak, 6), (uint32_t)RETRY_CAP_MS);

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
	if (now - update_issued_at >= UPDATE_TIMEOUT_MS) {
		atomic_set(&update_in_flight, 0);
		return false;
	}
	return true;
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_BACKOFF)
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
static bool same_params(const struct step *a, const struct step *b)
{
	return a->interval == b->interval && a->latency == b->latency && a->timeout == b->timeout;
}

/*
 * Neighbouring steps with the same parameters are one depth as far as the link
 * can tell -- the default cable steps come in pairs, S1/S2 and S3/S4 -- so a
 * cap never falls between two of them. Otherwise a loss at cable step 4 caps
 * the ladder at cable step 3, which runs the very parameters that just failed,
 * and it takes a second loss to get anywhere.
 */
static int group_first(bool cable, int depth)
{
	while (depth > 1 && same_params(step_at(level_for(cable, depth - 1)),
					step_at(level_for(cable, depth)))) {
		depth--;
	}
	return depth;
}

static int group_last(bool cable, int depth)
{
	while (depth < N_IDLE && same_params(step_at(level_for(cable, depth + 1)),
					     step_at(level_for(cable, depth)))) {
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
	if (away > CONFIRM_MS) {
		LOG_INF("split link was away %u.%u s -- treating as the other half "
			"sleeping, not a step failure",
			away / 1000U, away % 1000U / 100U);
		return;
	}

	/* Charged to the column it failed in; the other one keeps its depth.
	 * Below every step that runs the parameters that failed. */
	const bool cable = is_cable(lost_level);
	const int depth = depth_of(lost_level);
	const int limit = group_first(cable, depth) - 1;
	int *cap = cable ? &max_level_usb : &max_level;

	if (*cap > limit) {
		*cap = limit;
	}

	if (backoff_count < 255) {
		backoff_count++;
	}

	uint64_t widened = (uint64_t)STABLE_MS << MIN(backoff_count - 1, 5);

	stable_required = (uint32_t)MIN(widened, (uint64_t)STABLE_MAX_MS);

	LOG_WRN("split link dropped at step %d%s and returned in %u.%u s -- limiting "
		"depth%s to %d, retrying it in %u min",
		depth, col_tag(lost_level), away / 1000U, away % 1000U / 100U,
		col_tag(lost_level), *cap, stable_required / 60000U);
}

/*
 * A loss that came back waits here for the other half's report, which carries
 * its uptime. If that half restarted while the link was down -- its own deep
 * sleep, a reset, a battery pulled -- the link was never the problem, however
 * fast it came back: ZMK powers a sleeping half off without a word, so from
 * this side its sleep looks exactly like a supervision timeout, and a key
 * pressed on it a few seconds later looks exactly like a quick return. With no
 * report in time -- a half built without it -- the return alone decides, as
 * before.
 */
static void settle_judgment(uint32_t now)
{
	const uint32_t at = (uint32_t)atomic_get(&peer_report_at);

	if ((int32_t)(at - judge_back_at) >= 0) {
		const uint32_t up = (uint32_t)atomic_get(&peer_uptime);

		judge_pending = false;
		/* Up for less than the link was down: it booted after its last
		 * packet reached us. */
		if (up < at - last_heard) {
			LOG_INF("split link lost at step %d%s: the other half restarted while "
				"it was down (up %u.%u s) -- its sleep or a reset, not a step "
				"failure",
				depth_of(lost_level), col_tag(lost_level), up / 1000U,
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
static void link_went_down(uint32_t at)
{
	atomic_set(&update_in_flight, 0);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_BACKOFF)
	if (judge_pending) {
		/* Back and gone again before the other half reported: judge the
		 * earlier loss on its return alone, before this one overwrites it. */
		judge_pending = false;
		judge_loss(judge_back_at);
	}
	lost_at = at;
	lost_level = MAX(current_level, 0);
	last_heard = at - step_at(lost_level)->timeout * 10U;
	loss_armed = true;
#else
	ARG_UNUSED(at);
#endif
	pending_level = -1;
	superseded_level = -1;
}

/*
 * A split link came up at `at`. It comes up on PREF_*, which is exactly
 * ACTIVE. Restart the idle clock with it: without this, a reconnect deep into
 * an idle period drops the brand-new link straight to the deepest step a few
 * seconds later. If that depth is what broke the link in the first place, it
 * breaks again, reconnects, breaks again -- a loop that looks from the
 * outside like a half that never comes back.
 */
static void link_came_up(uint32_t at, uint32_t now)
{
	current_level = 0;
	pending_level = -1;
	superseded_level = -1;
	idle_since = now;
	atomic_set(&update_in_flight, 0);
	atomic_set(&update_confirmed, 0);
	reject_streak = 0;
#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_BACKOFF)
	if (loss_armed) {
		/* Judged on how long the link was really gone: from the
		 * disconnect to this connect, both stamped where they happened,
		 * not on when this handler got round to running. A loss that
		 * could count against a step first waits for the other half's
		 * report, which says whether it restarted meanwhile. */
		if (IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_USB_STEPS) && lost_level >= 1 &&
		    at - lost_at <= CONFIRM_MS) {
			loss_armed = false;
			judge_pending = true;
			judge_back_at = at;
			judge_until = now + JUDGE_WAIT_MS;
		} else {
			judge_loss(at);
		}
	}
	stable_since = now;
#else
	ARG_UNUSED(at);
#endif
	LOG_DBG("link re-established, level 0, idle clock restarted");
}

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
				depth_of(superseded_level), col_tag(superseded_level),
				depth_of(pending_level), col_tag(pending_level));
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
			depth_of(pending_level), col_tag(pending_level));
		pending_level = -1;
		superseded_level = -1;
		current_level = -1;
	}

	/*
	 * Link losses and re-connections. The callbacks only raise flags and
	 * stamp the time, so a disconnect and a connect can both be waiting
	 * here -- on_connected() even pushes this run SETTLE_MS out, over the
	 * immediate one on_disconnected() asked for. The flags cannot say which
	 * came first; the stamps can, and the order decides what the loss is
	 * charged to.
	 */
	const bool lost = atomic_cas(&link_lost, 1, 0);
	const bool reset = atomic_cas(&link_reset, 1, 0);
	const uint32_t lost_t = (uint32_t)atomic_get(&lost_stamp);
	const uint32_t reset_t = (uint32_t)atomic_get(&reset_stamp);

	if (lost && reset && (int32_t)(lost_t - reset_t) > 0) {
		/* Came back and dropped again before we ran: the link that
		 * dropped was the fresh one, on ACTIVE parameters. */
		link_came_up(reset_t, now);
		link_went_down(lost_t);
	} else {
		if (lost) {
			link_went_down(lost_t);
		}
		if (reset) {
			link_came_up(reset_t, now);
		}
	}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_BACKOFF)
	/* Before anything can step down: a cap it produces applies at once. */
	if (judge_pending) {
		settle_judgment(now);
	}
#endif

	/*
	 * The gate: may the ladder step down at all, and on which parameter
	 * set -- battery or cable? Evaluated before the link check, so that
	 * plugging and unplugging show up in the log even while the other half
	 * is away.
	 */
	const int mode = (int)atomic_get(&ladder_mode);
	const bool usb = usb_powered();
	/* The other half's report; false while unknown, and always false
	 * without USB_STEPS, where nothing reads it. */
	const bool both = usb && split_conn_ladder_peripheral_on_usb();
	const enum gate gate = gate_for(mode, usb, both);

	if (gate != last_gate) {
		if (gate == GATE_RUN && last_gate != GATE_UNKNOWN) {
			/*
			 * Onto the battery column from anything else -- a hold,
			 * or the cable column: start from the top, as if the
			 * keyboard had only now gone idle. Pulling the cable is
			 * usually the moment before somebody picks the keyboard
			 * up, and dropping straight to a deep battery step would
			 * make the first words from the other half pay the slowest
			 * return there is. Onto the cable column the clock keeps
			 * running: a cable step delays no key, so plugging in
			 * keeps the depth and only swaps the parameters.
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

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_BACKOFF)
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
		if (IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_USB_STEPS)) {
			LOG_INF("split link stable for %u min, allowing depth %d on battery, "
				"%d on cable again",
				stable_required / 60000U, max_level, max_level_usb);
		} else {
			LOG_INF("split link stable for %u min, allowing depth %d again",
				stable_required / 60000U, max_level);
		}
		if (max_level == N_IDLE && max_level_usb == N_IDLE) {
			/* Back to full depth: start the next accusation from the
			 * short retry again instead of the widened one. */
			backoff_count = 0;
			stable_required = STABLE_MS;
		}
	}
#endif

	/* The activity state is read here rather than in the listener, so that
	 * an already-running instance cannot clobber a just-applied ACTIVE. */
	enum zmk_activity_state st = zmk_activity_get_state();

	if (st == ZMK_ACTIVITY_SLEEP) {
		/* The board is powering off; there is nothing to restore. */
		return;
	}

	if (st == ZMK_ACTIVITY_ACTIVE || gate == GATE_USB || gate == GATE_OFF) {
		/* Deliberately not gated on update_pending(): coming back up is
		 * urgent, and the controller queues a second procedure behind
		 * the first rather than refusing it. A hold takes the same road:
		 * the other half plugging in too brings the link straight back. */
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
	const bool cable = gate == GATE_CABLE;
	const int cap = cable ? max_level_usb : max_level;
	int depth = 0;

	/* One timer for both columns: the depth depends on idle time only. */
	for (int i = N_IDLE - 1; i >= 0; i--) {
		if (elapsed >= idle_steps[i].after_ms) {
			depth = i + 1;
			break;
		}
	}

	depth = MIN(depth, cap);

	const int level = level_for(cable, depth);

	/*
	 * A fresh link is left alone for SETTLE_MS, whatever woke us. The delay
	 * on_connected() puts on this run is not enough on its own: a key, a
	 * cable, the &ladder key or the other half's power report pulls the run
	 * back in, and with S1_AFTER_MS below SETTLE_MS a step would then land
	 * in the middle of ZMK's set-up. ACTIVE never waits -- a new link is at
	 * ACTIVE anyway. The stamp is read before the clock, as with idle_since,
	 * so a connect that lands in between cannot wrap the difference.
	 */
	if (level != 0) {
		const uint32_t up_at = (uint32_t)atomic_get(&reset_stamp);
		const uint32_t up_for = k_uptime_get_32() - up_at;

		if (up_for < SETTLE_MS) {
			schedule_next(SETTLE_MS - up_for, true);
			return;
		}
	}

	/* Anything but ACTIVE waits for the update in flight -- a deeper step,
	 * and a change of column too. Stacking a second procedure behind one
	 * that has not landed makes the peripheral process two instants back
	 * to back, which is exactly when it drops one; and neither is urgent:
	 * a cable step delays no key, and a key press goes to ACTIVE, which
	 * never waits. */
	if (level != 0 && level != effective_level() && update_pending(now)) {
		reschedule(K_MSEC(RETRY_MS));
		return;
	}

	int err = apply_level(level);

	if (err) {
		retry_after_reject(err);
		return;
	}
	reject_streak = 0;

	if (depth < cap) {
		/* Uncapped, so the loop did not select idle_steps[depth]: elapsed
		 * is strictly below its after_ms and the difference is positive. */
		schedule_next(idle_steps[depth].after_ms - elapsed, true);
		return;
	}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_BACKOFF)
	if (max_level < N_IDLE || max_level_usb < N_IDLE) {
		/* A column held at a reduced depth by an earlier link loss -- this
		 * one, or the other. Keep a timer alive so the stability check
		 * above can hand the next notch back without waiting for the
		 * user to touch the keyboard. */
		uint32_t waited = k_uptime_get_32() - stable_since;

		schedule_next(waited >= stable_required ? RETRY_MS : stable_required - waited,
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

static void on_activity(enum zmk_activity_state state)
{
	LOG_INF("activity: %s", state_name(state));

	switch (state) {
	case ZMK_ACTIVITY_IDLE:
		idle_since = k_uptime_get_32();
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

	return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zmk_split_conn_ladder, on_event);
ZMK_SUBSCRIPTION(zmk_split_conn_ladder, zmk_activity_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(zmk_split_conn_ladder, zmk_usb_conn_state_changed);
#endif

/* The split link, as the callbacks see it: LE, and this half central. */
static bool is_split(struct bt_conn *conn)
{
	struct bt_conn_info info;

	return !bt_conn_get_info(conn, &info) && info.type == BT_CONN_TYPE_LE &&
	       info.role == BT_CONN_ROLE_CENTRAL;
}

/*
 * The link to the computer shares this half's radio with the split link, and
 * an ACTIVE split interval that is a multiple of the host's -- or the other
 * way round -- keeps the two links' events in one place relative to each
 * other for as long as the clocks take to drift apart: README, "The interval
 * and the computer's own link". Checked wherever the host's interval shows
 * up, so a host that picks a new one later is caught too.
 */
static void check_host_interval(uint16_t host)
{
	const uint16_t split = CONFIG_ZMK_SPLIT_BLE_PREF_INT;

	if (host && (split % host == 0 || host % split == 0)) {
		LOG_WRN("split interval %u.%02u ms and the host's %u.%02u ms are multiples: "
			"the two links will collide in long stretches",
			INT_MS_W(split), INT_MS_F(split), INT_MS_W(host), INT_MS_F(host));
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
		check_host_interval(info.le.interval);
		return;
	}

	/* Stamp first, flag second: the handler reads them the other way. */
	atomic_set(&reset_stamp, (atomic_val_t)k_uptime_get_32());
	atomic_set(&link_reset, 1);
	/*
	 * Right after `connected`, ZMK sets up the link -- encryption, GATT
	 * discovery, subscriptions -- and those are the procedures a step
	 * would have to queue behind. The handler holds every step back until
	 * SETTLE_MS after the stamp above, whatever else runs it earlier; this
	 * is simply when it next has something to decide.
	 */
	reschedule(K_MSEC(SETTLE_MS));
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	if (!is_split(conn)) {
		return;
	}

	atomic_set(&lost_stamp, (atomic_val_t)k_uptime_get_32());
	atomic_set(&link_lost, 1);
	if (reason == HCI_ERR_CONN_FAIL_TO_ESTAB) {
		/* `connected` fired, but the first packets never came back: a
		 * reconnect attempt that failed, not a link lost at some step.
		 * current_level still names the step of the previous link. */
		LOG_WRN("split link: reconnect attempt failed (reason 0x3e), ZMK retries");
	} else {
		/* 0x08 is a supervision timeout: the halves stopped hearing each
		 * other, which is what a missed parameter update looks like from
		 * here. The level is read without the handler's say-so, for the
		 * log only. */
		LOG_WRN("split link lost at step %d%s (reason 0x%02x)", depth_of(current_level),
			col_tag(current_level), reason);
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
		check_host_interval(interval);
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
			   CONFIG_ZMK_SPLIT_CONN_LADDER_THREAD_PRIORITY, &cfg);
	return 0;
}

SYS_INIT(zmk_split_conn_ladder_queue_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
SYS_INIT(zmk_split_conn_ladder_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
