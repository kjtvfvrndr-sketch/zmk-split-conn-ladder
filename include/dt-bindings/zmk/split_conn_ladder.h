/*
 * Copyright (c) 2026 eXidno
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/*
 * Parameters for the &ladder behavior, and the mode values the ladder itself
 * uses. The keymap includes this file; so does the C code, so the two cannot
 * disagree about what a number means.
 */

/* The power source decides: the battery steps on battery, the USB power
 * steps on USB power -- or ACTIVE there, when the other half reports USB
 * power too or ZMK_SPLIT_CONN_LADDER_USB_POWER_STEPS is off. */
#define LADDER_AUTO 0

/* The battery steps regardless of power -- for watching them over a USB log. */
#define LADDER_ON 1

/* Never step down. */
#define LADDER_OFF 2

/* ON <-> AUTO. From OFF it goes to ON. */
#define LADDER_TOG 3

/* AUTO -> ON -> OFF -> AUTO: every mode from one key. */
#define LADDER_CYCLE 4
