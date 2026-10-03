/*
 * Copyright (c) 2026 eXidno
 *
 * SPDX-License-Identifier: MIT
 *
 * Peripheral half: closes the split link before this half goes to sleep
 * (ZMK_SPLIT_CONN_LADDER_PERIPHERAL_CLOSE_LINK_BEFORE_SLEEP).
 *
 * ZMK's own sleep powers a half off without a word, so the central learns of
 * it only when its supervision timeout runs out. Until then it takes this
 * half for connected, and whatever it sends here -- a layer colour, an
 * indicator -- waits in the Bluetooth buffers it shares with the reports to
 * the computer: a trackball on the central stalls for as long.
 *
 * The SLEEP activity event comes before the power goes off (activity.c raises
 * it, then suspends the devices, then calls sys_poweroff(), all on the system
 * work queue), so this closes the link there -- "remote user terminated", which
 * the central sees at once as reason 0x13 -- and waits for the close to
 * complete. The controller cancels this half's latency for the procedure, so
 * the close goes out at the next connection event and is acknowledged at the
 * one after: two intervals. The wait allows that plus a margin, never less than
 * MIN_CLOSE_WAIT_MS, and never longer than the link's supervision timeout plus
 * the margin -- by then the controller has given the link up anyway.
 *
 * The wait polls the connection's state rather than waiting for the
 * disconnected callback: that callback runs on the system work queue, which
 * this very listener is holding. The state itself is set when the controller
 * reports the disconnection, on its priority receive thread, which runs while
 * this sleeps.
 *
 * Other threads run during the wait -- and already during bt_conn_disconnect(),
 * which waits for the controller. An LED driver that paints from a thread of
 * its own must therefore keep its LEDs dark once it has seen SLEEP, even when
 * it handled SLEEP before this listener: a colour arriving meanwhile would
 * otherwise light it up again, and a WS2812 keeps its colour through System
 * OFF.
 *
 * With no link -- the module's own early sleep goes only when there is none --
 * there is nothing to do.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>

LOG_MODULE_REGISTER(zmk_split_conn_ladder_close_link,
		    CONFIG_ZMK_SPLIT_CONN_LADDER_PERIPHERAL_CLOSE_LINK_BEFORE_SLEEP_LOG_LEVEL);

/* How long the close may take: two connection intervals plus this margin, at
 * least MIN_CLOSE_WAIT_MS. Past that, the half sleeps anyway. */
#define MIN_CLOSE_WAIT_MS   1500
#define CLOSE_WAIT_MARGIN_MS 500
#define CLOSE_POLL_MS       10

/* This half's link to the central: the only one where it is peripheral. */
static void find_link_to_central(struct bt_conn *conn, void *data)
{
	struct bt_conn **link = data;
	struct bt_conn_info info;

	if (*link == NULL && !bt_conn_get_info(conn, &info) && info.type == BT_CONN_TYPE_LE &&
	    info.role == BT_CONN_ROLE_PERIPHERAL && info.state == BT_CONN_STATE_CONNECTED) {
		*link = bt_conn_ref(conn);
	}
}

/* Two intervals and the margin, at least MIN_CLOSE_WAIT_MS, at most the
 * supervision timeout and the margin. */
static uint32_t close_wait_ms(const struct bt_conn *conn)
{
	struct bt_conn_info info;

	if (bt_conn_get_info(conn, &info)) {
		return MIN_CLOSE_WAIT_MS;
	}

	const uint32_t interval_ms = info.le.interval * 5U / 4U;
	const uint32_t timeout_ms = info.le.timeout * 10U;

	return MIN(MAX((uint32_t)MIN_CLOSE_WAIT_MS, 2U * interval_ms + CLOSE_WAIT_MARGIN_MS),
		   timeout_ms + CLOSE_WAIT_MARGIN_MS);
}

static bool link_is_closed(struct bt_conn *conn)
{
	struct bt_conn_info info;

	return bt_conn_get_info(conn, &info) || info.state == BT_CONN_STATE_DISCONNECTED;
}

static void close_link_to_central(void)
{
	struct bt_conn *link = NULL;

	bt_conn_foreach(BT_CONN_TYPE_LE, find_link_to_central, &link);
	if (link == NULL) {
		return;
	}

	/* From the parameters the link runs on now. */
	const uint32_t wait_ms = close_wait_ms(link);
	const int err = bt_conn_disconnect(link, BT_HCI_ERR_REMOTE_USER_TERM_CONN);

	if (err) {
		LOG_WRN("going to sleep: closing the link to the central failed (%d)", err);
		bt_conn_unref(link);
		return;
	}

	const uint32_t start = k_uptime_get_32();

	while (!link_is_closed(link) && k_uptime_get_32() - start < wait_ms) {
		k_msleep(CLOSE_POLL_MS);
	}

	const uint32_t took = k_uptime_get_32() - start;

	if (link_is_closed(link)) {
		LOG_INF("going to sleep: link to the central closed in %u ms", took);
	} else {
		LOG_WRN("going to sleep: link to the central not closed after %u ms", took);
	}
	bt_conn_unref(link);
}

static int on_activity(const zmk_event_t *eh)
{
	const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);

	if (ev && ev->state == ZMK_ACTIVITY_SLEEP) {
		close_link_to_central();
	}
	return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zmk_split_conn_ladder_close_link, on_activity);
ZMK_SUBSCRIPTION(zmk_split_conn_ladder_close_link, zmk_activity_state_changed);
