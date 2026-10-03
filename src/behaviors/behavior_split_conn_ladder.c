/*
 * Copyright (c) 2026 eXidno
 *
 * SPDX-License-Identifier: MIT
 *
 * &ladder -- switches the split connection ladder between AUTO, ON and OFF.
 *
 *   &ladder LADDER_TOG    ON <-> AUTO (from OFF: ON)
 *   &ladder LADDER_CYCLE  AUTO -> ON -> OFF -> AUTO
 *   &ladder LADDER_AUTO   battery steps on battery, USB power steps on USB power
 *                         (ACTIVE when both halves are on USB power)
 *   &ladder LADDER_ON     battery steps regardless of power
 *   &ladder LADDER_OFF    never step down
 *
 * Central locality: the ladder lives on the central half, and a key on the
 * peripheral half is resolved through the central's keymap anyway, so it
 * reaches this code on the central without anything crossing the split link.
 */

#define DT_DRV_COMPAT zmk_behavior_split_conn_ladder

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>

#include <dt-bindings/zmk/split_conn_ladder.h>
#include <zmk_split_conn_ladder/ladder.h>

LOG_MODULE_DECLARE(zmk_split_conn_ladder, CONFIG_ZMK_SPLIT_CONN_LADDER_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
/* Lets ZMK Studio offer the five commands by name. */
static const struct behavior_parameter_value_metadata mode_values[] = {
	{
		.display_name = "Toggle ON / AUTO",
		.value = LADDER_TOG,
		.type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
	},
	{
		.display_name = "Cycle AUTO / ON / OFF",
		.value = LADDER_CYCLE,
		.type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
	},
	{
		.display_name = "AUTO: by power source",
		.value = LADDER_AUTO,
		.type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
	},
	{
		.display_name = "ON: battery steps always",
		.value = LADDER_ON,
		.type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
	},
	{
		.display_name = "OFF: always ACTIVE",
		.value = LADDER_OFF,
		.type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
	},
};

static const struct behavior_parameter_metadata_set mode_set = {
	.param1_values = mode_values,
	.param1_values_len = ARRAY_SIZE(mode_values),
};

static const struct behavior_parameter_metadata metadata = {
	.sets_len = 1,
	.sets = &mode_set,
};
#endif /* IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA) */

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
				     struct zmk_behavior_binding_event event)
{
	ARG_UNUSED(event);

	int mode;

	switch (binding->param1) {
	case LADDER_AUTO:
	case LADDER_ON:
	case LADDER_OFF:
		mode = (int)binding->param1;
		break;
	case LADDER_TOG:
		/*
		 * Read, then write: not one atomic step. Key presses reach
		 * behaviors one at a time on the system work queue, so two
		 * toggles cannot interleave there; only a call from another
		 * thread (an input processor bound to &ladder, or other code
		 * using split_conn_ladder_set_mode()) could slip in between, and
		 * all it could do is swallow one toggle -- which the "mode X ->
		 * Y" log line shows, and the next press puts right.
		 */
		mode = split_conn_ladder_get_mode() == LADDER_ON ? LADDER_AUTO : LADDER_ON;
		break;
	case LADDER_CYCLE:
		switch (split_conn_ladder_get_mode()) {
		case LADDER_AUTO:
			mode = LADDER_ON;
			break;
		case LADDER_ON:
			mode = LADDER_OFF;
			break;
		default:
			mode = LADDER_AUTO;
			break;
		}
		break;
	default:
		LOG_ERR("unknown &ladder parameter %u", binding->param1);
		return -ENOTSUP;
	}

	int err = split_conn_ladder_set_mode(mode);

	return err ? err : ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
				      struct zmk_behavior_binding_event event)
{
	ARG_UNUSED(binding);
	ARG_UNUSED(event);

	return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_split_conn_ladder_driver_api = {
	.binding_pressed = on_keymap_binding_pressed,
	.binding_released = on_keymap_binding_released,
	.locality = BEHAVIOR_LOCALITY_CENTRAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
	.parameter_metadata = &metadata,
#endif
};

/* Stateless, so any number of nodes can share the one API. */
#define LADDER_INST(n)                                                               \
	BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,              \
				CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                  \
				&behavior_split_conn_ladder_driver_api);

DT_INST_FOREACH_STATUS_OKAY(LADDER_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
