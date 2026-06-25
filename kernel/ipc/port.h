/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, ASD Project Contributors
 * All rights reserved.
 */

#ifndef ASD_IPC_PORT_H
#define ASD_IPC_PORT_H

#include <stdint.h>
#include <stddef.h>
#include "ringbuf.h"
#include "../sched/sched.h"

/* ------------------------------------------------------------------------- */

#define PORT_NAME_LEN       64
#define PORT_MSG_MAX        4096
#define PORT_QUEUE_DEPTH    64
#define PORT_CREATE         0x01
#define PORT_OPEN           0x02

/* ------------------------------------------------------------------------- */

typedef struct {
    uint32_t type;     /* application-defined message type */
    uint32_t len;      /* payload bytes following this header */
    /* payload follows inline */
} msg_hdr_t;

/* ------------------------------------------------------------------------- */

#define PORT_SLOT_SIZE  (sizeof(msg_hdr_t) + 56)   /* 64 bytes per slot */

typedef struct {
    msg_hdr_t hdr;
    uint8_t   payload[56];
} port_slot_t;

/* For messages larger than 56 bytes, the payload contains a pointer to a heap-allocated buffer. */
#define PORT_INLINE_MAX  56

/* ------------------------------------------------------------------------- */

#define MAX_PORTS  1024

typedef struct port_obj {
    char       name[PORT_NAME_LEN];
    uint32_t   id;           /* handle value returned to userland */
    pid_t      owner_pid;
    uint32_t   lock;         /* spinlock */
    uint32_t   refcount;
    uint32_t   _pad;

    /* Tier 1 ring buffer backing the queue */
    uint8_t    ring_mem[sizeof(ringbuf_t) + PORT_SLOT_SIZE * PORT_QUEUE_DEPTH];
    ringbuf_t *ring;

    /* Process blocked waiting for a message */
    pcb_t     *waiter;
} port_obj_t;

/* ------------------------------------------------------------------------- */

void    port_subsystem_init(void);
port_t  port_open(const char *name, int flags, pid_t caller_pid);
void    port_close(port_t id);
int     port_send(port_t id, const void *msg, size_t len);
int     port_recv(port_t id, void *buf, size_t cap, size_t *len_out, int blocking);

#endif /* ASD_IPC_PORT_H */
