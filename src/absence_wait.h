/*
 * Copyright (c) 2026 eXidno
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

/*
 * ABSENCE_WAIT_MS, "W": how long a half goes on without something it needs
 * before it saves power. One number for both uses, on purpose:
 *
 *   the early sleep (early_sleep.c): W of idle without a computer, or
 *   without the other half, before a half sleeps;
 *
 *   the low-power search (low_power_search.c): W of ZMK's full search (or
 *   the module's middle one) for a missing other half before the central
 *   listens only now and then.
 *
 * The other half, gone, is itself W from its own early sleep -- its W counts
 * from the loss as well -- so the full or the middle search runs for as long
 * as that half may still be awake and calling, and the low-power one takes
 * over when it has gone to sleep. BLE_PAIRING_WAIT_MS, "P", replaces W for
 * the central's early sleep while the active profile is open for pairing.
 * Not an API.
 *
 * The one thing W must never do is cut off a settings write. ZMK writes a
 * changed Bluetooth profile, output, underglow, backlight or external power
 * state ZMK_SETTINGS_SAVE_DEBOUNCE after the change -- the change being a key
 * press -- and idle starts ZMK_IDLE_TIMEOUT after the last key press. W is
 * counted from idle, so it has to cover the rest of the debounce, and 5 s on
 * top. A peripheral half keeps only underglow, backlight and external power,
 * and with none of them it has nothing to write.
 */
#if IS_ENABLED(CONFIG_SETTINGS) &&                                                             \
	(IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW) ||     \
	 IS_ENABLED(CONFIG_ZMK_BACKLIGHT) || IS_ENABLED(CONFIG_ZMK_EXT_POWER))
#define SAVE_DEBOUNCE_MS CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE
#define SAVE_FORMULA_MS  (SAVE_DEBOUNCE_MS - CONFIG_ZMK_IDLE_TIMEOUT + 5000)
#else
#define SAVE_FORMULA_MS 0
#endif

#define ABSENCE_WAIT_FLOOR_MS 10000
#define ABSENCE_WAIT_AUTO_MS  MAX(SAVE_FORMULA_MS, 30000)
#define ABSENCE_WAIT_MS                                                                            \
	(CONFIG_ZMK_SPLIT_CONN_LADDER_ABSENCE_WAIT_MS                                              \
		 ? CONFIG_ZMK_SPLIT_CONN_LADDER_ABSENCE_WAIT_MS                                    \
		 : ABSENCE_WAIT_AUTO_MS)

BUILD_ASSERT(CONFIG_ZMK_SPLIT_CONN_LADDER_ABSENCE_WAIT_MS == 0 ||
		     CONFIG_ZMK_SPLIT_CONN_LADDER_ABSENCE_WAIT_MS >= ABSENCE_WAIT_FLOOR_MS,
	     "ZMK_SPLIT_CONN_LADDER_ABSENCE_WAIT_MS: 0 (auto) or at least 10000");
BUILD_ASSERT(CONFIG_ZMK_SPLIT_CONN_LADDER_ABSENCE_WAIT_MS == 0 ||
		     CONFIG_ZMK_SPLIT_CONN_LADDER_ABSENCE_WAIT_MS >= SAVE_FORMULA_MS,
	     "ZMK_SPLIT_CONN_LADDER_ABSENCE_WAIT_MS is below ZMK_SETTINGS_SAVE_DEBOUNCE - "
	     "ZMK_IDLE_TIMEOUT + 5000: a sleep could cut off a settings write. Raise it, or "
	     "set 0 for the automatic value");

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) &&                                                   \
	IS_ENABLED(CONFIG_ZMK_SPLIT_CONN_LADDER_EARLY_SLEEP)
#define BLE_PAIRING_WAIT_MS CONFIG_ZMK_SPLIT_CONN_LADDER_EARLY_SLEEP_BLE_PAIRING_WAIT_MS
BUILD_ASSERT(BLE_PAIRING_WAIT_MS >= ABSENCE_WAIT_MS,
	     "ZMK_SPLIT_CONN_LADDER_EARLY_SLEEP_BLE_PAIRING_WAIT_MS must not be below "
	     "ZMK_SPLIT_CONN_LADDER_ABSENCE_WAIT_MS");
BUILD_ASSERT(BLE_PAIRING_WAIT_MS >= SAVE_FORMULA_MS,
	     "ZMK_SPLIT_CONN_LADDER_EARLY_SLEEP_BLE_PAIRING_WAIT_MS is below "
	     "ZMK_SETTINGS_SAVE_DEBOUNCE - ZMK_IDLE_TIMEOUT + 5000: a sleep could cut off a "
	     "settings write");
#endif

/*
 * Underglow that switches itself off does not wait for a key: it asks for a
 * settings write at the moment it switches -- on entering idle
 * (ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE), on a cable going in or out
 * (_AUTO_OFF_USB) -- and that write lands SAVE_DEBOUNCE later. The early
 * sleep then waits for it from that moment on.
 */
#if IS_ENABLED(CONFIG_SETTINGS) && IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW) &&                        \
	IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE)
#define GUARD_IDLE 1
#else
#define GUARD_IDLE 0
#endif
#if IS_ENABLED(CONFIG_SETTINGS) && IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW) &&                        \
	IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)
#define GUARD_USB 1
#else
#define GUARD_USB 0
#endif
#if GUARD_IDLE || GUARD_USB
#define GUARD_MS (CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE + 5000)
#endif
