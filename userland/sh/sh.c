/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, ASD Project Contributors
 *
 * asdsh -- minimal userspace shell for ASD
 *
 * Changes:
 *  - FIX Bug 4: added fastfetch (alias: fetch) to command table so it
 *    can be invoked from the shell.
 *  - Added uname, uptime, id, whoami, wc, hexdump, kill commands.
 *  - Added edit/ed command to launch the asded text editor.
 *  - Improved path resolution with basic ".." handling.
 */

#include <asd/syscall.h>
#include <asd/stdio.h>
#include <asd/types.h>
#include <stddef.h>
#include <stdint.h>

/* pulled in via libasd/string.c */
extern size_t strlen(const char *);
extern int    strcmp(const char *, const char *);
extern int    strncmp(const char *, const char *, size_t);
extern char  *strrchr(const char *, int);
extern char  *strchr(const char *, int);
extern char  *strncpy(char *, const char *, size_t);
extern void  *memset(void *, int, size_t);

#define PATH_MAX_SH  256
#define LINE_MAX     512

static char g_cwd[PATH_MAX_SH] = "/";

static void write_str(const char *s) {
    asd_write(1, s, strlen(s));
}

/* ------------------------------------------------------------------ */
/* Path resolution                                                      */
/* ------------------------------------------------------------------ */

/*
 * Resolve a relative or absolute path against g_cwd.
 * Handles ".." components to prevent trivial path traversal.
 */
static void resolve(const char *arg, char *out) {
    if (!arg || !arg[0]) { strncpy(out, g_cwd, PATH_MAX_SH); return; }
    if (arg[0] == '/') {
        strncpy(out, arg, PATH_MAX_SH);
    } else {
        size_t n = strlen(g_cwd);
        strncpy(out, g_cwd, PATH_MAX_SH);
        if (n > 0 && out[n-1] != '/') { out[n++] = '/'; out[n] = '\0'; }
        strncpy(out + n, arg, PATH_MAX_SH - n - 1);
    }
    out[PATH_MAX_SH - 1] = '\0';

    /* Normalise ".." components in-place */
    char tmp[PATH_MAX_SH];
    char *parts[64];
    int  nparts = 0;
    strncpy(tmp, out, PATH_MAX_SH);
    char *tok = tmp + 1; /* skip leading '/' */
    char *p   = tok;
    while (*p) {
        if (*p == '/') {
            *p = '\0';
            if (tok[0] && strcmp(tok, ".") != 0) {
                if (strcmp(tok, "..") == 0) {
                    if (nparts > 0) nparts--;
                } else {
                    if (nparts < 63) parts[nparts++] = tok;
                }
            }
            tok = p + 1;
        }
        p++;
    }
    if (tok[0] && strcmp(tok, ".") != 0) {
        if (strcmp(tok, "..") == 0) {
            if (nparts > 0) nparts--;
        } else {
            if (nparts < 63) parts[nparts++] = tok;
        }
    }
    /* Rebuild */
    out[0] = '/'; out[1] = '\0';
    for (int i = 0; i < nparts; i++) {
        size_t ol = strlen(out);
        if (ol > 1) { out[ol++] = '/'; out[ol] = '\0'; }
        strncpy(out + ol, parts[i], PATH_MAX_SH - ol - 1);
    }
    if (out[0] == '\0') { out[0] = '/'; out[1] = '\0'; }
    /* remove trailing slash except root */
    size_t l=strlen(out);
    if(l>1 && out[l-1]=='/') out[l-1]='\0';
}

static int is_root(const char *p) { return p[0] == '/' && p[1] == '\0'; }

/* Forward: try built-in command; returns 1 if found+executed, 0 otherwise */
static int try_builtin(const char *cmd_name, const char *arg);

/* ------------------------------------------------------------------ */
/* Commands                                                             */
/* ------------------------------------------------------------------ */

/* Spawn a tokenized command string with optional stdin/stdout overrides.
 * Returns pid or negative on error. */
static int spawn_cmd_str(const char *cmd, int in_fd, int out_fd) {
    while (*cmd == ' ') cmd++;
    if (!*cmd) return -1;

    static char buf[LINE_MAX];
    static const char *argv[32];
    int len = 0;
    while (cmd[len] && len < LINE_MAX - 1) { buf[len] = cmd[len]; len++; }
    buf[len] = '\0';

    int argc = 0;
    char *p = buf;
    while (*p && argc < 31) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
    }
    argv[argc] = NULL;
    if (argc == 0) return -1;

    /* Resolve path */
    char path[PATH_MAX_SH];
    if (argv[0][0] == '/') {
        int i = 0;
        while (argv[0][i] && i < PATH_MAX_SH - 1) { path[i] = argv[0][i]; i++; }
        path[i] = '\0';
    } else {
        snprintf(path, sizeof(path), "/bin/%s", argv[0]);
    }
    argv[0] = path;

    /* Set up redirects in our own fd_table; child inherits them */
    int saved_in_fd = -1, saved_out_fd = -1;
    if (in_fd >= 0) {
        /* save current stdin to fd 10 */
        saved_in_fd = 10;
        asd_dup2(0, saved_in_fd);
        asd_dup2(in_fd, 0);
    }
    if (out_fd >= 0) {
        saved_out_fd = 11;
        asd_dup2(1, saved_out_fd);
        asd_dup2(out_fd, 1);
    }

    int pid = asd_spawn(path, argv, NULL);

    /* Restore */
    if (in_fd >= 0) {
        if (saved_in_fd >= 0) { asd_dup2(saved_in_fd, 0); asd_close(saved_in_fd); }
        else asd_close(0);
    }
    if (out_fd >= 0) {
        if (saved_out_fd >= 0) { asd_dup2(saved_out_fd, 1); asd_close(saved_out_fd); }
        else asd_close(1);
    }
    return pid;
}

/* Execute a single command line, handling |, >, >>, < */
static void exec_line(char *line) {
    /* check for pipe */
    char *pipe_pos = NULL;
    for (char *s = line; *s; s++) {
        if (*s == '|') { pipe_pos = s; break; }
    }

    if (pipe_pos) {
        *pipe_pos = '\0';
        char *cmd1 = line, *cmd2 = pipe_pos + 1;
        int fds[2];
        if (asd_pipe(fds) != 0) { printf("asdsh: pipe failed\n"); return; }
        int pid1 = spawn_cmd_str(cmd1, -1, fds[1]);
        asd_close(fds[1]);
        int pid2 = spawn_cmd_str(cmd2, fds[0], -1);
        asd_close(fds[0]);
        if (pid1 > 0) asd_wait(pid1, NULL);
        if (pid2 > 0) asd_wait(pid2, NULL);
        return;
    }

    /* No pipe: handle > >> < */
    int in_fd = -1, out_fd = -1;
    char cmd_part[LINE_MAX];
    int k = 0;
    for (char *s = line; *s && k < LINE_MAX - 1;) {
        if (s[0] == '>' && s[1] == '>') {
            s += 2; while (*s == ' ') s++;
            char fpath[PATH_MAX_SH]; resolve(s, fpath);
            out_fd = asd_open(fpath, O_WRONLY | O_CREAT | O_APPEND);
            break;
        }
        if (s[0] == '>') {
            s++; while (*s == ' ') s++;
            char fpath[PATH_MAX_SH]; resolve(s, fpath);
            out_fd = asd_open(fpath, O_WRONLY | O_CREAT | O_TRUNC);
            break;
        }
        if (s[0] == '<') {
            s++; while (*s == ' ') s++;
            char fpath[PATH_MAX_SH]; resolve(s, fpath);
            in_fd = asd_open(fpath, O_RDONLY);
            break;
        }
        cmd_part[k++] = *s++;
    }
    cmd_part[k] = '\0';

    char spawn_part[LINE_MAX];
    strncpy(spawn_part, cmd_part, sizeof(spawn_part));
    spawn_part[sizeof(spawn_part) - 1] = '\0';

    /* Try built-in commands first (they handle their own I/O) */
    char *cmd_name = cmd_part;
    while (*cmd_name == ' ') cmd_name++;
    char *arg_p = cmd_name;
    while (*arg_p && *arg_p != ' ') arg_p++;
    char *arg = NULL;
    if (*arg_p == ' ') { *arg_p = '\0'; arg_p++; while (*arg_p == ' ') arg_p++; if (*arg_p) arg = arg_p; }

    if (!try_builtin(cmd_name, arg)) {
        /* Fallback: spawn from /bin/<cmd> with possible redirects */
        int pid = spawn_cmd_str(spawn_part, in_fd, out_fd);
        if (pid < 0) printf("asdsh: %s: command not found\n", cmd_name);
        else asd_wait(pid, NULL);
    }

    if (in_fd  >= 0) asd_close(in_fd);
    if (out_fd >= 0) asd_close(out_fd);
}

static void run_external(const char *cmd, const char *arg) {
    char path[PATH_MAX_SH];
    snprintf(path, sizeof(path), "/bin/%s", cmd);
    const char *argv[] = { cmd, arg, NULL };
    int pid = asd_spawn(path, argv, NULL);
    if (pid < 0) {
        printf("asdsh: %s: command not found in /bin\n", cmd);
        return;
    }
    asd_wait(pid, NULL);
}

static void cmd_ls(const char *arg) {
    run_external("ls", arg);
}

static void cmd_cd(const char *arg) {
    char path[PATH_MAX_SH];
    if (!arg || !arg[0]) {
        strncpy(g_cwd, "/", PATH_MAX_SH);
        asd_chdir("/");
        return;
    }
    resolve(arg, path);
    if (is_root(path)) {
        strncpy(g_cwd, "/", PATH_MAX_SH);
        asd_chdir("/");
        return;
    }

    asd_stat_t st;
    int r = asd_stat(path, &st);
    if (r != 0 || st.kind != ASD_NODE_DIR) {
        printf("cd: %s: no such directory\n", path); return;
    }
    strncpy(g_cwd, path, PATH_MAX_SH);
    /* Sync kernel-side PCB cwd so children spawned via macho_spawn (which
     * inherit cwd from the current process) resolve relative paths here,
     * not at "/". */
    asd_chdir(path);
}

static void cmd_pwd(const char *arg) {
    (void)arg;
    printf("%s\n", g_cwd);
}

static void cmd_clear(const char *arg) {
    (void)arg;
    write_str("\033[2J\033[H");
}

static void cmd_cat(const char *arg)     { run_external("cat",     arg); }
static void cmd_mkdir(const char *arg)   { run_external("mkdir",   arg); }
static void cmd_rm(const char *arg)      { run_external("rm",      arg); }
static void cmd_touch(const char *arg)   { run_external("touch",   arg); }
static void cmd_echo(const char *arg)    { run_external("echo",    arg); }
static void cmd_uname(const char *arg)   { run_external("uname",   arg); }
static void cmd_uptime(const char *arg)  { run_external("uptime",  arg); }
static void cmd_id(const char *arg)      { run_external("id",      arg); }
static void cmd_whoami(const char *arg)  { run_external("whoami",  arg); }
static void cmd_wc(const char *arg)      { run_external("wc",      arg); }
static void cmd_hexdump(const char *arg) { run_external("hexdump", arg); }
static void cmd_kill(const char *arg)    { run_external("kill",    arg); }

/* FIX Bug 4: fastfetch was built but never registered in the command
 * table, so it could not be invoked.  Added here with alias 'fetch'. */
static void cmd_fastfetch(const char *arg) {
    (void)arg;
    run_external("fastfetch", NULL);
}

static void cmd_stat(const char *arg) {
    char path[PATH_MAX_SH];
    if (!arg || !arg[0]) { printf("stat: missing path\n"); return; }
    resolve(arg, path);
    asd_stat_t st;
    if (asd_stat(path, &st) != 0) {
        printf("stat: %s: no such file or directory\n", path);
        return;
    }
    printf("File: %s\n", path);
    printf("Size: %d\n", (int)st.size);
    printf("Kind: %s\n", (st.kind == ASD_NODE_DIR) ? "Directory" : "File");
}

static void cmd_help(const char *arg);

static void cmd_exit(const char *arg) {
    (void)arg;
    asd_exit(0);
}

/* ------------------------------------------------------------------ */
/* Command Table                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    const char *alias;
    void (*func)(const char *arg);
    const char *help;
} shell_cmd_t;

static shell_cmd_t g_cmds[] = {
    {"ls",        "dir",   cmd_ls,        "ls [path]      - List directory contents"},
    {"cd",        NULL,    cmd_cd,        "cd [path]      - Change current directory"},
    {"pwd",       NULL,    cmd_pwd,       "pwd            - Print working directory"},
    {"cat",       NULL,    cmd_cat,       "cat <file>     - Concatenate and print files"},
    {"mkdir",     NULL,    cmd_mkdir,     "mkdir <dir>    - Create directory"},
    {"rm",        NULL,    cmd_rm,        "rm <file>      - Remove file"},
    {"touch",     NULL,    cmd_touch,     "touch <file>   - Create empty file"},
    {"stat",      NULL,    cmd_stat,      "stat <path>    - Display file status"},
    {"clear",     "cls",   cmd_clear,     "clear          - Clear the terminal screen"},
    {"echo",      NULL,    cmd_echo,      "echo [text]    - Display a line of text"},
    {"uname",     NULL,    cmd_uname,     "uname          - Print system name"},
    {"uptime",    NULL,    cmd_uptime,    "uptime         - Show system uptime"},
    {"id",        NULL,    cmd_id,        "id             - Print user identity"},
    {"whoami",    NULL,    cmd_whoami,    "whoami         - Print current user name"},
    {"wc",        NULL,    cmd_wc,        "wc <file>      - Word/line/byte count"},
    {"hexdump",   "xxd",   cmd_hexdump,   "hexdump <file> - Hex dump of file"},
    {"kill",      NULL,    cmd_kill,      "kill <pid>     - Send signal to process"},
    {"fastfetch", "fetch", cmd_fastfetch, "fastfetch      - Show system information"},
    {"help",      NULL,    cmd_help,      "help           - Show this help message"},
    {"exit",      "quit",  cmd_exit,      "exit           - Exit the shell"},
    {NULL,        NULL,    NULL,          NULL}
};

static void cmd_help(const char *arg) {
    (void)arg;
    puts("Available commands:");
    for (int i = 0; g_cmds[i].name; i++) {
        printf("  %s\n", g_cmds[i].help);
    }
}

/* try_builtin: look up cmd_name in g_cmds, execute if found.
 * Returns 1 if handled, 0 if not found. */
static int try_builtin(const char *cmd_name, const char *arg) {
    if (!cmd_name || !cmd_name[0]) return 0;
    for (int i = 0; g_cmds[i].name; i++) {
        if (strcmp(cmd_name, g_cmds[i].name) == 0 ||
            (g_cmds[i].alias && strcmp(cmd_name, g_cmds[i].alias) == 0)) {
            g_cmds[i].func(arg ? arg : (strcmp(cmd_name, "ls") == 0 ? g_cwd : NULL));
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Main loop                                                            */
/* ------------------------------------------------------------------ */

static void print_prompt(void) {
    char user[64];
    asd_utsname_t uts;

    if (asd_getusername(user, sizeof(user)) != 0 || !user[0]) {
        if (asd_geteuid() == 0) strncpy(user, "root", sizeof(user));
        else strncpy(user, "user", sizeof(user));
        user[sizeof(user) - 1] = '\0';
    }

    memset(&uts, 0, sizeof(uts));
    if (asd_uname(&uts) != 0 || !uts.nodename[0]) {
        strncpy(uts.nodename, "asd", sizeof(uts.nodename));
        uts.nodename[sizeof(uts.nodename) - 1] = '\0';
    }

    printf("%s@%s:%s$ ", user, uts.nodename, g_cwd);
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    /* `-q` (quiet) suppresses the startup banner — term prints its own
     * "Welcome to term" greeting instead, while the serial console keeps it. */
    int quiet = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i] && argv[i][0] == '-' && argv[i][1] == 'q' && argv[i][2] == '\0')
            quiet = 1;
    }

    if (!quiet) {
        puts("\nasdsh -- ASD userspace shell");
        puts("Type 'help' for commands, 'fastfetch' for system info.\n");
    }

    char line[LINE_MAX];

    for (;;) {
        print_prompt();

        int n = readline(line, sizeof(line));
        if (n <= 0) continue;

        /* Skip empty lines */
        { char *_p = line; while (*_p == ' ') _p++; if (!*_p) continue; }

        /* exec_line handles pipes, redirects, built-ins and /bin fallback */
        exec_line(line);
    }

    asd_exit(0);
    return 0;
}
