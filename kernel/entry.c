/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, ASD Project Contributors
 * All rights reserved.
 */

#include "../boot/asdboot.h"
#include "mm/mm.h"
#include "sched/sched.h"
#include "ipc/port.h"
#include "ipc/ringbuf.h"
#include "vfs/vfs.h"
#include "vfs/fat32.h"
#include "vfs/ffs.h"
#include "drv/adf.h"
#include "drv/virtio_blk.h"
#include "drv/virtio_net.h"
#include "net/net.h"
#include "drv/ps2kbd.h"
#include "block/block.h"
#include "block/gpt.h"
#include "console/fbcon.h"
#include "../init/init_internal.h"
#include "usr/usr.h"
#include "arch/gdt.h"
#include "arch/idt.h"
#include "arch/pic.h"
#include "arch/pit.h"
#include "arch/syscall.h"
#include "security/security.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern uint32_t ramfs_boot_init(const void *file_table, uint32_t count);
extern uint32_t ramfs_root_init(const void *initrd_ptr, uint64_t initrd_sz);
extern int ramfs_root_mkdir(const char *path);

extern void asdinit_main(void) __attribute__((noreturn));

extern void amm_early_phys_scan(const void *mmap, uint32_t count, uint32_t entry_sz);
extern void amm_phys_init(const void *mmap, uint32_t count, uint32_t entry_sz);
extern void amm_kernel_pml4_init(void);

#define COM1_PORT  0x3F8



static void serial_init(void) {
    /* Configure COM1: 115200 baud, 8N1 */
    io_out8(COM1_PORT + 1, 0x00); /* disable interrupts */
    io_out8(COM1_PORT + 3, 0x80); /* DLAB on */
    io_out8(COM1_PORT + 0, 0x01); /* divisor low */
    io_out8(COM1_PORT + 1, 0x00); /* divisor high */
    io_out8(COM1_PORT + 3, 0x03); /* 8 bits, no parity, one stop */
    io_out8(COM1_PORT + 2, 0xC7); /* FIFO enable, clear, 14-byte threshold */
    io_out8(COM1_PORT + 4, 0x03); /* RTS/DTR only — polled I/O, no COM IRQ storm */
}



void serial_putu(uint64_t n);



static uint64_t fat_volume_sectors(block_dev_t *d0, uint64_t lba, uint64_t cap) {
    uint8_t bpb[512];
    if (!d0 || !d0->read || d0->read(d0, lba, 1, bpb) != 0)
        return cap;
    if (bpb[510] != 0x55 || bpb[511] != 0xAA)
        return cap;
    uint64_t total = rd16le(bpb + 19);
    if (total == 0)
        total = rd64le(bpb + 32);
    if (total == 0 || total > cap)
        return cap;
    return total;
}

/* Read a FAT volume from [lba, lba+sectors) and mount it at mount_pt. */
static int try_mount_fat_volume(block_dev_t *d0, uint64_t lba, uint64_t sectors,
                                const char *mount_pt) {
    if (!d0 || !d0->read || sectors == 0 || !mount_pt)
        return 0;

    /* Enough for make install / ESP; avoid multi-hundred-MiB kmalloc on 2G disks. */
    uint64_t max_img_sectors = (64ULL * 1024ULL * 1024ULL) / d0->sector_size;
    if (sectors > max_img_sectors)
        sectors = max_img_sectors;
    sectors = fat_volume_sectors(d0, lba, sectors);
    if (sectors > max_img_sectors)
        sectors = max_img_sectors;

    uint64_t img_bytes = sectors * (uint64_t)d0->sector_size;
    void *img_buf = kmalloc((size_t)img_bytes);
    if (!img_buf) {
        serial_puts("    fat: kmalloc failed for image\n");
        return 0;
    }

    int rd_ok = 1;
    uint32_t chunk = 128;
    for (uint64_t i = 0; i < sectors; i += chunk) {
        uint32_t cnt = (uint32_t)(sectors - i);
        if (cnt > chunk) cnt = chunk;
        if (d0->read(d0, lba + i, cnt,
                     (uint8_t *)img_buf + i * d0->sector_size) != 0) {
            rd_ok = 0;
            break;
        }
    }
    if (!rd_ok) {
        serial_puts("    fat: disk read failed\n");
        kfree(img_buf);
        return 0;
    }
    {
        const uint8_t *bpb = (const uint8_t *)img_buf;
        if (bpb[510] != 0x55 || bpb[511] != 0xAA) {
            serial_puts("    fat: no BPB signature at LBA ");
            serial_putu(lba);
            serial_puts("\n");
        }
    }
    if (fat32_mount_image(mount_pt, img_buf, (size_t)img_bytes) != 0) {
        serial_puts("    fat: mounted ");
        serial_puts(mount_pt);
        serial_puts(" (");
        serial_putu(sectors);
        serial_puts(" sectors)\n");
        return 1;
    }
    serial_puts("    fat: parse failed at ");
    serial_puts(mount_pt);
    serial_puts(" LBA ");
    serial_putu(lba);
    serial_puts("\n");
    kfree(img_buf);
    return 0;
}

/*
 * Copy binaries from /boot/bin → /bin using VFS open/write.
 * Works whether / is ramfs or FFS — no ramfs-specific calls.
 */
static void seed_asdsh_into_vfs(void) {
    const char *bins[] = {
        "asdsh", "mifetch", "hx", "ls", "cat", "mkdir", "rm", "touch",
        "echo", "pwd", "sysinfo", "uname", "uptime", "id", "whoami",
        "kill", "hexdump", "wc", "ping", "filetest", "nettest", "hxtest",
        "grep", "find", "sort", "head", "tail", "do", "apm", NULL
    };

    vfs_mkdir("/bin");

    for (int i = 0; bins[i]; i++) {
        char src_path[64];
        char dst_path[64];
        stat_info_t st;

        strncpy(src_path, "/boot/bin/", sizeof(src_path));
        strncat(src_path, bins[i], sizeof(src_path) - strlen(src_path) - 1);
        strncpy(dst_path, "/bin/", sizeof(dst_path));
        strncat(dst_path, bins[i], sizeof(dst_path) - strlen(dst_path) - 1);

        /* Skip if already present */
        if (vfs_stat(dst_path, &st) == 0) continue;
        if (vfs_stat(src_path, &st) != 0) continue;

        size_t file_sz = (st.size > 0 && st.size < 8 * 1024 * 1024)
                         ? (size_t)st.size : (8 * 1024 * 1024);

        uint8_t *buf = (uint8_t *)kmalloc(file_sz);
        if (!buf) continue;

        fd_t rfd = vfs_open(src_path, VFS_O_READ, NULL);
        if ((int)rfd <= 0) { kfree(buf); continue; }

        size_t total = 0, got = 0;
        while (total < file_sz) {
            if (vfs_read(rfd, buf + total, file_sz - total, &got) != 0 || got == 0)
                break;
            total += got;
        }
        vfs_close(rfd);
        if (total == 0) { kfree(buf); continue; }

        fd_t wfd = vfs_open(dst_path, VFS_O_WRITE | VFS_O_CREAT | VFS_O_TRUNC, NULL);
        if ((int)wfd > 0) {
            vfs_write(wfd, buf, total);
            vfs_close(wfd);
            serial_puts("    seeded ");
            serial_puts(dst_path);
            serial_puts("\n");
        }
        kfree(buf);
    }
}

void serial_putu(uint64_t n) {
    char buf[32];
    int i = 31;
    buf[i] = '\0';
    if (n == 0) { buf[--i] = '0'; }
    while (n) {
        buf[--i] = (char)('0' + (n % 10));
        n /= 10;
    }
    serial_puts(&buf[i]);
}

static const char *g_cmdline = NULL;
static uint32_t    g_cmdline_len = 0;
static const void *g_live_media_ptr = NULL;
static uint64_t    g_live_media_len = 0;

const void *kernel_live_media_ptr(void) {
    return g_live_media_ptr;
}

uint64_t kernel_live_media_len(void) {
    return g_live_media_len;
}

static void cmdline_init(const char *ptr, uint32_t len) {
    g_cmdline = ptr;
    g_cmdline_len = len;
}

int kernel_cmdline_has(const char *key) {
    if (!g_cmdline || !key || !key[0]) return 0;
    for (uint32_t i = 0; i < g_cmdline_len; i++) {
        const char *p = g_cmdline + i;
        const char *k = key;
        while (*k && i < g_cmdline_len && g_cmdline[i] == *k) {
            i++;
            k++;
        }
        if (!*k && (i >= g_cmdline_len || g_cmdline[i] == ' '))
            return 1;
        while (i < g_cmdline_len && g_cmdline[i] != ' ') i++;
    }
    return 0;
}

void kernel_main(asd_bib_t *bib, uint64_t magic)
    __attribute__((noreturn));

void kernel_main_body(asd_bib_t *bib, uint64_t magic)
    __attribute__((noreturn));

/* Dedicated main-thread kernel stack (256 KB).
 * The UEFI application stack is typically small and gets exhausted
 * during early init (virtio scan, VFS, installer). */
#define KMAIN_STACK_SIZE (256 * 1024)
static uint8_t g_kmain_stack[KMAIN_STACK_SIZE] __attribute__((aligned(16)));

void kernel_main(asd_bib_t *bib, uint64_t magic) {
    /* Validate magic */
    if (magic != ASD_BOOT_MAGIC || bib->magic != ASD_BIB_MAGIC) {
        /* Halt — no valid boot information */
        for (;;) __asm__ volatile("hlt");
    }

    /* UEFI may leave IF=1; keep IRQs off until asdinit is ready. */
    __asm__ volatile("cli");

    /* Early serial output for debugging */
    serial_init();
    fb_console_init(bib);
    serial_puts("ASD kernel booting...\n");

    /* Enable SSE/AVX for userland (fastfetch uses it for CPUID/xgetbv) */
    {
        uint32_t eax, ebx, ecx, edx;
        
        /* Check for SSE support */
        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
        if (edx & (1 << 25)) { /* SSE bit */
            uint64_t cr0, cr4;
            __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
            cr0 &= ~(1ULL << 2); /* Clear EM (Emulation) */
            cr0 |=  (1ULL << 1); /* Set MP (Monitor Coprocessor) */
            __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

            __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
            cr4 |= (1ULL << 9);  /* OSFXSR: FXSAVE/FXRSTOR support */
            cr4 |= (1ULL << 10); /* OSXMMEXCPT: SIMD exception support */
            
            /* Check for XSAVE support before enabling OSXSAVE */
            if (ecx & (1 << 26)) { /* XSAVE bit */
                cr4 |= (1ULL << 18); /* OSXSAVE: XSAVE and xgetbv support */
                __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));

                /* Enable AVX in XCR0 if OSXSAVE is set and AVX is supported */
                uint32_t xcr0_mask = 3; /* X87 and SSE */
                if (ecx & (1 << 28)) { /* AVX bit */
                    xcr0_mask |= 4;
                }
                __asm__ volatile(
                    "xor %%ecx, %%ecx\n\t"
                    "xgetbv\n\t"
                    "or %0, %%eax\n\t"
                    "xsetbv"
                    : : "r"(xcr0_mask) : "rax", "rcx", "rdx"
                );
                if (xcr0_mask & 4) {
                    serial_puts("  sse/avx: enabled\n");
                } else {
                    serial_puts("  sse: enabled (no avx)\n");
                }
            } else {
                __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
                serial_puts("  sse: enabled (no avx)\n");
            }
        } else {
            serial_puts("  sse: not supported\n");
        }
    }
    serial_puts("fb: phys=");
    serial_putu(bib->fb_phys);
    serial_puts(" w=");
    serial_putu(bib->fb_width);
    serial_puts(" h=");
    serial_putu(bib->fb_height);
    serial_puts(" stride=");
    serial_putu(bib->fb_stride);
    serial_puts(" fmt=");
    serial_putu(bib->fb_format);
    serial_puts("\n");

    /* Leave the tiny UEFI stack before mm/virtio/VFS — stack overflow
     * there corrupts the return address and faults as #GP rip=0. */
    {
        uint64_t rsp = (uint64_t)(uintptr_t)(g_kmain_stack + KMAIN_STACK_SIZE);
        __asm__ volatile(
            "mov %0, %%rsp\n\t"
            "call kernel_main_body"
            :
            : "r"(rsp), "D"(bib), "S"(magic)
            : "memory"
        );
    }
    __builtin_unreachable();
}

void kernel_main_body(asd_bib_t *bib, uint64_t magic) {
    (void)magic;

    /* ----------------------------------------------------------------- */

    serial_puts("  [1/7] memory manager\n");

    const uint8_t *mmap = (const uint8_t *)(uintptr_t)bib->mmap_phys;

    /* Phase 1: scan mmap to record phys_top, init bootstrap bump allocator */
    amm_early_phys_scan(mmap, bib->mmap_count, bib->mmap_entry_sz);

    /* Phase 2: build kernel page tables (uses bump allocator + UEFI identity
       mapping), then switch CR3 — after this PHYS_MAP_BASE is live */
    amm_kernel_pml4_init();

    mmap = (const uint8_t *)amm_phys_access((paddr_t)bib->mmap_phys);

    /* Phase 3: now PHYS_MAP_BASE / PAGE_INFO_BASE are mapped; populate buddy */
    amm_phys_init(mmap, bib->mmap_count, bib->mmap_entry_sz);

    /* ----------------------------------------------------------------- */

    serial_puts("  [1.5/7] GDT + IDT + PIC + PIT\n");
    gdt_init();       /* load our GDT with ring-0/ring-3 segments + TSS */
    pic_init();       /* remap 8259: IRQ0-7 → 0x20, IRQ8-15 → 0x28      */
    idt_init();       /* install 256 ISR stubs, lidt                      */
    pit_init();       /* PIT channel-0, 100 Hz → IRQ0 → sched_tick       */
    /* IRQ0 stays masked until sti in asdinit — avoids timer IRQs during
     * early boot while the scheduler/context paths are not fully safe. */
    pic_mask(0);

    /* ----------------------------------------------------------------- */

    serial_puts("  [2/7] scheduler\n");
    sched_init(1); /* single CPU for now; SMP later */

    /* ----------------------------------------------------------------- */

    serial_puts("  [3/7] IPC\n");
    port_subsystem_init();

    /* ----------------------------------------------------------------- */

    serial_puts("  [3.5/7] user/group database\n");
    usr_init();

    /* ----------------------------------------------------------------- */

    serial_puts("  [3.7/7] security module\n");
    security_init();

    /* ----------------------------------------------------------------- */

    serial_puts("  [x] block devices\n");
    block_init();
    virtio_blk_init();

    serial_puts("  [x] network\n");
    virtio_net_init();
    net_init();
    {
        int n = block_count();
        serial_puts("    found=");
        serial_putu(n);
        serial_puts("\n");
        for (int i = 0; i < n; i++) {
            block_dev_t *d = block_get(i);
            if (!d) continue;
            serial_puts("    [");
            serial_putu((uint64_t)i);
            serial_puts("] ");
            serial_puts(d->name);
            serial_puts(" sectors=");
            serial_putu(d->sector_count);
            serial_puts(" bsz=");
            serial_putu(d->sector_size);
            serial_puts("\n");
        }
        /* Live media (e.g. live.img + install disk): do not touch block
         * devices here.  virtio-blk I/O with IRQs still masked can stall for
         * a long time; the installer handles partitioning later. */
        if (n == 1) {
            block_dev_t *d0 = block_get(0);
            if (d0 && d0->read && d0->write) {
                uint8_t mbr[512];
                serial_puts("    reading mbr from ");
                serial_puts(d0->name);
                serial_puts("... ");
                if (d0->read(d0, 0, 1, mbr) == 0) {
                    uint16_t sig = (uint16_t)mbr[510] | ((uint16_t)mbr[511] << 8);
                    uint8_t gpt_hdr[512];
                    int has_gpt = (d0->read(d0, 1, 1, gpt_hdr) == 0 &&
                                   memcmp(gpt_hdr, "EFI PART", 8) == 0);
                    serial_puts("    mbr sig=");
                    serial_putu(sig);
                    serial_puts("\n");
                    if (has_gpt) {
                        serial_puts("    gpt: partition table present\n");
                        } else if (disk_sector0_is_fat(mbr)) { /* defined in gpt.c */
                        serial_puts("    disk: FAT volume at LBA 0 (preserved)\n");
                    } else if (sig != 0xAA55) {
                        serial_puts("    gpt: laying out ESP+root on ");
                        serial_puts(d0->name);
                        serial_puts("\n");
                        if (gpt_init_single_disk(d0) == 0) {
                            serial_puts("    gpt: ok\n");
                        } else {
                            serial_puts("    gpt: failed\n");
                        }
                    } else {
                        serial_puts("    disk: boot sector present, skip GPT layout\n");
                    }
                } else {
                    serial_puts("    mbr read failed\n");
                }
            }
        } else if (n > 1) {
            serial_puts("    live media: deferring disk probe\n");
        }
        /* Load passwd from install disk LBA 34 (polled virtio, IRQs still off). */
        if (n >= 1)
            boot_early_load_passwd_db();
    }

    /* ----------------------------------------------------------------- */

    serial_puts("  [4/7] VFS\n");
    vfs_init();

    /* ----------------------------------------------------------------- */

    serial_puts("  [5/7] filesystems\n");

    /* Mount the root filesystem on "/".
     * We check if the disk has a GPT layout. If so, we mount the second partition
     * (ASD root) as the root filesystem. Otherwise, we fall back to mounting
     * the entire first block device (live media mode).
     *
     * With multiple block devices (live + install), root comes from initrd/ramfs;
     * avoid reading up to 128 MiB from vda during early boot. */
    int mounted_boot = 0;

    if (block_count() == 1) {
        block_dev_t *d0 = block_get(0);
        if (d0) {
            extern uint32_t ramfs_mount_root(void);
            int ffs_root_ok = 0;

            /* Check for GPT and try FFS on partition 2, FAT32 on partition 1 */
            uint8_t hdr_sec[512];
            int has_gpt = (d0->read(d0, 1, 1, hdr_sec) == 0 &&
                           memcmp(hdr_sec, "EFI PART", 8) == 0);
            if (has_gpt) {
                uint8_t part_sec[512];
                if (d0->read(d0, 2, 1, part_sec) == 0) {
                    /* Partition 0 = ESP, partition 1 = FFS root */
                    const uint8_t *pe0 = part_sec;
                    const uint8_t *pe1 = part_sec + 128;
                    uint64_t esp_first = rd64le(pe0 + 32);
                    uint64_t esp_last  = rd64le(pe0 + 40);
                    uint64_t rt_first  = rd64le(pe1 + 32);
                    uint64_t rt_last   = rd64le(pe1 + 40);

                    /* Try FFS on root partition */
                    if (rt_first && rt_last > rt_first) {
                        uint32_t fid = ffs_mount_dev("/", d0, rt_first,
                                                     (rt_last - rt_first) + 1);
                        if (fid) {
                            serial_puts("    vfs: FFS at / (root partition LBA ");
                            serial_putu(rt_first);
                            serial_puts(")\n");
                            ffs_root_ok = 1;
                        }
                    }

                    /* Mount ESP (FAT32) at /boot */
                    if (esp_first && esp_last > esp_first) {
                        if (try_mount_fat_volume(d0, esp_first,
                                                 (esp_last - esp_first) + 1,
                                                 "/boot")) {
                            stat_info_t st;
                            if (vfs_stat("/boot/bin/asdsh", &st) == 0 ||
                                vfs_stat("/boot/EFI/BOOT/BOOTX64.EFI", &st) == 0)
                                mounted_boot = 1;
                        }
                    }
                }
            }

            if (!ffs_root_ok) {
                /* Fallback: ramfs at / */
                ramfs_mount_root();
                serial_puts("    vfs: ramfs at / (fallback)\n");

                if (!has_gpt) {
                    /* Flat FAT disk (make install) */
                    if (try_mount_fat_volume(d0, 0, d0->sector_count, "/boot")) {
                        stat_info_t st;
                        if (vfs_stat("/boot/bin/asdsh", &st) == 0 ||
                            vfs_stat("/boot/asdboot.conf", &st) == 0)
                            mounted_boot = 1;
                    }
                }
            }
        }
    } else if (block_count() > 1) {
        extern uint32_t ramfs_mount_root(void);
        ramfs_mount_root();
        serial_puts("    vfs: mounted ramfs at / (live media)\n");
        ramfs_root_mkdir("/bin");

        /* Smaller virtio disk is the live FAT image (live.img). */
        block_dev_t *live = block_get(0);
        for (int i = 1; i < block_count(); i++) {
            block_dev_t *d = block_get(i);
            if (d && live && d->sector_count < live->sector_count)
                live = d;
        }
        if (live && try_mount_fat_volume(live, 0, live->sector_count, "/boot")) {
            stat_info_t st;
            if (vfs_stat("/boot/bin/asdsh", &st) == 0 ||
                vfs_stat("/boot/asdboot.conf", &st) == 0 ||
                vfs_stat("/boot/boot/asdboot.conf", &st) == 0) {
                mounted_boot = 1;
                serial_puts("    mounted /boot from live disk (FAT)\n");
            }
        }
    }

    int initrd_used_for_fat32 = 0;
    if (bib->initrd_phys && bib->initrd_len > 0 && !mounted_boot) {
        void *img = amm_phys_access((paddr_t)bib->initrd_phys);
        g_live_media_ptr = img;
        g_live_media_len = bib->initrd_len;
        uint32_t fat_id = fat32_mount_image("/boot", img, (size_t)bib->initrd_len);
        if (fat_id != 0) {
            serial_puts("    mounted /boot from FAT image\n");
            mounted_boot = 1;
            initrd_used_for_fat32 = 1;
        }
    }

    if (!mounted_boot) {
        ramfs_boot_init(NULL, 0);
        serial_puts("    mounted /boot from ramfs fallback\n");
    }

    /* Always mount /root as writable ramfs.
     * If initrd was not consumed by FAT32, seed it from the initrd blob;
     * otherwise start empty (writable scratch space for the live session
     * or the installed system). */
    if (bib->initrd_phys && bib->initrd_len > 0 && !initrd_used_for_fat32) {
        void *initrd = amm_phys_access((paddr_t)bib->initrd_phys);
        ramfs_root_init(initrd, bib->initrd_len);
        serial_puts("    mounted /root from initrd\n");
    } else {
        ramfs_root_init(NULL, 0);
        serial_puts("    mounted /root as empty ramfs\n");
    }

    /* Seed binaries from /boot/bin into / (works for ramfs or FFS). */
    seed_asdsh_into_vfs();

    /* Second chance: passwd file on ESP (written by installer as /ASDPW1). */
    boot_vfs_load_passwd_db();

    {
        stat_info_t st;
        if (vfs_stat("/boot/asdboot.conf", &st) == 0 ||
            vfs_stat("/boot/boot/asdboot.conf", &st) == 0) {
            serial_puts("    smoke: /boot/asdboot.conf found\n");
        } else {
            serial_puts("    smoke: /boot/asdboot.conf missing\n");
        }
        if (vfs_stat("/bin/asdsh", &st) == 0) {
            serial_puts("    smoke: /bin/asdsh found, size=");
            serial_putu(st.size);
            serial_puts("\n");
        } else {
            serial_puts("    smoke: /bin/asdsh missing\n");
        }
    }

    {
        vfs_dirent_t entries[8];
        uint32_t out_n = 0;
        if (vfs_readdir("/boot", entries, 8, &out_n) == 0) {
            serial_puts("    smoke: /boot entries=");
            serial_putu(out_n);
            serial_puts("\n");
        } else {
            serial_puts("    smoke: /boot readdir failed\n");
        }
    }

    /* ----------------------------------------------------------------- */

    serial_puts("  [6/7] driver framework\n");
    adf_init();

    /* ----------------------------------------------------------------- */

    serial_puts("  [6.5/7] syscall + keyboard\n");
    syscall_init();   /* SYSCALL/SYSRET MSRs, per-CPU GS struct */
    ps2kbd_init();    /* flush PS/2, unmask IRQ1                 */

    /* ----------------------------------------------------------------- */

    serial_puts("  [7/7] cmdline\n");
    if (bib->cmdline_phys && bib->cmdline_len > 0) {
        const char *cmd = (const char *)amm_phys_access((paddr_t)bib->cmdline_phys);
        cmdline_init(cmd, bib->cmdline_len);
    }

    /* ----------------------------------------------------------------- */

    serial_puts("  launching asdinit (PID 1)\n");
    serial_puts("  entering asdinit main loop\n");

    pic_unmask(0);    /* PIT / sched_tick */
    __asm__ volatile("sti");

    /* These are defined in sched.c but may be static or named differently in this build */
    extern void sched_bootstrap_init_thread(void);
    extern void sched_bootstrap_enter(void (*fn)(void)) __attribute__((noreturn));
    sched_bootstrap_init_thread();
    sched_bootstrap_enter(asdinit_main);
}
