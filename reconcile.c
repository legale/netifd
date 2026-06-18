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

#define REC_DELAY_MS	1000
#define REC_PERIOD_MS	10000

static struct uloop_timeout rec_timer;
static enum reconcile_reason rec_reason;
static bool rec_pending;

static void
rec_run(void)
{
	rec_pending = false;

	/* Stage 1 is audit-only. Checks are added in separate small commits. */
	uloop_timeout_set(&rec_timer, REC_PERIOD_MS);
}

static void
rec_timeout_cb(struct uloop_timeout *t)
{
	rec_run();
}

void
reconcile_schedule(enum reconcile_reason reason)
{
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
	reconcile_schedule(REC_REASON_INIT);
}
