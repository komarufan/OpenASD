#include "virtio_blk.h"
#include "../block/block.h"
#include "../console/fbcon.h"
#include "../mm/mm.h"
#include <stdint.h>
#include <string.h>

#define PCI_CFG_ADDR 0xCF8
#define PCI_CFG_DATA 0xCFC
#define VIRTIO_ACKNOWLEDGE 1
#define VIRTIO_DRIVER      2
#define VIRTIO_DRIVER_OK   4

#define VIRTIO_PCI_HOST_FEATURES 0x00
#define VIRTIO_PCI_GUEST_FEATURES 0x04
#define VIRTIO_PCI_QUEUE_PFN 0x08
#define VIRTIO_PCI_QUEUE_NUM 0x0C
#define VIRTIO_PCI_QUEUE_SEL 0x0E
#define VIRTIO_PCI_QUEUE_NOTIFY 0x10
#define VIRTIO_PCI_STATUS 0x12

#define VIRTIO_BLK_T_IN  0
#define VIRTIO_BLK_T_OUT 1

#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2

static inline void io_out32(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" :: "a"(val), "d"(port));
}
static inline uint16_t io_in16(uint16_t port) {
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "d"(port));
    return v;
}
static inline uint8_t io_in8(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "d"(port));
    return v;
}
static inline uint32_t io_in32(uint16_t port) {
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "d"(port));
    return v;
}
static inline void io_out8(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "d"(port));
}
static inline void io_out16(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" :: "a"(val), "d"(port));
}

static uint32_t pci_cfg_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t addr = (1u << 31) |
                    ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) |
                    (off & 0xFC);
    io_out32(PCI_CFG_ADDR, addr);
    return io_in32(PCI_CFG_DATA);
}

static uint16_t pci_cfg_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = pci_cfg_read32(bus, slot, func, (uint8_t)(off & 0xFC));
    return (uint16_t)((v >> ((off & 2) * 8)) & 0xFFFFu);
}

static void pci_cfg_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint16_t val) {
    uint8_t aoff = (uint8_t)(off & 0xFC);
    uint32_t oldv = pci_cfg_read32(bus, slot, func, aoff);
    uint32_t shift = (uint32_t)((off & 2) * 8);
    uint32_t mask = 0xFFFFu << shift;
    uint32_t newv = (oldv & ~mask) | ((uint32_t)val << shift);
    uint32_t addr = (1u << 31) |
                    ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) |
                    aoff;
    io_out32(PCI_CFG_ADDR, addr);
    io_out32(PCI_CFG_DATA, newv);
}

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) vr_desc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed)) vr_avail_t;

typedef struct {
    uint32_t id;
    uint32_t len;
} __attribute__((packed)) vr_used_elem_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    vr_used_elem_t ring[];
} __attribute__((packed)) vr_used_t;

typedef struct {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed)) vblk_req_hdr_t;

typedef struct {
    uint16_t io_base;
    uint16_t qnum;
    uint16_t last_used_idx;
    paddr_t  q_phys;
    uint8_t *q_virt;
    uint64_t capacity_sectors;

    vr_desc_t  *desc;
    vr_avail_t *avail;
    vr_used_t  *used;
    paddr_t     desc_phys;
    paddr_t     avail_phys;
    paddr_t     used_phys;

    uint8_t    *req_area;
    paddr_t     req_phys;
} virtio_blk_ctx_t;

static uint64_t align_up_u64(uint64_t x, uint64_t a) {
    return (x + a - 1) & ~(a - 1);
}

/* virtio-blk request layout inside req_area. */
#define VBLK_REQ_HDR_OFF   0
#define VBLK_REQ_DATA_OFF  32
#define VBLK_MAX_XFER_SECTORS 32
#define VBLK_REQ_DATA_SIZE (VBLK_MAX_XFER_SECTORS * 512)
#define VBLK_REQ_STATUS_OFF (VBLK_REQ_DATA_OFF + VBLK_REQ_DATA_SIZE)
#define VBLK_REQ_TOTAL_SIZE (VBLK_REQ_STATUS_OFF + 1)

static int virtio_blk_submit(virtio_blk_ctx_t *ctx, uint64_t lba, void *buf, uint32_t sectors, int is_write) {
    if (!ctx || !buf || sectors == 0 || sectors > VBLK_MAX_XFER_SECTORS) return -1;
    vblk_req_hdr_t *hdr = (vblk_req_hdr_t *)(void *)(ctx->req_area + 0);
    uint8_t *data = ctx->req_area + VBLK_REQ_DATA_OFF;
    uint8_t *status = ctx->req_area + VBLK_REQ_STATUS_OFF;
    paddr_t hdr_pa = ctx->req_phys + 0;
    paddr_t data_pa = ctx->req_phys + VBLK_REQ_DATA_OFF;
    paddr_t status_pa = ctx->req_phys + VBLK_REQ_STATUS_OFF;

    uint32_t data_len = sectors * 512u;
    if (is_write) {
        memcpy(data, buf, data_len);
    } else {
        memset(data, 0, data_len);
    }
    *status = 0xFF;

    hdr->type = is_write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    hdr->reserved = 0;
    hdr->sector = lba;

    ctx->desc[0].addr = hdr_pa;
    ctx->desc[0].len = sizeof(vblk_req_hdr_t);
    ctx->desc[0].flags = VRING_DESC_F_NEXT;
    ctx->desc[0].next = 1;

    ctx->desc[1].addr = data_pa;
    ctx->desc[1].len = data_len;
    ctx->desc[1].flags = VRING_DESC_F_NEXT | (is_write ? 0 : VRING_DESC_F_WRITE);
    ctx->desc[1].next = 2;

    ctx->desc[2].addr = status_pa;
    ctx->desc[2].len = 1;
    ctx->desc[2].flags = VRING_DESC_F_WRITE;
    ctx->desc[2].next = 0;

    uint16_t aidx = ctx->avail->idx;
    ctx->avail->ring[aidx % ctx->qnum] = 0;
    __asm__ volatile("" ::: "memory");
    ctx->avail->idx = (uint16_t)(aidx + 1);
    io_out16((uint16_t)(ctx->io_base + VIRTIO_PCI_QUEUE_NOTIFY), 0);

    /* Poll for completion (QEMU legacy virtio updates the used ring in RAM). */
    for (uint32_t spins = 0; spins < 50000000; spins++) {
        __asm__ volatile("mfence" ::: "memory");
        uint16_t used_idx = *(volatile uint16_t *)&ctx->used->idx;
        if (used_idx != ctx->last_used_idx) {
            ctx->last_used_idx = used_idx;
            /* Acknowledge/clear the interrupt on the VirtIO device */
            (void)io_in8((uint16_t)(ctx->io_base + 0x13));
            if (*status != 0) return -1;
            if (!is_write) memcpy(buf, data, data_len);
            return 0;
        }
        if ((spins & 0x3FFF) == 0)
            fb_console_tick();
        __asm__ volatile("pause" ::: "memory");
    }
    return -1; /* Timeout */
}

static int vblk_stub_rw(block_dev_t *dev, uint64_t lba, uint32_t count, void *buf) {
    if (!dev || !dev->ctx || !buf || count == 0) return -1;
    virtio_blk_ctx_t *ctx = (virtio_blk_ctx_t *)dev->ctx;
    if (lba + count > ctx->capacity_sectors) return -1;
    uint8_t *p = (uint8_t *)buf;
    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > VBLK_MAX_XFER_SECTORS) chunk = VBLK_MAX_XFER_SECTORS;
        if (virtio_blk_submit(ctx, lba + done, p + done * 512u, chunk, 0) == 0) {
            done += chunk;
            continue;
        }
        /* Fallback for flaky multi-sector transfers: retry one-by-one. */
        for (uint32_t i = 0; i < chunk; i++) {
            if (virtio_blk_submit(ctx, lba + done + i, p + (done + i) * 512u, 1, 0) != 0)
                return -1;
        }
        done += chunk;
    }
    return 0;
}
static int vblk_stub_wr(block_dev_t *dev, uint64_t lba, uint32_t count, const void *buf) {
    if (!dev || !dev->ctx || !buf || count == 0) return -1;
    virtio_blk_ctx_t *ctx = (virtio_blk_ctx_t *)dev->ctx;
    if (lba + count > ctx->capacity_sectors) return -1;
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > VBLK_MAX_XFER_SECTORS) chunk = VBLK_MAX_XFER_SECTORS;
        if (virtio_blk_submit(ctx, lba + done, (void *)(uintptr_t)(p + done * 512u), chunk, 1) == 0) {
            done += chunk;
            continue;
        }
        /* Fallback for flaky multi-sector transfers: retry one-by-one. */
        for (uint32_t i = 0; i < chunk; i++) {
            if (virtio_blk_submit(ctx, lba + done + i,
                                  (void *)(uintptr_t)(p + (done + i) * 512u), 1, 1) != 0)
                return -1;
        }
        done += chunk;
    }
    return 0;
}

void virtio_blk_init(void) {
    int idx = 0;
    for (uint16_t bus = 0; bus < 0x100; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t vd = pci_cfg_read32((uint8_t)bus, slot, 0, 0x00);
            if ((vd & 0xFFFFu) == 0xFFFFu) continue;

            uint16_t vendor = (uint16_t)(vd & 0xFFFFu);
            uint16_t device = (uint16_t)(vd >> 16);
            if (vendor != 0x1AF4u) continue;
            if (device != 0x1001u && device != 0x1042u) continue; /* transitional/modern blk */

            /* Ensure I/O and bus mastering are enabled for legacy BAR access. */
            uint16_t cmd = pci_cfg_read16((uint8_t)bus, slot, 0, 0x04);
            cmd |= 0x0005u; /* IO space + bus master */
            pci_cfg_write16((uint8_t)bus, slot, 0, 0x04, cmd);

            if (device != 0x1001u) continue; /* only legacy transitional path for now */

            uint32_t bar0 = pci_cfg_read32((uint8_t)bus, slot, 0, 0x10);
            if ((bar0 & 1u) == 0) continue;
            uint16_t io_base = (uint16_t)(bar0 & ~0x3u);

            io_out8((uint16_t)(io_base + VIRTIO_PCI_STATUS), 0);
            io_out8((uint16_t)(io_base + VIRTIO_PCI_STATUS), VIRTIO_ACKNOWLEDGE);
            io_out8((uint16_t)(io_base + VIRTIO_PCI_STATUS), VIRTIO_ACKNOWLEDGE | VIRTIO_DRIVER);
            io_out32((uint16_t)(io_base + VIRTIO_PCI_GUEST_FEATURES), 0);

            io_out16((uint16_t)(io_base + VIRTIO_PCI_QUEUE_SEL), 0);
            uint16_t qnum = io_in16((uint16_t)(io_base + VIRTIO_PCI_QUEUE_NUM));
            if (qnum < 3) continue;

            uint64_t desc_bytes = (uint64_t)16 * qnum;
            /* vring_avail_t: flags(u16)+idx(u16)+ring[qnum]*u16 */
            uint64_t avail_bytes = 4 + (uint64_t)2 * qnum;
            /* vring_used_t: flags(u16)+idx(u16)+ring[qnum]*vr_used_elem_t(8 bytes) */
            uint64_t used_bytes = 4 + (uint64_t)8 * qnum;
            uint64_t off_desc = 0;
            uint64_t off_avail = off_desc + desc_bytes;
            uint64_t off_used = align_up_u64(off_avail + avail_bytes, 4096);
            uint64_t off_req = align_up_u64(off_used + used_bytes, 16);
            uint64_t total = align_up_u64(off_req + VBLK_REQ_TOTAL_SIZE, PAGE_SIZE);
            uint32_t order = 0;
            while (((uint64_t)PAGE_SIZE << order) < total) order++;

            paddr_t qphys = amm_phys_alloc(order);
            if (!qphys) continue;
            uint8_t *qvirt = (uint8_t *)amm_phys_access(qphys);
            memset(qvirt, 0, (size_t)((uint64_t)PAGE_SIZE << order));

            io_out32((uint16_t)(io_base + VIRTIO_PCI_QUEUE_PFN), (uint32_t)(qphys >> 12));

            virtio_blk_ctx_t *ctx = (virtio_blk_ctx_t *)kmalloc(sizeof(*ctx));
            if (!ctx) continue;
            memset(ctx, 0, sizeof(*ctx));
            ctx->io_base = io_base;
            ctx->qnum = qnum;
            ctx->q_phys = qphys;
            ctx->q_virt = qvirt;
            ctx->desc = (vr_desc_t *)(void *)(qvirt + off_desc);
            ctx->avail = (vr_avail_t *)(void *)(qvirt + off_avail);
            ctx->used = (vr_used_t *)(void *)(qvirt + off_used);
            ctx->desc_phys = qphys + off_desc;
            ctx->avail_phys = qphys + off_avail;
            ctx->used_phys = qphys + off_used;
            ctx->req_area = qvirt + off_req;
            ctx->req_phys = qphys + off_req;
            ctx->last_used_idx = 0;

            block_dev_t dev;
            memset(&dev, 0, sizeof(dev));
            dev.sector_size = 512;
            uint32_t cap_lo = io_in32((uint16_t)(io_base + 0x14));
            uint32_t cap_hi = io_in32((uint16_t)(io_base + 0x18));
            ctx->capacity_sectors = ((uint64_t)cap_hi << 32) | cap_lo;
            dev.sector_count = ctx->capacity_sectors;
            dev.read = vblk_stub_rw;
            dev.write = vblk_stub_wr;
            dev.ctx = ctx;
            dev.name[0] = 'v';
            dev.name[1] = 'd';
            dev.name[2] = (char)('a' + idx);
            dev.name[3] = '\0';
            io_out8((uint16_t)(io_base + VIRTIO_PCI_STATUS),
                    VIRTIO_ACKNOWLEDGE | VIRTIO_DRIVER | VIRTIO_DRIVER_OK);

            if (block_register(&dev) == 0) idx++;
            if (idx >= BLOCK_MAX_DEVS) return;
        }
    }
}
