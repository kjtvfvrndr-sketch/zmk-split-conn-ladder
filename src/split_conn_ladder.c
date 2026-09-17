/*
 * Copyright (c) 2026 eXidno
 *
 * SPDX-License-Identifier: MIT
 *
 * Steps the SPLIT-link connection parameters down while the keyboard is idle
 * and restores them the moment activity resumes.
 *
 * Why the split link and not the host link: on the split link this half is
 * central, so bt_conn_le_param_update() becomes LL_CONNECTION_UPDATE_IND --
 * an order applied at a Link Layer instant, with no negotiation and no way
 * for the peer to refuse. Toward the host this half is peripheral and could
 * only ask; and there is nothing to win there anyway, because the negotiated
 * peripheral latency already keeps that link at roughly two connection
 * events per second, against ~133 for a 7.5 ms split link. A central cannot
 * use latency at all -- it must open every single connection event -- so on
 * the split link the interval is the only lever, and it is a big one.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/activity.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/workqueue.h>

LOG_MODULE_REGISTER(zmk_split_conn_ladder, CONFIG_ZMK_SPLIT_CONN_LADDER_LOG_LEVEL);

#define SETTLE_MS CONFIG_ZMK_SPLIT_CONN_LADDER_SETTLE_MS
#define RETRY_MS  CONFIG_ZMK_SPLIT_CONN_LADDER_RETRY_MS

/* Bluetooth interval units (1.25 ms) to milliseconds, integer only. */
#define INT_MS_W(i) ((i) * 125 / 100)
#define INT_MS_F(i) ((i) * 125 % 100)

struct step {
	uint32_t after_ms;
	uint16_t interval;
	uint16_t latency;
	uint16_t timeout;
};

#define STEP(n)                                                      \
	{                                                            \
		.after_ms = CONFIG_ZMK_SPLIT_CONN_LADDER_S##n##_AFTER_MS,    \
		.interval = CONFIG_ZMK_SPLIT_CONN_LADDER_S##n##_INT,         \
		.latency = CONFIG_ZMK_SPLIT_CONN_LADDER_S##n##_LATENCY,      \
		.timeout = CONFIG_ZMK_SPLIT_CONN_LADDER_S##n##_TIMEOUT,      \
	}

/*
 * Zephyr's bt_le_conn_params_valid() rejects anything that fails
 *     timeout * 4 > (1 + latency) * interval_max
 * (BT Core 5.0, Vol 2, Part E, 7.8.12). Catching that at build time beats
 * chasing dropped links on the desk. Same inequality, multiplied by 5.
 */
#define CHECK(name, to, lat, in)                                                     \
	BUILD_ASSERT((to) * 20 > (1 + (lat)) * (in) * 5,                             \
		     name ": supervision timeout too low for interval/latency")

#define CHECK_STEP(n)                                            \
	CHECK("step " #n, CONFIG_ZMK_SPLIT_CONN_LADDER_S##n##_TIMEOUT,   \
	      CONFIG_ZMK_SPLIT_CONN_LADDER_S##n##_LATENCY,               \
	      CONFIG_ZMK_SPLIT_CONN_LADDER_S##n##_INT)

/*
 * ACTIVE reads the very symbols the link is created with in
 * bt_conn_le_create(), so our idea of "fast" and the link's actual state
 * cannot diverge. Do not turn these into separate Kconfig options.
 */
static const struct step step_active = {
	.interval = CONFIG_ZMK_SPLIT_BLE_PREF_INT,
	.latency = CONFIG_ZMK_SPLIT_BLE_PREF_LATENCY,
	.timeout = CONFIG_ZMK_SPLIT_BLE_PREF_TIMEOUT,
};

CHECK("active", CONFIG_ZMK_SPLIT_BLE_PREF_TIMEOUT, CONFIG_ZMK_SPLIT_BLE_PREF_LATENCY,
      CONFIG_ZMK_SPLIT_BLE_PREF_INT);

static const struct step idle_steps[] = {
	STEP(1),
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEPS >= 2
	STEP(2),
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEPS >= 3
	STEP(3),
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEPS >= 4
	STEP(4),
#endif
};

CHECK_STEP(1);
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEPS >= 2
CHECK_STEP(2);
BUILD_ASSERT(CONFIG_ZMK_SPLIT_CONN_LADDER_S2_AFTER_MS >
		     CONFIG_ZMK_SPLIT_CONN_LADDER_S1_AFTER_MS,
	     "step 2 must come after step 1");
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEPS >= 3
CHECK_STEP(3);
BUILD_ASSERT(CONFIG_ZMK_SPLIT_CONN_LADDER_S3_AFTER_MS >
		     CONFIG_ZMK_SPLIT_CONN_LADDER_S2_AFTER_MS,
	     "step 3 must come after step 2");
#endif
#if CONFIG_ZMK_SPLIT_CONN_LADDER_STEPS >= 4
CHECK_STEP(4);
BUILD_ASSERT(CONFIG_ZMK_SPLIT_CONN_LADDER_S4_AFTER_MS >
		     CONFIG_ZMK_SPLIT_CONN_LADDER_S3_AFTER_MS,
	     "step 4 must come after step 3");
#endif

#define N_IDLE ((int)ARRAY_SIZE(idle_steps))

/*
 * current_level: 0 = ACTIVE, 1..N_IDLE = idle_steps[level - 1].
 *
 * Exactly one writer -- ladder_work_cb, on the low-priority work queue.
 * on_connected() runs on the Bluetooth RX thread and only raises a flag,
 * because the compare-then-set inside apply_level() is not atomic: a
 * reconnect landing in the middle of an update would otherwise leave our
 * belief and the link's reality permanently out of step, and the early
 * return would then freeze us on the wrong parameters.
 */
static int current_level;

/* 32-bit uptime: the read is atomic on a 32-bit target and unsigned
 * subtraction stays correct across the ~49-day wrap. */
static uint32_t idle_since;

static atomic_t link_reset;

static const struct step *step_at(int level)
{
	return level == 0 ? &step_active : &idle_steps[level - 1];
}

struct apply_ctx {
	const struct step *s;
	int err;
	int matched;
};

static void apply_to_conn(struct bt_conn *conn, void *data)
{
	struct apply_ctx *ctx = data;
	struct bt_conn_info info;

	if (bt_conn_get_info(conn, &info) || info.type != BT_CONN_TYPE_LE) {
		return;
	}

	/* Central role means the other half. Peripheral role means the host;
	 * that link is not ours to command and not worth optimising. */
	if (info.role != BT_CONN_ROLE_CENTRAL) {
		return;
	}

	ctx->matched++;

	struct bt_le_conn_param p = BT_LE_CONN_PARAM_INIT(ctx->s->interval, ctx->s->interval,
							  ctx->s->latency, ctx->s->timeout);

	int err = bt_conn_le_param_update(conn, &p);

	if (err && err != -EALREADY) {
		LOG_WRN("param update rejected: %d", err);
		ctx->err = err;
	}
}

/*
 * Returns 0 when the level was applied (or was already current), otherwise
 * the errno of the last failure. With no split link up, matched stays 0 and
 * the level is recorded as intent -- on_connected() will force a re-apply
 * once the link is back, so recording it here cannot strand us.
 */
static int apply_level(int level)
{
	if (level == current_level) {
		return 0;
	}

	const struct step *s = step_at(level);
	struct apply_ctx ctx = {.s = s, .err = 0, .matched = 0};

	bt_conn_foreach(BT_CONN_TYPE_LE, apply_to_conn, &ctx);

	if (ctx.err) {
		return ctx.err;
	}

	current_level = level;

	if (ctx.matched == 0) {
		LOG_DBG("level %d recorded, no split link up yet", level);
		return 0;
	}

	LOG_INF("step %d/%d: interval %u.%02u ms, latency %u, timeout %u ms "
		"(peripheral on air every %u ms, ramp-up %u ms)",
		level, N_IDLE, INT_MS_W(s->interval), INT_MS_F(s->interval), s->latency,
		s->timeout * 10, INT_MS_W(s->interval) * (s->latency + 1),
		INT_MS_W(s->interval) * 6);

	return 0;
}

static void ladder_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(ladder_work, ladder_work_cb);

static void reschedule(k_timeout_t delay)
{
	/* Never the system work queue: for a central,
	 * bt_conn_le_param_update() goes into bt_hci_cmd_send_sync() and
	 * blocks until the controller answers. */
	k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &ladder_work, delay);
}

static void ladder_work_cb(struct k_work *work)
{
	ARG_UNUSED(work);

	if (atomic_cas(&link_reset, 1, 0)) {
		/* A re-created link comes up on PREF_*, which is exactly
		 * ACTIVE. Record the truth before computing anything. */
		current_level = 0;
		LOG_DBG("link re-established, level forced to 0");
	}

	/* The activity state is read here rather than in the listener, so that
	 * an already-running instance cannot clobber a just-applied ACTIVE. */
	enum zmk_activity_state st = zmk_activity_get_state();

	if (st == ZMK_ACTIVITY_ACTIVE) {
		if (apply_level(0)) {
			reschedule(K_MSEC(RETRY_MS));
		}
		return;
	}

	if (st == ZMK_ACTIVITY_SLEEP) {
		/* The board is powering off; there is nothing to restore. */
		return;
	}

	uint32_t elapsed = k_uptime_get_32() - idle_since;
	int level = 0;

	for (int i = N_IDLE - 1; i >= 0; i--) {
		if (elapsed >= idle_steps[i].after_ms) {
			level = i + 1;
			break;
		}
	}

	if (apply_level(level)) {
		reschedule(K_MSEC(RETRY_MS));
		return;
	}

	if (level < N_IDLE) {
		/* The loop did not select idle_steps[level], so elapsed is
		 * strictly below its after_ms and the difference is positive. */
		reschedule(K_MSEC(idle_steps[level].after_ms - elapsed));
	}
}

static const char *state_name(enum zmk_activity_state st)
{
	switch (st) {
	case ZMK_ACTIVITY_ACTIVE:
		return "ACTIVE";
	case ZMK_ACTIVITY_IDLE:
		return "IDLE";
	case ZMK_ACTIVITY_SLEEP:
		return "SLEEP";
	default:
		return "?";
	}
}

static int on_activity(const zmk_event_t *eh)
{
	const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);

	if (!ev) {
		return ZMK_EV_EVENT_BUBBLE;
	}

	LOG_INF("activity: %s", state_name(ev->state));

	switch (ev->state) {
	case ZMK_ACTIVITY_IDLE:
		idle_since = k_uptime_get_32();
		reschedule(K_NO_WAIT);
		break;
	case ZMK_ACTIVITY_ACTIVE:
		reschedule(K_NO_WAIT);
		break;
	case ZMK_ACTIVITY_SLEEP:
		/* Otherwise a pending instance fires during
		 * zmk_pm_suspend_devices() and blocks on HCI against a radio
		 * that is already going down. */
		k_work_cancel_delayable(&ladder_work);
		break;
	default:
		break;
	}

	return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zmk_split_conn_ladder, on_activity);
ZMK_SUBSCRIPTION(zmk_split_conn_ladder, zmk_activity_state_changed);

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	struct bt_conn_info info;

	if (err || bt_conn_get_info(conn, &info) || info.role != BT_CONN_ROLE_CENTRAL) {
		return;
	}

	atomic_set(&link_reset, 1);
	reschedule(K_MSEC(SETTLE_MS));
}

/*
 * What the controller actually applied. This is the line the whole exercise
 * is measured by: the delay between a "step N" line and this one is the real
 * Link Layer instant, and comparing the values shows whether the request
 * survived intact. Host-link updates are logged too -- that is where the
 * interval and latency the host granted become visible.
 */
static void on_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
			     uint16_t timeout)
{
	struct bt_conn_info info;

	if (bt_conn_get_info(conn, &info)) {
		return;
	}

	LOG_INF("%s link applied: interval %u.%02u ms, latency %u, timeout %u ms",
		info.role == BT_CONN_ROLE_CENTRAL ? "split" : "host", INT_MS_W(interval),
		INT_MS_F(interval), latency, timeout * 10);
}

static struct bt_conn_cb conn_cb = {
	.connected = on_connected,
	.le_param_updated = on_param_updated,
};

static int zmk_split_conn_ladder_init(void)
{
	/* void in Zephyr 3.5, returns int from 3.7 on -- revisit when the
	 * ZMK base moves. */
	bt_conn_cb_register(&conn_cb);
	current_level = 0;
	return 0;
}

SYS_INIT(zmk_split_conn_ladder_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
