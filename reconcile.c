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

#include <uci.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REC_DEFAULT_DELAY_SEC		1
#define REC_DEFAULT_PERIOD_SEC		10
#define REC_DEFAULT_SETUP_WARN_SEC	45
#define REC_DEFAULT_SETUP_RESTART_SEC	60
#define REC_DEFAULT_SETUP_CONFIRM_SEC	5
#define REC_DEFAULT_ACTION_BACKOFF_SEC	15
#define REC_DEFAULT_ACTION_SUPPRESS_SEC	60
#define REC_DEFAULT_FAIL_LIMIT		3

#ifndef REC_ENABLE_ACTIONS
#define REC_ENABLE_ACTIONS		1
#endif

#ifndef REC_ENABLE_WIRELESS_CHECK
#if REC_ENABLE_WIRELESS_RECOVER
#define REC_ENABLE_WIRELESS_CHECK	1
#else
#define REC_ENABLE_WIRELESS_CHECK	REC_ENABLE_ACTIONS
#endif
#endif

#ifndef REC_ENABLE_SETUP_RESTART
#define REC_ENABLE_SETUP_RESTART	1
#endif

static struct uloop_timeout rec_timer;
static enum reconcile_reason rec_reason;
static enum reconcile_reason rec_trigger;
static bool rec_pending;
static bool rec_inited;
static bool rec_need_wireless_check;

static const struct reconcile_config rec_default_cfg = {
	.enabled = true,
	.actions = !!REC_ENABLE_ACTIONS,
	.setup_restart = !!REC_ENABLE_SETUP_RESTART,
	.wireless_check = !!REC_ENABLE_WIRELESS_CHECK,
	.delay_sec = REC_DEFAULT_DELAY_SEC,
	.period_sec = REC_DEFAULT_PERIOD_SEC,
	.setup_warn_sec = REC_DEFAULT_SETUP_WARN_SEC,
	.setup_restart_sec = REC_DEFAULT_SETUP_RESTART_SEC,
	.setup_confirm_sec = REC_DEFAULT_SETUP_CONFIRM_SEC,
	.action_backoff_sec = REC_DEFAULT_ACTION_BACKOFF_SEC,
	.action_suppress_sec = REC_DEFAULT_ACTION_SUPPRESS_SEC,
	.fail_limit = REC_DEFAULT_FAIL_LIMIT,
};

static struct reconcile_config rec_cfg = { 0 };

static const char *rec_last_action;
static const char *rec_last_reason;
static enum reconcile_reason rec_last_trigger;
static unsigned int rec_run_cnt;
static unsigned int rec_event_cnt;
static unsigned int rec_action_cnt;
static unsigned int rec_ifup_cnt;
static unsigned int rec_restart_cnt;
static unsigned int rec_restart_failed_cnt;
static unsigned int rec_suppress_cnt;
static unsigned int rec_blocked_cnt;
static unsigned int rec_confirm_cnt;
static unsigned int rec_wireless_check_cnt;
static unsigned int rec_wireless_event_cnt;
static unsigned int rec_wireless_recover_cnt;
static unsigned int rec_wireless_confirm_cnt;
static unsigned int rec_wireless_suppress_cnt;
static unsigned int rec_wireless_blocked_cnt;
static unsigned int rec_wireless_last_count;
static char rec_wireless_radio[32];
static char rec_wireless_action[32];
static char rec_wireless_reason[64];
static char rec_wireless_section[64];
static char rec_wireless_ifname[IFNAMSIZ];


static void
rec_str_set(char *dst, size_t len, const char *src)
{
	snprintf(dst, len, "%s", src ? src : "none");
}

static void
rec_event_count(const char *action, const char *reason)
{
	rec_event_cnt++;
	rec_last_action = action;
	rec_last_reason = reason;
	rec_last_trigger = rec_trigger;

	if (!strcmp(action, "none")) {
		if (!strcmp(reason, "setup_confirm"))
			rec_confirm_cnt++;
		return;
	}

	if (!strcmp(action, "suppress")) {
		rec_suppress_cnt++;
		return;
	}

	if (!strcmp(action, "blocked")) {
		rec_blocked_cnt++;
		return;
	}

	rec_action_cnt++;
	if (!strcmp(action, "ifup"))
		rec_ifup_cnt++;
	else if (!strcmp(action, "restart"))
		rec_restart_cnt++;
	else if (!strcmp(action, "restart_failed"))
		rec_restart_failed_cnt++;
}

static void
rec_config_reset(void)
{
	rec_cfg = rec_default_cfg;
}

static const char *
rec_uci_opt(struct uci_section *s, const char *name)
{
	if (!s)
		return NULL;

	return uci_lookup_option_string(s->package->ctx, s, name);
}

static bool
rec_parse_bool(const char *val, bool def)
{
	if (!val)
		return def;

	if (!strcmp(val, "1") || !strcmp(val, "true") ||
	    !strcmp(val, "on") || !strcmp(val, "yes") ||
	    !strcmp(val, "enabled"))
		return true;

	if (!strcmp(val, "0") || !strcmp(val, "false") ||
	    !strcmp(val, "off") || !strcmp(val, "no") ||
	    !strcmp(val, "disabled"))
		return false;

	return def;
}

static unsigned int
rec_parse_uint(const char *val, unsigned int def, unsigned int min)
{
	char *end;
	unsigned long ret;

	if (!val)
		return def;

	errno = 0;
	ret = strtoul(val, &end, 0);
	if (errno || end == val || *end)
		return def;

	if (ret < min)
		return min;

	if (ret > UINT_MAX)
		return def;

	return ret;
}

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
	rec_event_count(action, reason);
	netifd_log_message(L_NOTICE,
		"reconcile: iface=%s want=up state=%s dev=%s l3=%s ifindex=%d age=%u action=%s reason=%s trigger=%s\n",
		iface->name, rec_state_name(iface->state), dev, l3, ifindex, age,
		action, reason, rec_reason_name(rec_trigger));
}

static void
rec_iface_action_save(struct interface *iface, const char *action,
		      const char *reason, time_t now)
{
	iface->rec_last_action = now;
	iface->rec_last_action_name = action;
	iface->rec_last_reason = reason;
	rec_last_action = action;
	rec_last_reason = reason;
	rec_last_trigger = rec_trigger;
}

static void
rec_iface_action_reset(struct interface *iface)
{
	iface->rec_fail_cnt = 0;
	iface->rec_suppress_until = 0;
	iface->rec_setup_time = 0;
	iface->rec_setup_confirm = 0;
	iface->rec_last_reason = NULL;
	iface->rec_last_action_name = NULL;
}

static bool
rec_iface_action_allowed(struct interface *iface, const char *dev,
			 const char *l3, const char *reason, time_t now)
{
	if (iface->rec_suppress_until) {
		if (now < iface->rec_suppress_until) {
			rec_iface_log(iface, dev, l3, "suppress",
				      "action_suppressed", 0, 0);
			return false;
		}

		iface->rec_suppress_until = 0;
		iface->rec_fail_cnt = 0;
	}

	if (iface->rec_last_action &&
	    now < iface->rec_last_action + rec_cfg.action_backoff_sec) {
		rec_iface_log(iface, dev, l3, "suppress", "action_backoff",
			      0, 0);
		return false;
	}

	if (iface->rec_fail_cnt >= rec_cfg.fail_limit) {
		iface->rec_suppress_until = now + rec_cfg.action_suppress_sec;
		rec_iface_action_save(iface, "blocked", reason, now);
		rec_iface_log(iface, dev, l3, "blocked", "action_fail_limit",
			      0, 0);
		return false;
	}

	return true;
}

static void
rec_iface_action_ifup(struct interface *iface, const char *dev,
		     const char *l3, const char *reason)
{
	time_t now;

	if (!rec_cfg.actions) {
		rec_iface_log(iface, dev, l3, "none", reason, 0, 0);
		return;
	}

	now = system_get_rtime();
	if (!rec_iface_action_allowed(iface, dev, l3, reason, now))
		return;

	iface->rec_fail_cnt++;
	rec_iface_action_save(iface, "ifup", reason, now);
	rec_iface_log(iface, dev, l3, "ifup", reason, 0, 0);
	interface_set_up(iface);
}

static void
rec_iface_action_restart(struct interface *iface, const char *dev,
			 const char *l3, const char *reason, unsigned int age)
{
	time_t now;
	int ret;

	if (!rec_cfg.setup_restart) {
		rec_iface_log(iface, dev, l3, "none", reason, 0, age);
		return;
	}

	now = system_get_rtime();
	if (iface->rec_setup_time != iface->setup_time) {
		iface->rec_setup_time = iface->setup_time;
		iface->rec_setup_confirm = now;
		rec_iface_log(iface, dev, l3, "none", "setup_confirm", 0, age);
		return;
	}

	if (!iface->rec_setup_confirm)
		iface->rec_setup_confirm = now;

	if (now < iface->rec_setup_confirm + rec_cfg.setup_confirm_sec) {
		rec_iface_log(iface, dev, l3, "none", "setup_confirm", 0, age);
		return;
	}

	if (!rec_iface_action_allowed(iface, dev, l3, reason, now))
		return;

	iface->rec_setup_time = 0;
	iface->rec_setup_confirm = 0;
	ret = interface_restart(iface);
	if (ret) {
		iface->rec_fail_cnt++;
		rec_iface_action_save(iface, "restart_failed", reason, now);
		rec_iface_log(iface, dev, l3, "restart_failed", reason, 0, age);
		return;
	}

	iface->rec_fail_cnt++;
	rec_iface_action_save(iface, "restart", reason, now);
	rec_iface_log(iface, dev, l3, "restart", reason, 0, age);
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
		iface->rec_setup_time = 0;
		iface->rec_setup_confirm = 0;
		if (iface->state == IFS_DOWN)
			rec_iface_action_ifup(iface, dev_name, l3_name, "not_up");
		else
			rec_iface_log(iface, dev_name, l3_name, "none",
				      "not_up", 0, 0);
		return;
	}

	if (iface->state == IFS_SETUP) {
		if (!iface->setup_time) {
			iface->rec_setup_time = 0;
			iface->rec_setup_confirm = 0;
			return;
		}

		now = system_get_rtime();
		if (now > iface->setup_time)
			age = now - iface->setup_time;

		if (age >= rec_cfg.setup_restart_sec)
			rec_iface_action_restart(iface, dev_name, l3_name,
					       "setup_timeout", age);
		else if (age >= rec_cfg.setup_warn_sec)
			rec_iface_log(iface, dev_name, l3_name, "none",
				      "setup_timeout", 0, age);
		else {
			iface->rec_setup_time = 0;
			iface->rec_setup_confirm = 0;
		}
		return;
	}

	iface->rec_setup_time = 0;
	iface->rec_setup_confirm = 0;

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

	if (!system_if_resolve(dev)) {
		rec_iface_log(iface, dev_name, l3_name, "none", "missing_ifindex", 0, 0);
		return;
	}

	rec_iface_action_reset(iface);
}

static bool
rec_reason_needs_wireless_check(enum reconcile_reason reason)
{
	switch (reason) {
	case REC_REASON_INIT:
	case REC_REASON_PERIODIC:
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

	if (!rec_cfg.wireless_check)
		return;

	netifd_log_message(L_DEBUG,
		"reconcile: action=wireless_check trigger=%s\n",
		rec_reason_name(rec_trigger));
	rec_wireless_check_cnt++;
	rec_last_action = "wireless_check";
	rec_last_reason = "scheduled";
	rec_last_trigger = rec_trigger;
	netifd_ucode_check_network_enabled();
}

static void
rec_run(void)
{
	struct interface *iface;

	rec_trigger = rec_reason;
	rec_pending = false;

	if (!rec_cfg.enabled)
		return;

	rec_run_cnt++;

	if (rec_reason_needs_wireless_check(rec_trigger))
		rec_need_wireless_check = true;
	rec_wireless_check();

	vlist_for_each_element(&interfaces, iface, node)
		rec_iface_check(iface);

	if (rec_pending)
		return;

	rec_reason = REC_REASON_PERIODIC;
	uloop_timeout_set(&rec_timer, (rec_cfg.period_sec * 1000));
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
	if (!rec_inited || !rec_cfg.enabled)
		return;

	if (rec_reason_needs_wireless_check(reason))
		rec_need_wireless_check = true;

	rec_reason = reason;

	if (rec_pending)
		return;

	rec_pending = true;
	uloop_timeout_set(&rec_timer, (rec_cfg.delay_sec * 1000));
}


void
reconcile_wireless_event(const char *radio, const char *action,
			 const char *reason, const char *section,
			 const char *ifname, unsigned int count)
{
	rec_wireless_event_cnt++;
	rec_wireless_last_count = count;
	rec_str_set(rec_wireless_radio, sizeof(rec_wireless_radio), radio);
	rec_str_set(rec_wireless_action, sizeof(rec_wireless_action), action);
	rec_str_set(rec_wireless_reason, sizeof(rec_wireless_reason), reason);
	rec_str_set(rec_wireless_section, sizeof(rec_wireless_section), section);
	rec_str_set(rec_wireless_ifname, sizeof(rec_wireless_ifname), ifname);

	if (!strcmp(action ? action : "", "teardown_setup"))
		rec_wireless_recover_cnt++;
	else if (!strcmp(action ? action : "", "suppress"))
		rec_wireless_suppress_cnt++;
	else if (!strcmp(action ? action : "", "blocked"))
		rec_wireless_blocked_cnt++;
	else if (!strcmp(reason ? reason : "", "recover_confirm"))
		rec_wireless_confirm_cnt++;

	rec_last_action = rec_wireless_action;
	rec_last_reason = rec_wireless_reason;
}

bool
reconcile_wireless_recover_enabled(void)
{
	if (!rec_cfg.delay_sec)
		return rec_default_cfg.enabled && rec_default_cfg.wireless_check;

	return rec_cfg.enabled && rec_cfg.wireless_check;
}

void
reconcile_config_load(struct uci_section *globals)
{
	const char *wireless;

	rec_config_reset();

	rec_cfg.enabled = rec_parse_bool(rec_uci_opt(globals, "reconcile_enabled"),
		rec_cfg.enabled);
	rec_cfg.actions = rec_parse_bool(rec_uci_opt(globals, "reconcile_actions"),
		rec_cfg.actions);
	rec_cfg.setup_restart = rec_parse_bool(rec_uci_opt(globals, "reconcile_setup_restart"),
		rec_cfg.setup_restart);

	wireless = rec_uci_opt(globals, "reconcile_wireless_recover");
	if (!wireless)
		wireless = rec_uci_opt(globals, "reconcile_wireless_check");
	rec_cfg.wireless_check = rec_parse_bool(wireless, rec_cfg.wireless_check);

	rec_cfg.delay_sec = rec_parse_uint(rec_uci_opt(globals, "reconcile_delay_sec"),
		rec_cfg.delay_sec, 1);
	rec_cfg.period_sec = rec_parse_uint(rec_uci_opt(globals, "reconcile_period_sec"),
		rec_cfg.period_sec, 1);
	rec_cfg.setup_warn_sec = rec_parse_uint(rec_uci_opt(globals, "reconcile_setup_warn_sec"),
		rec_cfg.setup_warn_sec, 1);
	rec_cfg.setup_restart_sec = rec_parse_uint(rec_uci_opt(globals, "reconcile_setup_restart_sec"),
		rec_cfg.setup_restart_sec, 1);
	rec_cfg.setup_confirm_sec = rec_parse_uint(rec_uci_opt(globals, "reconcile_setup_confirm_sec"),
		rec_cfg.setup_confirm_sec, 1);
	rec_cfg.action_backoff_sec = rec_parse_uint(rec_uci_opt(globals, "reconcile_action_backoff_sec"),
		rec_cfg.action_backoff_sec, 1);
	rec_cfg.action_suppress_sec = rec_parse_uint(rec_uci_opt(globals, "reconcile_action_suppress_sec"),
		rec_cfg.action_suppress_sec, 1);
	rec_cfg.fail_limit = rec_parse_uint(rec_uci_opt(globals, "reconcile_fail_limit"),
		rec_cfg.fail_limit, 1);

	netifd_ucode_set_reconcile_wireless_recover(
		reconcile_wireless_recover_enabled());

	if (rec_inited && !rec_cfg.enabled) {
		uloop_timeout_cancel(&rec_timer);
		rec_pending = false;
	}
}

void
reconcile_dump_status(struct blob_buf *b)
{
	blobmsg_add_u8(b, "enabled", rec_cfg.enabled);
	blobmsg_add_u8(b, "actions", rec_cfg.actions);
	blobmsg_add_u8(b, "setup_restart", rec_cfg.setup_restart);
	blobmsg_add_u8(b, "wireless_check", rec_cfg.wireless_check);
	blobmsg_add_u8(b, "pending", rec_pending);
	blobmsg_add_u8(b, "wireless_check_pending", rec_need_wireless_check);
	blobmsg_add_string(b, "last_trigger", rec_reason_name(rec_last_trigger));
	blobmsg_add_string(b, "last_action", rec_last_action ? rec_last_action : "none");
	blobmsg_add_string(b, "last_reason", rec_last_reason ? rec_last_reason : "none");
	blobmsg_add_u32(b, "run_count", rec_run_cnt);
	blobmsg_add_u32(b, "event_count", rec_event_cnt);
	blobmsg_add_u32(b, "action_count", rec_action_cnt);
	blobmsg_add_u32(b, "ifup_count", rec_ifup_cnt);
	blobmsg_add_u32(b, "restart_count", rec_restart_cnt);
	blobmsg_add_u32(b, "restart_failed_count", rec_restart_failed_cnt);
	blobmsg_add_u32(b, "suppress_count", rec_suppress_cnt);
	blobmsg_add_u32(b, "blocked_count", rec_blocked_cnt);
	blobmsg_add_u32(b, "confirm_count", rec_confirm_cnt);
	blobmsg_add_u32(b, "wireless_check_count", rec_wireless_check_cnt);
	blobmsg_add_u32(b, "wireless_event_count", rec_wireless_event_cnt);
	blobmsg_add_u32(b, "wireless_recover_count", rec_wireless_recover_cnt);
	blobmsg_add_u32(b, "wireless_confirm_count", rec_wireless_confirm_cnt);
	blobmsg_add_u32(b, "wireless_suppress_count", rec_wireless_suppress_cnt);
	blobmsg_add_u32(b, "wireless_blocked_count", rec_wireless_blocked_cnt);
	blobmsg_add_string(b, "last_wireless_radio", rec_wireless_radio[0] ? rec_wireless_radio : "none");
	blobmsg_add_string(b, "last_wireless_action", rec_wireless_action[0] ? rec_wireless_action : "none");
	blobmsg_add_string(b, "last_wireless_reason", rec_wireless_reason[0] ? rec_wireless_reason : "none");
	blobmsg_add_string(b, "last_wireless_section", rec_wireless_section[0] ? rec_wireless_section : "none");
	blobmsg_add_string(b, "last_wireless_ifname", rec_wireless_ifname[0] ? rec_wireless_ifname : "none");
	blobmsg_add_u32(b, "last_wireless_count", rec_wireless_last_count);
	blobmsg_add_u32(b, "delay_sec", rec_cfg.delay_sec);
	blobmsg_add_u32(b, "period_sec", rec_cfg.period_sec);
	blobmsg_add_u32(b, "setup_warn_sec", rec_cfg.setup_warn_sec);
	blobmsg_add_u32(b, "setup_restart_sec", rec_cfg.setup_restart_sec);
	blobmsg_add_u32(b, "setup_confirm_sec", rec_cfg.setup_confirm_sec);
	blobmsg_add_u32(b, "action_backoff_sec", rec_cfg.action_backoff_sec);
	blobmsg_add_u32(b, "action_suppress_sec", rec_cfg.action_suppress_sec);
	blobmsg_add_u32(b, "fail_limit", rec_cfg.fail_limit);
}

void
reconcile_init(void)
{
	if (!rec_cfg.delay_sec)
		rec_config_reset();

	rec_timer.cb = rec_timeout_cb;
	rec_inited = true;
	reconcile_schedule(REC_REASON_INIT);
}
