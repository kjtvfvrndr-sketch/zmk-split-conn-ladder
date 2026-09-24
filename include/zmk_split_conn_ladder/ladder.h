/*
 * Copyright (c) 2026 eXidno
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <dt-bindings/zmk/split_conn_ladder.h>

/*
 * Mode values are LADDER_AUTO, LADDER_ON and LADDER_OFF. The mode lives in RAM
 * only: every boot starts from CONFIG_ZMK_SPLIT_CONN_LADDER_PAUSE_ON_USB.
 */

int split_conn_ladder_get_mode(void);

/* Returns 0, or -EINVAL for anything that is not a mode. Safe from any thread;
 * the ladder re-evaluates on its own work queue. */
int split_conn_ladder_set_mode(int mode);
