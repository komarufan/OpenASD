/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, ASD Project Contributors
 *
 * virtio-net legacy PCI driver (vendor=0x1AF4, device=0x1000/0x1041).
 * Uses I/O-port BAR0 (legacy virtio 0.9.5 spec).
 *
 * Ring layout: receiveq = queue 0, transmitq = queue 1.
 * Each queue has VNET_QUEUE_SIZE descriptors.
 * RX descriptors are pre-filled with host-writable DMA buffers.
 * TX: build descriptor chain, notify, poll used ring.
 */

#include "virtio_net.h"
#include "../console/fbcon.h"
#include "../mm/mm.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* PCI helpers (same pattern as virtio_blk.c)                          */
/* ------------------------------------------------------------------ */

#define PCI_CFG_ADDR 0xCF8
#define PCI_CFG_DATA 0xCFC

/* Serial debug helper — must come before any function that uses it */
static void vnet_dbg(const char *s) {
    for (; *s; s++) {
        uint8_t st;
        for (int t = 200000; t; t--) {
            __asm__ volatile("inb %1,%0" : "=a"(st) : "Nd"((uint16_t)0x3FD));
            if (st & 0x20) break;
            __asm__ volatile("pause");
        }
        __asm__ volatile("outb %0,%1" :: "a"((uint8_t)*s), "Nd"((uint16_t)0x3F8));
    }
}

static inline void io_out32(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0,%1" ::"a"(val),"Nd"(port));
}
static inline uint32_t io_in32(uint16_t port) {
    uint32_t v;
    __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(port));
    return v;
}
static inline uint16_t io_in16(uint16_t port) {
    uint16_t v;
    __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(port));
    return v;
}
static inline uint8_t io_in8(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port));
    return v;
}
static inline void io_out8(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0,%1"::"a"(val),"Nd"(port));
}
static inline void io_out16(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0,%1"::"a"(val),"Nd"(port));
}

static uint32_t pci_cfg_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t addr = (1u<<31)|((uint32_t)bus<<16)|((uint32_t)slot<<11)|((uint32_t)func<<8)|(off&0xFC);
    io_out32(PCI_CFG_ADDR, addr);
    return io_in32(PCI_CFG_DATA);
}
static uint16_t pci_cfg_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = pci_cfg_read32(bus, slot, func, (uint8_t)(off & 0xFC));
    return (uint16_t)((v >> ((off & 2)*8)) & 0xFFFFu);
}
static uint8_t pci_cfg_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = pci_cfg_read32(bus, slot, func, (uint8_t)(off & 0xFC));
    return (uint8_t)((v >> ((off & 3)*8)) & 0xFFu);
}
static void pci_cfg_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint16_t val) {
    uint8_t aoff = (uint8_t)(off & 0xFC);
    uint32_t old = pci_cfg_read32(bus, slot, func, aoff);
    uint32_t sh  = (uint32_t)((off & 2)*8);
    uint32_t nv  = (old & ~(0xFFFFu<<sh)) | ((uint32_t)val<<sh);
    uint32_t addr = (1u<<31)|((uint32_t)bus<<16)|((uint32_t)slot<<11)|((uint32_t)func<<8)|aoff;
    io_out32(PCI_CFG_ADDR, addr);
    io_out32(PCI_CFG_DATA, nv);
}

/* ------------------------------------------------------------------ */
/* virtio legacy register offsets (from BAR0 I/O base)                 */
/* ------------------------------------------------------------------ */

#define VNET_HOST_FEATURES  0x00
#define VNET_GUEST_FEATURES 0x04
#define VNET_QUEUE_PFN      0x08
#define VNET_QUEUE_NUM      0x0C
#define VNET_QUEUE_SEL      0x0E
#define VNET_QUEUE_NOTIFY   0x10
#define VNET_STATUS         0x12
#define VNET_ISR            0x13
#define VNET_MAC_BASE       0x14   /* 6 bytes of MAC */
#define VNET_NET_STATUS     0x1A

#define VIRTIO_ACKNOWLEDGE  1
#define VIRTIO_DRIVER       2
#define VIRTIO_DRIVER_OK    4

/* ------------------------------------------------------------------ */
/* virtio vring structures                                              */
/* ------------------------------------------------------------------ */

#define VNET_QUEUE_SIZE  256   /* must match what QEMU reports (or be smaller) */

#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2  /* device writes (RX direction) */

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) vnet_desc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VNET_QUEUE_SIZE];
} __attribute__((packed)) vnet_avail_t;

typedef struct {
    uint32_t id;
    uint32_t len;
} __attribute__((packed)) vnet_used_elem_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    vnet_used_elem_t ring[VNET_QUEUE_SIZE];
} __attribute__((packed)) vnet_used_t;

/* virtio-net header prepended to every packet */
typedef struct {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} __attribute__((packed)) vnet_hdr_t;

/* ------------------------------------------------------------------ */
/* Queue state                                                          */
/* ------------------------------------------------------------------ */

#define VNET_BUF_SIZE  1536   /* enough for 1500-byte Ethernet frame */

/* virtio vring layout per spec section 2.4:
 *   desc table:  N * 16 bytes
 *   avail ring:  4 + N*2 bytes
 *   pad to 4096-byte boundary
 *   used ring:   4 + N*8 bytes
 *
 * For N=256: desc=4096, avail=516, pad=3580, used=2052  → total=10244 (3 pages)
 * For N=64:  desc=1024, avail=132, pad=2940, used=516   → total=4612  (2 pages)
 */
#define VRING_DESC_BYTES  (VNET_QUEUE_SIZE * 16u)
#define VRING_AVAIL_BYTES (4u + VNET_QUEUE_SIZE * 2u)
#define VRING_PAD_BYTES   ((4096u - (VRING_DESC_BYTES + VRING_AVAIL_BYTES) % 4096u) % 4096u)
#define VRING_USED_BYTES  (4u + VNET_QUEUE_SIZE * 8u)

typedef struct {
    vnet_desc_t  desc [VNET_QUEUE_SIZE];
    vnet_avail_t avail;
    uint8_t      _pad_avail[VRING_PAD_BYTES];
    vnet_used_t  used;
} __attribute__((packed,aligned(4096))) vnet_ring_t;

/* DMA ring storage: 2 queues */
static vnet_ring_t *g_rxq;
static vnet_ring_t *g_txq;
static paddr_t      g_rxq_phys;
static paddr_t      g_txq_phys;

/* RX data buffers: one per descriptor */
typedef struct {
    vnet_hdr_t  hdr;
    uint8_t     data[VNET_BUF_SIZE];
} __attribute__((aligned(4))) rx_buf_t;

static rx_buf_t *g_rx_bufs;
static paddr_t   g_rx_bufs_phys;

/* TX scratch buffer */
static uint8_t  g_tx_scratch[sizeof(vnet_hdr_t) + VNET_BUF_SIZE] __attribute__((aligned(4)));
static paddr_t  g_tx_scratch_phys;

/* ------------------------------------------------------------------ */
/* Device state                                                         */
/* ------------------------------------------------------------------ */

static uint16_t g_io_base;
static uint8_t  g_mac[6];
static int      g_ready;
static uint16_t g_rx_last_seen;   /* last used-ring index processed */

/* ------------------------------------------------------------------ */
/* RX ring pre-fill                                                     */
/* ------------------------------------------------------------------ */

static void rx_fill(void) {
    for (int i = 0; i < VNET_QUEUE_SIZE; i++) {
        paddr_t buf_phys = g_rx_bufs_phys + (paddr_t)i * sizeof(rx_buf_t);

        g_rxq->desc[i].addr  = (uint64_t)buf_phys;
        g_rxq->desc[i].len   = (uint32_t)sizeof(rx_buf_t);
        g_rxq->desc[i].flags = VRING_DESC_F_WRITE;
        g_rxq->desc[i].next  = 0;

        g_rxq->avail.ring[i] = (uint16_t)i;
    }
    __asm__ volatile("" ::: "memory");
    g_rxq->avail.idx = VNET_QUEUE_SIZE;
    __asm__ volatile("" ::: "memory");
    /* Notify device of queue 0 */
    io_out16((uint16_t)(g_io_base + VNET_QUEUE_NOTIFY), 0);
}

/* ------------------------------------------------------------------ */
/* PCI enumeration and init                                             */
/* ------------------------------------------------------------------ */

void virtio_net_init(void) {
    g_ready = 0;

    for (uint16_t bus = 0; bus < 0x100; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t vd = pci_cfg_read32((uint8_t)bus, slot, 0, 0x00);
            uint16_t vendor = (uint16_t)(vd & 0xFFFFu);
            uint16_t device = (uint16_t)(vd >> 16);

            if (vendor != 0x1AF4) continue;
            /* Legacy virtio-net: 0x1000, modern: 0x1041 */
            if (device != 0x1000 && device != 0x1041) continue;

            /* Subsystem device ID 0x0001 = net for legacy */
            uint32_t ss = pci_cfg_read32((uint8_t)bus, slot, 0, 0x2C);
            uint16_t ss_dev = (uint16_t)(ss >> 16);
            if (device == 0x1000 && ss_dev != 0x0001 && ss_dev != 0x0000)
                continue;

            /* BAR0 = I/O base (bit0=1 → I/O space) */
            uint32_t bar0 = pci_cfg_read32((uint8_t)bus, slot, 0, 0x10);
            if (!(bar0 & 1)) continue;          /* not I/O mapped */
            g_io_base = (uint16_t)(bar0 & 0xFFFC);

            /* Bus-master DMA enable */
            uint16_t cmd = pci_cfg_read16((uint8_t)bus, slot, 0, 0x04);
            pci_cfg_write16((uint8_t)bus, slot, 0, 0x04, (uint16_t)(cmd | 0x04));

            /* IRQ line */
            uint8_t irq = pci_cfg_read8((uint8_t)bus, slot, 0, 0x3C);

            /* --- virtio reset & feature negotiation --- */
            io_out8((uint16_t)(g_io_base + VNET_STATUS), 0);          /* reset */
            io_out8((uint16_t)(g_io_base + VNET_STATUS), VIRTIO_ACKNOWLEDGE);
            io_out8((uint16_t)(g_io_base + VNET_STATUS),
                    VIRTIO_ACKNOWLEDGE | VIRTIO_DRIVER);

            /* Feature bits: accept whatever host offers (no GSO needed) */
            uint32_t host_feat = io_in32((uint16_t)(g_io_base + VNET_HOST_FEATURES));
            /* Clear GSO/checksum features we don't support */
            host_feat &= ~((1u<<0)|(1u<<1)|(1u<<6)|(1u<<7)|(1u<<11));
            io_out32((uint16_t)(g_io_base + VNET_GUEST_FEATURES), host_feat);

            /* Read MAC (device config starts at VNET_MAC_BASE) */
            for (int m = 0; m < 6; m++)
                g_mac[m] = io_in8((uint16_t)(g_io_base + VNET_MAC_BASE + m));

            /* --- Allocate and configure RX queue (0) --- */
            io_out16((uint16_t)(g_io_base + VNET_QUEUE_SEL), 0);
            uint16_t rxsz = io_in16((uint16_t)(g_io_base + VNET_QUEUE_NUM));
            if (rxsz == 0 || rxsz > VNET_QUEUE_SIZE) rxsz = VNET_QUEUE_SIZE;
            (void)rxsz;

            /* DMA ring for RX queue */
            g_rxq_phys = amm_phys_alloc(2);   /* 4 pages = enough for Q=256 ring */
            if (!g_rxq_phys) goto fail;
            g_rxq = (vnet_ring_t *)amm_phys_access(g_rxq_phys);
            memset(g_rxq, 0, sizeof(vnet_ring_t));

            /* DMA RX data buffers */
            uint32_t rx_buf_pages = (VNET_QUEUE_SIZE * sizeof(rx_buf_t) + 4095) / 4096;
            uint32_t rx_buf_order = 0;
            while ((1u << rx_buf_order) < rx_buf_pages) rx_buf_order++;
            g_rx_bufs_phys = amm_phys_alloc(rx_buf_order);
            if (!g_rx_bufs_phys) goto fail;
            g_rx_bufs = (rx_buf_t *)amm_phys_access(g_rx_bufs_phys);
            memset(g_rx_bufs, 0, (size_t)rx_buf_pages * 4096);

            io_out32((uint16_t)(g_io_base + VNET_QUEUE_PFN),
                     (uint32_t)(g_rxq_phys >> 12));

            /* --- Allocate and configure TX queue (1) --- */
            io_out16((uint16_t)(g_io_base + VNET_QUEUE_SEL), 1);
            uint16_t txsz = io_in16((uint16_t)(g_io_base + VNET_QUEUE_NUM));
            if (txsz == 0 || txsz > VNET_QUEUE_SIZE) txsz = VNET_QUEUE_SIZE;

            g_txq_phys = amm_phys_alloc(2);   /* 4 pages for Q=256 ring */
            if (!g_txq_phys) goto fail;
            g_txq = (vnet_ring_t *)amm_phys_access(g_txq_phys);
            memset(g_txq, 0, sizeof(vnet_ring_t));

            /* TX scratch DMA buffer (separate page for packet data) */
            g_tx_scratch_phys = amm_phys_alloc(1);  /* 2 pages — enough for any packet */
            if (!g_tx_scratch_phys) goto fail;

            /* Register TX queue PFN with device */
            io_out16((uint16_t)(g_io_base + VNET_QUEUE_SEL), 1);
            io_out32((uint16_t)(g_io_base + VNET_QUEUE_PFN),
                     (uint32_t)(g_txq_phys >> 12));

            /* --- Enable IRQ and set DRIVER_OK --- */
            extern void pic_unmask(int irq);
            if (irq < 16) pic_unmask((int)irq);

            io_out8((uint16_t)(g_io_base + VNET_STATUS),
                    VIRTIO_ACKNOWLEDGE | VIRTIO_DRIVER | VIRTIO_DRIVER_OK);

            /* Pre-fill RX ring */
            rx_fill();
            g_rx_last_seen = 0;
            g_ready = 1;

            fb_console_putc('N'); fb_console_putc('E'); fb_console_putc('T');
            fb_console_putc(':'); fb_console_putc('O'); fb_console_putc('K');
            fb_console_putc('\n');

            /* Store IRQ for use in isr_dispatch registration */
            extern void net_set_irq(uint8_t irq);
            net_set_irq(irq);
            return;
        }
    }
    return;

fail:
    g_ready = 0;
}

/* ------------------------------------------------------------------ */
/* TX                                                                   */
/* ------------------------------------------------------------------ */

int virtio_net_send(const void *frame, uint16_t len) {
    if (!g_ready || !frame || len == 0 || len > VNET_BUF_SIZE)
        return -1;

    /* Build packet in the TX DMA page: virtio-net header + Ethernet frame */
    uint8_t *dma = (uint8_t *)amm_phys_access(g_tx_scratch_phys);
    vnet_hdr_t *hdr = (vnet_hdr_t *)dma;
    memset(hdr, 0, sizeof(*hdr));
    memcpy(dma + sizeof(vnet_hdr_t), frame, len);
    uint32_t total = (uint32_t)(sizeof(vnet_hdr_t) + len);

    /* Record used.idx before submit so we can detect completion */
    uint16_t used_before = g_txq->used.idx;
    __asm__ volatile("" ::: "memory");

    /* Pick the next descriptor slot (round-robin) */
    uint16_t di = g_txq->avail.idx % VNET_QUEUE_SIZE;

    g_txq->desc[di].addr  = (uint64_t)g_tx_scratch_phys;
    g_txq->desc[di].len   = total;
    g_txq->desc[di].flags = 0;   /* device-readable, no NEXT */
    g_txq->desc[di].next  = 0;

    /* Publish descriptor to device via avail ring */
    g_txq->avail.ring[g_txq->avail.idx % VNET_QUEUE_SIZE] = di;
    __asm__ volatile("" ::: "memory");
    g_txq->avail.idx++;
    __asm__ volatile("" ::: "memory");

    /* Kick device: notify queue 1 (TX) */
    io_out16((uint16_t)(g_io_base + VNET_QUEUE_NOTIFY), 1);

    /*
     * Poll until QEMU processes the TX descriptor.
     * Under TCG, QEMU processes virtio I/O only during its event loop,
     * which runs when the CPU executes HLT (with interrupts enabled).
     *
     * We check the RFLAGS IF bit: if IRQs are already enabled (normal
     * kernel operation after boot), use sti+hlt+cli to yield efficiently.
     * If IRQs are off (early boot), do a pure spin — the packet will be
     * sent once QEMU gets its event loop turn.
     */
    {
        uint64_t rflags;
        __asm__ volatile("pushfq; pop %0" : "=r"(rflags));
        int irqs_on = (rflags >> 9) & 1;

        for (int i = 0; i < 1000; i++) {
            __asm__ volatile("" ::: "memory");
            if (g_txq->used.idx != used_before)
                return 0;
            if (irqs_on) {
                __asm__ volatile("sti; hlt; cli" ::: "memory");
            } else {
                for (int s = 0; s < 10000; s++)
                    __asm__ volatile("pause");
            }
        }
    }
    /* Packet submitted; QEMU will process it eventually */
    return 0;
}

/* ------------------------------------------------------------------ */
/* RX                                                                   */
/* ------------------------------------------------------------------ */

/* Called from net_rx_dispatch (net.c) — forward declaration */
extern void net_rx_dispatch(const void *frame, uint16_t len);

void virtio_net_rx_poll(void) {
    if (!g_ready) return;

    __asm__ volatile("" ::: "memory");

    while (g_rx_last_seen != g_rxq->used.idx) {
        __asm__ volatile("" ::: "memory");
        uint16_t ui = g_rx_last_seen % VNET_QUEUE_SIZE;
        uint32_t desc_id = g_rxq->used.ring[ui].id;
        uint32_t recv_len = g_rxq->used.ring[ui].len;

        if (desc_id < VNET_QUEUE_SIZE && recv_len > sizeof(vnet_hdr_t)) {
            rx_buf_t *rb = &g_rx_bufs[desc_id];
            uint32_t frame_len = recv_len - (uint32_t)sizeof(vnet_hdr_t);
            if (frame_len > VNET_BUF_SIZE)
                frame_len = VNET_BUF_SIZE;
            net_rx_dispatch(rb->data, (uint16_t)frame_len);
        }

        /* Re-add this descriptor to the available ring */
        g_rxq->avail.ring[g_rxq->avail.idx % VNET_QUEUE_SIZE] = (uint16_t)desc_id;
        __asm__ volatile("" ::: "memory");
        g_rxq->avail.idx++;
        __asm__ volatile("" ::: "memory");
        io_out16((uint16_t)(g_io_base + VNET_QUEUE_NOTIFY), 0);

        g_rx_last_seen++;
    }

    /* ACK ISR status register */
    (void)io_in8((uint16_t)(g_io_base + VNET_ISR));
}

void virtio_net_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = g_mac[i];
}

int virtio_net_ready(void) {
    return g_ready;
}

uint16_t virtio_net_rx_used_idx(void) {
    if (!g_ready || !g_rxq) return 0;
    __asm__ volatile("" ::: "memory");
    return g_rxq->used.idx;
}
