/*
 * Copyright (c) 2026 eXidno
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/bluetooth/uuid.h>

/*
 * The USB power report, peripheral_power.c -> peripheral_power_client.c.
 * Random UUIDs, fixed for good: both halves have to agree on them, and they
 * are built from different .conf files, possibly months apart.
 *
 * The value, readable and notified on a power change:
 *   [0]     flags: bit 0 set while the reporting half has USB power
 *   [1..4]  that half's uptime in ms, little-endian, saturating -- tells the
 *           ladder whether a loss was that half restarting (its sleep)
 * A reader takes what it understands: an older one reads byte 0 alone.
 *
 * The service is exactly [service] [characteristic declaration] [value]
 * [CCC], in that order: the client subscribes at value handle + 1 instead of
 * spending a round trip on finding the CCC.
 */
#define LADDER_POWER_SVC_UUID                                                             \
	BT_UUID_128_ENCODE(0x97db0e4b, 0x49aa, 0x42a5, 0x89ef, 0x821979a255dd)
#define LADDER_POWER_CHR_UUID                                                             \
	BT_UUID_128_ENCODE(0x827fe824, 0x8da9, 0x4b99, 0x826f, 0xe0407e3178b4)

#define LADDER_POWER_FLAG_USB   0x01
#define LADDER_POWER_REPORT_LEN 5
