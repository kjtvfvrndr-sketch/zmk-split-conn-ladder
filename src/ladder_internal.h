/*
 * Copyright (c) 2026 eXidno
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>

#include <zephyr/kernel.h>

/*
 * Between the ladder and the client that learns the other half's USB power
 * (peripheral_power_client.c). Not an API: nothing outside this module may
 * rely on it.
 */

/* The ladder's own work queue. The client starts its GATT discovery there,
 * off the system work queue, because a GATT request may block for a buffer. */
struct k_work_q *split_conn_ladder_work_q(void);

/* Called by the client, from the Bluetooth RX thread, when the report changes;
 * the ladder re-reads the gate on its own queue. */
void split_conn_ladder_peripheral_power_changed(void);

/* Called by the client with the other half's uptime, from every report that
 * carries it. Lets the ladder tell a link loss from that half restarting. */
void split_conn_ladder_peripheral_uptime(uint32_t uptime_ms);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_USB_STEPS)
/* Whether the other half reported USB power on the current connection. False
 * while there is no connection, before the first report, and when the other
 * half does not report at all. */
bool split_conn_ladder_peripheral_on_usb(void);
#else
/* Without the cable column nothing asks: AUTO holds ACTIVE on USB anyway. */
static inline bool split_conn_ladder_peripheral_on_usb(void)
{
	return false;
}
#endif
