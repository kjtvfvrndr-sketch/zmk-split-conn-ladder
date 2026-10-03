/*
 * Copyright (c) 2026 eXidno
 *
 * SPDX-License-Identifier: MIT
 *
 * The early sleep: a half with nothing to work for goes to deep sleep after
 * ABSENCE_WAIT_MS of idle, instead of waiting for ZMK's own sleep -- which, on
 * the central half, never comes while a cable is in, and on either half comes
 * only after ZMK_IDLE_SLEEP_TIMEOUT, however pointless being awake has been.
 *
 *   central     no computer: none connected on the active Bluetooth
 *               profile, and none that configured the board over USB (a
 *               computer asleep counts, a charger does not) -- on any power;
 *               with EARLY_SLEEP_WITHOUT_PERIPHERAL, no other half either,
 *               unless on USB power with a computer there.
 *   peripheral  no central -- on USB power too.
 *
 * Idle is ZMK's: the count starts when ZMK enters IDLE, or when the reason
 * arises, whichever is later, and starts again whenever a computer, the other
 * half or USB power comes or goes -- a computer connecting to pair included.
 * While the active profile is open for pairing, the central waits
 * EARLY_SLEEP_BLE_PAIRING_WAIT_MS instead.
 *
 * The sleep itself is ZMK's, step for step (app/src/activity.c): the SLEEP
 * activity event first, so that whatever acts on it puts itself away -- a
 * WS2812 keeps its colour through System OFF unless it is told to go black;
 * code that asks zmk_activity_get_state() instead sees IDLE, see below -- then
 * zmk_pm_suspend_devices() and sys_poweroff(). The kscan and whatever else ZMK
 * leaves armed as a wake-up source wakes the half, and it boots. It all runs
 * on the system work queue, as ZMK's does: that queue is cooperative, so
 * nothing else runs between the event and the power going off unless a
 * listener or a device's suspend blocks -- as with ZMK's own sleep. The reason
 * is logged ANNOUNCE_MS before, while the log still has a chance to get out.
 *
 * ZMK's own activity state is not touched: it stays IDLE, and if suspending
 * the devices fails the IDLE event is raised again -- the listeners come back
 * from SLEEP -- and the sleep is retried RETRY_MS later.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/activity.h>
#include <zmk/pm.h>
#include <zmk/events/activity_state_changed.h>

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
#include <zmk/usb.h>
#include <zmk/events/usb_conn_state_changed.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#include <zmk/events/ble_active_profile_changed.h>
#endif
LOG_MODULE_DECLARE(zmk_split_conn_ladder, CONFIG_ZMK_SPLIT_CONN_LADDER_LOG_LEVEL);
#else
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/events/split_peripheral_status_changed.h>
LOG_MODULE_REGISTER(zmk_split_conn_ladder_early_sleep,
		    CONFIG_ZMK_SPLIT_CONN_LADDER_EARLY_SLEEP_LOG_LEVEL);
#endif

#include "absence_wait.h"

#define ANNOUNCE_MS 100
#define RETRY_MS    5000

#define CENTRAL IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

/* A test build (Kconfig): on the cable of the computer that takes the log,
 * act as on battery with no USB host. Central half only. */
#define IGNORE_USB IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_TEST_SLEEP_AND_SEARCH_IGNORE_USB)
#if IGNORE_USB
#warning "zmk-split-conn-ladder: TEST_SLEEP_AND_SEARCH_IGNORE_USB is on -- a test build: a USB host does not count as a computer"
#endif

enum reason {
	R_NONE,
	R_NO_HOST,
	R_NO_PERIPHERAL,
	R_NO_CENTRAL,
};

static const char *const reason_text[] = {
	[R_NONE] = "",
	[R_NO_HOST] = "no computer",
	[R_NO_PERIPHERAL] = "no other half",
	[R_NO_CENTRAL] = "no central half",
};

/*
 * Stamps, k_uptime_get_32(), written where things happen -- listeners and
 * Bluetooth callbacks, on whatever thread raised them -- and read by the
 * handler. Unsigned 32-bit: the read is atomic and differences survive the
 * ~49-day wrap.
 */
static atomic_t idle_at;  /* ZMK entered IDLE */
static atomic_t input_at; /* a computer, the other half or USB power came or went */
#if GUARD_USB
static atomic_t usb_at; /* a cable went in or out */
#endif

/* Set while this file raises an activity event of its own, so that its own
 * listener does not take it for ZMK's. The raise is synchronous and runs on
 * the system work queue, like everything that could look at this. */
static bool raising;

/* Handler-only: the reason was logged, and the sleep follows on the next run
 * if nothing changed in between. */
static bool announced;

static void sleep_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sleep_work, sleep_work_cb);

static void stamp(atomic_t *at)
{
	atomic_set(at, (atomic_val_t)k_uptime_get_32());
}

static void evaluate_soon(void)
{
	(void)k_work_reschedule(&sleep_work, K_NO_WAIT);
}

#if CENTRAL
static bool usb_powered(void)
{
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
	return !IGNORE_USB && zmk_usb_is_powered();
#else
	return false;
#endif
}

static void count_split(struct bt_conn *conn, void *data)
{
	struct bt_conn_info info;

	if (bt_conn_get_info(conn, &info) || info.type != BT_CONN_TYPE_LE ||
	    info.state != BT_CONN_STATE_CONNECTED || info.role != BT_CONN_ROLE_CENTRAL) {
		return;
	}
	(*(int *)data)++;
}

static bool other_half_up(void)
{
	int n = 0;

	bt_conn_foreach(BT_CONN_TYPE_LE, count_split, &n);
	return n > 0;
}

/* A computer: connected on the active Bluetooth profile, or one that
 * configured the board over USB -- asleep counts, a charger does not. */
static bool host_present(void)
{
#if IS_ENABLED(CONFIG_ZMK_BLE)
	if (zmk_ble_active_profile_is_connected()) {
		return true;
	}
#endif
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
	if (!IGNORE_USB && zmk_usb_is_hid_ready()) {
		return true;
	}
#endif
	return false;
}

static bool pairing(void)
{
#if IS_ENABLED(CONFIG_ZMK_BLE)
	return zmk_ble_active_profile_is_open();
#else
	return false;
#endif
}

static enum reason why_sleep(void)
{
	if (!host_present()) {
		return R_NO_HOST;
	}
	/* With a computer, the other half is a reason only on battery: on USB
	 * power with a computer there, this half never sleeps early. */
	if (IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_EARLY_SLEEP_WITHOUT_PERIPHERAL) &&
	    !usb_powered() && !other_half_up()) {
		return R_NO_PERIPHERAL;
	}
	return R_NONE;
}

static uint32_t wait_ms(void)
{
	return pairing() ? BLE_PAIRING_WAIT_MS : ABSENCE_WAIT_MS;
}
#else
static enum reason why_sleep(void)
{
	return zmk_split_bt_peripheral_is_connected() ? R_NONE : R_NO_CENTRAL;
}

static uint32_t wait_ms(void)
{
	return ABSENCE_WAIT_MS;
}
#endif

/* How much of `wait` is left since `from`; 0 once it has passed. */
static uint32_t left_of(uint32_t from, uint32_t wait, uint32_t now)
{
	const uint32_t gone = now - from;

	return gone >= wait ? 0 : wait - gone;
}

static void go_to_sleep(enum reason r)
{
	raising = true;
	(void)raise_zmk_activity_state_changed(
		(struct zmk_activity_state_changed){.state = ZMK_ACTIVITY_SLEEP});
	raising = false;

	if (zmk_pm_suspend_devices() < 0) {
		LOG_ERR("early sleep (%s): suspending the devices failed -- retrying in %d s",
			reason_text[r], RETRY_MS / 1000);
		zmk_pm_resume_devices();
		/* Back from SLEEP to what ZMK holds, IDLE: whatever went dark for
		 * the sleep comes back as it is in idle. */
		raising = true;
		(void)raise_zmk_activity_state_changed(
			(struct zmk_activity_state_changed){.state = zmk_activity_get_state()});
		raising = false;
		(void)k_work_reschedule(&sleep_work, K_MSEC(RETRY_MS));
		return;
	}

	sys_poweroff();
}

static void sleep_work_cb(struct k_work *work)
{
	ARG_UNUSED(work);

	/* ACTIVE: nothing to do until the next IDLE. SLEEP: ZMK's own sleep is
	 * under way. */
	if (zmk_activity_get_state() != ZMK_ACTIVITY_IDLE) {
		announced = false;
		return;
	}

	const enum reason r = why_sleep();

	if (r == R_NONE) {
		/* The next change of a computer, the other half or USB power runs
		 * this again. */
		announced = false;
		return;
	}

	/* Stamps first, the clock second, so that a stamp taken in between
	 * cannot lie in the future. */
	const uint32_t idle = (uint32_t)atomic_get(&idle_at);
	const uint32_t input = (uint32_t)atomic_get(&input_at);
#if GUARD_USB
	const uint32_t usb = (uint32_t)atomic_get(&usb_at);
#endif
	const uint32_t now = k_uptime_get_32();
	/* The later of the two, by age rather than by comparing the stamps: a
	 * stamp weeks old -- idle for a month on a computer, and then it goes --
	 * would pass for the later one once the two are 2^31 ms apart, and the
	 * sleep would come at once. An age can only wrap after 49.7 days, and
	 * then only delays the sleep. */
	const uint32_t from = now - idle < now - input ? idle : input;
	uint32_t left = left_of(from, wait_ms(), now);

#if GUARD_IDLE
	left = MAX(left, left_of(idle, GUARD_MS, now));
#endif
#if GUARD_USB
	left = MAX(left, left_of(usb, GUARD_MS, now));
#endif

	if (left) {
		announced = false;
		(void)k_work_reschedule(&sleep_work, K_MSEC(left));
		return;
	}

	if (!announced) {
		LOG_INF("early sleep in %d ms: %s, idle for %u s", ANNOUNCE_MS, reason_text[r],
			(now - idle) / 1000U);
		announced = true;
		(void)k_work_reschedule(&sleep_work, K_MSEC(ANNOUNCE_MS));
		return;
	}

	announced = false;
	go_to_sleep(r);
}

static int on_event(const zmk_event_t *eh)
{
	const struct zmk_activity_state_changed *act = as_zmk_activity_state_changed(eh);

	if (act) {
		if (raising) {
			return ZMK_EV_EVENT_BUBBLE; /* our own */
		}
		if (act->state == ZMK_ACTIVITY_IDLE) {
			stamp(&idle_at);
		}
		evaluate_soon();
		return ZMK_EV_EVENT_BUBBLE;
	}

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
	/* Plugged, unplugged, a computer configuring the board or letting go
	 * of it. */
	if (as_zmk_usb_conn_state_changed(eh)) {
		/* A test build ignores USB for the count; the underglow's
		 * settings write still gets its guard. */
		if (!IGNORE_USB) {
			stamp(&input_at);
		}
#if GUARD_USB
		stamp(&usb_at);
#endif
		evaluate_soon();
		return ZMK_EV_EVENT_BUBBLE;
	}
#endif

#if CENTRAL && IS_ENABLED(CONFIG_ZMK_BLE)
	/* Raised on a profile switch and on the active profile's connect and
	 * disconnect alike. */
	if (as_zmk_ble_active_profile_changed(eh)) {
		stamp(&input_at);
		evaluate_soon();
	}
#endif

#if !CENTRAL
	if (as_zmk_split_peripheral_status_changed(eh)) {
		stamp(&input_at);
		evaluate_soon();
	}
#endif

	return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zmk_split_conn_ladder_early_sleep, on_event);
ZMK_SUBSCRIPTION(zmk_split_conn_ladder_early_sleep, zmk_activity_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(zmk_split_conn_ladder_early_sleep, zmk_usb_conn_state_changed);
#endif
#if CENTRAL && IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(zmk_split_conn_ladder_early_sleep, zmk_ble_active_profile_changed);
#endif
#if !CENTRAL
ZMK_SUBSCRIPTION(zmk_split_conn_ladder_early_sleep, zmk_split_peripheral_status_changed);
#endif

#if CENTRAL
/* The split link (this half central), and -- while the active profile is
 * open for pairing -- a computer's (this half peripheral). A computer on the
 * active profile shows up as the profile event above; one pairing with an
 * open profile does not until the pairing is done, and the count must not run
 * out in the middle of it. Only then: other computers' links coming and going
 * (a bonded one on another profile, one whose pairing is refused) must not
 * put the sleep off for ever. */
static bool counts(struct bt_conn *conn)
{
	struct bt_conn_info info;

	return !bt_conn_get_info(conn, &info) && info.type == BT_CONN_TYPE_LE &&
	       (info.role == BT_CONN_ROLE_CENTRAL || pairing());
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	if (err || !counts(conn)) {
		return;
	}
	stamp(&input_at);
	evaluate_soon();
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(reason);

	if (!counts(conn)) {
		return;
	}
	stamp(&input_at);
	evaluate_soon();
}

BT_CONN_CB_DEFINE(early_sleep_conn_cb) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
};
#endif

static int early_sleep_init(void)
{
	/* Everything counts from boot until something happens. */
	stamp(&input_at);
	if (IGNORE_USB) {
		LOG_WRN("early sleep: TEST build -- a USB host is not a computer, USB power "
			"changes nothing");
	}
	LOG_INF("early sleep after %u s of idle %s", (uint32_t)ABSENCE_WAIT_MS / 1000U,
		CENTRAL ? (IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_EARLY_SLEEP_WITHOUT_PERIPHERAL)
				   ? "without a computer, or without the other half on battery"
				   : "without a computer")
			: "without the central half");
	return 0;
}

SYS_INIT(early_sleep_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
