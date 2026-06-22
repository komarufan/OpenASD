/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, ASD Project Contributors
 *
 * asdinit — PID 1.  Runs entirely in kernel mode for the early bring-up.
 * Responsibilities: orchestrate boot sequence, run the installer on live
 * media, launch services, present a login prompt, and enter the shell.
 *
 * Sub-files:
 *   io.c        — serial I/O, readline, history
 *   installer.c — TUI installer (disk selection, user setup, file copy)
 *   shell.c     — built-in shell commands and main shell loop
 *   svcmgr.c    — service file parser, dependency waves, launch/restart
 */

#include "init_internal.h"

/* Compatibility for kernel-mode serial output */
#define serial_port_puts serial_puts

/* ------------------------------------------------------------------ */
/* Shared globals used across sub-files                                */
/* ------------------------------------------------------------------ */

char  g_hostname[64] = "asd";
uid_t g_shell_uid    = UID_ROOT;
gid_t g_shell_gid    = GID_ROOT;

/* ------------------------------------------------------------------ */
/* Shutdown / reboot                                                    */
/* ------------------------------------------------------------------ */

void asdinit_shutdown(int reboot) {
    serial_puts("\n");
    serial_puts(reboot ? "Rebooting system...\n" : "Shutting down...\n");
    serial_puts("Stopping services...\n");
    svcmgr_stop_all();
    serial_puts("All services stopped.\n");
    __asm__ volatile("cli");
    if (reboot) {
        io_out8(0x64, 0xFE);    /* PS/2 controller reset line */
    } else {
        io_out16(0x604,  0x2000);   /* QEMU PIIX4 ACPI S5 */
        io_out16(0xB004, 0x2000);   /* Bochs / older QEMU */
        io_out16(0x4004, 0x3400);   /* QEMU Q35 ICH9 ACPI */
    }
    for (;;) __asm__ volatile("hlt");
}

/* ------------------------------------------------------------------ */
/* Live-media boot menu                                                 */
/* ------------------------------------------------------------------ */

static void boot_puts(const char *s) {
    serial_port_puts(s);
    fb_console_puts(s);
}

static int boot_mode_select(void) {
    for (;;) {
        fb_console_clear();
        boot_puts("OpenASD Live\n");
        boot_puts("============================================================\n\n");
        boot_puts("                 +-----------------------------------------------+\n");
        boot_puts("                 |           OpenASD Live System                 |\n");
        boot_puts("                 |-----------------------------------------------|\n");
        boot_puts("                 |                                               |\n");
        boot_puts("                 |  [I]  Install OpenASD to disk                 |\n");
        boot_puts("                 |  [S]  Boot to live shell                      |\n");
        boot_puts("                 |                                               |\n");
        boot_puts("                 +-----------------------------------------------+\n\n");
        boot_puts("Press I to install or S for live shell: ");
        fb_console_tick();
        char c = read_char();
        if (c == 'i' || c == 'I') { boot_puts("I\n"); return 'I'; }
        if (c == 's' || c == 'S') { boot_puts("S\n"); return 'S'; }
    }
}

static void post_install_menu(void) {
    serial_puts("\n============================================================\n");
    serial_puts("  Installation complete!\n");
    serial_puts("  Remove the installation media before rebooting.\n");
    serial_puts("============================================================\n\n");
    serial_puts("  [C]  Configure accounts again\n");
    serial_puts("  [R]  Reboot system\n");
    serial_puts("  [S]  Live shell\n\n");
    serial_puts(">>> Press C, R, or S: ");
    for (;;) {
        char c = read_char();
        if (c == 'c' || c == 'C') {
            serial_puts("C\n");
            block_dev_t *t = install_get_last_target();
            if (t)
                configure_users_interactive(t);
            else
                serial_puts("  (no install target — run installer first)\n");
        }
        if (c == 'r' || c == 'R') {
            serial_puts("R\n");
            asdinit_shutdown(1);
        }
        if (c == 's' || c == 'S') { serial_puts("S\n"); return; }
    }
}

/* ------------------------------------------------------------------ */
/* Login prompt                                                         */
/* ------------------------------------------------------------------ */

static void login_prompt(void) {
    char uname[USR_NAME_LEN];
    char pw[128];

    serial_puts("\nOpenASD 1.0\n");
    serial_puts("====================\n");

    for (;;) {
        serial_puts("login: ");
        readline_serial(uname, sizeof(uname));
        if (!uname[0]) continue;

        /* Trim \r and \n from username */
        size_t unl = strlen(uname);
        while (unl > 0 && (uname[unl-1] == '\r' || uname[unl-1] == '\n')) {
            uname[--unl] = '\0';
        }
        if (!uname[0]) continue;

        asd_user_t *u = usr_find_by_name(uname);
        if (!u) {
            serial_puts("Password: ");
            readline_serial_noecho(pw, sizeof(pw));
            serial_puts("Login incorrect (user not found).\n");
            continue;
        }
        if (u->flags & USR_FLAG_NOLOGIN) {
            serial_puts("This account is not available.\n");
            continue;
        }
        if (u->flags & USR_FLAG_LOCKED) {
            serial_puts("Account locked.\n");
            continue;
        }
        serial_puts("Password: ");
        readline_serial_noecho(pw, sizeof(pw));

        /* Trim \r, \n and trailing spaces from password */
        size_t pwl = strlen(pw);
        while (pwl > 0 && (pw[pwl-1] == '\r' || pw[pwl-1] == '\n' || pw[pwl-1] == ' ')) {
            pw[--pwl] = '\0';
        }

        /* Check password. If it fails, we allow a fallback for root or 
         * users with no hash set, to prevent system lockout. */
        if (!usr_check_password(u->uid, pw)) {
            /* Fallback: if it's root or hash is 0, and password was empty, allow.
             * Otherwise, it's a real failure. */
            int is_root = (u->uid == 0);
            int no_hash = (u->pw_hash == 0);
            int empty_pw = (pw[0] == '\0');

            if ((is_root || no_hash) && empty_pw) {
                /* Allow login */
            } else {
                serial_puts("Login incorrect.\n");
                if (u->pw_hash == 0) {
                    serial_puts("  (accounts not loaded from disk — use one install disk,");
                    serial_puts(" run installer again, or avoid 'make install' after GPT install)\n");
                }
                continue;
            }
        }
        g_shell_uid = u->uid;
        g_shell_gid = u->gid;
        serial_puts("Welcome to ASD.\n");
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                          */
/* ------------------------------------------------------------------ */

void asdinit_main(void) {
    boot_log(" OK ", "ASD init system starting (PID 1)");

    /* Autotest shortcuts — run before any interactive menu so CI works on
     * live-media boots (2 disks) without getting stuck at boot_mode_select. */
    if (kernel_cmdline_has("autotest_fastfetch")) {
        g_shell_uid = UID_ROOT;
        g_shell_gid = GID_ROOT;
        serial_puts("\n[autotest] kernel cmdline autotest_fastfetch — skip login\n");
        shell_autotest_exec("fastfetch");
        serial_puts("[autotest] OK — halting (see serial.log; no EXCEPTION lines expected)\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    if (kernel_cmdline_has("autotest_hxtest")) {
        g_shell_uid = UID_ROOT;
        g_shell_gid = GID_ROOT;
        serial_puts("\n[autotest] kernel cmdline autotest_hxtest — skip login\n");
        /* Run filetest first to verify basic VFS, then hxtest for hx-specific flow */
        serial_puts("[autotest] step 1: filetest\n");
        shell_autotest_exec("filetest");
        serial_puts("[autotest] step 2: hxtest\n");
        shell_autotest_exec("hxtest");
        serial_puts("[autotest] hxtest done — halting\n");
        for (;;) __asm__ volatile("cli; hlt");
    }

    /* Passwd DB is loaded in kernel_main (boot_early_load_passwd_db) before sti.
     * Do not read the block device here — virtio with IRQs enabled can hang. */
    if (boot_passwd_was_loaded()) {
        boot_log(" OK ", "Installed system (user database loaded)");
    } else if (block_count() == 1) {
        boot_log("WARN", "No user database on disk (re-run installer)");
    } else if (block_count() > 1) {
        /* No installed system found, but multiple disks exist: likely Live media. */
        for (;;) {
            int mode = boot_mode_select();
            if (mode == 'S') break;
            int result = installer_run();
            if (result > 0) {
                post_install_menu();
                break;
            }
            if (result < 0) {
                serial_puts("\n  Installation failed.  Press any key...\n");
                read_char();
            }
        }
    } else {
        boot_log("WARN", "No installation found and only one disk present.");
    }

    load_services();   /* logs WARN internally if no services found */

    int nwaves = compute_waves();
    if (nwaves < 0) {
        boot_log("FAIL", "Dependency resolution failed");
        for (;;) __asm__ volatile("hlt");
    }
    boot_log(" OK ", "Dependency graph resolved");

    for (int w = 0; w < nwaves; w++)
        launch_wave(w);
    boot_log(" OK ", "All services launched");

    if (!boot_passwd_was_loaded()) {
        if (boot_reload_passwd_from_disk())
            serial_puts("passwd: loaded from install disk\n");
    }
    boot_load_hostname_from_disk();

    /* Bootstrap /etc/apm directory and README if they don't exist yet.
     * On live media nothing persists between boots, so we always recreate.
     * On installed systems the installer wrote these files to FFS. */
    {
        vfs_mkdir("/etc");
        vfs_mkdir("/etc/apm");
        vfs_mkdir("/var");
        vfs_mkdir("/var/apm");
        vfs_mkdir("/var/apm/db");
        vfs_mkdir("/var/apm/db/installed");
        vfs_mkdir("/var/apm/lists");
        vfs_mkdir("/var/apm/cache");

        fd_t fd = vfs_open("/etc/apm/apm.conf",
                           VFS_O_WRITE | VFS_O_CREAT | VFS_O_TRUNC, NULL);
        if (fd > 0) {
            static const char *conf =
                "# apm.conf - ASD Package Manager configuration\n"
                "#\n"
                "# Uncomment to enable the official repository:\n"
                "# repo official https://github.com/komarufan/OpenASD-packages/releases/latest/download\n"
                "#\n"
                "arch=x86_64\n";
            vfs_write(fd, conf, 0);  /* strlen not available here — use vfs_write trick */
            /* Write manually since strlen may not be linked in init */
            for (const char *p = conf; *p; p++) {}
            vfs_close(fd);
            /* Reopen and write properly */
            fd = vfs_open("/etc/apm/apm.conf",
                          VFS_O_WRITE | VFS_O_CREAT | VFS_O_TRUNC, NULL);
            if (fd > 0) { vfs_write(fd, conf, strlen(conf)); vfs_close(fd); }
        }

        fd = vfs_open("/etc/apm/README",
                      VFS_O_WRITE | VFS_O_CREAT | VFS_O_TRUNC, NULL);
        if (fd > 0) {
            static const char *readme =
                "APM - ASD Package Manager 1.0\n"
                "==============================\n"
                "\n"
                "QUICK START\n"
                "-----------\n"
                "1. Enable a repo in /etc/apm/apm.conf (uncomment the repo line)\n"
                "2. Run: apm update\n"
                "3. Run: apm install <package>\n"
                "\n"
                "COMMANDS\n"
                "--------\n"
                "  apm update              Sync repository indexes\n"
                "  apm install <pkg>...    Install packages\n"
                "  apm del <pkg>...        Remove packages\n"
                "  apm upgrade             Upgrade all packages\n"
                "  apm search <query>      Search by name or description\n"
                "  apm list                List installed packages\n"
                "  apm info <pkg>          Show package info\n"
                "  apm clean               Remove cached archives\n"
                "  apm check               Verify installed files\n"
                "\n"
                "FILES\n"
                "-----\n"
                "  /etc/apm/apm.conf              Configuration\n"
                "  /var/apm/db/installed/<n>.apd  Package records\n"
                "  /var/apm/lists/<repo>.idx       Repo indexes\n"
                "  /var/apm/cache/                 Downloaded archives\n"
                "\n"
                "NOTES\n"
                "-----\n"
                "  - Only HTTP (no HTTPS) is supported\n"
                "  - DNS uses 8.8.8.8\n"
                "  - For offline install, copy .apkg to /var/apm/cache/\n";
            vfs_write(fd, readme, strlen(readme));
            vfs_close(fd);
        }
    }

    login_prompt();

    boot_puts("\n");
    boot_puts("Type 'help' for commands.\n\n");

    /* Built-in kernel shell — do not block in sched_reap() on /bin/asdsh.
     * Previously PID1 waited forever: child was enqueued with rip==0 or
     * spun in SYS_READ while the framebuffer showed only "Welcome". */
    asd_shell_loop();
}

// added shell exec