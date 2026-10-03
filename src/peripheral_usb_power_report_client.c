/*
 * Copyright (c) 2026 eXidno
 *
 * SPDX-License-Identifier: MIT
 *
 * Central half: learns whether the OTHER half is on USB power, so that the
 * computed ACTIVE can drop its latency to 0 while it is -- that half has
 * nothing to save then -- and the ladder can hold ACTIVE when both are.
 *
 * The other half publishes its report (peripheral_usb_power_report.c, on by default
 * there, ZMK_SPLIT_CONN_LADDER_PERIPHERAL_USB_POWER_REPORT). On every split connection this
 * finds it, subscribes to changes and reads the current value. A half built
 * without the report simply has no such service: that is logged once per
 * connection and the other half counts as on battery, which is what the
 * ladder assumed before this existed.
 *
 * Discovery starts a couple of seconds after the connection, so that ZMK's own
 * discovery -- the one that makes the other half's keys work -- goes first.
 * It starts from the ladder's work queue rather than the system one, because
 * a GATT request may block for a buffer. Everything after that runs in GATT
 * callbacks on the Bluetooth RX thread, which only touch atomics and ask the
 * ladder to look again.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>

#include "ladder_internal.h"
#include "peripheral_usb_power_report_gatt.h"

LOG_MODULE_DECLARE(zmk_split_conn_ladder, CONFIG_ZMK_SPLIT_CONN_LADDER_LOG_LEVEL);

#define START_DELAY_MS 2000
#define START_RETRY_MS 5000
#define START_TRIES    3

static const struct bt_uuid_128 svc_uuid = BT_UUID_INIT_128(LADDER_POWER_SVC_UUID);
static const struct bt_uuid_128 chr_uuid = BT_UUID_INIT_128(LADDER_POWER_CHR_UUID);

static struct bt_gatt_discover_params disc;
static struct bt_gatt_subscribe_params sub;
static struct bt_gatt_read_params rd;

/* 1 while the other half reports USB power on the current connection. */
static atomic_t on_usb;
/* Whether it has reported at all on this connection; for the log only. */
static atomic_t heard;
/* Failed attempts to start discovery on this connection. */
static atomic_t tries;

bool split_conn_ladder_peripheral_on_usb(void)
{
	return atomic_get(&on_usb) != 0;
}

static void set_power(uint8_t value)
{
	const atomic_val_t now = value ? 1 : 0;
	const bool first = !atomic_set(&heard, 1);
	const atomic_val_t was = atomic_set(&on_usb, now);

	if (first) {
		LOG_INF("other half reports USB power %s", now ? "present" : "absent");
	} else if (was != now) {
		LOG_INF("other half: USB power %s", now ? "present" : "absent");
	}
	if (was != now) {
		split_conn_ladder_peripheral_usb_power_changed();
	}
}

/* One report, read or notified: the uptime first, so the ladder run the power
 * change asks for already sees it. */
static void take_report(const uint8_t *data, uint16_t length)
{
	if (length >= LADDER_POWER_REPORT_LEN) {
		split_conn_ladder_peripheral_uptime(sys_get_le32(&data[1]));
	}
	if (length >= 1) {
		set_power(data[0] & LADDER_POWER_FLAG_USB);
	}
}

static uint8_t on_notify(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
			 const void *data, uint16_t length)
{
	ARG_UNUSED(conn);

	if (!data) {
		/* The subscription was removed -- the link went down, and it is
		 * volatile. on_disconnected() has already cleared the state. */
		params->value_handle = 0;
		return BT_GATT_ITER_STOP;
	}
	take_report(data, length);
	return BT_GATT_ITER_CONTINUE;
}

static void on_subscribed(struct bt_conn *conn, uint8_t err,
			  struct bt_gatt_subscribe_params *params)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(params);

	if (err) {
		/* The value read below still lands; only changes are missed. */
		LOG_WRN("other half's USB power: subscribing failed (ATT error 0x%02x)", err);
	}
}

static uint8_t on_read(struct bt_conn *conn, uint8_t err, struct bt_gatt_read_params *params,
		       const void *data, uint16_t length)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(params);

	if (err) {
		LOG_WRN("other half's USB power: reading failed (ATT error 0x%02x)", err);
	} else if (data) {
		take_report(data, length);
	}
	return BT_GATT_ITER_STOP;
}

static uint8_t on_characteristic(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				 struct bt_gatt_discover_params *params)
{
	ARG_UNUSED(params);

	if (!attr) {
		LOG_WRN("other half's power service has no power characteristic");
		return BT_GATT_ITER_STOP;
	}

	const uint16_t value = ((const struct bt_gatt_chrc *)attr->user_data)->value_handle;
	int err;

	sub.notify = on_notify;
	sub.subscribe = on_subscribed;
	sub.value = BT_GATT_CCC_NOTIFY;
	sub.value_handle = value;
	/* Our own service, laid out in peripheral_usb_power_report_gatt.h: the CCC sits
	 * right after the value. */
	sub.ccc_handle = value + 1;
	/* Dropped with the link rather than kept for the bond: every connection
	 * subscribes afresh, and the handles may differ after a reflash. */
	atomic_set_bit(sub.flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);

	err = bt_gatt_subscribe(conn, &sub);
	if (err && err != -EALREADY) {
		LOG_WRN("other half's USB power: subscribing failed (err %d)", err);
	}

	/* A notification only comes with a change; the current value has to be
	 * read. Queued behind the CCC write. */
	rd.func = on_read;
	rd.handle_count = 1;
	rd.single.handle = value;
	rd.single.offset = 0;

	err = bt_gatt_read(conn, &rd);
	if (err) {
		LOG_WRN("other half's USB power: reading failed (err %d)", err);
	}
	return BT_GATT_ITER_STOP;
}

static uint8_t on_service(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  struct bt_gatt_discover_params *params)
{
	if (!attr) {
		LOG_INF("other half does not report its USB power (no power service) -- "
			"it counts as on battery");
		return BT_GATT_ITER_STOP;
	}

	const uint16_t end = ((const struct bt_gatt_service_val *)attr->user_data)->end_handle;

	/* Reusing the parameters from inside their own callback is allowed:
	 * the procedure is over once this returns STOP. */
	params->uuid = &chr_uuid.uuid;
	params->func = on_characteristic;
	params->start_handle = attr->handle + 1;
	params->end_handle = end;
	params->type = BT_GATT_DISCOVER_CHARACTERISTIC;

	int err = bt_gatt_discover(conn, params);

	if (err) {
		LOG_WRN("other half's USB power: discovery failed (err %d)", err);
	}
	return BT_GATT_ITER_STOP;
}

/* The split link: LE, this half central, and fully up. */
static bool is_split(struct bt_conn *conn, bool connected)
{
	struct bt_conn_info info;

	return !bt_conn_get_info(conn, &info) && info.type == BT_CONN_TYPE_LE &&
	       info.role == BT_CONN_ROLE_CENTRAL &&
	       (!connected || info.state == BT_CONN_STATE_CONNECTED);
}

static void find_split(struct bt_conn *conn, void *data)
{
	struct bt_conn **found = data;

	if (!*found && is_split(conn, true)) {
		*found = bt_conn_ref(conn);
	}
}

static void start_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(start_work, start_cb);

static void start_cb(struct k_work *work)
{
	ARG_UNUSED(work);

	struct bt_conn *conn = NULL;

	/* Looked up rather than kept: a reference held across callbacks is one
	 * more thing a disconnect could leave dangling. */
	bt_conn_foreach(BT_CONN_TYPE_LE, find_split, &conn);
	if (!conn) {
		return; /* gone again; the next connect starts over */
	}

	disc.uuid = &svc_uuid.uuid;
	disc.func = on_service;
	disc.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	disc.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	disc.type = BT_GATT_DISCOVER_PRIMARY;

	int err = bt_gatt_discover(conn, &disc);

	bt_conn_unref(conn);

	if (!err) {
		return;
	}
	/* Most likely -ENOMEM: every ATT request slot taken by ZMK's own
	 * set-up. That clears; try again a little later. */
	if (atomic_inc(&tries) + 1 < START_TRIES) {
		LOG_WRN("other half's USB power: discovery failed (err %d), retrying", err);
		(void)k_work_reschedule_for_queue(split_conn_ladder_work_q(), &start_work,
						  K_MSEC(START_RETRY_MS));
	} else {
		LOG_WRN("other half's USB power: discovery failed (err %d), giving up until "
			"the next connection",
			err);
	}
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	if (err || !is_split(conn, false)) {
		return;
	}
	atomic_set(&heard, 0);
	atomic_set(&tries, 0);
	(void)k_work_reschedule_for_queue(split_conn_ladder_work_q(), &start_work,
					  K_MSEC(START_DELAY_MS));
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(reason);

	if (!is_split(conn, false)) {
		return;
	}
	(void)k_work_cancel_delayable(&start_work);
	/* Unknown again until the next connection reports. */
	if (atomic_set(&on_usb, 0)) {
		split_conn_ladder_peripheral_usb_power_changed();
	}
}

static struct bt_conn_cb conn_cb = {
	.connected = on_connected,
	.disconnected = on_disconnected,
};

static int usb_power_report_client_init(void)
{
	bt_conn_cb_register(&conn_cb);
	return 0;
}

SYS_INIT(usb_power_report_client_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
