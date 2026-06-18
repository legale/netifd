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

enum reconcile_reason {
	REC_REASON_INIT,
	REC_REASON_PERIODIC,
	REC_REASON_IFACE_EVENT,
};

void reconcile_init(void);
void reconcile_schedule(enum reconcile_reason reason);

#endif
