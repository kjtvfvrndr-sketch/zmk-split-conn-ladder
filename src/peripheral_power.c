/*
 * Copyright (c) 2026 eXidno
 *
 * SPDX-License-Identifier: MIT
 *
 * Peripheral half: tells the central half whether this half is on USB power.
 * With both halves on USB there is nothing to save anywhere, and the ladder on
 * the central holds ACTIVE (peripheral_power_client.c reads this).
 *
 * A peripheral half has no USB stack -- ZMK builds it without one -- so ZMK's
 * own "USB powered" test does not exist here. The nRF52840's VBUS detector
 * does not need one: the POWER peripheral reports VBUS whether or not USB is
 * in use. It is polled once a second; that is quick enough for a decision
 * nobody is waiting on, and a register read per second costs well under a
 * microamp.
 *
 * The report also carries this half's uptime. ZMK powers a sleeping half off
 * without closing the link, so to the central its sleep looks exactly like the
 * link failing; the uptime after the reconnect tells the two apart.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>

#include <hal/nrf_power.h>

#include "peripheral_power_gatt.h"

LOG_MODULE_REGISTER(zmk_split_conn_ladder_power,
		    CONFIG_ZMK_SPLIT_CONN_LADDER_PERIPHERAL_POWER_LOG_LEVEL);

#if !NRF_POWER_HAS_USBREG
#error "ZMK_SPLIT_CONN_LADDER_PERIPHERAL_POWER needs a SoC with a VBUS detector (nRF52840/nRF52833/nRF52820)"
#endif

#define POLL_MS 1000

/* 1 while VBUS is present. Written on the system work queue, read by GATT
 * from the Bluetooth RX thread; a single byte needs no more than that. */
static uint8_t vbus;

static uint8_t vbus_now(void)
{
	return nrf_power_usbregstatus_vbusdet_get(NRF_POWER) ? 1 : 0;
}

/* peripheral_power_gatt.h: flags, then the uptime in ms, saturating. */
static void fill_report(uint8_t report[LADDER_POWER_REPORT_LEN])
{
	const int64_t up = k_uptime_get();

	report[0] = vbus ? LADDER_POWER_FLAG_USB : 0;
	sys_put_le32(up > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)up, &report[1]);
}

static ssize_t read_report(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			   uint16_t len, uint16_t offset)
{
	uint8_t report[LADDER_POWER_REPORT_LEN];

	fill_report(report);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, report, sizeof(report));
}

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	LOG_DBG("central %s", value == BT_GATT_CCC_NOTIFY ? "subscribed" : "unsubscribed");
}

/*
 * Laid out as peripheral_power_gatt.h promises: the CCC directly after the
 * value. No encryption is demanded -- one bit of "is it charging" is nothing
 * to protect, and this way it cannot fail on a link that is still securing.
 */
BT_GATT_SERVICE_DEFINE(ladder_power_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(LADDER_POWER_SVC_UUID)),
		       BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(LADDER_POWER_CHR_UUID),
					      BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
					      BT_GATT_PERM_READ, read_report, NULL, NULL),
		       BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE), );

static void poll_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(poll_work, poll_cb);

static void poll_cb(struct k_work *work)
{
	ARG_UNUSED(work);

	const uint8_t now = vbus_now();

	if (now != vbus) {
		uint8_t report[LADDER_POWER_REPORT_LEN];

		vbus = now;
		LOG_INF("USB power %s", now ? "present" : "absent");
		/* To whoever subscribed -- the central, when it is connected. When
		 * it is not, nothing is lost: it reads the value on connecting.
		 * ZMK notifies its battery level from this queue the same way. The
		 * data is copied into the PDU before this returns. */
		fill_report(report);
		(void)bt_gatt_notify(NULL, &ladder_power_svc.attrs[1], report, sizeof(report));
	}
	(void)k_work_schedule(&poll_work, K_MSEC(POLL_MS));
}

static int peripheral_power_init(void)
{
	vbus = vbus_now();
	LOG_INF("USB power %s at boot", vbus ? "present" : "absent");
	(void)k_work_schedule(&poll_work, K_MSEC(POLL_MS));
	return 0;
}

SYS_INIT(peripheral_power_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
