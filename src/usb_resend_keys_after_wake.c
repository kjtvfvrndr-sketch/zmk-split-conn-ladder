/*
 * Copyright (c) 2026 eXidno
 *
 * SPDX-License-Identifier: MIT
 *
 * Gets the keyboard state to a USB host that has just been woken by a key.
 *
 * Two things go wrong in stock ZMK when a key wakes a sleeping computer over
 * USB, and both are about reports that never reach it.
 *
 * 1. The waking key. While the bus is suspended, zmk_usb_hid_send_report()
 *    (usb_hid.c) only asks for a remote wakeup and returns; the report itself
 *    is never written. The log shows it as
 *
 *        <err> zmk: FAILED TO SEND OVER USB: -11
 *        <inf> usb_hid: Device resumed
 *
 *    where -11 is the wakeup request's own status (-EAGAIN from the nRF
 *    driver); when that request succeeds, ZMK logs nothing and still drops
 *    the report.
 *
 * 2. The wake itself. The nRF driver reports USB_DC_RESUME the moment the
 *    DEVICE starts signalling the wakeup (NRFX_USBD_EVT_WUREQ), long before a
 *    host in system sleep starts polling again -- that can take seconds. From
 *    then on ZMK believes the bus is up. Its next report is parked in the IN
 *    endpoint, and every report after that finds the endpoint busy and is
 *    dropped (-EAGAIN; the first one after a successful write waits 30 ms for
 *    the endpoint, the rest fail at once). When the host finally polls, it
 *    reads the parked report, which may be a key-down whose key-up was one of
 *    the dropped ones -- a key that auto-repeats until the next keystroke.
 *
 * What this does. A HID keyboard report is a snapshot of what is held, not a
 * keystroke: the host builds key events from differences between snapshots,
 * and sending the same snapshot twice repeats nothing. After every wake of the
 * bus -- whatever caused it: the gap between "bus up" and "host reading" is
 * the same for a wake by the trackball, a layer key or the host itself; a
 * plug-in is not a wake (on_usb()):
 *
 *   - if a key report was dropped and what the host will hold differs from
 *     what is held now -- a waking key still down, or a release the host
 *     never got -- the current snapshot is written;
 *   - every key change until the host is seen reading is written again after
 *     ZMK's own write, retrying while the endpoint is busy, so whatever ZMK
 *     parked there is followed by the true state;
 *   - "seen reading" means a write went through after an earlier one did.
 *
 * It never writes a snapshot the host already holds: an empty report parked
 * for nothing would only take the one slot the next real keystroke needs.
 *
 * What it cannot do: deliver key changes that happened entirely while the
 * host was not reading. Only one report fits in the endpoint, and the ones ZMK
 * failed to write are gone. The log counts them. Mouse reports carry relative
 * movement, which a second copy would apply twice, and are left alone -- their
 * buttons with them: a click released behind a parked report stays held until
 * the next mouse report.
 *
 * Independent of the ladder; it lives in the same module only for convenience.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usb_device.h>

#include <dt-bindings/zmk/hid_usage_pages.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/hid.h>
#include <zmk/usb.h>
#include <zmk/usb_hid.h>

LOG_MODULE_REGISTER(zmk_usb_resend_keys_after_wake, CONFIG_ZMK_USB_RESEND_KEYS_AFTER_WAKE_LOG_LEVEL);

#define DELAY_MS   CONFIG_ZMK_USB_RESEND_KEYS_AFTER_WAKE_DELAY_MS
#define GIVE_UP_MS CONFIG_ZMK_USB_RESEND_KEYS_AFTER_WAKE_GIVE_UP_MS

/*
 * Retries while the endpoint is busy: 20 ms, doubling to 160 ms. Every refused
 * write is also eleven "Failed to write endpoint buffer" warnings in a logging
 * build (usb_write() retries ZMK's USB_NUMOF_EP_WRITE_RETRIES=10 times), so
 * the backoff matters. The ceiling stays under the shortest auto-repeat delay
 * any host offers (225 ms on macOS), so a key-down the host reads is followed
 * by its key-up before it can repeat. After GIVE_UP_MS, only a change still
 * waiting to be written is retried, once a second -- a host that first reads
 * that late can repeat a key for up to that second.
 */
#define RETRY_MS      20
#define RETRY_MAX_MS  160
#define RETRY_SLOW_MS 1000

/* After a key change, before writing the snapshot. ZMK's hid_listener links
 * ahead of this module and has normally updated the report already; the delay
 * makes that ordering not matter, and folds a burst of changes into one write. */
#define SETTLE_MS 5

#define PAGE_KEY      0x1
#define PAGE_CONSUMER 0x2

/*
 * Touched from any thread that raises key events (an input processor can
 * invoke behaviors from the input thread), so atomics:
 *   dropped  pages ZMK threw away while the bus was suspended
 *   dirty    pages with a change to (re)write during a sync
 *   syncing  a sync is running
 *   changes  key changes seen during the sync, for the log
 */
static atomic_t dropped;
static atomic_t dirty;
static atomic_t syncing;
static atomic_t changes;

static uint32_t dropped_at; /* diagnostic only; written wherever the drop is seen */

/*
 * What the host will hold once it has read everything already written into
 * the endpoint. Moved on by every key change ZMK delivers while the bus is up
 * and no sync runs, and by every write of ours that goes through; emptied when
 * the host re-enumerates. Read by sync_cb only while a sync runs, when the
 * awake path no longer writes it.
 */
static struct zmk_hid_keyboard_report_body host_view_kbd;
static struct zmk_hid_consumer_report_body host_view_cons;

/*
 * System work queue only: the USB event and sync_cb both run there.
 *
 * Two different facts about our own writes, kept apart on purpose:
 *   landed       one went through since this wake; the NEXT one going through
 *                proves the host reads now (it must have taken the first)
 *   ours_unread  one went through and nobody has proven it read -- it may be
 *                parked in the endpoint still, in front of whatever comes
 *                next. Survives a restart; only a proof, a port reset, an
 *                unplug or the output moving clears it.
 * A single flag for both would either end a sync on one write after a nap
 * (a write read BEFORE the nap proves nothing now) or walk away from a report
 * of ours still parked.
 */
static uint32_t sync_start;
static uint32_t first_write_ms; /* when ours first went through, since the wake */
static uint32_t refused;
static uint32_t retry_ms = RETRY_MS;
static atomic_val_t check; /* dropped pages still to compare with the host */
static bool landed;
static bool ours_unread;
static bool told_late; /* the "still not reading" line is out */

/* For the log only: whether the bus went to sleep at all, and for how long.
 * The first thing to know when a wake does not behave -- a host that only
 * turned its display off never suspends the bus, and then there is nothing
 * here to do. */
static bool suspended;
static uint32_t suspended_at;
/* Left a suspend, and the watch below starts once the bus is up again. */
static bool wake_pending;

static bool usb_selected(void)
{
	return zmk_endpoints_selected().transport == ZMK_TRANSPORT_USB;
}

/* Configured and not suspended. Writing into a suspended bus would take
 * ZMK's suspend branch and wake a host that went back to sleep on purpose. */
static bool bus_up(void)
{
	return zmk_usb_is_hid_ready() && zmk_usb_get_status() != USB_DC_SUSPEND;
}

static bool any_set(const void *data, size_t len)
{
	const uint8_t *p = data;

	for (size_t i = 0; i < len; i++) {
		if (p[i]) {
			return true;
		}
	}
	return false;
}

static bool held(atomic_val_t pages)
{
	bool h = false;

	if (pages & PAGE_KEY) {
		const struct zmk_hid_keyboard_report *r = zmk_hid_get_keyboard_report();

		h |= r->body.modifiers || any_set(r->body.keys, sizeof(r->body.keys));
	}
	if (pages & PAGE_CONSUMER) {
		const struct zmk_hid_consumer_report *r = zmk_hid_get_consumer_report();

		h |= any_set(r->body.keys, sizeof(r->body.keys));
	}
	return h;
}

/* Whether the host will already hold what this page holds now. */
static bool host_has(atomic_val_t page)
{
	if (page == PAGE_KEY) {
		return !memcmp(&zmk_hid_get_keyboard_report()->body, &host_view_kbd, sizeof(host_view_kbd));
	}
	return !memcmp(&zmk_hid_get_consumer_report()->body, &host_view_cons, sizeof(host_view_cons));
}

static void host_takes(atomic_val_t pages)
{
	if (pages & PAGE_KEY) {
		memcpy(&host_view_kbd, &zmk_hid_get_keyboard_report()->body, sizeof(host_view_kbd));
	}
	if (pages & PAGE_CONSUMER) {
		memcpy(&host_view_cons, &zmk_hid_get_consumer_report()->body, sizeof(host_view_cons));
	}
}

static void host_forgets(void)
{
	memset(&host_view_kbd, 0, sizeof(host_view_kbd));
	memset(&host_view_cons, 0, sizeof(host_view_cons));
}

static void stop(void)
{
	atomic_set(&syncing, 0);
	atomic_set(&dirty, 0);
	check = 0;
}

static void sync_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sync_work, sync_cb);

/* write_page(): 0 wrote it, WRITE_SKIP the page cannot be written at all in
 * this protocol, anything else the endpoint was busy or the bus went away. */
#define WRITE_SKIP 1

/*
 * Straight to the USB HID layer rather than through zmk_endpoints_send_report:
 * the endpoint check is done here already, and a busy endpoint is the expected
 * answer while the host wakes, not an error worth ZMK's own log line.
 */
static int write_page(atomic_val_t page)
{
	/* The bus can suspend between sync_cb's check and this write -- the
	 * host is free to go back to sleep at any point. ZMK answers a write
	 * into a suspended bus by requesting a remote wakeup and returning
	 * THAT request's status, which is 0 when the host grants it. Taking
	 * that branch here would wake a computer that went to sleep on
	 * purpose, and its 0 would be read below as a report the host has
	 * seen. So the bus is checked on both sides of the call, and a bus
	 * that is not up is reported as a refusal, never as a write. */
	if (!bus_up()) {
		return -EAGAIN;
	}

	int err = page == PAGE_KEY ? zmk_usb_hid_send_keyboard_report()
				   : zmk_usb_hid_send_consumer_report();

	if (!bus_up()) {
		return -EAGAIN;
	}

	/* Consumer reports do not exist in the boot protocol. The keyboard
	 * page always does, so -ENOTSUP there is a real failure. */
	if (page == PAGE_CONSUMER && err == -ENOTSUP) {
		return WRITE_SKIP;
	}

	return err;
}

static void sync_cb(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!atomic_get(&syncing)) {
		return;
	}

	const uint32_t now = k_uptime_get_32();

	if (!usb_selected()) {
		LOG_DBG("output moved off USB, sync abandoned");
		stop();
		return;
	}
	if (!bus_up()) {
		/* Asleep again, or being reset: the next USB event re-arms this. */
		LOG_DBG("bus not up, waiting");
		return;
	}

	/*
	 * First pass after a wake: a page ZMK threw away needs a write only if
	 * the host is left holding something else. The usual waking tap was
	 * pressed and released before the host could read either, the host
	 * holds "nothing" and nothing is held: writing that empty snapshot
	 * would park a report the host does not need in the one slot the
	 * next keystroke does.
	 */
	if (check) {
		atomic_val_t need = 0;

		for (atomic_val_t page = PAGE_KEY; page <= PAGE_CONSUMER; page <<= 1) {
			if ((check & page) && !host_has(page)) {
				need |= page;
			}
		}
		check = 0;

		if (need) {
			atomic_or(&dirty, need);
			LOG_INF("host resumed %u ms after a dropped report -- re-sending, %s",
				now - dropped_at,
				held(need) ? "a key is still held and will be typed"
					   : "the host still holds a key that is up");
		} else {
			LOG_INF("host resumed %u ms after a dropped report -- nothing to "
				"re-send (a tap that ended before the wake is lost)",
				now - dropped_at);
		}
	}

	const bool before_give_up = now - sync_start < GIVE_UP_MS;
	atomic_val_t pages = atomic_get(&dirty);
	bool probe = false;

	if (!pages) {
		if (before_give_up && landed) {
			/* Unchanged since our write this wake: rewrite the keyboard
			 * snapshot as the probe that tells whether it was read. It
			 * cannot take a slot from anything -- while ours is unread
			 * it is simply refused. */
			pages = PAGE_KEY;
			probe = true;
		} else if (before_give_up) {
			/*
			 * Nothing to deliver, nothing of ours written this wake:
			 * only watch. A key typed before the host polls is how
			 * ZMK's own parked report turns into a stuck key, and
			 * on_keycode() wakes this for it. No probe here: if ours
			 * from before a nap is still parked, a probe could only
			 * be refused, and if it was read then, a probe would park
			 * a report in the slot the next keystroke needs.
			 */
			k_work_reschedule(&sync_work, K_MSEC(GIVE_UP_MS - (now - sync_start)));
			return;
		} else if (ours_unread) {
			/*
			 * Past GIVE_UP_MS with a write of ours perhaps still
			 * unread. It holds the current state, so nothing needs
			 * correcting now; stop polling for it, but stay armed --
			 * a key change would be refused behind it, and
			 * on_keycode() brings us back for that. No timer runs.
			 */
			if (!told_late) {
				told_late = true;
				LOG_WRN("host still not reading %u s after the wake -- will "
					"re-send when a key changes",
					(now - sync_start) / 1000U);
			}
			return;
		} else {
			LOG_DBG("no key changed before the host was reading; done");
			stop();
			return;
		}
	}

	bool busy = false;
	bool wrote = false;

	for (atomic_val_t page = PAGE_KEY; page <= PAGE_CONSUMER; page <<= 1) {
		if (!(pages & page)) {
			continue;
		}
		/* Cleared before the write: a change that lands during it sets
		 * the bit again and gets its own write. A probe owns no bit. */
		if (!probe) {
			atomic_and(&dirty, ~page);
		}

		int err = write_page(page);

		if (err == WRITE_SKIP) {
			continue; /* nothing to retry: this host cannot take it */
		}
		if (err) {
			if (!probe) {
				atomic_or(&dirty, page);
			}
			busy = true;
			continue;
		}
		host_takes(page);
		wrote = true;
	}

	if (wrote) {
		/* Whatever else happened in this pass, a report of ours is in
		 * the endpoint now. */
		ours_unread = true;
	}

	if (busy) {
		/* The endpoint still holds an unread report: the host is not
		 * polling yet -- or, with two pages to write, is holding the one
		 * we just wrote in front of the other. Either way not in sync
		 * yet; a write that did go through still counts for the proof. */
		if (wrote) {
			landed = true;
		}
		refused++;
		if (before_give_up) {
			k_work_reschedule(&sync_work, K_MSEC(retry_ms));
			retry_ms = MIN(retry_ms * 2, RETRY_MAX_MS);
		} else {
			if (!told_late) {
				told_late = true;
				LOG_WRN("host still not reading %u s after the wake -- a key "
					"change is waiting, retrying every %u ms",
					(now - sync_start) / 1000U, RETRY_SLOW_MS);
			}
			k_work_reschedule(&sync_work, K_MSEC(RETRY_SLOW_MS));
		}
		return;
	}
	retry_ms = RETRY_MS;

	if (!wrote) {
		/* Every page in the set was skipped; next pass decides again. */
		k_work_reschedule(&sync_work, K_MSEC(RETRY_MS));
		return;
	}

	if (landed) {
		/*
		 * This write found the endpoint free, so our previous one from
		 * this wake was read: the host is polling, and holds the latest
		 * state or will within a millisecond.
		 *
		 * When it started polling is known only if a write was refused
		 * first. With none refused, it was reading by the time we first
		 * had anything to write -- which after a wake that dropped
		 * nothing is the first key change, whenever that came -- and the
		 * time says when that was, not how long the host took.
		 */
		if (refused) {
			LOG_INF("host reading again %u ms after the wake -- in sync; %u key "
				"changes meanwhile, %u writes refused",
				now - sync_start, (unsigned int)atomic_get(&changes), refused);
		} else {
			LOG_INF("host already reading at our first write, %u ms after the wake -- "
				"in sync; %u key changes meanwhile",
				first_write_ms, (unsigned int)atomic_get(&changes));
		}
		ours_unread = false;
		stop();
		return;
	}

	landed = true;
	first_write_ms = now - sync_start;
	if (before_give_up) {
		k_work_reschedule(&sync_work, K_MSEC(RETRY_MS)); /* the probe */
	}
	/* Past GIVE_UP_MS: no probing. The next key change, written after this
	 * one, is the proof -- on_keycode() brings us back for it. */
}

/*
 * A new wake always starts from scratch, even over a sync still running from
 * an earlier one: the host may have slept in between, and a write it read
 * before proves nothing about now. `check` is compared against the host on the
 * first pass; `carry` is changes an interrupted sync had not written yet.
 * Assigning rather than OR-ing keeps a bit left over by a sync that was
 * abandoned mid-change out of the new one.
 */
static void start_sync(atomic_val_t check_pages, atomic_val_t carry)
{
	check = check_pages;
	atomic_set(&dirty, carry);
	atomic_set(&changes, 0);
	sync_start = k_uptime_get_32();
	refused = 0;
	retry_ms = RETRY_MS;
	landed = false; /* ours_unread deliberately survives */
	told_late = false;
	atomic_set(&syncing, 1);
}

static void on_keycode(const struct zmk_keycode_state_changed *ev)
{
	atomic_val_t page;

	switch (ev->usage_page) {
	case HID_USAGE_KEY:
		page = PAGE_KEY;
		break;
	case HID_USAGE_CONSUMER:
		page = PAGE_CONSUMER;
		break;
	default:
		return;
	}

	/* A consumer key with modifiers moves the keyboard page too:
	 * hid_listener sends a keyboard report for the modifiers first. */
	if (page == PAGE_CONSUMER && (ev->implicit_modifiers || ev->explicit_modifiers)) {
		page |= PAGE_KEY;
	}

	if (!usb_selected()) {
		return;
	}

	if (!bus_up()) {
		/* ZMK is throwing this report away right now: its suspend branch
		 * asks for a wakeup and drops it, and a bus being reset or not yet
		 * configured again refuses it (-ENODEV) -- the output can still
		 * name USB for a moment after the bus went, until ZMK's event for
		 * it has run. */
		if (!atomic_or(&dropped, page)) {
			dropped_at = k_uptime_get_32();
		}
		return;
	}

	if (atomic_get(&syncing)) {
		/* ZMK's own write of this change may well have been refused, or
		 * parked in front of a later one. Write the snapshot again. */
		atomic_inc(&changes);
		atomic_or(&dirty, page);
		k_work_reschedule(&sync_work, K_MSEC(SETTLE_MS));
		return;
	}

	/* Awake, nothing to correct: ZMK's own write carries this to the host. */
	host_takes(page);
}

static void on_usb(void)
{
	const enum usb_dc_status_code status = zmk_usb_get_status();

	if (status == USB_DC_SUSPEND) {
		/*
		 * Only a bus the host has configured can be put to sleep. Before
		 * that -- the cable just went in, or the keyboard booted on one --
		 * the bus idles until the host's first reset, and the nRF
		 * controller reports the idle as a suspend too: Zephyr passes on
		 * SUSPEND, RESET, RESUME ahead of CONFIGURED (zephyr#14826).
		 * Nobody is asleep then, and nothing of ZMK's has gone to USB;
		 * counted, it made the enumeration that follows look like a wake.
		 */
		if (!zmk_usb_is_hid_ready()) {
			return;
		}
		if (!suspended) {
			suspended = true;
			suspended_at = k_uptime_get_32();
			LOG_INF("USB suspended: the host is asleep");
		}
		return; /* nothing else to do until it comes back */
	}
	if (suspended) {
		const uint32_t ms = k_uptime_get_32() - suspended_at;

		suspended = false;
		wake_pending = status != USB_DC_DISCONNECTED;
		/* A layer key sends no report at all, and mouse movement is
		 * never re-sent: waking by either drops no key. */
		LOG_INF("USB %s after %u.%u s suspended -- %s",
			status == USB_DC_DISCONNECTED ? "unplugged"
			: status == USB_DC_RESET      ? "reset by the host"
						      : "resumed",
			ms / 1000U, ms % 1000U / 100U,
			atomic_get(&dropped) ? "a key change was dropped meanwhile"
					     : "no key change was dropped");
	}

	if (status == USB_DC_DISCONNECTED) {
		/* Unplugged: nothing to send it to, and ZMK moves to BLE. */
		wake_pending = false;
		atomic_set(&dropped, 0);
		stop();
		host_forgets();
		landed = false;
		ours_unread = false;
		return;
	}
	if (status == USB_DC_RESET) {
		/* The endpoint is flushed and the host enumerates us afresh:
		 * it holds nothing, and nothing of ours is parked any more. The
		 * event can be missed -- ZMK raises it from a work item that
		 * reports whatever the status is by the time it runs. What that
		 * leaves is stale bookkeeping: a redundant write or two, or a
		 * key held straight through the re-enumeration going
		 * unannounced until it changes, which is what stock ZMK does
		 * with it anyway. */
		host_forgets();
		landed = false;
		ours_unread = false;
	}

	if (!bus_up()) {
		return;
	}

	/* RESUME -- announced by the nRF driver as soon as the wakeup starts --
	 * or CONFIGURED, after a host that reset the port on the way out. */
	atomic_val_t pages = atomic_set(&dropped, 0);
	const bool live = atomic_get(&syncing) != 0;
	/*
	 * Any wake, not only one a key caused. Woken by the trackball, by a
	 * layer key or by the host itself, the bus is up again long before the
	 * host reads, and a key typed into that gap meets exactly the parked
	 * report and the refused release that make a stuck key. With nothing
	 * dropped the sync starts as a watch: it writes nothing unless a key
	 * changes before the host is seen reading.
	 */
	const bool watch = wake_pending && usb_selected();

	wake_pending = false;
	if (!pages && !live && !watch) {
		return;
	}

	/* A resume in the middle of a sync means the host went away and came
	 * back: a write it read before that proves nothing about now, so the
	 * sync restarts -- clock, counters and all -- rather than carrying on
	 * where it left off. What it had not got to stays: changes not yet
	 * written, and dropped pages not yet compared (a port reset right after
	 * the resume leaves the first pass no bus to run on). */
	start_sync(pages | (live ? check : 0), live ? atomic_get(&dirty) : 0);
	k_work_reschedule(&sync_work, K_MSEC(DELAY_MS));
}

/*
 * The output moved, to USB or away from it. ZMK clears every report on the way
 * (zmk_endpoints_clear_current()) and sends the empty ones to the endpoint it
 * leaves, outside the keycode events this file watches. What the USB host
 * holds is then nothing -- told so on the way out, or never told anything
 * since -- and a sync for the old state has nothing left to do.
 */
static void on_output(void)
{
	stop();
	atomic_set(&dropped, 0);
	host_forgets();
	landed = false;
	ours_unread = false;
}

static int on_event(const zmk_event_t *eh)
{
	const struct zmk_keycode_state_changed *kc = as_zmk_keycode_state_changed(eh);

	if (kc) {
		on_keycode(kc);
	} else if (as_zmk_usb_conn_state_changed(eh)) {
		on_usb();
	} else if (as_zmk_endpoint_changed(eh)) {
		on_output();
	}

	return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zmk_usb_resend_keys_after_wake, on_event);
ZMK_SUBSCRIPTION(zmk_usb_resend_keys_after_wake, zmk_keycode_state_changed);
ZMK_SUBSCRIPTION(zmk_usb_resend_keys_after_wake, zmk_usb_conn_state_changed);
ZMK_SUBSCRIPTION(zmk_usb_resend_keys_after_wake, zmk_endpoint_changed);
