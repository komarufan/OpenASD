/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, ASD Project Contributors
 * All rights reserved.
 */

#include "port.h"
#include "../mm/mm.h"
#include <stddef.h>
#include <string.h>

static inline void spin_lock(uint32_t *l) {
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
}
static inline void spin_unlock(uint32_t *l) {
    __atomic_store_n(l, 0, __ATOMIC_RELEASE);
}

static port_obj_t  g_port_table[MAX_PORTS];
static uint32_t    g_port_lock;

void port_subsystem_init(void) {
    memset(g_port_table, 0, sizeof(g_port_table));
    g_port_lock = 0;
    for (uint32_t i = 0; i < MAX_PORTS; i++)
        g_port_table[i].id = 0; /* 0 = free slot */
}

static port_obj_t *port_find_by_name(const char *name) {
    for (uint32_t i = 0; i < MAX_PORTS; i++) {
        if (g_port_table[i].id != 0 &&
            strncmp(g_port_table[i].name, name, PORT_NAME_LEN) == 0)
            return &g_port_table[i];
    }
    return NULL;
}

static port_obj_t *port_find_by_id(port_t id) {
    if (id == 0 || id > MAX_PORTS) return NULL;
    port_obj_t *p = &g_port_table[id - 1];
    return (p->id == id) ? p : NULL;
}

port_t port_open(const char *name, int flags, pid_t caller_pid) {
    if (!name || !name[0]) return 0;

    spin_lock(&g_port_lock);

    port_obj_t *p = port_find_by_name(name);
    if (p) {
        /* Port already exists — attach to it */
        if (!(flags & PORT_OPEN)) { spin_unlock(&g_port_lock); return 0; }
        spin_lock(&p->lock);
        p->refcount++;
        spin_unlock(&p->lock);
        spin_unlock(&g_port_lock);
        return p->id;
    }

    if (!(flags & PORT_CREATE)) { spin_unlock(&g_port_lock); return 0; }

    /* Find a free slot */
    port_obj_t *slot = NULL;
    uint32_t    slot_id = 0;
    for (uint32_t i = 0; i < MAX_PORTS; i++) {
        if (g_port_table[i].id == 0) {
            slot    = &g_port_table[i];
            slot_id = i + 1;
            break;
        }
    }
    if (!slot) { spin_unlock(&g_port_lock); return 0; }

    memset(slot, 0, sizeof(*slot));
    strncpy(slot->name, name, PORT_NAME_LEN - 1);
    slot->id        = slot_id;
    slot->owner_pid = caller_pid;
    slot->refcount  = 1;
    slot->waiter    = NULL;

    /* Initialise ring buffer */
    slot->ring = (ringbuf_t *)slot->ring_mem;
    ringbuf_init(slot->ring, PORT_SLOT_SIZE, PORT_QUEUE_DEPTH);

    spin_unlock(&g_port_lock);
    return slot_id;
}

void port_close(port_t id) {
    port_obj_t *p = port_find_by_id(id);
    if (!p) return;

    spin_lock(&p->lock);
    if (p->refcount > 0) p->refcount--;
    int destroy = (p->refcount == 0);
    spin_unlock(&p->lock);

    if (destroy) {
        /* Wake any blocked receiver with an error */
        if (p->waiter) sched_wake(p->waiter);
        p->id = 0; /* mark free */
    }
}

int port_send(port_t id, const void *msg, size_t len) {
    if (len == 0 || len > PORT_MSG_MAX) return -1;
    port_obj_t *p = port_find_by_id(id);
    if (!p) return -1;

    port_slot_t slot;
    memset(&slot, 0, sizeof(slot));
    slot.hdr.len = (uint32_t)len;

    if (len <= PORT_INLINE_MAX) {
        /* Inline path */
        memcpy(slot.payload, (const uint8_t *)msg + sizeof(msg_hdr_t),
               len - sizeof(msg_hdr_t) < PORT_INLINE_MAX ?
                   len - sizeof(msg_hdr_t) : PORT_INLINE_MAX);
        const msg_hdr_t *mh = (const msg_hdr_t *)msg;
        slot.hdr.type = mh->type;
    } else {
        /* Out-of-line: allocate kernel buffer, store pointer in payload */
        void *buf = kmalloc(len);
        if (!buf) return -1;
        memcpy(buf, msg, len);
        uint64_t ptr = (uint64_t)(uintptr_t)buf;
        memcpy(slot.payload, &ptr, sizeof(ptr));
        const msg_hdr_t *mh = (const msg_hdr_t *)msg;
        slot.hdr.type = mh->type;
    }

    /* Spin-retry on full queue (bounded) */
    int tries = 1000;
    while (tries-- > 0) {
        if (ringbuf_push(p->ring, &slot, sizeof(slot)) == 0) {
            /* Wake receiver if blocked */
            if (p->waiter) {
                sched_wake(p->waiter);
                p->waiter = NULL;
            }
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -1; /* queue full */
}

int port_recv(port_t id, void *buf, size_t cap, size_t *len_out, int blocking) {
    if (!buf || !len_out || cap < sizeof(port_slot_t)) return -1;
    port_obj_t *p = port_find_by_id(id);
    if (!p) return -1;

    port_slot_t slot;

    /* Block until a message is available */
    while (ringbuf_pop(p->ring, &slot, sizeof(slot)) != 0) {
        if (!blocking) return -2;
        /* Register as waiter and go to sleep */
        pcb_t *cur = sched_current();
        p->waiter  = cur;
        cur->state = PROC_WAIT;
        sched_yield();
        /* Woken up — retry */
    }

    size_t payload_len = slot.hdr.len;
    if (payload_len <= PORT_INLINE_MAX) {
        /* Inline */
        msg_hdr_t *mh = (msg_hdr_t *)buf;
        mh->type = slot.hdr.type;
        mh->len  = (uint32_t)payload_len;
        size_t copy = payload_len < cap - sizeof(msg_hdr_t) ?
                      payload_len : cap - sizeof(msg_hdr_t);
        memcpy((uint8_t *)buf + sizeof(msg_hdr_t), slot.payload, copy);
        *len_out = sizeof(msg_hdr_t) + copy;
    } else {
        /* Out-of-line */
        uint64_t ptr;
        memcpy(&ptr, slot.payload, sizeof(ptr));
        void *kbuf = (void *)(uintptr_t)ptr;
        size_t copy = payload_len < cap ? payload_len : cap;
        memcpy(buf, kbuf, copy);
        *len_out = copy;
        kfree(kbuf);
    }
    return 0;
}
