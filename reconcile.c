/*
 * netifd - network interface daemon
 * Copyright (C) 2026 Isaev Ruslan <legale.legale@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation
 */
#include "netifd.h"
#include "reconcile.h"
#include "config.h"
#include "device.h"
#include "interface.h"
#include "system.h"
#include "ucode.h"

#define REC_DELAY_MS	1000
#define REC_PERIOD_MS	10000
#define REC_SETUP_WARN_SEC	45

static struct uloop_timeout rec_timer;
static enum reconcile_reason rec_reason;
static enum reconcile_reason rec_trigger;
static bool rec_pending;
static bool rec_inited;
static bool rec_need_wireless_check;

static const char *
rec_reason_name(enum reconcile_reason reason)
{
	switch (reason) {
	case REC_REASON_INIT:
		return "init";
	case REC_REASON_PERIODIC:
		return "periodic";
	case REC_REASON_CONFIG:
		return "config";
	case REC_REASON_IFACE_EVENT:
		return "iface_event";
	case REC_REASON_IFACE_ACTION:
		return "iface_action";
	case REC_REASON_DEVICE_EVENT:
		return "device_event";
	case REC_REASON_HOTPLUG:
		return "hotplug";
	case REC_REASON_WIRELESS_CHECK:
		return "wireless_check";
	default:
		return "unknown";
	}
}

static const char *
rec_state_name(enum interface_state state)
{
	switch (state) {
	case IFS_SETUP:
		return "setup";
	case IFS_UP:
		return "up";
	case IFS_TEARDOWN:
		return "teardown";
	case IFS_DOWN:
		return "down";
	default:
		return "unknown";
	}
}

static bool
rec_iface_want_up(struct interface *iface)
{
	bool link;

	if (config_init)
		return false;

	if (iface->config_state != IFC_NORMAL)
		return false;

	if (!iface->autostart || !iface->enabled || !iface->available)
		return false;

	link = iface->link_state || iface->force_link ||
	       iface->carrier_loss_timer.pending;

	return link;
}

static void
rec_iface_log(struct interface *iface, const char *dev, const char *l3,
	      const char *action, const char *reason, int ifindex,
	      unsigned int age)
{
	netifd_log_message(L_NOTICE,
		"reconcile: iface=%s want=up state=%s dev=%s l3=%s ifindex=%d age=%u action=%s reason=%s trigger=%s\n",
		iface->name, rec_state_name(iface->state), dev, l3, ifindex, age,
		action, reason, rec_reason_name(rec_trigger));
}

static void
rec_iface_check(struct interface *iface)
{
	struct device *dev = iface->main_dev.dev;
	struct device *l3 = iface->l3_dev.dev;
	const char *dev_name = dev ? dev->ifname : "(null)";
	const char *l3_name = l3 ? l3->ifname : "(null)";
	unsigned int age = 0;
	time_t now;

	if (!rec_iface_want_up(iface))
		return;

	if (iface->state == IFS_TEARDOWN)
		return;

	if (iface->main_dev.claimed && dev && !dev->present) {
		rec_iface_log(iface, dev_name, l3_name, "none", "claimed_dev_missing", 0, 0);
		return;
	}

	if (iface->l3_dev.claimed && l3 && !l3->present) {
		rec_iface_log(iface, dev_name, l3_name, "none", "claimed_l3_missing", 0, 0);
		return;
	}

	if (iface->state != IFS_UP && iface->state != IFS_SETUP) {
		rec_iface_log(iface, dev_name, l3_name,
			      iface->state == IFS_DOWN ? "ifup" : "none",
			      "not_up", 0, 0);
		if (iface->state == IFS_DOWN)
			interface_set_up(iface);
		return;
	}

	if (iface->state == IFS_SETUP) {
		if (!iface->setup_time)
			return;

		now = system_get_rtime();
		if (now > iface->setup_time)
			age = now - iface->setup_time;

		if (age >= REC_SETUP_WARN_SEC)
			rec_iface_log(iface, dev_name, l3_name, "none", "setup_timeout",
				      0, age);
		return;
	}

	if (!l3) {
		rec_iface_log(iface, dev_name, l3_name, "none", "missing_l3", 0, 0);
		return;
	}

	if (!dev)
		return;

	if (!dev->present) {
		rec_iface_log(iface, dev_name, l3_name, "none", "missing_dev", 0, 0);
		return;
	}

	if (!system_if_resolve(dev))
		rec_iface_log(iface, dev_name, l3_name, "none", "missing_ifindex", 0, 0);
}

static bool
rec_reason_needs_wireless_check(enum reconcile_reason reason)
{
	switch (reason) {
	case REC_REASON_INIT:
	case REC_REASON_CONFIG:
	case REC_REASON_DEVICE_EVENT:
	case REC_REASON_HOTPLUG:
		return true;
	default:
		return false;
	}
}

static void
rec_wireless_check(void)
{
	if (!rec_need_wireless_check)
		return;

	rec_need_wireless_check = false;
	netifd_log_message(L_NOTICE,
		"reconcile: action=wireless_check trigger=%s\n",
		rec_reason_name(rec_trigger));
	netifd_ucode_check_network_enabled();
}

static void
rec_run(void)
{
	struct interface *iface;

	rec_trigger = rec_reason;
	rec_pending = false;

	rec_wireless_check();

	vlist_for_each_element(&interfaces, iface, node)
		rec_iface_check(iface);

	if (rec_pending)
		return;

	rec_reason = REC_REASON_PERIODIC;
	uloop_timeout_set(&rec_timer, REC_PERIOD_MS);
}

static void
rec_timeout_cb(struct uloop_timeout *t)
{
	(void)t;
	rec_run();
}

void
reconcile_schedule(enum reconcile_reason reason)
{
	if (!rec_inited)
		return;

	if (rec_reason_needs_wireless_check(reason))
		rec_need_wireless_check = true;

	rec_reason = reason;

	if (rec_pending)
		return;

	rec_pending = true;
	uloop_timeout_set(&rec_timer, REC_DELAY_MS);
}

void
reconcile_init(void)
{
	rec_timer.cb = rec_timeout_cb;
	rec_inited = true;
	reconcile_schedule(REC_REASON_INIT);
}
