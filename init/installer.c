/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, ASD Project Contributors
 *
 * Interactive TUI installer: disk selection, user setup, file copy.
 *
*/

#include "init_internal.h"
#include "../kernel/block/gpt.h"
#include "../kernel/vfs/fat32.h"
#include "../kernel/vfs/ffs.h"
#include "../kernel/mm/mm.h"

/* Sectors reserved between GPT entries and first usable LBA (2048).
 * Stores the user database and hostname; never overlaps any partition. */
#define PASSWD_SECS     8U
#define HOSTNAME_OFFSET (PASSWD_SECS * 512 - 64)

/* Copy chunk size in sectors — 64 × 512 = 32 KiB per I/O */
#define COPY_CHUNK_SECS  64U

/* Installer credentials — static to avoid large stack frames in PID 1
 * (SSE memset of stack arrays can #GP when the stack is deep/misaligned). */
static char g_install_root_pw[128];
static char g_install_user[USR_NAME_LEN];
static char g_install_user_pw[128];

static block_dev_t *g_last_install_target;

/* Fresh stack for disk I/O — PID1/TUI stack is too small for GPT+virtio+IRQ. */
#define INSTALL_DISK_STACK_SIZE (512 * 1024)
static uint8_t g_install_disk_stack[INSTALL_DISK_STACK_SIZE]
    __attribute__((aligned(16)));
static uintptr_t g_install_saved_rsp;
static block_dev_t *g_stack_tgt;
static block_dev_t *g_stack_src;

static int install_boot_media(block_dev_t *target, block_dev_t *source);

static int installer_invoke_on_stack(int (*fn)(void)) {
    uintptr_t top = (uintptr_t)(g_install_disk_stack + INSTALL_DISK_STACK_SIZE);
    uintptr_t sp  = (top - 8) & ~(uintptr_t)15ULL;
    int ret = -1;

    __asm__ volatile(
        "mov %%rsp, %[save]\n"
        "mov %[alt], %%rsp\n"
        "call *%[fn]\n"
        "mov %[save], %%rsp\n"
        : [save]"=m"(g_install_saved_rsp), "=a"(ret)
        : [alt]"r"(sp), [fn]"r"(fn)
        : "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
          "memory", "cc"
    );
    return ret;
}

static int wrap_gpt_init(void) {
    return gpt_init_single_disk(g_stack_tgt);
}

static int wrap_copy_boot(void) {
    return install_boot_media(g_stack_tgt, g_stack_src);
}

typedef struct {
    uint8_t  type_guid[16];
    uint8_t  part_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attrs;
    uint16_t name[36];
} __attribute__((packed)) gpt_part_entry_t;

/* ------------------------------------------------------------------ */
/* Forward declarations for TUI screens (installer_tui.c)              */
/* ------------------------------------------------------------------ */

int  installer_run_tui(void);
int  tui_screen_disk(block_dev_t **out);
void tui_screen_hostname(char *buf, size_t sz);
void tui_screen_configure_accounts(void);
void tui_screen_root_pw(char *buf, size_t sz);
int  tui_screen_user(char *name, size_t nsz, char *pw, size_t psz, int *wheel);
int  tui_screen_confirm(const char *disk, const char *host);
void tui_screen_progress(int step, uint64_t done, uint64_t total);
void tui_screen_done(int success);

static int persist_users(block_dev_t *target);
static void deploy_directory_structure(const char *user_name);

/* ------------------------------------------------------------------ */
/* Disk copy                                                            */
/* ------------------------------------------------------------------ */

static uint8_t g_esp_hdr[512];
static uint8_t g_esp_sec[512];

static int read_esp_range(block_dev_t *dev,
                          uint64_t *esp_first, uint64_t *esp_last) {
    if (dev->read(dev, 1, 1, g_esp_hdr) != 0) return -1;
    if (memcmp(g_esp_hdr, "EFI PART", 8) != 0) return -1;

    uint64_t entries_lba = rd64le(g_esp_hdr + 72);
    uint32_t num_parts   = rd32le(g_esp_hdr + 80);
    uint32_t entry_size  = rd32le(g_esp_hdr + 84);
    if (entry_size < sizeof(gpt_part_entry_t) ||
        num_parts == 0 || num_parts > 128) return -1;

    const uint8_t esp_guid[16] = {
        0xC1, 0x2A, 0x73, 0x28, 0xF8, 0x1F, 0x11, 0xD2,
        0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
    };
    uint32_t parts_per_sec = 512u / entry_size;
    if (parts_per_sec == 0) return -1;
    uint32_t need_secs = (num_parts + parts_per_sec - 1u) / parts_per_sec;

    for (uint32_t s = 0; s < need_secs; s++) {
        if (dev->read(dev, entries_lba + s, 1, g_esp_sec) != 0) return -1;
        for (uint32_t off = 0; off + entry_size <= 512; off += entry_size) {
            const gpt_part_entry_t *p =
                (const gpt_part_entry_t *)(const void *)(g_esp_sec + off);
            if (memcmp(p->type_guid, esp_guid, 16) != 0) continue;
            if (p->first_lba == 0 || p->last_lba < p->first_lba) continue;
            *esp_first = p->first_lba;
            *esp_last  = p->last_lba;
            return 0;
        }
    }
    return -1;
}

/* Copy live-media FAT image into the target ESP.
 * Uses 32 KiB chunks (COPY_CHUNK_SECS) for speed.
 * Progress is reported via tui_screen_progress(). */
static int install_boot_media(block_dev_t *target, block_dev_t *source) {
    if (!target || !target->write || !target->read ||
        !source || !source->read) return -1;

    uint64_t esp_first = 0, esp_last = 0;
    if (read_esp_range(target, &esp_first, &esp_last) != 0) {
        serial_puts("install error: ESP partition not found in GPT\n");
        return -1;
    }

    uint64_t esp_sectors = (esp_last - esp_first) + 1;
    uint64_t src_sectors = source->sector_count;

    /* Read BPB total-sector count from source FAT volume header. */
    {
        static uint8_t bpb[512];
        if (source->read(source, 0, 1, bpb) == 0 &&
            bpb[510] == 0x55 && bpb[511] == 0xAA) {
            uint16_t bps   = rd16le(bpb + 11);
            uint32_t total = rd16le(bpb + 19);
            if (total == 0) total = rd32le(bpb + 32);
            if (bps == 512 && total > 0) src_sectors = total;
        }
    }

    if (src_sectors == 0) {
        serial_puts("install error: source sector count is zero\n");
        return -1;
    }
    /* Clamp to physical limits and 128 MiB max */
    if (src_sectors > source->sector_count) src_sectors = source->sector_count;
    if (src_sectors > 262144ULL)            src_sectors = 262144ULL;
    if (src_sectors > esp_sectors)          src_sectors = esp_sectors;

    if (esp_first >= target->sector_count) {
        serial_puts("install error: esp_first beyond disk capacity\n");
        return -1;
    }
    {
        uint64_t phys_avail = target->sector_count - esp_first;
        if (src_sectors > phys_avail) src_sectors = phys_avail;
    }

    /* 32 KiB copy buffer */
    static uint8_t sec[512 * COPY_CHUNK_SECS];
    uint64_t done      = 0;
    uint64_t last_done = ~0ULL;
    uint64_t log_next  = 0;

    serial_port_puts("[installer] copy sectors=");
    put_u64(src_sectors);
    serial_port_puts("\n");
    tui_screen_progress(1, 0, src_sectors);

    while (done < src_sectors) {
        uint32_t chunk = (uint32_t)(src_sectors - done);
        if (chunk > COPY_CHUNK_SECS) chunk = COPY_CHUNK_SECS;

        if (source->read(source, done, chunk, sec) != 0) {
            serial_puts("\ninstall read error at source sector ");
            put_u64(done); serial_puts("\n");
            return -1;
        }
        if (target->write(target, esp_first + done, chunk, sec) != 0) {
            serial_puts("\ninstall write error at target sector ");
            put_u64(done); serial_puts("\n");
            return -1;
        }
        done += chunk;
        fb_console_tick();

        if (done >= log_next) {
            serial_port_puts("[installer] copy ");
            put_u64(done);
            serial_port_puts("/");
            put_u64(src_sectors);
            serial_port_puts("\n");
            log_next += src_sectors / 32;
            if (log_next == done) log_next += 1;
        }

        if (done != last_done) {
            tui_screen_progress(1, done, src_sectors);
            last_done = done;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* User database persistence                                            */
/* ------------------------------------------------------------------ */

int passwd_db_magic_ok(const uint8_t *sector0) {
    if (!sector0) return 0;
    return sector0[0] == '#' && sector0[1] == 'A' && sector0[2] == 'S' &&
           sector0[3] == 'D' && sector0[4] == 'P' && sector0[5] == 'W' &&
           sector0[6] == '1';
}

/* True if buffer has a real user DB (not the host/live "#ASDPW1" stub). */
static int passwd_buf_has_root_account(const uint8_t *buf, size_t len) {
    if (!buf || len < 16 || !passwd_db_magic_ok(buf)) return 0;
    const char *p   = (const char *)buf;
    const char *end = (const char *)buf + len;
    for (; p + 8 <= end; p++) {
        if (p[0] == 'r' && p[1] == 'o' && p[2] == 'o' && p[3] == 't' &&
            p[4] == ' ' && p[5] == '0' && p[6] == ' ' && p[7] == '0')
            return 1;
    }
    return 0;
}

static int g_boot_passwd_loaded;

static void passwd_apply_hostname_from_raw(const uint8_t *raw) {
    if (!raw || !passwd_db_magic_ok(raw)) return;
    char *h = (char *)(raw + HOSTNAME_OFFSET);
    h[63] = '\0';
    char c0 = h[0];
    if ((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') ||
        (c0 >= '0' && c0 <= '9')) {
        strncpy(g_hostname, h, sizeof(g_hostname));
        g_hostname[sizeof(g_hostname) - 1] = '\0';
    }
}

/* Load passwd DB from one 4 KiB slot; returns number of user lines loaded. */
static int passwd_load_slot(block_dev_t *dev, uint64_t lba) {
    static uint8_t raw[PASSWD_SECS * 512];

    if (!dev || !dev->read) return -1;
    if (dev->read(dev, lba, PASSWD_SECS, raw) != 0) return -1;
    if (!passwd_buf_has_root_account(raw, sizeof(raw))) return -1;
    raw[sizeof(raw) - 1] = '\0';
    int n = usr_load_passwd((const char *)raw, sizeof(raw) - 1);
    if (n > 0)
        passwd_apply_hostname_from_raw(raw);
    return n;
}

int boot_try_load_passwd_from_dev(block_dev_t *dev) {
    if (!dev) return 0;

    if (passwd_load_slot(dev, PASSWD_LBA) > 0)
        goto ok;

    /* Last sectors of the disk (no GPT walk — avoids multi-minute ESP seeks). */
    if (dev->sector_count >= PASSWD_LBA + PASSWD_SECS + 64) {
        uint64_t tail = dev->sector_count - PASSWD_SECS;
        if (passwd_load_slot(dev, tail) > 0)
            goto ok;
    }

    return 0;

ok:
    g_boot_passwd_loaded = 1;
    return 1;
}

void boot_load_hostname_from_disk(void) {
    for (int i = 0; i < block_count(); i++) {
        block_dev_t *d = block_get(i);
        if (d)
            try_load_hostname_from_disk(d);
    }
}

block_dev_t *find_passwd_disk(void) {
    for (int i = 0; i < block_count(); i++) {
        block_dev_t *d = block_get(i);
        static uint8_t probe[PASSWD_SECS * 512];
        if (!d || !d->read) continue;
        if (d->read(d, PASSWD_LBA, PASSWD_SECS, probe) != 0) continue;
        if (passwd_buf_has_root_account(probe, sizeof(probe)))
            return d;
    }
    return NULL;
}

int boot_passwd_was_loaded(void) {
    return g_boot_passwd_loaded;
}

void boot_apply_passwd_buffer(const uint8_t *raw, size_t len) {
    if (!raw || len == 0 || !passwd_db_magic_ok(raw)) return;

    passwd_apply_hostname_from_raw(raw);

    /* Stub /boot/ASDPW1 is only "#ASDPW1\\n" — usr_load_passwd returns 0. */
    if (usr_load_passwd((const char *)raw, len) <= 0) return;
    g_boot_passwd_loaded = 1;
}

void boot_vfs_load_passwd_db(void) {
    if (g_boot_passwd_loaded) return;

    static const char *paths[] = {
        "/boot/ASDPW1",
        "/boot/asdpw1",
        NULL
    };

    for (int i = 0; paths[i]; i++) {
        fd_t fd = vfs_open(paths[i], VFS_O_READ, NULL);
        if (fd <= 0) continue;

        static char buf[PASSWD_SECS * 512];
        size_t got = 0;
        int n = vfs_read(fd, buf, sizeof(buf) - 1, &got);
        vfs_close(fd);
        if (n < 0) n = (int)got;
        if (n <= 0) continue;

        buf[n] = '\0';
        boot_apply_passwd_buffer((const uint8_t *)buf, (size_t)n);
        if (g_boot_passwd_loaded) {
            serial_puts("    passwd: loaded from ");
            serial_puts(paths[i]);
            serial_puts("\n");
            return;
        }
    }
}

void boot_early_load_passwd_db(void) {
    g_boot_passwd_loaded = 0;
    for (int i = 0; i < block_count(); i++) {
        block_dev_t *d = block_get(i);
        if (boot_try_load_passwd_from_dev(d)) {
            serial_puts("    passwd: loaded from ");
            serial_puts(d->name);
            serial_puts("\n");
            return;
        }
    }
}

int try_load_users_from_disk(block_dev_t *dev) {
    return boot_try_load_passwd_from_dev(dev) ? 1 : -1;
}

int boot_reload_passwd_from_disk(void) {
    if (g_boot_passwd_loaded) return 1;
    for (int i = 0; i < block_count(); i++) {
        block_dev_t *d = block_get(i);
        if (boot_try_load_passwd_from_dev(d))
            return 1;
    }
    return 0;
}

block_dev_t *install_get_last_target(void) {
    return g_last_install_target;
}

static void trim_install_passwords(void) {
    char *p = g_install_root_pw;
    size_t pl = strlen(p);
    while (pl > 0 && (p[pl - 1] == '\r' || p[pl - 1] == '\n' || p[pl - 1] == ' '))
        p[--pl] = '\0';

    p = g_install_user_pw;
    pl = strlen(p);
    while (pl > 0 && (p[pl - 1] == '\r' || p[pl - 1] == '\n' || p[pl - 1] == ' '))
        p[--pl] = '\0';
}

/* Apply credentials from installer globals into the in-memory user DB. */
static void apply_install_credentials(int has_user, int wheel) {
    if (g_install_root_pw[0]) {
        usr_set_password(UID_ROOT, g_install_root_pw);
    } else {
        asd_user_t *root = usr_find_by_uid(UID_ROOT);
        if (root) {
            root->pw_hash = 0;
            root->pw_salt = 0;
        }
    }

    if (!has_user || !g_install_user[0]) return;

    static char home_path[USR_HOME_LEN];
    memset(home_path, 0, sizeof(home_path));
    strncpy(home_path, "/home/", sizeof(home_path) - 1);
    size_t n = strlen(home_path);
    if (n < sizeof(home_path) - 1)
        strncpy(home_path + n, g_install_user, sizeof(home_path) - n - 1);

    /* Create the user's home directory on disk — deploy_directory_structure
     * is called with NULL before the username is known, so it skips this. */
    vfs_mkdir(home_path);

    uint32_t uflags = wheel ? USR_FLAG_WHEEL : 0;
    asd_user_t *u = usr_find_by_uid((uid_t)UID_USER_MIN);
    if (!u) {
        if (usr_add((uid_t)UID_USER_MIN, GID_USERS, g_install_user,
                    g_install_user_pw, uflags, home_path, "/bin/asdsh") == 0) {
            g_shell_uid = (uid_t)UID_USER_MIN;
            g_shell_gid = GID_USERS;
        }
        return;
    }

    strncpy(u->name, g_install_user, USR_NAME_LEN - 1);
    u->name[USR_NAME_LEN - 1] = '\0';
    u->gid   = GID_USERS;
    u->flags = uflags;
    strncpy(u->home, home_path, USR_HOME_LEN - 1);
    u->home[USR_HOME_LEN - 1] = '\0';
    strncpy(u->shell, "/bin/asdsh", USR_SHELL_LEN - 1);
    u->shell[USR_SHELL_LEN - 1] = '\0';
    usr_set_password((uid_t)UID_USER_MIN, g_install_user_pw);
}

int configure_users_interactive(block_dev_t *target) {
    if (!target || !target->write) {
        serial_puts("Cannot save accounts: target disk is not writable.\n");
        return -1;
    }

    g_last_install_target = target;
    tui_screen_progress(3, 0, 1);
    tui_screen_configure_accounts();

    g_install_root_pw[0] = '\0';
    tui_screen_root_pw(g_install_root_pw, sizeof(g_install_root_pw));

    g_install_user[0]    = '\0';
    g_install_user_pw[0] = '\0';
    int wheel            = 0;
    int has_user         = tui_screen_user(g_install_user, sizeof(g_install_user),
                                           g_install_user_pw, sizeof(g_install_user_pw),
                                           &wheel);

    trim_install_passwords();
    serial_port_puts("[installer] applying credentials...\n");
    fb_console_tick();
    apply_install_credentials(has_user, wheel);

    serial_port_puts("[installer] saving accounts to disk...\n");
    fb_console_tick();
    if (persist_users(target) != 0) {
        serial_puts("  FAILED: accounts were not saved — do not reboot yet.\n");
        return -1;
    }

    g_boot_passwd_loaded = 1;
    tui_screen_progress(4, 1, 1);
    serial_port_puts("[installer] accounts saved\n");
    return 0;
}

void try_load_hostname_from_disk(block_dev_t *dev) {
    if (!dev || !dev->read) return;
    static uint8_t buf[PASSWD_SECS * 512];
    if (dev->read(dev, PASSWD_LBA, PASSWD_SECS, buf) != 0) return;
    if (!passwd_db_magic_ok(buf)) return;
    char *h = (char *)(buf + HOSTNAME_OFFSET);
    h[63] = '\0';
    char c0 = h[0];
    if ((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') ||
        (c0 >= '0' && c0 <= '9')) {
        strncpy(g_hostname, h, sizeof(g_hostname) - 1);
        g_hostname[sizeof(g_hostname) - 1] = '\0';
    }
}

static int persist_users(block_dev_t *target) {
    if (!target || !target->write) {
        serial_puts("  (no target -- users not persisted)\n");
        return -1;
    }
    static char buf[PASSWD_SECS * 512];
    int len = usr_write_passwd(buf, sizeof(buf));
    if (len < 0) { serial_puts("  (error writing user database)\n"); return -1; }

    /* Ensure the buffer is zeroed out after the user database content */
    for (size_t i = (size_t)len; i < sizeof(buf); i++) buf[i] = 0;

    if ((size_t)len >= HOSTNAME_OFFSET) {
        serial_puts("  WARNING: passwd db too large; hostname may not fit\n");
    } else {
        size_t hlen = strlen(g_hostname);
        if (hlen > 63) hlen = 63;
        memcpy(buf + HOSTNAME_OFFSET, g_hostname, hlen);
        buf[HOSTNAME_OFFSET + hlen] = '\0';
    }

    serial_port_puts("[installer] writing passwd LBA 34 (hostname=");
    serial_port_puts(g_hostname);
    serial_port_puts(")...\n");
    fb_console_tick();
    if (target->write(target, PASSWD_LBA, PASSWD_SECS, buf) != 0) {
        serial_puts("  WARNING: failed to write user database to disk\n");
        return -1;
    }
    fb_console_tick();

    {
        static uint8_t verify[PASSWD_SECS * 512];
        if (target->read(target, PASSWD_LBA, PASSWD_SECS, verify) != 0 ||
            !passwd_buf_has_root_account(verify, sizeof(verify))) {
            serial_puts("  WARNING: passwd verify after write failed\n");
            return -1;
        }
    }

    if (target->sector_count >= PASSWD_LBA + PASSWD_SECS + 64) {
        uint64_t tail = target->sector_count - PASSWD_SECS;
        if (target->write(target, tail, PASSWD_SECS, buf) == 0)
            serial_puts("  Users backup at end of disk\n");
    }

    serial_puts("  Users written to disk (LBA 34):\n");
    for (uint32_t i = 0; i < usr_count(); i++) {
        asd_user_t *u = usr_get(i);
        if (!u) continue;
        serial_puts("    "); serial_puts(u->name);
        serial_puts(" uid="); put_u64(u->uid);
        if (u->flags & USR_FLAG_WHEEL) serial_puts(" [wheel]");
        serial_puts("\n");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Salted password hashing                                              */
/* ------------------------------------------------------------------ */

/*
 * Improved password hashing: mix the password with a per-user salt
 * derived from the username using multiple FNV rounds.
 * Still not bcrypt, but substantially better than bare FNV-1a.
 */
static uint64_t hash_password(const char *pw, uint64_t salt) {
    /* Round 1: seed with salt */
    uint64_t h = 14695981039346656037ULL ^ salt;
    h *= 1099511628211ULL;

    /* Round 2: mix password */
    for (const char *s = pw ? pw : ""; *s; s++) {
        h ^= (uint8_t)*s;
        h *= 1099511628211ULL;
    }
    /* Round 3: additional mixing passes to slow brute-force */
    for (int i = 0; i < 1024; i++) {
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ULL;
        h ^= h >> 33;
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* Helpers for reading the GPT root partition bounds                    */
/* ------------------------------------------------------------------ */

static uint8_t g_gpt_hdr_buf[512];
static uint8_t g_gpt_pe_buf[512];

/*
 * Read the second GPT partition entry (index 1 = ASD root) from `dev`.
 * Fills *first_lba and *sector_count on success. Returns 0 on success.
 */
static int gpt_root_partition(block_dev_t *dev,
                               uint64_t *first_lba, uint64_t *sector_count) {
    if (!dev || !dev->read) return -1;
    if (dev->read(dev, 1, 1, g_gpt_hdr_buf) != 0) return -1;
    if (memcmp(g_gpt_hdr_buf, "EFI PART", 8) != 0) return -1;

    uint64_t pe_lba  = rd64le(g_gpt_hdr_buf + 72);
    uint32_t nparts  = rd32le(g_gpt_hdr_buf + 80);
    uint32_t pe_size = rd32le(g_gpt_hdr_buf + 84);
    if (nparts < 2 || pe_size < 128 || pe_size > 512) return -1;

    /* Read the first sector of partition entries (LBA 2 by default).
     * Entry 0 = ESP, Entry 1 = ASD root — both 128 bytes, fit in 512 bytes. */
    if (dev->read(dev, pe_lba, 1, g_gpt_pe_buf) != 0) return -1;

    const uint8_t *pe1 = g_gpt_pe_buf + pe_size; /* second entry */
    if (pe1 + 48 > g_gpt_pe_buf + 512) return -1; /* sanity */

    uint64_t first = rd64le(pe1 + 32);
    uint64_t last  = rd64le(pe1 + 40);
    if (first == 0 || last < first) return -1;
    *first_lba    = first;
    *sector_count = (last - first) + 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Copy a binary from src_path to dst_path on the mounted FFS          */
/* ------------------------------------------------------------------ */

static void copy_file_to_ffs(const char *src_path, const char *dst_path) {
    stat_info_t st;
    if (vfs_stat(src_path, &st) != 0) return;
    if (st.kind != VFS_NODE_FILE || st.size <= 0) return;

    fd_t src = vfs_open(src_path, VFS_O_READ, NULL);
    if ((int)src < 0) return;

    fd_t dst = vfs_open(dst_path, VFS_O_WRITE | VFS_O_CREAT | VFS_O_TRUNC, NULL);
    if ((int)dst < 0) { vfs_close(src); return; }

    static uint8_t cpbuf[4096];
    size_t got = 0;
    while (vfs_read(src, cpbuf, sizeof(cpbuf), &got) == 0 && got > 0)
        vfs_write(dst, cpbuf, got);

    vfs_close(src);
    vfs_close(dst);
    serial_puts("    copied: ");
    serial_puts(dst_path);
    serial_puts("\n");
}

/* ------------------------------------------------------------------ */
/* Deploy base directory structure + binaries to FFS root              */
/* ------------------------------------------------------------------ */

static void deploy_directory_structure(const char *user_name) {
    serial_puts("  [deploy] Creating base directory structure on FFS\n");

    static const char *dirs[] = {
        "/bin", "/sbin", "/boot", "/etc",
        "/var", "/var/log", "/home", "/root", "/tmp",
        NULL
    };
    for (int i = 0; dirs[i]; i++) {
        int r = vfs_mkdir(dirs[i]);
        serial_puts(r == 0 ? "    mkdir: " : "    mkdir (exists): ");
        serial_puts(dirs[i]);
        serial_puts("\n");
    }

    if (user_name && user_name[0]) {
        static char home[128];
        strncpy(home, "/home/", sizeof(home) - 1);
        size_t n = strlen(home);
        strncpy(home + n, user_name, sizeof(home) - n - 1);
        vfs_mkdir(home);
        serial_puts("    mkdir: "); serial_puts(home); serial_puts("\n");
    }

    /* /etc/hostname */
    {
        fd_t fd = vfs_open("/etc/hostname",
                           VFS_O_WRITE | VFS_O_CREAT | VFS_O_TRUNC, NULL);
        if (fd > 0) {
            vfs_write(fd, g_hostname, strlen(g_hostname));
            vfs_write(fd, "\n", 1);
            vfs_close(fd);
            serial_puts("    wrote: /etc/hostname\n");
        }
    }

    /* /etc/os-release */
    {
        fd_t fd = vfs_open("/etc/os-release",
                           VFS_O_WRITE | VFS_O_CREAT | VFS_O_TRUNC, NULL);
        if (fd > 0) {
            static const char *rel =
                "NAME=\"OpenASD\"\nVERSION=\"1.0\"\nID=openasd\n"
                "VERSION_ID=\"1.0\"\nPRETTY_NAME=\"OpenASD 1.0\"\n";
            vfs_write(fd, rel, strlen(rel));
            vfs_close(fd);
            serial_puts("    wrote: /etc/os-release\n");
        }
    }

    /* /etc/motd */
    {
        fd_t fd = vfs_open("/etc/motd",
                           VFS_O_WRITE | VFS_O_CREAT | VFS_O_TRUNC, NULL);
        if (fd > 0) {
            static const char *motd =
                "Welcome to OpenASD 1.0\n"
                "Type 'help' for commands.\n"
                "Tip: 'apm help' to manage packages.\n";
            vfs_write(fd, motd, strlen(motd));
            vfs_close(fd);
            serial_puts("    wrote: /etc/motd\n");
        }
    }

    /* /etc/apm/apm.conf */
    {
        vfs_mkdir("/etc/apm");
        fd_t fd = vfs_open("/etc/apm/apm.conf",
                           VFS_O_WRITE | VFS_O_CREAT | VFS_O_TRUNC, NULL);
        if (fd > 0) {
            static const char *conf =
                "# apm.conf - ASD Package Manager 1.0 configuration\n"
                "#\n"
                "repo official https://github.com/komarufan/OpenASD-packages/releases/latest/download\n"
                "#\n"
                "arch=x86_64\n";
            vfs_write(fd, conf, strlen(conf));
            vfs_close(fd);
            serial_puts("    wrote: /etc/apm/apm.conf\n");
        }
    }

    /* /etc/apm/README — documentation readable via `cat /etc/apm/README` */
    {
        fd_t fd = vfs_open("/etc/apm/README",
                           VFS_O_WRITE | VFS_O_CREAT | VFS_O_TRUNC, NULL);
        if (fd > 0) {
            static const char *readme =
                "APM — ASD Package Manager 1.0\n"
                "==============================\n"
                "\n"
                "QUICK START\n"
                "-----------\n"
                "  1. Enable a repository in /etc/apm/apm.conf:\n"
                "       hx /etc/apm/apm.conf\n"
                "     Uncomment the line:\n"
                "       # repo official https://github.com/komarufan/OpenASD-packages/releases/latest/download\n"
                "\n"
                "  2. Fetch the package index:\n"
                "       apm update\n"
                "\n"
                "  3. Search and install:\n"
                "       apm search grep\n"
                "       apm install grep\n"
                "\n"
                "COMMANDS\n"
                "--------\n"
                "  apm update              Fetch/refresh repository package indexes\n"
                "  apm install <pkg>...    Install one or more packages\n"
                "  apm del <pkg>...        Remove installed packages\n"
                "  apm upgrade             Upgrade all installed packages\n"
                "  apm search <query>      Search by name or description\n"
                "  apm list                List all installed packages\n"
                "  apm info <pkg>          Show package details\n"
                "  apm clean               Remove cached archives (/var/apm/cache/)\n"
                "  apm check               Verify integrity of installed files\n"
                "  apm help                Show usage\n"
                "\n"
                "FILES\n"
                "-----\n"
                "  /etc/apm/apm.conf              Configuration (repos, arch)\n"
                "  /var/apm/db/installed/<n>.apd  Installed package records\n"
                "  /var/apm/lists/<repo>.idx       Cached repository indexes\n"
                "  /var/apm/cache/                 Downloaded package archives\n"
                "\n"
                "OFFLINE INSTALL\n"
                "---------------\n"
                "  When network is unavailable, copy .apkg files manually:\n"
                "    mkdir -p /var/apm/cache\n"
                "    # copy <pkg>-1.0-x86_64.apkg to /var/apm/cache/\n"
                "    # place a minimal index.idx in /var/apm/lists/local.idx\n"
                "    # add: repo local /var/apm/cache  to /etc/apm/apm.conf\n"
                "    apm install <pkg>\n"
                "\n"
                "NOTES\n"
                "-----\n"
                "  - Network downloads require QEMU user-mode networking\n"
                "    (the default QEMU setup: -netdev user,id=net0)\n"
                "  - Only HTTP is supported (no HTTPS/TLS in v1)\n"
                "  - DNS uses 8.8.8.8 over UDP port 53\n"
                "  - Package archives use the .apkg format (magic: APKG)\n"
                "\n"
                "See documentation/10-apm-package-manager.md for full details.\n";
            vfs_write(fd, readme, strlen(readme));
            vfs_close(fd);
            serial_puts("    wrote: /etc/apm/README\n");
        }
    }

    /* Copy user binaries from live ESP /bin → FFS /bin */
    serial_puts("  [deploy] Copying binaries to FFS\n");
    static const char *bins[] = {
        "asdsh", "ls", "cat", "mkdir", "rm", "touch", "echo", "pwd",
        "sysinfo", "uname", "uptime", "id", "whoami", "kill",
        "hexdump", "wc", "hx", "mifetch", "ping", "ws", "dock", "term", NULL
    };
    for (int i = 0; bins[i]; i++) {
        static char src[64], dst[64];
        strncpy(src, "/boot/bin/", sizeof(src) - 1);
        strncat(src, bins[i], sizeof(src) - strlen(src) - 1);
        strncpy(dst, "/bin/", sizeof(dst) - 1);
        strncat(dst, bins[i], sizeof(dst) - strlen(dst) - 1);
        copy_file_to_ffs(src, dst);
    }

    /* sbin */
    static const char *sbins[] = { "asdlog", "netd", NULL };
    for (int i = 0; sbins[i]; i++) {
        static char src[64], dst[64];
        strncpy(src, "/boot/sbin/", sizeof(src) - 1);
        strncat(src, sbins[i], sizeof(src) - strlen(src) - 1);
        strncpy(dst, "/sbin/", sizeof(dst) - 1);
        strncat(dst, sbins[i], sizeof(dst) - strlen(dst) - 1);
        copy_file_to_ffs(src, dst);
    }

    serial_puts("  [deploy] Directory structure ready\n");
}

/* ------------------------------------------------------------------ */
/* Main installer entry point                                           */
/* ------------------------------------------------------------------ */
int installer_run(void) {
    /* Screen 1: Welcome — returns 1=install, 0=shell, -1=quit */
    int welcome = installer_run_tui();
    if (welcome <= 0) return welcome;   /* 0 = live shell, -1 = quit */

    /* Screen 2: Disk selection */
    flush_input();
    block_dev_t *target = NULL;
    int pick = tui_screen_disk(&target);
    if (pick <= 0 || !target) return 0;

    g_last_install_target = target;

    /* Screen 3: Hostname */
    flush_input();
    serial_port_puts("[installer] hostname screen\n");
    tui_screen_hostname(g_hostname, sizeof(g_hostname));

    /* Screen 6: Confirm */
    flush_input();
    serial_port_puts("[installer] confirm screen\n");
    int confirmed = tui_screen_confirm(target->name, g_hostname);
    if (!confirmed) return 0;

    /* ---- Find live-media source ---- */
    block_dev_t *source = NULL;
    uint64_t best_sz = 0;
    for (int i = 0; i < block_count(); i++) {
        block_dev_t *d = block_get(i);
        if (!d || !d->read || d == target) continue;
        if (d->sector_count > best_sz) { best_sz = d->sector_count; source = d; }
    }
    if (!source) {
        serial_puts("Installer error: live media disk not found.\n");
        tui_screen_done(0);
        int key; do { key = read_menu_key(); } while (key != 3 && key != 4);
        return -1;
    }

    /* ---- Step 1: GPT (no TUI here — full redraw overflows PID1 stack) ---- */
    tui_screen_progress(0, 0, 1);
    serial_port_puts("[installer] writing GPT to ");
    serial_port_puts(target->name);
    serial_port_puts("...\n");
    g_stack_tgt = target;
    __asm__ volatile("cli");
    int gpt_rc = installer_invoke_on_stack(wrap_gpt_init);
    __asm__ volatile("sti");
    if (gpt_rc != 0) {
        serial_puts("FAILED: cannot write GPT layout.\n");
        tui_screen_done(0);
        int key; do { key = read_menu_key(); } while (key != 3 && key != 4);
        return -1;
    }
    serial_port_puts("[installer] GPT ok\n");

    /* ---- Step 2: Copy boot media ---- */
    serial_port_puts("[installer] copying boot media (may take a while on TCG)...\n");
    g_stack_tgt = target;
    g_stack_src = source;
    __asm__ volatile("cli");
    int copy_rc = installer_invoke_on_stack(wrap_copy_boot);
    __asm__ volatile("sti");
    if (copy_rc != 0) {
        serial_puts("FAILED: cannot copy boot media into ESP.\n");
        tui_screen_done(0);
        int key; do { key = read_menu_key(); } while (key != 3 && key != 4);
        return -1;
    }
    serial_port_puts("[installer] copy ok\n");

    /* ---- Step 2b: Format root partition (UFS/FFS) ---- */
    serial_port_puts("[installer] formatting root (UFS) partition...\n");
    tui_screen_progress(2, 0, 3);
    {
        uint64_t root_first = 0, root_secs = 0;
        if (gpt_root_partition(target, &root_first, &root_secs) != 0) {
            serial_puts("WARN: could not locate root partition — skipping FFS format\n");
        } else {
            serial_port_puts("[installer] root partition: LBA=");
            put_u64(root_first);
            serial_port_puts(" secs=");
            put_u64(root_secs);
            serial_port_puts("\n");

            if (ffs_format(target, root_first, root_secs) != 0) {
                serial_puts("WARN: FFS format failed — continuing without root FS\n");
            } else {
                serial_port_puts("[installer] FFS format ok\n");

                /* Mount FFS at / (replaces ramfs for deploy) */
                uint32_t ffs_id = ffs_mount_dev("/", target,
                                                root_first, root_secs);
                if (ffs_id == 0) {
                    serial_puts("WARN: FFS mount failed — deploying to ramfs\n");
                } else {
                    serial_port_puts("[installer] FFS mounted at /\n");
                }
            }
        }
    }

    /* ---- Step 3: Deploy directory layout to FFS / ---- */
    serial_port_puts("[installer] preparing directory layout...\n");
    tui_screen_progress(2, 1, 3);
    deploy_directory_structure(NULL);

    serial_port_puts("[installer] configuring user accounts...\n");
    configure_users_interactive(target);

    /* ---- Done ---- */
    tui_screen_done(1);
    serial_puts("OK -- installation complete.\n");

    /* Wait for Enter (reboot) or Q (shell) */
    for (;;) {
        int key = read_menu_key();
        if (key == 3) asdinit_shutdown(1);   /* reboot */
        if (key == 4) return 1;              /* drop to shell */
    }
}
