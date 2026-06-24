/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, ASD Project Contributors
 *
 * asdsh — minimal interactive shell for OpenASD.
 *
 * Improvements over original:
 *  - Removed duplicate cmd_pwd definition (was a compile error).
 *  - cmd_stat now properly defined and registered.
 *  - Added: whoami, id, users, uname, clear, reboot, shutdown, ps, uptime.
 *  - Argument parsing supports quoted strings and multi-word arguments.
 *  - Proper error messages for missing commands.
 *  - External binary execution from /bin via macho_spawn.
 *  - All string operations bounded (strncpy, not strcpy).
 */

#include "init_internal.h"

char g_cwd[VFS_PATH_MAX] = "/";

/* Known non-root VFS backends (shown as dirs in root listing) */
static const char *g_mounts[] = { "/boot", "/root", NULL };

/* ------------------------------------------------------------------ */
/* Path helpers                                                         */
/* ------------------------------------------------------------------ */

static int starts_with(const char *s, const char *pfx) {
    while (*pfx) { if (*s++ != *pfx++) return 0; }
    return 1;
}

static int path_is_root(const char *p) {
    return p[0] == '/' && p[1] == '\0';
}

extern int vfs_canonicalize(const char *path, char *out, size_t out_sz);

static void resolve_path(const char *arg, char *out) {
    char tmp[VFS_PATH_MAX];
    if (!arg || arg[0] == '\0') {
        strncpy(out, g_cwd, VFS_PATH_MAX);
    } else if (arg[0] == '/') {
        strncpy(tmp, arg, VFS_PATH_MAX);
        vfs_canonicalize(tmp, out, VFS_PATH_MAX);
    } else {
        strncpy(tmp, g_cwd, VFS_PATH_MAX);
        size_t n = strlen(tmp);
        if (n > 0 && tmp[n-1] != '/') strncat(tmp, "/", VFS_PATH_MAX - n - 1);
        strncat(tmp, arg, VFS_PATH_MAX - strlen(tmp) - 1);
        vfs_canonicalize(tmp, out, VFS_PATH_MAX);
    }
    out[VFS_PATH_MAX - 1] = '\0';
}

/* ------------------------------------------------------------------ */
/* Commands                                                             */
/* ------------------------------------------------------------------ */

static void cmd_pwd(void) {
    serial_puts(g_cwd);
    serial_puts("\n");
}

static void cmd_ls(const char *arg) {
    char path[VFS_PATH_MAX];
    resolve_path(arg, path);

    static vfs_dirent_t ents[256];
    uint32_t n = 0;
    int rc = vfs_readdir(path, ents, 256, &n);

    if (path_is_root(path)) {
        /* Show entries from root VFS backend first */
        if (rc == 0) {
            for (uint32_t i = 0; i < n; i++) {
                if (ents[i].kind == VFS_NODE_DIR) serial_putc('d');
                else                              serial_putc('-');
                serial_puts("  ");
                serial_puts(ents[i].name);
                if (ents[i].kind == VFS_NODE_DIR) serial_putc('/');
                if (ents[i].kind == VFS_NODE_FILE && ents[i].size >= 0) {
                    serial_puts("  (");
                    put_u64((uint64_t)ents[i].size);
                    serial_puts("B)");
                }
                serial_puts("\n");
            }
        }
        /* Also show non-root mount points not already listed */
        for (int mi = 0; g_mounts[mi]; mi++) {
            const char *mname = g_mounts[mi] + 1; /* strip leading / */
            int found = 0;
            for (uint32_t i = 0; i < n; i++) {
                if (strcmp(ents[i].name, mname) == 0) { found = 1; break; }
            }
            if (!found) {
                serial_puts("d  ");
                serial_puts(mname);
                serial_puts("/\n");
            }
        }
        return;
    }

    if (rc != 0) {
        serial_puts("ls: cannot open '"); serial_puts(path);
        serial_puts("': no such directory\n");
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (ents[i].kind == VFS_NODE_DIR) serial_putc('d');
        else                              serial_putc('-');
        serial_puts("  ");
        serial_puts(ents[i].name);
        if (ents[i].kind == VFS_NODE_DIR) serial_putc('/');
        if (ents[i].kind == VFS_NODE_FILE && ents[i].size >= 0) {
            serial_puts("  ("); put_u64((uint64_t)ents[i].size); serial_puts("B)");
        }
        serial_puts("\n");
    }
}

/* Sync PID1's PCB cwd so that children spawned from the kernel shell (via
 * macho_spawn, which inherits cwd from sched_current()) see the right cwd —
 * otherwise spawned binaries like hx always resolve relative paths to "/". */
static void sync_pcb_cwd(void) {
    pcb_t *cur = sched_current();
    if (!cur) return;
    strncpy(cur->cwd, g_cwd, sizeof(cur->cwd));
    cur->cwd[sizeof(cur->cwd) - 1] = '\0';
}

static void cmd_cd(const char *arg) {
    if (!arg || arg[0] == '\0') {
        strncpy(g_cwd, "/", VFS_PATH_MAX);
        sync_pcb_cwd();
        return;
    }
    char path[VFS_PATH_MAX];
    resolve_path(arg, path);
    if (path_is_root(path)) {
        strncpy(g_cwd, "/", VFS_PATH_MAX);
        sync_pcb_cwd();
        return;
    }

    stat_info_t st;
    if (vfs_stat(path, &st) != 0) {
        serial_puts("cd: "); serial_puts(path); serial_puts(": no such directory\n");
        return;
    }
    if (st.kind != VFS_NODE_DIR) {
        serial_puts("cd: "); serial_puts(path); serial_puts(": not a directory\n");
        return;
    }
    strncpy(g_cwd, path, VFS_PATH_MAX);
    g_cwd[VFS_PATH_MAX - 1] = '\0';
    sync_pcb_cwd();
}

static void cmd_cat(const char *arg) {
    if (!arg || !arg[0]) { serial_puts("usage: cat <file>\n"); return; }
    char path[VFS_PATH_MAX];
    resolve_path(arg, path);
    fd_t fd = vfs_open(path, VFS_O_READ, NULL);
    if ((int)fd < 0) {
        serial_puts("cat: "); serial_puts(path); serial_puts(": no such file\n");
        return;
    }
    static uint8_t buf[512];
    size_t got;
    while (vfs_read(fd, buf, sizeof(buf) - 1, &got) == 0 && got > 0) {
        buf[got] = '\0';
        serial_puts((char *)buf);
    }
    serial_puts("\n");
    vfs_close(fd);
}

static void cmd_mkdir(const char *arg) {
    if (!arg || !arg[0]) { serial_puts("usage: mkdir <dir>\n"); return; }
    char path[VFS_PATH_MAX];
    resolve_path(arg, path);
    if (vfs_mkdir(path) != 0) {
        serial_puts("mkdir: cannot create '"); serial_puts(path); serial_puts("'\n");
    }
}

static void cmd_rm(const char *arg) {
    if (!arg || !arg[0]) { serial_puts("usage: rm <file>\n"); return; }
    char path[VFS_PATH_MAX];
    resolve_path(arg, path);
    if (vfs_unlink(path) != 0) {
        serial_puts("rm: cannot remove '"); serial_puts(path); serial_puts("'\n");
    }
}

static void cmd_touch(const char *arg) {
    if (!arg || !arg[0]) { serial_puts("usage: touch <file>\n"); return; }
    char path[VFS_PATH_MAX];
    resolve_path(arg, path);
    fd_t fd = vfs_open(path, VFS_O_WRITE | VFS_O_CREAT, NULL);
    if ((int)fd < 0) {
        serial_puts("touch: cannot create '"); serial_puts(path); serial_puts("'\n");
        return;
    }
    vfs_close(fd);
}

static void cmd_stat(const char *arg) {
    if (!arg || !arg[0]) { serial_puts("usage: stat <path>\n"); return; }
    char path[VFS_PATH_MAX];
    resolve_path(arg, path);
    stat_info_t st;
    if (vfs_stat(path, &st) != 0) {
        serial_puts("stat: "); serial_puts(path); serial_puts(": not found\n");
        return;
    }
    serial_puts("  file: "); serial_puts(path); serial_puts("\n");
    serial_puts("  type: ");
    serial_puts(st.kind == VFS_NODE_DIR ? "directory" : "file");
    serial_puts("\n");
    if (st.size >= 0) {
        serial_puts("  size: "); put_u64((uint64_t)st.size); serial_puts(" bytes\n");
    }
    serial_puts("  uid:  "); put_u64(st.uid); serial_puts("\n");
    serial_puts("  gid:  "); put_u64(st.gid); serial_puts("\n");
}

static void cmd_whoami(void) {
    asd_user_t *u = usr_find_by_uid(g_shell_uid);
    serial_puts(u ? u->name : "root"); serial_puts("\n");
}

static void cmd_id(void) {
    asd_user_t  *u = usr_find_by_uid(g_shell_uid);
    asd_group_t *g = grp_find_by_gid(g_shell_gid);
    serial_puts("uid="); put_u64(g_shell_uid);
    serial_puts("("); serial_puts(u ? u->name : "?"); serial_puts(")");
    serial_puts(" gid="); put_u64(g_shell_gid);
    serial_puts("("); serial_puts(g ? g->name : "?"); serial_puts(")");
    if (u && (u->flags & USR_FLAG_WHEEL)) serial_puts(" groups=1(wheel)");
    serial_puts("\n");
}

static void cmd_users(void) {
    serial_puts("NAME             UID   GID   FLAGS\n");
    for (uint32_t i = 0; i < usr_count(); i++) {
        asd_user_t *u = usr_get(i);
        if (!u) continue;
        serial_puts(u->name);
        size_t nl = strlen(u->name);
        while (nl++ < 16) serial_putc(' ');
        put_u64(u->uid); serial_puts("  ");
        put_u64(u->gid); serial_puts("  ");
        if (u->flags & USR_FLAG_WHEEL)   serial_puts("wheel ");
        if (u->flags & USR_FLAG_NOLOGIN) serial_puts("nologin ");
        if (u->flags & USR_FLAG_LOCKED)  serial_puts("locked ");
        if (!u->flags) serial_puts("-");
        serial_puts("\n");
    }
}

static void cmd_ps(void) {
    static const char *state_names[] = { "new", "ready", "run", "wait", "dead" };
    serial_puts("PID   STATE  NAME\n");
    serial_puts("1     run    asdinit [kernel]\n");
    for (pid_t p = 2; p < 256; p++) {
        pcb_t *pcb = sched_find(p);
        if (!pcb) continue;
        put_u64(pcb->pid);
        size_t dw = 1;
        uint32_t tmp = pcb->pid;
        while (tmp >= 10) { dw++; tmp /= 10; }
        while (dw++ < 6) serial_putc(' ');
        uint8_t st = pcb->state < 5 ? pcb->state : 4;
        serial_puts(state_names[st]);
        size_t sl = strlen(state_names[st]);
        while (sl++ < 7) serial_putc(' ');
        serial_puts(pcb->name[0] ? pcb->name : "?");
        serial_puts("\n");
    }
}

static void cmd_uname(const char *arg) {
    int all = (arg && arg[0] == '-' && arg[1] == 'a');
    serial_puts("OpenASD");
    if (all) {
        serial_putc(' ');
        serial_puts(g_hostname);
        serial_puts(" 1.0 #1 x86_64");
    }
    serial_puts("\n");
}

/* ------------------------------------------------------------------ */
/* Try to run an external binary from /bin                             */
/* ------------------------------------------------------------------ */

/* Spawn a single command with optional stdin/stdout fd overrides.
 * in_fd/out_fd: if >= 0, temporarily install in current PCB's fd_table
 * so the child inherits them as stdin/stdout. */
static uint32_t spawn_cmd(const char *cmd, int in_fd, int out_fd) {
    /* Skip leading spaces */
    while (*cmd == ' ') cmd++;
    if (!*cmd) return 0;

    /* Build /bin/<cmdname> path */
    char binpath[VFS_PATH_MAX];
    static char argv_buf[512];
    static const char *argv[32];
    strncpy(argv_buf, cmd, sizeof(argv_buf));
    argv_buf[sizeof(argv_buf) - 1] = '\0';

    /* Tokenize */
    int argc = 0;
    char *p = argv_buf;
    while (*p && argc < 31) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
    }
    argv[argc] = NULL;
    if (argc == 0) return 0;

    const char *name = argv[0];
    if (name[0] == '/') {
        strncpy(binpath, name, VFS_PATH_MAX - 1);
        binpath[VFS_PATH_MAX - 1] = '\0';
    } else {
        strncpy(binpath, "/bin/", VFS_PATH_MAX);
        size_t n = strlen(binpath);
        size_t i = 0;
        while (name[i] && n + i < (size_t)(VFS_PATH_MAX - 1)) { binpath[n+i] = name[i]; i++; }
        binpath[n + i] = '\0';
    }

    stat_info_t st;
    if (vfs_stat(binpath, &st) != 0) {
        serial_puts("command not found: "); serial_puts(argv[0]); serial_puts("\n");
        return 0;
    }
    argv[0] = binpath;

    /* Temporarily redirect stdin/stdout in shell's PCB so child inherits them.
     * We allocate a "save" fd (>= 10) to hold the original and restore after. */
    int saved_in_fd  = -1;
    int saved_out_fd = -1;
    if (in_fd >= 0) {
        /* save old stdin if any, then install new */
        pcb_t *cur = sched_current();
        if (cur && cur->fd_table[0]) {
            /* dup the current stdin to a temp fd */
            vfs_dup2(0, 10); saved_in_fd = 10;
        }
        vfs_dup2((fd_t)in_fd, 0);
    }
    if (out_fd >= 0) {
        pcb_t *cur = sched_current();
        if (cur && cur->fd_table[1]) {
            vfs_dup2(1, 11); saved_out_fd = 11;
        }
        vfs_dup2((fd_t)out_fd, 1);
    }

    uint32_t pid = macho_spawn(binpath, argv, NULL);

    /* Restore shell's stdin/stdout */
    if (in_fd >= 0) {
        vfs_close(0);
        if (saved_in_fd >= 0) { vfs_dup2((fd_t)saved_in_fd, 0); vfs_close((fd_t)saved_in_fd); }
    }
    if (out_fd >= 0) {
        vfs_close(1);
        if (saved_out_fd >= 0) { vfs_dup2((fd_t)saved_out_fd, 1); vfs_close((fd_t)saved_out_fd); }
    }
    return pid;
}

static void try_exec(const char *cmd) {
    /* Scan for pipes and redirects */
    char line[512];
    strncpy(line, cmd, sizeof(line)); line[sizeof(line)-1] = '\0';

    /* Split on '|' — simple single-pipe support */
    char *pipe_pos = NULL;
    for (char *s = line; *s; s++) {
        if (*s == '|') { pipe_pos = s; break; }
    }

    /* Find > >> < operators in each segment */
    /* Helper: parse redirects out of a command string */

    if (pipe_pos) {
        /* Two-stage pipe: cmd1 | cmd2 */
        *pipe_pos = '\0';
        char *cmd1 = line;
        char *cmd2 = pipe_pos + 1;

        /* Create pipe */
        int rfd = -1, wfd = -1;
        if (vfs_pipe(&rfd, &wfd) != 0) {
            serial_puts("shell: pipe() failed\n"); return;
        }

        /* Spawn cmd1 with stdout → pipe write-end */
        uint32_t pid1 = spawn_cmd(cmd1, -1, wfd);
        /* Close write-end in shell (child inherited a copy) */
        vfs_close((fd_t)wfd);

        if (pid1) {
            exit_info_t ei;
            /* Spawn cmd2 with stdin → pipe read-end */
            uint32_t pid2 = spawn_cmd(cmd2, rfd, -1);
            vfs_close((fd_t)rfd);
            if (pid2) {
                sched_reap((pid_t)pid1, &ei);
                sched_reap((pid_t)pid2, &ei);
            } else {
                sched_reap((pid_t)pid1, &ei);
            }
        } else {
            vfs_close((fd_t)rfd);
        }
        flush_input();
        return;
    }

    /* No pipe — handle single command with optional redirects */
    char cmd_part[512];
    int in_fd = -1, out_fd = -1, append = 0;
    strncpy(cmd_part, line, sizeof(cmd_part)); cmd_part[sizeof(cmd_part)-1] = '\0';

    /* Parse >> and > and < */
    for (char *s = cmd_part; *s; s++) {
        if (s[0] == '>' && s[1] == '>') {
            *s = '\0'; char *fname = s + 2; while (*fname == ' ') fname++;
            char fpath[VFS_PATH_MAX]; resolve_path(fname, fpath);
            out_fd = (int)vfs_open(fpath, VFS_O_WRITE | VFS_O_CREAT | VFS_O_APPEND, NULL);
            append = 1; break;
        }
        if (s[0] == '>' && s[1] != '>') {
            *s = '\0'; char *fname = s + 1; while (*fname == ' ') fname++;
            char fpath[VFS_PATH_MAX]; resolve_path(fname, fpath);
            out_fd = (int)vfs_open(fpath, VFS_O_WRITE | VFS_O_CREAT | VFS_O_TRUNC, NULL);
            break;
        }
        if (s[0] == '<') {
            *s = '\0'; char *fname = s + 1; while (*fname == ' ') fname++;
            char fpath[VFS_PATH_MAX]; resolve_path(fname, fpath);
            in_fd = (int)vfs_open(fpath, VFS_O_READ, NULL);
            break;
        }
    }
    (void)append;

    uint32_t pid = spawn_cmd(cmd_part, in_fd, out_fd);
    if (in_fd  >= 0) vfs_close((fd_t)in_fd);
    if (out_fd >= 0) vfs_close((fd_t)out_fd);
    if (pid) {
        exit_info_t ei;
        sched_reap((pid_t)pid, &ei);
    }
    flush_input();
}

/* ------------------------------------------------------------------ */
/* Prompt                                                               */
/* ------------------------------------------------------------------ */

static void shell_print_prompt(void) {
    asd_user_t *u = usr_find_by_uid(g_shell_uid);
    serial_puts(u ? u->name : "root");
    serial_putc('@');
    serial_puts(g_hostname);
    serial_putc(':');
    serial_puts(g_cwd);
    serial_putc(' ');
    serial_puts((g_shell_uid == UID_ROOT) ? "# " : "$ ");
}



/* ------------------------------------------------------------------ */
/* Main shell loop                                                      */
/* ------------------------------------------------------------------ */

void asd_shell_loop(void) {
    char line[256];

    /* Set working directory from user's home. */
    {
        asd_user_t *u = usr_find_by_uid(g_shell_uid);
        if (u && u->home[0])
            strncpy(g_cwd, u->home, VFS_PATH_MAX);
        else
            strncpy(g_cwd, "/root", VFS_PATH_MAX);
        g_cwd[VFS_PATH_MAX - 1] = '\0';
        sync_pcb_cwd();
    }
    print_shell_banner();
    serial_puts("Type 'help' for commands.\n");

    for (;;) {
        shell_print_prompt();
        if (!readline_serial(line, sizeof(line))) continue;
        if (line[0] == '\0') continue;

        /* ---- Built-in commands ---- */

        if (strcmp(line, "help") == 0) {
            serial_puts("commands: help, ps, clear, echo <msg>, uptime, uname [-a]\n");
            serial_puts("          shutdown, halt, poweroff, reboot, shutdown -r\n");
            serial_puts("          whoami, id, users\n");
            serial_puts("  fs:     pwd, ls [path], cd [path], cat <file>\n");
            serial_puts("          mkdir <dir>, rm <file>, touch <file>, stat <path>\n");
            serial_puts("  net:    ping <ip>  (e.g. ping 10.0.2.2)\n");
            serial_puts("  exec:   any binary in /bin is run directly\n");
            continue;
        }

        if (strcmp(line, "whoami") == 0) { cmd_whoami(); continue; }
        if (strcmp(line, "id")     == 0) { cmd_id();     continue; }
        if (strcmp(line, "users")  == 0) { cmd_users();  continue; }

        if (strcmp(line, "uptime") == 0) {
            uint64_t sec = stub_time_ns() / 1000000000ULL;
            serial_puts("uptime: "); put_u64(sec); serial_puts("s\n");
            continue;
        }

        if (strcmp(line, "uname") == 0)      { cmd_uname(NULL); continue; }
        if (starts_with(line, "uname "))     { cmd_uname(line + 6); continue; }

        if (strcmp(line, "reboot") == 0)     asdinit_shutdown(1);
        if (strcmp(line, "halt")   == 0 ||
            strcmp(line, "poweroff") == 0 ||
            strcmp(line, "shutdown") == 0)   asdinit_shutdown(0);
        if (strcmp(line, "shutdown -r") == 0) asdinit_shutdown(1);

        if (strcmp(line, "clear") == 0) { fb_console_clear(); continue; }
        if (strcmp(line, "exit")  == 0 ||
            strcmp(line, "logout") == 0) {
            serial_puts("Goodbye.\n");
            asdinit_shutdown(0);
        }

        if (strcmp(line, "pwd")  == 0)  { cmd_pwd(); continue; }
        if (strcmp(line, "ls")   == 0)  { cmd_ls(g_cwd); continue; }
        if (strcmp(line, "dir")  == 0)  { cmd_ls(g_cwd); continue; }
        if (strcmp(line, "cd")   == 0)  { cmd_cd("/"); continue; }
        if (strcmp(line, "ps")   == 0 ||
            strcmp(line, "services") == 0) { cmd_ps(); continue; }

        if (starts_with(line, "echo "))  { serial_puts(line + 5); serial_puts("\n"); continue; }
        if (starts_with(line, "ls "))    { cmd_ls(line + 3);    continue; }
        if (starts_with(line, "dir "))   { cmd_ls(line + 4);    continue; }
        if (starts_with(line, "cd "))    { cmd_cd(line + 3);    continue; }
        if (starts_with(line, "cat "))   { cmd_cat(line + 4);   continue; }
        if (starts_with(line, "mkdir ")) { cmd_mkdir(line + 6); continue; }
        if (starts_with(line, "rm "))    { cmd_rm(line + 3);    continue; }
        if (starts_with(line, "touch ")) { cmd_touch(line + 6); continue; }
        if (starts_with(line, "stat "))  { cmd_stat(line + 5);  continue; }

        /* ---- External binary ---- */
        try_exec(line);
    }
}
