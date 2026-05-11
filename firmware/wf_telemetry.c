/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Waveform Labs
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "config.h"

#ifdef WAVEFORM_TELEMETRY

#include <string.h>
#include "wf_telemetry.h"
#include "system.h"     /* disable_irq_save / restore_irq */
#include "kernel.h"     /* current_tick */

/* Backing ring in BSS — zero-initialised at boot, no allocation. */
static struct wf_event wf_event_ring[WF_EVENT_RING_SIZE];
static volatile size_t wf_event_head;   /* next write index, [0, RING_SIZE) */
static volatile size_t wf_event_total;  /* total records ever written */

void wf_event_record(uint16_t sub, uint16_t evt,
                     uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    int irq = disable_irq_save();
    size_t slot = wf_event_head;
    struct wf_event *e = &wf_event_ring[slot];
    e->tick = (uint32_t)current_tick;
    e->subsystem = sub;
    e->event = evt;
    e->a = a;
    e->b = b;
    e->c = c;
    e->d = d;
    e->reserved = 0;
    wf_event_head = (slot + 1) % WF_EVENT_RING_SIZE;
    wf_event_total++;
    restore_irq(irq);
}

const struct wf_event *wf_event_snapshot(size_t *count_out, size_t *head_out)
{
    if (count_out)
        *count_out = wf_event_total;
    if (head_out)
        *head_out = wf_event_head;
    return wf_event_ring;
}

void wf_event_reset(void)
{
    int irq = disable_irq_save();
    memset(wf_event_ring, 0, sizeof(wf_event_ring));
    wf_event_head = 0;
    wf_event_total = 0;
    restore_irq(irq);
}

#endif /* WAVEFORM_TELEMETRY */
