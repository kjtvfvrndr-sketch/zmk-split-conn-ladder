/*
 * Copyright (c) 2026 eXidno
 *
 * SPDX-License-Identifier: MIT
 *
 * The low-power search for a missing other half
 * (ZMK_SPLIT_CONN_LADDER_LOW_POWER_PERIPHERAL_SEARCH), central half.
 *
 * ZMK searches for a missing other half with a passive scan of 30 ms every
 * 60 ms (BT_LE_SCAN_PASSIVE), about 2.5 mA, for as long as that half is
 * missing -- all night when it sleeps. Here the search goes in stages from
 * when the other half goes (or from boot):
 *
 *   for ABSENCE_WAIT_MS, ZMK's full search -- or, while the keys go to a
 *   Bluetooth host, this file's middle search, 30 ms every
 *   MIDDLE_PERIPHERAL_SEARCH_INTERVAL_MS: the full search takes half the
 *   radio's time, and the link to that host loses its events to it (a
 *   trackball on it stutters);
 *
 *   after that, this file's low-power search, 30 ms every 1.28 s, about
 *   0.1 mA.
 *
 * For either of this file's own scans ZMK's Bluetooth split transport is
 * switched off through its own API -- set_enabled(false), which only stops
 * its scan, since nothing is connected.
 *
 * ABSENCE_WAIT_MS is the early sleep's wait too, on purpose: the other half,
 * once it has lost this one, waits the same before its own early sleep, so
 * the full or the middle search runs for as long as that half is awake and
 * calling, and the low-power one takes over when it has gone to sleep
 * (absence_wait.h).
 *
 * What it listens for is the other half calling:
 *
 *   directed advertising (ADV_DIRECT_IND), which a ZMK peripheral sends only
 *   to the central it is paired with. The controller passes only those
 *   addressed to this half, so any that arrives is ours. A half waking from
 *   sleep sends it at full rate for 1.28 s, and one window every 1.28 s
 *   lands in that; after that at 100-150 ms, which a window catches about
 *   one time in four;
 *
 *   undirected advertising with the ZMK split service (ADV_IND), which a
 *   peripheral sends while it has no pairing -- any ZMK split half nearby
 *   in that state does. It wakes the search only when it comes from the half
 *   this one is paired with (a half that lost its own pairing keeps its
 *   address), or when this half is paired with none: exactly what ZMK
 *   itself would connect to.
 *
 * Either one stops this file's scan and switches ZMK's transport back on,
 * whose full search then finds the half and connects. Where the middle search
 * would run, that full search lasts CALLED_FULL_SEARCH_MS -- ZMK hears the
 * next call within a fraction of that; elsewhere ABSENCE_WAIT_MS. A call that
 * does not end in a connection counts as a loss at that moment: the middle
 * or the full search for ABSENCE_WAIT_MS from the call, then the low-power
 * one.
 *
 * The full search stays on only while the keys go to a USB host: there it
 * costs no battery and takes no airtime from a link to the computer, because
 * none carries the output. USB power alone is not enough -- with the output
 * on Bluetooth the full search takes half the radio from that link, whatever
 * powers the half.
 *
 * ZMK's split core does not switch the transport back on by itself: it only
 * selects a transport again when the one it has is no longer available
 * (app/src/split/central.c), and this one stays available throughout.
 *
 * The scan is started and stopped on the ladder's work queue: both block on
 * HCI. The Bluetooth callbacks -- the scan and a connection on the RX
 * thread, a disconnect on the system work queue -- only raise flags.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/net/buf.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/split/bluetooth/uuid.h>
#include <zmk/split/transport/central.h>

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
#include <zmk/usb.h>
#include <zmk/events/usb_conn_state_changed.h>
#endif

#include "ladder_internal.h"
#include "absence_wait.h"

LOG_MODULE_DECLARE(zmk_split_conn_ladder, CONFIG_ZMK_SPLIT_CONN_LADDER_LOG_LEVEL);

/* Both of this file's scans listen 30 ms at a time; the low-power one every
 * 1.28 s, the middle one every MIDDLE_PERIPHERAL_SEARCH_INTERVAL_MS (0: no
 * middle search). In 0.625 ms units. */
#define SCAN_WINDOW             0x0030
#define LOW_POWER_SCAN_INTERVAL 0x0800
#define MIDDLE_SEARCH_INTERVAL_MS CONFIG_ZMK_SPLIT_CONN_LADDER_MIDDLE_PERIPHERAL_SEARCH_INTERVAL_MS
#define MIDDLE_SCAN_INTERVAL    ((MIDDLE_SEARCH_INTERVAL_MS * 8U) / 5U)

BUILD_ASSERT(MIDDLE_SEARCH_INTERVAL_MS == 0 || MIDDLE_SEARCH_INTERVAL_MS >= 60,
	     "ZMK_SPLIT_CONN_LADDER_MIDDLE_PERIPHERAL_SEARCH_INTERVAL_MS is 0 (no middle search) "
	     "or at least 60: the scan listens 30 ms at a time, and 60 ms is ZMK's own full "
	     "search");

/* The full search a call brings back where the middle search would run: ZMK's
 * scan hears one of the half's calls (every 100-150 ms, and at full rate for
 * 1.28 s after it wakes) with even odds or better, so in 2 s it has had some
 * sixteen chances. */
#define CALLED_FULL_SEARCH_MS 2000

/* Switching ZMK's transport back on can fail like any scan start; tried again
 * this soon, a few times, then every ABSENCE_WAIT_MS. */
#define REENABLE_RETRY_MS 1000
#define REENABLE_TRIES    5

/* ZMK's Bluetooth split transport (app/src/split/bluetooth/central.c,
 * ZMK_SPLIT_TRANSPORT_CENTRAL_REGISTER). */
extern struct zmk_split_transport_central bt_central;

enum search_stage {
	FULL_SEARCH,      /* ZMK's own; this file leaves its transport alone */
	MIDDLE_SEARCH,    /* this file's scans: ZMK's transport is off */
	LOW_POWER_SEARCH,
};

/* Handler-only. */
static enum search_stage stage = FULL_SEARCH;
static bool need_enable;       /* ZMK's transport still has to be switched on */
static uint8_t enable_tries;
static uint32_t missing_since; /* the other half gone -- or last calling -- since then */
static bool back_for_call;     /* the full search comes back because the half called */
static bool full_forced;       /* the full search runs until full_until, whatever else */
static uint32_t full_until;

/* From the Bluetooth callbacks. */
static atomic_t scanning; /* our scan is on: its reports mean something */
static atomic_t wake;     /* the other half called */
static atomic_t lost_at;  /* the split link went down */
static atomic_t lost;     /* ... and the handler has not seen it yet */

/* The half this one is paired with, if any; looked up on switching ZMK's
 * transport off, the only time it is read. */
static bool peer_known;
static bt_addr_le_t peer_addr;

static void search_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(search_work, search_work_cb);

static void run_soon(void)
{
	(void)k_work_reschedule_for_queue(split_conn_ladder_work_q(), &search_work, K_NO_WAIT);
}

static void run_in(uint32_t ms)
{
	(void)k_work_reschedule_for_queue(split_conn_ladder_work_q(), &search_work, K_MSEC(ms));
}

static bool split_uuid(struct bt_data *data, void *user_data)
{
	bool *found = user_data;

	if (data->type != BT_DATA_UUID128_ALL && data->type != BT_DATA_UUID128_SOME) {
		return true;
	}
	for (int i = 0; i + 16 <= data->data_len; i += 16) {
		struct bt_uuid_128 uuid;

		if (bt_uuid_create(&uuid.uuid, &data->data[i], 16) &&
		    !bt_uuid_cmp(&uuid.uuid, BT_UUID_DECLARE_128(ZMK_SPLIT_BT_SERVICE_UUID))) {
			*found = true;
			return false;
		}
	}
	return true;
}

static void on_scan(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
		    struct net_buf_simple *ad)
{
	ARG_UNUSED(rssi);

	if (!atomic_get(&scanning)) {
		return;
	}

	if (type == BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
		if (!atomic_set(&wake, 1)) {
			run_soon();
		}
		return;
	}

	if (type != BT_GAP_ADV_TYPE_ADV_IND) {
		return;
	}

	bool found = false;
	struct net_buf_simple_state state;

	net_buf_simple_save(ad, &state);
	bt_data_parse(ad, split_uuid, &found);
	net_buf_simple_restore(ad, &state);

	if (found && (!peer_known || !bt_addr_le_cmp(addr, &peer_addr))) {
		if (!atomic_set(&wake, 2)) {
			run_soon();
		}
	}
}

/* A bond that belongs to no host profile is the other half's. */
static void find_peer(const struct bt_bond_info *info, void *data)
{
	ARG_UNUSED(data);

	if (!peer_known && zmk_ble_profile_index(&info->addr) < 0) {
		bt_addr_le_copy(&peer_addr, &info->addr);
		peer_known = true;
	}
}

static void any_split(struct bt_conn *conn, void *data)
{
	struct bt_conn_info info;

	/* Any state: a connection still being set up counts too -- ZMK found
	 * the half and is connecting. */
	if (!bt_conn_get_info(conn, &info) && info.type == BT_CONN_TYPE_LE &&
	    info.role == BT_CONN_ROLE_CENTRAL) {
		(*(int *)data)++;
	}
}

static bool split_present(void)
{
	int n = 0;

	bt_conn_foreach(BT_CONN_TYPE_LE, any_split, &n);
	return n > 0;
}

/* The keys go to a USB host: the only case the full search stays on. A test
 * build searches as on battery with no USB host (Kconfig). */
static bool output_on_usb_host(void)
{
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
	return !IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_TEST_SLEEP_AND_SEARCH_IGNORE_USB) &&
	       zmk_usb_is_hid_ready() && zmk_endpoints_selected().transport == ZMK_TRANSPORT_USB;
#else
	return false;
#endif
}

/* The keys go to a connected Bluetooth host: the case for the middle search.
 * In a normal build asking the profile would do, since this is asked only
 * when the keys do not go to a USB host, and then ZMK sends them over
 * Bluetooth whenever its active profile is connected (endpoints.c). A test
 * build ignores the USB host, and there the output as ZMK selected it decides
 * -- so &out picks the case to watch. */
static bool output_on_ble_host(void)
{
	return zmk_ble_active_profile_is_connected() &&
	       zmk_endpoints_selected().transport == ZMK_TRANSPORT_BLE;
}

/* The full search for ms from now, whatever the stage would be: after a call,
 * and after a scan of our own that could not start. */
static void force_full(uint32_t ms)
{
	full_forced = true;
	full_until = k_uptime_get_32() + ms;
}

/* ZMK's transport back on. A failed scan start leaves ZMK believing it scans
 * (split/bluetooth/central.c sets is_scanning before trying), and only a
 * disable clears that; so a failure is followed by one, and a retry -- soon
 * at first, then every ABSENCE_WAIT_MS, but never given up: without it the
 * other half could not connect at all. */
static void enable_transport(void)
{
	const int err = bt_central.api->set_enabled(true);

	if (!err) {
		need_enable = false;
		enable_tries = 0;
		/* A call counts as the half's last sign of life, from when the
		 * full search really runs. Nothing else moves that: brought back
		 * for the output alone, the search still has the half gone as long
		 * as it has been. */
		if (back_for_call) {
			back_for_call = false;
			missing_since = k_uptime_get_32();
			force_full(CALLED_FULL_SEARCH_MS);
		}
		return;
	}
	(void)bt_central.api->set_enabled(false);
	if (enable_tries < UINT8_MAX) {
		enable_tries++;
	}

	const uint32_t delay = enable_tries < REENABLE_TRIES ? REENABLE_RETRY_MS : ABSENCE_WAIT_MS;

	LOG_WRN("low-power search: switching the full search back on failed (%d), retry %u "
		"in %u ms",
		err, enable_tries, delay);
	run_in(delay);
}

static void leave_own_scan(const char *why)
{
	atomic_set(&scanning, 0);
	(void)bt_le_scan_stop();
	stage = FULL_SEARCH;
	need_enable = true;
	LOG_INF("low-power search: %s -- full search on", why);
}

/* The middle or the low-power search: from the full one, or from the middle
 * one to the low-power one -- the same scan, sparser. */
static void start_own_scan(enum search_stage want, uint32_t gone)
{
	const struct bt_le_scan_param param = {
		.type = BT_LE_SCAN_TYPE_PASSIVE,
		.options = BT_LE_SCAN_OPT_FILTER_DUPLICATE,
		.interval = want == MIDDLE_SEARCH ? MIDDLE_SCAN_INTERVAL : LOW_POWER_SCAN_INTERVAL,
		.window = SCAN_WINDOW,
	};

	if (stage == FULL_SEARCH) {
		const struct zmk_split_transport_status st = bt_central.api->get_status();

		/* Only a transport ZMK itself runs: not before its settings are
		 * in, not one it has switched off or never selected. */
		if (!st.available || !st.enabled) {
			force_full(ABSENCE_WAIT_MS);
			return;
		}
		/* ZMK may have found the half since the handler looked: switching
		 * its transport off now would drop that connection. Looked at
		 * again as late as possible; the connect runs the handler. */
		if (split_present()) {
			return;
		}
		(void)bt_central.api->set_enabled(false);

		peer_known = false;
		bt_foreach_bond(BT_ID_DEFAULT, find_peer, NULL);
		atomic_set(&wake, 0);
	} else {
		/* A call heard meanwhile stays raised, and its run comes next. */
		atomic_set(&scanning, 0);
		(void)bt_le_scan_stop();
	}

	atomic_set(&scanning, 1);

	const int err = bt_le_scan_start(&param, on_scan);

	if (err) {
		atomic_set(&scanning, 0);
		LOG_WRN("low-power search: scan start failed (%d) -- full search again", err);
		stage = FULL_SEARCH;
		need_enable = true;
		force_full(ABSENCE_WAIT_MS);
		return;
	}
	stage = want;
	if (want == MIDDLE_SEARCH) {
		LOG_INF("low-power search: other half away, keys on a Bluetooth host -- listening "
			"30 ms every %u ms%s",
			MIDDLE_SEARCH_INTERVAL_MS, peer_known ? "" : " (no paired half: any may pair)");
	} else {
		LOG_INF("low-power search: other half away for %u s -- listening 30 ms every "
			"1.28 s%s",
			gone / 1000U, peer_known ? "" : " (no paired half: any may pair)");
	}
}

static const char *why_back(int called, bool present)
{
	switch (called) {
	case 1:
		return "the other half called";
	case 2:
		return "a half asks to pair";
	default:
		return present ? "the other half connected" : "output on USB";
	}
}

static void search_work_cb(struct k_work *work)
{
	ARG_UNUSED(work);

	const int called = (int)atomic_set(&wake, 0);

	if (atomic_cas(&lost, 1, 0)) {
		missing_since = (uint32_t)atomic_get(&lost_at);
		full_forced = false;
	}

	const bool present = split_present();
	const bool usb = output_on_usb_host();

	/* Connected, or the keys on USB: a full search forced before is
	 * over, and left set it would outlive a clock wrap. */
	if (present || usb) {
		full_forced = false;
	}
	if (stage != FULL_SEARCH && (present || usb || called)) {
		back_for_call = called && !present;
		leave_own_scan(why_back(called, present));
	}
	if (need_enable) {
		enable_transport();
		if (need_enable) {
			return; /* its retry is scheduled */
		}
	}
	/* Connected or connecting, or the output on USB: nothing to time. In the
	 * low-power search: the scan wakes this. */
	if (present || usb || stage == LOW_POWER_SEARCH) {
		return;
	}

	/* Read only now: switching the full search on above blocks on HCI and
	 * stamps missing_since and full_until when it is done; a clock read
	 * before it would lag behind those stamps. */
	const uint32_t now = k_uptime_get_32();
	const uint32_t gone = now - missing_since;
	enum search_stage want;
	uint32_t next;

	if (full_forced && (int32_t)(full_until - now) > 0) {
		want = FULL_SEARCH;
		next = full_until - now;
	} else if (gone >= ABSENCE_WAIT_MS) {
		full_forced = false;
		want = LOW_POWER_SEARCH;
		next = 0;
	} else {
		full_forced = false;
		want = MIDDLE_SEARCH_INTERVAL_MS && output_on_ble_host() ? MIDDLE_SEARCH : FULL_SEARCH;
		next = ABSENCE_WAIT_MS - gone;
	}

	if (want == FULL_SEARCH) {
		if (stage != FULL_SEARCH) {
			back_for_call = false;
			leave_own_scan("keys not on a Bluetooth host");
		}
	} else if (want != stage) {
		start_own_scan(want, gone);
		if (stage == FULL_SEARCH) {
			next = 0; /* it could not start: timed by the forced full search */
		}
	}
	if (need_enable) {
		enable_transport();
		if (need_enable) {
			return;
		}
	}
	/* The low-power search waits for the scan; the others for their end, or
	 * for an event (a loss, a call, the output or the host link changing). */
	if (stage != LOW_POWER_SEARCH) {
		run_in(next);
	}
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	struct bt_conn_info info;

	ARG_UNUSED(err);

	/* A failed attempt too: Zephyr reports it here, with err, and never as
	 * a disconnect -- and the handler, having seen the half connecting,
	 * waits for one of the two. */
	if (bt_conn_get_info(conn, &info) || info.type != BT_CONN_TYPE_LE ||
	    info.role != BT_CONN_ROLE_CENTRAL) {
		return;
	}
	run_soon();
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	struct bt_conn_info info;

	ARG_UNUSED(reason);

	if (bt_conn_get_info(conn, &info) || info.type != BT_CONN_TYPE_LE ||
	    info.role != BT_CONN_ROLE_CENTRAL) {
		return;
	}
	atomic_set(&lost_at, (atomic_val_t)k_uptime_get_32());
	atomic_set(&lost, 1);
	run_soon();
}

BT_CONN_CB_DEFINE(low_power_search_conn_cb) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
};

/* The output moving between USB and Bluetooth, the cable going in or out, the
 * active profile's host connecting or going. */
static int on_output(const zmk_event_t *eh)
{
	ARG_UNUSED(eh);

	run_soon();
	return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zmk_split_conn_ladder_low_power_search, on_output);
ZMK_SUBSCRIPTION(zmk_split_conn_ladder_low_power_search, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(zmk_split_conn_ladder_low_power_search, zmk_ble_active_profile_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(zmk_split_conn_ladder_low_power_search, zmk_usb_conn_state_changed);
#endif

static int low_power_search_init(void)
{
	if (IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_TEST_SLEEP_AND_SEARCH_IGNORE_USB)) {
		LOG_WRN("low-power search: TEST build -- a USB host is ignored");
	}
	/* The half is missing from boot. The ladder's queue is up by now
	 * (POST_KERNEL). */
	missing_since = k_uptime_get_32();
	run_in(ABSENCE_WAIT_MS);
	return 0;
}

SYS_INIT(low_power_search_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
