/*
 * netifd - network interface daemon
 * Copyright (C) 2026 Isaev Ruslan <legale.legale@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation
 */
#ifndef __NETIFD_RECONCILE_H
#define __NETIFD_RECONCILE_H

#include <stdbool.h>
#include <libubox/blobmsg.h>

struct uci_section;

enum reconcile_reason {
	REC_REASON_INIT,
	REC_REASON_PERIODIC,
	REC_REASON_CONFIG,
	REC_REASON_IFACE_EVENT,
	REC_REASON_IFACE_ACTION,
	REC_REASON_DEVICE_EVENT,
	REC_REASON_HOTPLUG,
	REC_REASON_WIRELESS_CHECK,
};

struct reconcile_config {
	bool enabled;
	bool actions;
	bool setup_restart;
	bool wireless_check;
	unsigned int delay_sec;
	unsigned int period_sec;
	unsigned int setup_warn_sec;
	unsigned int setup_restart_sec;
	unsigned int setup_confirm_sec;
	unsigned int action_backoff_sec;
	unsigned int action_suppress_sec;
	unsigned int fail_limit;
};

void reconcile_init(void);
void reconcile_schedule(enum reconcile_reason reason);
void reconcile_config_load(struct uci_section *globals);
void reconcile_dump_status(struct blob_buf *b);
bool reconcile_wireless_recover_enabled(void);

#endif
