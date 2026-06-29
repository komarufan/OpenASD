/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, ASD Project Contributors
 * All rights reserved.
 */

#ifndef ASD_IPC_RINGBUF_H
#define ASD_IPC_RINGBUF_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------------------- */

#define RINGBUF_MAGIC  0x52494E4742554631ULL  /* "RINGBUF1" */
#define RINGBUF_VERSION 1

typedef struct ringbuf_t {
    uint64_t magic;        /*  0: RINGBUF_MAGIC                          */
    uint32_t version;      /*  8: RINGBUF_VERSION                        */
    uint32_t slot_size;    /* 12: bytes per slot (power of 2, >= 8)      */
    uint32_t slot_count;   /* 16: number of slots (power of 2, >= 2)     */
    uint32_t _pad;         /* 20                                         */
    uint64_t write_idx;    /* 24: monotonic write index (producer)       */
    uint64_t read_idx;     /* 32: monotonic read index (consumer)        */
    uint8_t  _reserved[24];/* 40                                         */
    /* 64: data[] follows (slot_count * slot_size bytes)                 */
} ringbuf_t;

/* ------------------------------------------------------------------------- */

static inline int is_pow2(uint32_t n) {
    return n && !(n & (n - 1));
}

/* Total bytes required for a ring buffer with given parameters */
static inline size_t ringbuf_required_size(uint32_t slot_size,
                                           uint32_t slot_count)
{
    return sizeof(ringbuf_t) + (size_t)slot_size * slot_count;
}

/* Pointer to the data region of a ring buffer */
static inline void *ringbuf_data(ringbuf_t *rb) {
    return (uint8_t *)rb + sizeof(ringbuf_t);
}

/* ------------------------------------------------------------------------- */

static inline int
ringbuf_init(void *mem, uint32_t slot_size, uint32_t slot_count)
{
    if (!mem) return -1;
    /* slot_size must be a power of two >= 1.  (Byte-stream pipes use 1-byte
     * slots; data is moved with byte memcpy so no alignment is required.) */
    if (!is_pow2(slot_size) || slot_size < 1)  return -1;
    if (!is_pow2(slot_count) || slot_count < 2) return -1;

    ringbuf_t *rb = (ringbuf_t *)mem;
    rb->magic      = RINGBUF_MAGIC;
    rb->version    = RINGBUF_VERSION;
    rb->slot_size  = slot_size;
    rb->slot_count = slot_count;
    rb->_pad       = 0;

    __atomic_store_n(&rb->write_idx, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&rb->read_idx,  0, __ATOMIC_RELAXED);
    memset(rb->_reserved, 0, sizeof(rb->_reserved));
    memset(ringbuf_data(rb), 0, (size_t)slot_size * slot_count);
    return 0;
}

/* ------------------------------------------------------------------------- */

static inline int ringbuf_valid(const ringbuf_t *rb) {
    return rb &&
           rb->magic   == RINGBUF_MAGIC &&
           rb->version == RINGBUF_VERSION &&
           is_pow2(rb->slot_size)  && rb->slot_size  >= 8 &&
           is_pow2(rb->slot_count) && rb->slot_count >= 2;
}

/* ------------------------------------------------------------------------- */

static inline int
ringbuf_push(ringbuf_t *rb, const void *data, uint32_t size)
{
    uint64_t widx = __atomic_load_n(&rb->write_idx, __ATOMIC_RELAXED);
    uint64_t ridx = __atomic_load_n(&rb->read_idx,  __ATOMIC_ACQUIRE);

    if (widx - ridx == rb->slot_count) return -1; /* full */
    if (size > rb->slot_size) return -1;

    void *slot = (uint8_t *)ringbuf_data(rb) +
                 (widx & (uint64_t)(rb->slot_count - 1)) * rb->slot_size;
    memcpy(slot, data, size);

    __atomic_store_n(&rb->write_idx, widx + 1, __ATOMIC_RELEASE);
    return 0;
}

/* ------------------------------------------------------------------------- */

static inline uint32_t
ringbuf_push_batch(ringbuf_t *rb, const void *items,
                   uint32_t item_size, uint32_t n)
{
    uint64_t widx  = __atomic_load_n(&rb->write_idx, __ATOMIC_RELAXED);
    uint64_t ridx  = __atomic_load_n(&rb->read_idx,  __ATOMIC_ACQUIRE);
    uint32_t avail = rb->slot_count - (uint32_t)(widx - ridx);
    if (avail < n) n = avail;
    if (item_size > rb->slot_size) return 0;

    const uint8_t *src = (const uint8_t *)items;
    for (uint32_t i = 0; i < n; i++) {
        void *slot = (uint8_t *)ringbuf_data(rb) +
                     ((widx + i) & (uint64_t)(rb->slot_count - 1)) * rb->slot_size;
        memcpy(slot, src + (size_t)i * item_size, item_size);
    }
    __atomic_store_n(&rb->write_idx, widx + n, __ATOMIC_RELEASE);
    return n;
}

/* ------------------------------------------------------------------------- */

static inline int
ringbuf_pop(ringbuf_t *rb, void *buf, uint32_t size)
{
    uint64_t ridx = __atomic_load_n(&rb->read_idx,  __ATOMIC_RELAXED);
    uint64_t widx = __atomic_load_n(&rb->write_idx, __ATOMIC_ACQUIRE);

    if (ridx == widx) return -1; /* empty */
    if (size > rb->slot_size) return -1;

    const void *slot = (const uint8_t *)ringbuf_data(rb) +
                       (ridx & (uint64_t)(rb->slot_count - 1)) * rb->slot_size;
    memcpy(buf, slot, size);

    __atomic_store_n(&rb->read_idx, ridx + 1, __ATOMIC_RELEASE);
    return 0;
}

/* ------------------------------------------------------------------------- */

static inline uint32_t
ringbuf_pop_batch(ringbuf_t *rb, void *buf,
                  uint32_t item_size, uint32_t n)
{
    uint64_t ridx  = __atomic_load_n(&rb->read_idx,  __ATOMIC_RELAXED);
    uint64_t widx  = __atomic_load_n(&rb->write_idx, __ATOMIC_ACQUIRE);
    uint32_t avail = (uint32_t)(widx - ridx);
    if (avail < n) n = avail;
    if (item_size > rb->slot_size) return 0;

    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0; i < n; i++) {
        const void *slot = (const uint8_t *)ringbuf_data(rb) +
            ((ridx + i) & (uint64_t)(rb->slot_count - 1)) * rb->slot_size;
        memcpy(dst + (size_t)i * item_size, slot, item_size);
    }
    __atomic_store_n(&rb->read_idx, ridx + n, __ATOMIC_RELEASE);
    return n;
}

/* ------------------------------------------------------------------------- */

static inline uint32_t ringbuf_used(const ringbuf_t *rb) {
    uint64_t w = __atomic_load_n(&rb->write_idx, __ATOMIC_RELAXED);
    uint64_t r = __atomic_load_n(&rb->read_idx,  __ATOMIC_RELAXED);
    uint32_t d = (uint32_t)(w - r);
    return (d < rb->slot_count) ? d : rb->slot_count;
}

static inline uint32_t ringbuf_free(const ringbuf_t *rb) {
    return rb->slot_count - ringbuf_used(rb);
}

static inline int ringbuf_empty(const ringbuf_t *rb) {
    return __atomic_load_n(&rb->write_idx, __ATOMIC_RELAXED) ==
           __atomic_load_n(&rb->read_idx,  __ATOMIC_RELAXED);
}

static inline int ringbuf_full(const ringbuf_t *rb) {
    return ringbuf_used(rb) == rb->slot_count;
}

#endif /* ASD_IPC_RINGBUF_H */
