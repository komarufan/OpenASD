/*
 * apm — ASD Package Manager v1.0
 *
 * Inspired by apk (Alpine) and xbps (Void Linux).
 * Manages binary packages for OpenASD x86-64.
 *
 * Layout:
 *   /etc/apm/apm.conf              — configuration (repos, options)
 *   /var/apm/db/installed/<n>.apd  — installed package records
 *   /var/apm/lists/<repo>.idx      — cached repository indexes
 *   /var/apm/cache/                — downloaded package archives
 *
 * Package archive format (.apkg):
 *   "APKG" magic  (4 bytes)
 *   version       (uint16_t LE, = 1)
 *   flags         (uint16_t LE, = 0)
 *   meta_len      (uint32_t LE)
 *   meta          (meta_len bytes, key=value lines)
 *   nfiles        (uint32_t LE)
 *   per file:
 *     path_len    (uint16_t LE)
 *     path        (path_len bytes, absolute, e.g. "/bin/grep")
 *     data_len    (uint32_t LE)
 *     data        (data_len bytes)
 *
 * Repository index format (.idx):
 *   Lines of text. Records separated by "---\n".
 *   First record is the repo header, rest are packages.
 *   Each field: "key=value\n"
 *
 * Usage:
 *   apm update                 — fetch/refresh repository indexes
 *   apm install <pkg>...       — install packages
 *   apm del <pkg>...           — remove packages
 *   apm upgrade                — upgrade all installed packages
 *   apm search <query>         — search available packages
 *   apm list                   — list installed packages
 *   apm info <pkg>             — show package information
 *   apm clean                  — remove cached archives
 *   apm check                  — verify installed package files
 */

#include <asd/syscall.h>
#include <stdint.h>

/* ================================================================
 * Constants
 * ================================================================ */

#define APM_VERSION      "1.0"
#define APM_ARCH         "x86_64"

#define APM_CONF         "/etc/apm/apm.conf"
#define APM_DB_DIR       "/var/apm/db/installed"
#define APM_LISTS_DIR    "/var/apm/lists"
#define APM_CACHE_DIR    "/var/apm/cache"

#define MAX_REPOS         16
#define MAX_PKGS          256
#define MAX_FILES_PER_PKG 128
#define MAX_DEPS           32

#define PKG_NAME_LEN     64
#define PKG_VER_LEN      32
#define PKG_DESC_LEN     256
#define PKG_CKSUM_LEN    80
#define URL_LEN          2048
#define PATH_LEN         256
#define LINE_LEN         512

/* ================================================================
 * Types
 * ================================================================ */

typedef struct {
    char name[PKG_NAME_LEN];
    char url[URL_LEN];
    int  enabled;
} Repo;

typedef struct {
    char name[PKG_NAME_LEN];
    char version[PKG_VER_LEN];
    char arch[16];
    char description[PKG_DESC_LEN];
    char filename[PKG_NAME_LEN + PKG_VER_LEN + 20]; /* <name>-<ver>-<arch>.apkg */
    char checksum[PKG_CKSUM_LEN];
    long size;           /* archive size in bytes */
    long installed_size; /* unpacked size in bytes */
    char depends[MAX_DEPS][PKG_NAME_LEN];
    int  ndepends;
    char repo[PKG_NAME_LEN]; /* which repo this came from */
    /* installed-only fields */
    char files[MAX_FILES_PER_PKG][PATH_LEN];
    int  nfiles;
} PkgInfo;

typedef struct {
    Repo repos[MAX_REPOS];
    int  nrepos;
    char arch[16];
    char db_dir[PATH_LEN];
    char cache_dir[PATH_LEN];
} Config;

/* ================================================================
 * I/O helpers
 * ================================================================ */

static void out(const char *s) {
    int n = 0; while (s[n]) n++;
    if (n) asd_write(1, s, (size_t)n);
}
static void outn(const char *s) { out(s); asd_write(1, "\n", 1); }
static void err(const char *s)  { asd_write(2, s, (size_t)({ int n=0; while(s[n])n++; n; })); }
static void errn(const char *s) {
    int n = 0; while (s[n]) n++;
    asd_write(2, s, (size_t)n); asd_write(2, "\n", 1);
}

/* ANSI colours */
#define C_RESET  "\x1b[0m"
#define C_BOLD   "\x1b[1m"
#define C_GREEN  "\x1b[1;32m"
#define C_CYAN   "\x1b[1;36m"
#define C_YELLOW "\x1b[1;33m"
#define C_RED    "\x1b[1;31m"
#define C_DIM    "\x1b[2m"

static void outnum(long n) {
    char buf[24]; int i = 24; int neg = n < 0;
    if (neg) n = -n;
    if (n == 0) { asd_write(1, "0", 1); return; }
    while (n > 0) { buf[--i] = '0' + (int)(n % 10); n /= 10; }
    if (neg) buf[--i] = '-';
    asd_write(1, buf + i, (size_t)(24 - i));
}

static void out_u16(uint16_t n) {
    outnum((long)n);
}

static void outsz(long bytes) {
    if (bytes < 1024) { outnum(bytes); out(" B"); }
    else if (bytes < 1024*1024) { outnum(bytes/1024); out(" KiB"); }
    else { outnum(bytes/(1024*1024)); out(" MiB"); }
}

/* ── progress bar (xbps-style) ──────────────────────────────────────────
 * Prints: label [████████████--------] 62%  123 KiB
 * Call with known = 0 to show spinner during header fetch.
 * ─────────────────────────────────────────────────────────────────────*/
#define PBAR_WIDTH 30

static void draw_progress(const char *label, long done, long total) {
    (void)total;
    out("\x1b[2K\r");
    out(C_BOLD); out(label); out(C_RESET " [");
    /* grow 1 char per KiB, capped at PBAR_WIDTH */
    int filled = (int)(done / 1024);
    if (filled > PBAR_WIDTH) filled = PBAR_WIDTH;
    out(C_GREEN);
    for (int i = 0; i < filled; i++)          out("#");
    out(C_RESET);
    for (int i = filled; i < PBAR_WIDTH; i++) out("-");
    out("] ");
    outsz(done);
}

static void finish_progress(const char *label, long total) {
    out("\x1b[2K\r");
    out(C_GREEN C_BOLD); out(label); out(C_RESET " [");
    out(C_GREEN);
    for (int i = 0; i < PBAR_WIDTH; i++) out("#");
    out(C_RESET "] ");
    outsz(total);
    out("\n");
}

/* ================================================================
 * String helpers
 * ================================================================ */

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static int seq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static int spfx(const char *s, const char *prefix) {
    while (*prefix) { if (*s++ != *prefix++) return 0; }
    return 1;
}

/* case-insensitive substring search */
static int scontains(const char *hay, const char *needle) {
    int hl = slen(hay), nl = slen(needle);
    for (int i = 0; i <= hl - nl; i++) {
        int ok = 1;
        for (int j = 0; j < nl; j++) {
            char a = hay[i+j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { ok = 0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

static void scopy(char *dst, const char *src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void scat(char *dst, const char *src, int max) {
    int dl = slen(dst);
    scopy(dst + dl, src, max - dl);
}

static void snum(char *dst, long n, int max) {
    char tmp[24]; int i = 24; int neg = n < 0;
    if (neg) n = -n;
    if (n == 0) { scopy(dst, "0", max); return; }
    while (n > 0) { tmp[--i] = '0' + (int)(n % 10); n /= 10; }
    if (neg) tmp[--i] = '-';
    scopy(dst, tmp + i, max);
}

/* Trim trailing newline/whitespace */
static void strim(char *s) {
    int n = slen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' '))
        s[--n] = 0;
}

/* ================================================================
 * File I/O utilities
 * ================================================================ */

static int file_exists(const char *path) {
    asd_stat_t st;
    return asd_stat(path, &st) == 0;
}

static int is_dir(const char *path) {
    asd_stat_t st;
    if (asd_stat(path, &st) != 0) return 0;
    return st.kind == ASD_NODE_DIR;
}

/* Ensure directory exists (create if needed, including parents) */
static void mkdir_p(const char *path) {
    char tmp[PATH_LEN];
    scopy(tmp, path, PATH_LEN);
    for (int i = 1; tmp[i]; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            asd_mkdir(tmp);
            tmp[i] = '/';
        }
    }
    asd_mkdir(tmp);
}

/* Read entire file into caller-provided buffer. Returns bytes read or -1. */
static long read_file(const char *path, char *buf, long cap) {
    int fd = asd_open(path, O_RDONLY);
    if (fd < 0) return -1;
    long total = 0;
    while (total < cap - 1) {
        long r = asd_read(fd, buf + total, (size_t)(cap - 1 - total));
        if (r <= 0) break;
        total += r;
        asd_yield();
    }
    buf[total] = 0;
    asd_close(fd);
    return total;
}

/* Write entire buffer to file (O_CREAT|O_TRUNC). Returns 0 on success. */
static int write_file(const char *path, const char *buf, long len) {
    int fd = asd_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    long done = 0;
    while (done < len) {
        long w = asd_write(fd, buf + done, (size_t)(len - done));
        if (w <= 0) { asd_close(fd); return -1; }
        done += w;
        asd_yield();
    }
    asd_close(fd);
    return 0;
}

/* Append a line to a file */
static int append_line(const char *path, const char *line) {
    int fd = asd_open(path, O_WRONLY | O_CREAT | O_APPEND);
    if (fd < 0) return -1;
    asd_write(fd, line, (size_t)slen(line));
    asd_write(fd, "\n", 1);
    asd_close(fd);
    return 0;
}

/* ================================================================
 * Key=value parser helpers
 * ================================================================ */

/* Given a block of text `data`, find the value for `key=`.
 * Result is written to `out[out_max]`. Returns 1 if found. */
static int kv_get(const char *data, const char *key, char *out_val, int out_max) {
    int kl = slen(key);
    const char *p = data;
    while (*p) {
        if (spfx(p, key) && p[kl] == '=') {
            p += kl + 1;
            int i = 0;
            while (*p && *p != '\n' && *p != '\r' && i < out_max - 1)
                out_val[i++] = *p++;
            out_val[i] = 0;
            return 1;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return 0;
}

/* ================================================================
 * Config parser
 * ================================================================ */

static Config g_cfg;
static char   g_cfg_buf[8192];

static void cfg_defaults(Config *c) {
    c->nrepos = 0;
    scopy(c->arch,      APM_ARCH,     16);
    scopy(c->db_dir,    APM_DB_DIR,   PATH_LEN);
    scopy(c->cache_dir, APM_CACHE_DIR, PATH_LEN);
}

static int cfg_load(Config *c) {
    cfg_defaults(c);
    long n = read_file(APM_CONF, g_cfg_buf, (long)sizeof(g_cfg_buf));
    if (n < 0) return 0; /* no config yet, use defaults */

    const char *p = g_cfg_buf;
    while (*p) {
        /* skip blank lines and comments */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n') {
            while (*p && *p != '\n') p++;
            if (*p) p++;
            continue;
        }

        char line[LINE_LEN];
        int li = 0;
        while (*p && *p != '\n' && li < LINE_LEN - 1) line[li++] = *p++;
        line[li] = 0;
        if (*p == '\n') p++;
        strim(line);
        if (!line[0]) continue;

        if (spfx(line, "repo ")) {
            /* repo <name> <url> */
            char *rest = line + 5;
            while (*rest == ' ') rest++;
            char rname[PKG_NAME_LEN], rurl[URL_LEN];
            int i = 0;
            while (*rest && *rest != ' ' && i < PKG_NAME_LEN - 1)
                rname[i++] = *rest++;
            rname[i] = 0;
            while (*rest == ' ') rest++;
            scopy(rurl, rest, URL_LEN);
            strim(rurl);
            if (rname[0] && rurl[0] && c->nrepos < MAX_REPOS) {
                scopy(c->repos[c->nrepos].name, rname, PKG_NAME_LEN);
                scopy(c->repos[c->nrepos].url,  rurl,  URL_LEN);
                c->repos[c->nrepos].enabled = 1;
                c->nrepos++;
            }
        } else if (spfx(line, "arch=")) {
            scopy(c->arch, line + 5, 16);
        } else if (spfx(line, "db_dir=")) {
            scopy(c->db_dir, line + 7, PATH_LEN);
        } else if (spfx(line, "cache_dir=")) {
            scopy(c->cache_dir, line + 10, PATH_LEN);
        }
    }
    return 1;
}

/* ================================================================
 * Package database (per-package .apd files)
 * ================================================================ */

/* Build path to installed package record */
static void db_path(char *out, int max, const char *pkgname) {
    scopy(out, APM_DB_DIR, max);
    scat(out, "/", max);
    scat(out, pkgname, max);
    scat(out, ".apd", max);
}

/* Serialize PkgInfo to text */
static long pkg_serialize(const PkgInfo *p, char *buf, long cap) {
    long n = 0;
#define EMIT(k, v) do { \
    int kl = slen(k), vl = slen(v); \
    if (n + kl + vl + 2 < cap) { \
        for (int _i = 0; _i < kl; _i++) buf[n++] = k[_i]; \
        buf[n++] = '='; \
        for (int _i = 0; _i < vl; _i++) buf[n++] = v[_i]; \
        buf[n++] = '\n'; \
    } } while(0)
#define EMITL(k, v) do { char _t[32]; snum(_t, v, 32); EMIT(k, _t); } while(0)

    EMIT("name",        p->name);
    EMIT("version",     p->version);
    EMIT("arch",        p->arch);
    EMIT("description", p->description);
    EMIT("checksum",    p->checksum);
    EMITL("size",           p->size);
    EMITL("installed_size", p->installed_size);
    EMIT("repo",        p->repo);

    /* depends */
    {
        char deps[512]; deps[0] = 0;
        for (int i = 0; i < p->ndepends; i++) {
            if (i) scat(deps, " ", 512);
            scat(deps, p->depends[i], 512);
        }
        EMIT("depends", deps);
    }

    /* files */
    for (int i = 0; i < p->nfiles; i++) {
        EMIT("file", p->files[i]);
    }
    buf[n] = 0;
    return n;
}

/* Parse PkgInfo from .apd text (or index record) */
static void pkg_parse(PkgInfo *p, const char *data) {
    char tmp[512];
    __builtin_memset(p, 0, sizeof(*p));
    kv_get(data, "name",           p->name,        PKG_NAME_LEN);
    kv_get(data, "version",        p->version,     PKG_VER_LEN);
    kv_get(data, "arch",           p->arch,        16);
    kv_get(data, "description",    p->description, PKG_DESC_LEN);
    kv_get(data, "filename",       p->filename,    sizeof(p->filename));
    kv_get(data, "checksum",       p->checksum,    PKG_CKSUM_LEN);
    kv_get(data, "repo",           p->repo,        PKG_NAME_LEN);
    if (kv_get(data, "size", tmp, 32))
        for (int i = 0; tmp[i]; i++) p->size = p->size * 10 + (tmp[i] - '0');
    if (kv_get(data, "installed_size", tmp, 32))
        for (int i = 0; tmp[i]; i++) p->installed_size = p->installed_size * 10 + (tmp[i] - '0');

    /* parse depends (space-separated) */
    if (kv_get(data, "depends", tmp, 512)) {
        int i = 0;
        char *tp = tmp;
        while (*tp && p->ndepends < MAX_DEPS) {
            while (*tp == ' ') tp++;
            if (!*tp) break;
            i = 0;
            while (*tp && *tp != ' ' && i < PKG_NAME_LEN - 1)
                p->depends[p->ndepends][i++] = *tp++;
            p->depends[p->ndepends][i] = 0;
            if (i) p->ndepends++;
        }
    }

    /* parse file list */
    const char *q = data;
    while (*q) {
        if (spfx(q, "file=") && p->nfiles < MAX_FILES_PER_PKG) {
            q += 5;
            int fi = 0;
            while (*q && *q != '\n' && fi < PATH_LEN - 1)
                p->files[p->nfiles][fi++] = *q++;
            p->files[p->nfiles][fi] = 0;
            if (fi) p->nfiles++;
        } else {
            while (*q && *q != '\n') q++;
        }
        if (*q == '\n') q++;
    }
}

/* Check if package is installed */
static int db_installed(const char *name) {
    char path[PATH_LEN];
    db_path(path, PATH_LEN, name);
    return file_exists(path);
}

/* Load installed package info */
static int db_load(const char *name, PkgInfo *p) {
    char path[PATH_LEN];
    db_path(path, PATH_LEN, name);
    char buf[8192];
    if (read_file(path, buf, sizeof(buf)) < 0) return 0;
    pkg_parse(p, buf);
    return 1;
}

/* Save installed package record */
static int db_save(const PkgInfo *p) {
    mkdir_p(APM_DB_DIR);
    char path[PATH_LEN];
    db_path(path, PATH_LEN, p->name);
    char buf[16384];
    long n = pkg_serialize(p, buf, sizeof(buf));
    return write_file(path, buf, n);
}

/* Remove package record */
static int db_remove(const char *name) {
    char path[PATH_LEN];
    db_path(path, PATH_LEN, name);
    return asd_unlink(path);
}

/* ================================================================
 * Repository index parser
 * ================================================================ */

static char g_idx_bufs[MAX_REPOS][65536];

/* Load cached index for repo into g_idx_bufs[idx_slot].
 * Returns number of bytes or -1 if not cached. */
static long idx_load_cached(int slot, const char *repo_name) {
    char path[PATH_LEN];
    scopy(path, APM_LISTS_DIR, PATH_LEN);
    scat(path, "/", PATH_LEN);
    scat(path, repo_name, PATH_LEN);
    scat(path, ".idx", PATH_LEN);
    return read_file(path, g_idx_bufs[slot], sizeof(g_idx_bufs[slot]));
}

/* Iterate over package records in an index buffer.
 * Returns pointer to next record start after `pos`, or NULL at end.
 * Copies the record text (up to sep "---") into `rec[rec_max]`. */
static const char *idx_next_record(const char *pos, char *rec, int rec_max) {
    if (!pos || !*pos) return NULL;
    int i = 0;
    while (*pos) {
        /* detect record separator */
        if (pos[0] == '-' && pos[1] == '-' && pos[2] == '-') {
            while (*pos && *pos != '\n') pos++;
            if (*pos == '\n') pos++;
            rec[i] = 0;
            return pos;
        }
        if (*pos != '\r') {  /* skip CR in CRLF line endings */
            if (i < rec_max - 1) rec[i++] = *pos;
        }
        pos++;
    }
    rec[i] = 0;
    return (*rec) ? pos : NULL;
}

/* Find package by name in all cached repo indexes.
 * Returns 1 and fills p if found. */
static int idx_find(const char *name, PkgInfo *p) {
    for (int ri = 0; ri < g_cfg.nrepos; ri++) {
        char rec[4096];
        const char *pos = g_idx_bufs[ri];
        /* skip header record */
        pos = idx_next_record(pos, rec, sizeof(rec));
        while (pos) {
            char tmpname[PKG_NAME_LEN];
            if (kv_get(rec, "name", tmpname, PKG_NAME_LEN) && seq(tmpname, name)) {
                pkg_parse(p, rec);
                scopy(p->repo, g_cfg.repos[ri].name, PKG_NAME_LEN);
                return 1;
            }
            pos = idx_next_record(pos, rec, sizeof(rec));
        }
    }
    return 0;
}

/* Load all repo indexes from disk into g_idx_bufs */
static void idx_load_all(void) {
    for (int ri = 0; ri < g_cfg.nrepos; ri++) {
        g_idx_bufs[ri][0] = 0;
        idx_load_cached(ri, g_cfg.repos[ri].name);
    }
}

/* ================================================================
 * HTTP/1.0 GET over TCP
 * Supports http://host/path (no HTTPS — no TLS in v1).
 * ================================================================ */

/* Parse "http[s]://host[:port]/path" into components.
 * Returns 0 on success. Sets *is_tls=1 for https. */
static int url_parse(const char *url,
                     char *host, int host_max,
                     uint16_t *port_out, int *is_tls,
                     char *path, int path_max) {
    const char *p;
    if (spfx(url, "https://")) {
        *is_tls = 1; *port_out = 443; p = url + 8;
    } else if (spfx(url, "http://")) {
        *is_tls = 0; *port_out = 80;  p = url + 7;
    } else {
        errn("apm: URL must start with http:// or https://");
        return -1;
    }

    /* host */
    int hi = 0;
    while (*p && *p != '/' && *p != ':' && hi < host_max - 1)
        host[hi++] = *p++;
    host[hi] = 0;

    /* optional port override (e.g. http://host:8080/path) */
    if (*p == ':') {
        p++;
        uint16_t pt = 0;
        while (*p >= '0' && *p <= '9') { pt = (uint16_t)(pt * 10 + (*p - '0')); p++; }
        if (pt) *port_out = pt;
    }

    /* path */
    int pi = 0;
    if (*p == '/') {
        while (*p && pi < path_max - 1) path[pi++] = *p++;
    } else {
        path[0] = '/'; path[1] = 0; pi = 1;
    }
    path[pi] = 0;
    return 0;
}

/* Resolve relative Location header against current URL */
static void resolve_location(const char *base_url, const char *loc,
                             char *out, int max) {
    if (spfx(loc, "http://") || spfx(loc, "https://")) {
        scopy(out, loc, max); return;
    }
    /* Relative: take scheme + host from base, append loc */
    scopy(out, base_url, max);
    for (int i = slen(out) - 1; i >= 0; i--) {
        if (out[i] == '/') { out[i+1] = 0; break; }
    }
    if (loc[0] == '/') {
        int slashes = 0, host_end = 0;
        for (int i = 0; out[i]; i++) {
            if (out[i] == '/') slashes++;
            if (slashes == 3) { host_end = i; break; }
        }
        if (host_end) out[host_end] = 0;
    }
    scat(out, loc, max);
}

/* HTTP/1.0 GET — downloads URL content into dest_path.
 * Supports http:// and https:// (TLS 1.3).
 * Follows up to 5 redirects (3xx with Location header).
 * Returns 0 on success. */
static int http_get(const char *url, const char *dest_path, const char *label) {
    char cur_url[URL_LEN];
    scopy(cur_url, url, URL_LEN);
    int redirects = 0;

    for (;;) {
        char host[256], path[URL_LEN];
        uint16_t port = 80;
        int is_tls = 0;

        if (url_parse(cur_url, host, sizeof(host), &port, &is_tls, path, sizeof(path)) != 0)
            return -1;

        out("  url: "); outn(cur_url);
        out("  resolving: "); outn(host);

        uint32_t ip = 0;
        if (asd_dns_resolve(host, &ip) != 0) {
            errn("apm: DNS resolution failed");
            return -1;
        }

        out("  connecting: "); out(host); out(":"); out_u16(port);
        outn(is_tls ? " (TLS)" : "");

        int tcp_conn = -1;
        if (asd_tcp_connect(ip, port, &tcp_conn) != 0) {
            errn("apm: TCP connect failed"); return -1;
        }

        int tls_conn = -1;
        if (is_tls) {
            outn("  TLS handshake...");
            tls_conn = asd_tls_connect(tcp_conn, host);
            if (tls_conn < 0) {
                errn("apm: TLS handshake failed");
                asd_tcp_close(tcp_conn); return -1;
            }
        }

        /* Build HTTP/1.0 GET request */
        char req[4096]; req[0] = 0;
        scopy(req, "GET ", sizeof(req)); scat(req, path, sizeof(req));
        scat(req, " HTTP/1.0\r\nHost: ", sizeof(req)); scat(req, host, sizeof(req));
        scat(req, "\r\nUser-Agent: apm/1.0\r\nConnection: close\r\n\r\n", sizeof(req));

        out("  GET "); outn(path);

        int send_ok;
        if (is_tls) send_ok = (asd_tls_send(tls_conn, req, (size_t)slen(req)) >= 0);
        else         send_ok = (asd_tcp_send(tcp_conn, req, (size_t)slen(req)) >= 0);

        if (!send_ok) {
            errn("apm: HTTP send failed");
            if (is_tls) asd_tls_close(tls_conn);
            asd_tcp_close(tcp_conn); return -1;
        }

        /* Read response headers into buf, then process */
        static char buf[16384];
        int buf_n = 0;
        int eoh = -1; /* position of \r\n\r\n */
        int status_code = 0;
        char location[URL_LEN]; location[0] = 0;

        outn("  waiting for response headers...");

        while (eoh < 0) {
            long n = is_tls
                     ? asd_tls_recv(tls_conn, buf + buf_n, (size_t)(sizeof(buf) - 1 - buf_n), 1)
                     : asd_tcp_recv(tcp_conn, buf + buf_n, (size_t)(sizeof(buf) - 1 - buf_n), 1);
            if (n <= 0) break;
            buf_n += (int)n;
            asd_yield();
            buf[buf_n] = 0;
            /* search for \r\n\r\n */
            for (int i = 0; i < buf_n - 3; i++) {
                if (buf[i] == '\r' && buf[i+1] == '\n' &&
                    buf[i+2] == '\r' && buf[i+3] == '\n') {
                    eoh = i; break;
                }
            }
        }

        if (eoh < 0) {
            errn("apm: incomplete HTTP response (no headers)");
            if (is_tls) asd_tls_close(tls_conn);
            asd_tcp_close(tcp_conn); return -1;
        }

        /* Parse status code: "HTTP/1.x NNN" — digits at positions 9,10,11 */
        if (buf_n > 12 && spfx(buf, "HTTP/1."))
            status_code = (buf[9]-'0')*100 + (buf[10]-'0')*10 + (buf[11]-'0');

        out("  HTTP status: "); outnum(status_code); out("\n");

        if (status_code == 0) {
            errn("apm: bad HTTP status line");
            if (is_tls) asd_tls_close(tls_conn);
            asd_tcp_close(tcp_conn); return -1;
        }

        /* Parse Location header for redirects — search within headers only */
        if (status_code >= 300 && status_code < 400) {
            /* Null-terminate just the headers section for safe search */
            char saved = buf[eoh]; buf[eoh] = 0;
            const char *p = buf;
            while (*p) {
                /* advance to start of next line */
                while (*p && *p != '\n') p++;
                if (*p == '\n') p++;
                /* skip leading whitespace (some servers indent headers) */
                while (*p == ' ' || *p == '\t') p++;
                /* case-insensitive match for "location:" */
                if ((p[0]=='L'||p[0]=='l') &&
                    (p[1]=='o'||p[1]=='O') &&
                    (p[2]=='c'||p[2]=='C') &&
                    (p[3]=='a'||p[3]=='A') &&
                    (p[4]=='t'||p[4]=='T') &&
                    (p[5]=='i'||p[5]=='I') &&
                    (p[6]=='o'||p[6]=='O') &&
                    (p[7]=='n'||p[7]=='N') &&
                    (p[8]==':')) {
                    p += 9;
                    while (*p == ' ' || *p == '\t') p++;
                    int li = 0;
                    while (*p && *p != '\r' && *p != '\n' && li < URL_LEN-1)
                        location[li++] = *p++;
                    location[li] = 0;
                    break;
                }
            }
            buf[eoh] = saved;
        }

        /* Handle redirect */
        if (status_code >= 300 && status_code < 400) {
            if (redirects >= 5) {
                errn("apm: too many redirects");
                if (is_tls) asd_tls_close(tls_conn);
                asd_tcp_close(tcp_conn); return -1;
            }
            if (!location[0]) {
                errn("apm: redirect with no Location header");
                if (is_tls) asd_tls_close(tls_conn);
                asd_tcp_close(tcp_conn); return -1;
            }
            char new_url[URL_LEN];
            resolve_location(cur_url, location, new_url, URL_LEN);
            out("  -> "); outnum(status_code); out(" "); outn(new_url);
            redirects++;
            scopy(cur_url, new_url, URL_LEN);
            if (is_tls) asd_tls_close(tls_conn);
            asd_tcp_close(tcp_conn);
            continue;
        }

        /* Handle non-2xx error */
        if (status_code < 200 || status_code >= 300) {
            int sl = 0;
            while (sl < 64 && buf[sl] && buf[sl] != '\r') sl++;
            if (sl > 0) { char st[65]; for(int j=0;j<sl;j++)st[j]=buf[j]; st[sl]=0; errn(st); }
            if (is_tls) asd_tls_close(tls_conn);
            asd_tcp_close(tcp_conn); return -1;
        }

        /* 2xx: write body to file */
        int fd = asd_open(dest_path, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0) {
            errn("apm: cannot create dest file");
            if (is_tls) asd_tls_close(tls_conn);
            asd_tcp_close(tcp_conn); return -1;
        }

        long body_bytes = 0;
        int body_start = eoh + 4;
        if (body_start < buf_n) {
            int nb = buf_n - body_start;
            asd_write(fd, buf + body_start, (size_t)nb);
            body_bytes += nb;
            asd_yield();
        }

        /* Drain remaining body data */
        while (1) {
            long n = is_tls
                     ? asd_tls_recv(tls_conn, buf, sizeof(buf), 1)
                     : asd_tcp_recv(tcp_conn, buf, sizeof(buf), 1);
            if (n <= 0) break;
            asd_write(fd, buf, (size_t)n);
            body_bytes += n;
            draw_progress(label, body_bytes, -1);
            asd_yield();
        }

        asd_close(fd);
        if (is_tls) asd_tls_close(tls_conn);
        asd_tcp_close(tcp_conn);

        if (body_bytes == 0) {
            errn("\napm: empty response body");
            return -1;
        }
        finish_progress(label, body_bytes);
        return 0;
    }
}

/* ================================================================
 * Package archive (.apkg) extractor
 * ================================================================ */

#define APKG_MAGIC  "APKG"

static int apkg_extract(const char *path, PkgInfo *p) {
    static char apkg_buf[512 * 1024]; /* 512 KiB max package */
    long n = read_file(path, apkg_buf, sizeof(apkg_buf));
    if (n < 12) { errn("apm: invalid or truncated .apkg archive"); return -1; }

    /* Verify magic */
    if (apkg_buf[0] != 'A' || apkg_buf[1] != 'P' ||
        apkg_buf[2] != 'K' || apkg_buf[3] != 'G') {
        errn("apm: not a valid .apkg archive (bad magic)");
        return -1;
    }

    uint16_t ver = (uint8_t)apkg_buf[4] | ((uint8_t)apkg_buf[5] << 8);
    if (ver != 1) { errn("apm: unsupported .apkg version"); return -1; }

    uint32_t meta_len = (uint8_t)apkg_buf[8]
                      | ((uint8_t)apkg_buf[9]  << 8)
                      | ((uint8_t)apkg_buf[10] << 16)
                      | ((uint8_t)apkg_buf[11] << 24);

    if (12 + meta_len + 4 > (uint32_t)n) {
        errn("apm: .apkg truncated in metadata section");
        return -1;
    }

    /* Parse metadata */
    char meta[4096];
    if (meta_len >= sizeof(meta)) meta_len = sizeof(meta) - 1;
    for (uint32_t i = 0; i < meta_len; i++) meta[i] = apkg_buf[12 + i];
    meta[meta_len] = 0;
    pkg_parse(p, meta);

    /* Extract files */
    long pos = 12 + meta_len;
    uint32_t nfiles = (uint8_t)apkg_buf[pos]
                    | ((uint8_t)apkg_buf[pos+1] << 8)
                    | ((uint8_t)apkg_buf[pos+2] << 16)
                    | ((uint8_t)apkg_buf[pos+3] << 24);
    pos += 4;
    p->nfiles = 0;

    for (uint32_t fi = 0; fi < nfiles && fi < MAX_FILES_PER_PKG; fi++) {
        if (pos + 2 > n) break;
        uint16_t plen = (uint8_t)apkg_buf[pos] | ((uint8_t)apkg_buf[pos+1] << 8);
        pos += 2;
        if (pos + plen > n) break;

        char fpath[PATH_LEN];
        if (plen >= PATH_LEN) plen = PATH_LEN - 1;
        for (int i = 0; i < plen; i++) fpath[i] = apkg_buf[pos + i];
        fpath[plen] = 0;
        pos += plen;

        if (pos + 4 > n) break;
        uint32_t dlen = (uint8_t)apkg_buf[pos]
                      | ((uint8_t)apkg_buf[pos+1] << 8)
                      | ((uint8_t)apkg_buf[pos+2] << 16)
                      | ((uint8_t)apkg_buf[pos+3] << 24);
        pos += 4;
        if (pos + dlen > (uint32_t)n) break;

        /* Write file to filesystem */
        int fd = asd_open(fpath, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0) {
            out("apm: warning: cannot write "); outn(fpath);
        } else {
            long written = 0;
            while ((uint32_t)written < dlen) {
                long w = asd_write(fd, apkg_buf + pos + written,
                                   (size_t)(dlen - written));
                if (w <= 0) break;
                written += w;
                asd_yield();
            }
            asd_close(fd);
            scopy(p->files[p->nfiles++], fpath, PATH_LEN);
        }
        pos += dlen;
    }
    return 0;
}

/* ================================================================
 * Dependency resolver (simple: install missing deps first)
 * ================================================================ */

static int install_package(const char *name, int dry_run);

static int install_deps(PkgInfo *p, int dry_run) {
    for (int i = 0; i < p->ndepends; i++) {
        if (!db_installed(p->depends[i])) {
            out("apm: installing dependency: "); outn(p->depends[i]);
            if (install_package(p->depends[i], dry_run) != 0) return -1;
        }
    }
    return 0;
}

/* ================================================================
 * Command: update (fetch repository indexes)
 * ================================================================ */

static int cmd_update(void) {
    if (g_cfg.nrepos == 0) {
        errn("apm: no repositories configured.");
        errn("     Edit /etc/apm/apm.conf and add a 'repo' line.");
        return 1;
    }

    mkdir_p(APM_LISTS_DIR);
    out(C_BOLD "==> " C_RESET "Syncing package indexes\n");

    for (int ri = 0; ri < g_cfg.nrepos; ri++) {
        Repo *r = &g_cfg.repos[ri];
        char idx_url[URL_LEN], idx_path[PATH_LEN];

        scopy(idx_url, r->url, URL_LEN);
        scat(idx_url, "/index.idx", URL_LEN);

        scopy(idx_path, APM_LISTS_DIR, PATH_LEN);
        scat(idx_path, "/", PATH_LEN);
        scat(idx_path, r->name, PATH_LEN);
        scat(idx_path, ".idx", PATH_LEN);

        out(C_BOLD "Fetching" C_RESET " ["); out(r->name); outn("] index...");

        if (http_get(idx_url, idx_path, "index.idx") != 0) {
            errn("\napm: failed to fetch index");
            return 1;
        }
    }
    out(C_GREEN C_BOLD "==> " C_RESET "Package indexes up to date.\n");
    return 0;
}

/* ================================================================
 * Command: install
 * ================================================================ */

static int install_package(const char *name, int dry_run) {
    if (db_installed(name)) {
        out(C_DIM "[already installed] " C_RESET); outn(name);
        return 0;
    }

    PkgInfo p;
    if (!idx_find(name, &p)) {
        out("apm: package not found: "); outn(name);
        return -1;
    }

    /* Install dependencies first */
    if (install_deps(&p, dry_run) != 0) return -1;

    out(C_BOLD "==> " C_RESET "Installing " C_CYAN);
    out(p.name); out("-"); out(p.version);
    out(C_RESET " ("); outsz(p.installed_size); out(")\n");
    if (dry_run) return 0;

    /* Check cache */
    char cache_path[PATH_LEN];
    mkdir_p(APM_CACHE_DIR);
    scopy(cache_path, APM_CACHE_DIR, PATH_LEN);
    scat(cache_path, "/", PATH_LEN);
    scat(cache_path, p.filename, PATH_LEN);

    if (!file_exists(cache_path)) {
        /* Build download URL */
        char dl_url[URL_LEN];
        int found_repo = 0;
        for (int ri = 0; ri < g_cfg.nrepos; ri++) {
            if (seq(g_cfg.repos[ri].name, p.repo)) {
                scopy(dl_url, g_cfg.repos[ri].url, URL_LEN);
                scat(dl_url, "/", URL_LEN);
                scat(dl_url, p.filename, URL_LEN);
                found_repo = 1;
                break;
            }
        }
        if (!found_repo) { errn("apm: cannot find repo URL"); return -1; }
        if (http_get(dl_url, cache_path, p.filename) != 0) return -1;
    } else {
        out("  Using cached archive: "); outn(cache_path);
    }

    /* Extract archive */
    out("    Extracting... ");
    if (apkg_extract(cache_path, &p) != 0) return -1;
    outn("done");

    /* Register in database */
    if (db_save(&p) != 0) {
        errn("apm: failed to write package database");
        return -1;
    }

    out(C_GREEN C_BOLD "[installed] " C_RESET);
    out(p.name); out("-"); outn(p.version);
    return 0;
}

static int cmd_install(int argc, const char **argv) {
    if (argc < 3) { errn("usage: apm install <package>..."); return 1; }
    idx_load_all();
    int rc = 0;
    for (int i = 2; i < argc; i++) {
        if (install_package(argv[i], 0) != 0) rc = 1;
        asd_yield();
    }
    return rc;
}

/* ================================================================
 * Command: del (remove)
 * ================================================================ */

static int cmd_del(int argc, const char **argv) {
    if (argc < 3) { errn("usage: apm del <package>..."); return 1; }
    int rc = 0;
    for (int i = 2; i < argc; i++) {
        const char *name = argv[i];
        PkgInfo p;
        if (!db_load(name, &p)) {
            out("apm: not installed: "); outn(name);
            rc = 1;
            continue;
        }
        out("Removing "); outn(p.name);
        /* Remove installed files */
        for (int fi = 0; fi < p.nfiles; fi++) {
            out("  rm "); outn(p.files[fi]);
            if (asd_unlink(p.files[fi]) != 0) {
                out("  warning: could not remove "); outn(p.files[fi]);
            }
        }
        /* Remove database record */
        db_remove(name);
        out("[removed] "); outn(name);
    }
    return rc;
}

/* ================================================================
 * Command: upgrade
 * ================================================================ */

static int cmd_upgrade(void) {
    idx_load_all();
    out("Checking for upgrades...\n");

    /* Enumerate installed packages */
    asd_dirent_t ents[MAX_PKGS];
    uint32_t nents = 0;
    if (asd_readdir(APM_DB_DIR, ents, MAX_PKGS, &nents) != 0) {
        errn("apm: cannot read package database");
        return 1;
    }

    int upgraded = 0;
    char rec[4096];
    for (uint32_t ei = 0; ei < nents; ei++) {
        /* Each entry is <name>.apd */
        char *fname = ents[ei].name;
        int fl = slen(fname);
        if (fl < 5) continue;
        if (!seq(fname + fl - 4, ".apd")) continue;
        fname[fl - 4] = 0; /* strip .apd */

        PkgInfo installed, available;
        if (!db_load(fname, &installed)) continue;
        if (!idx_find(fname, &available)) continue;

        /* Simple version compare: if different, upgrade */
        if (!seq(installed.version, available.version)) {
            out("Upgrading "); out(fname);
            out(" ["); out(installed.version);
            out(" -> "); out(available.version); outn("]");
            /* Remove old files */
            for (int fi = 0; fi < installed.nfiles; fi++)
                asd_unlink(installed.files[fi]);
            db_remove(fname);
            if (install_package(fname, 0) == 0) upgraded++;
        }
        fname[fl - 4] = '.'; /* restore */
    }

    if (upgraded == 0) outn("All packages up to date.");
    else { outnum(upgraded); outn(" package(s) upgraded."); }
    return 0;
}

/* ================================================================
 * Command: search
 * ================================================================ */

static int cmd_search(int argc, const char **argv) {
    if (argc < 3) { errn("usage: apm search <query>"); return 1; }
    const char *query = argv[2];
    idx_load_all();

    int found = 0;
    char rec[4096];
    for (int ri = 0; ri < g_cfg.nrepos; ri++) {
        const char *pos = g_idx_bufs[ri];
        /* skip header */
        pos = idx_next_record(pos, rec, sizeof(rec));
        while (pos) {
            char nm[PKG_NAME_LEN], desc[PKG_DESC_LEN], ver[PKG_VER_LEN];
            kv_get(rec, "name",        nm,   PKG_NAME_LEN);
            kv_get(rec, "version",     ver,  PKG_VER_LEN);
            kv_get(rec, "description", desc, PKG_DESC_LEN);
            if (nm[0] && (scontains(nm, query) || scontains(desc, query))) {
                /* Status indicator */
                if (db_installed(nm)) out("[installed] ");
                else                   out("            ");
                out(nm); out("-"); out(ver);
                out(" ("); out(g_cfg.repos[ri].name); out(")\n");
                out("  "); outn(desc);
                found++;
            }
            pos = idx_next_record(pos, rec, sizeof(rec));
        }
    }
    if (!found) { out("No packages found for: "); outn(query); }
    return 0;
}

/* ================================================================
 * Command: list
 * ================================================================ */

static int cmd_list(void) {
    asd_dirent_t ents[MAX_PKGS];
    uint32_t nents = 0;
    if (asd_readdir(APM_DB_DIR, ents, MAX_PKGS, &nents) != 0) {
        errn("apm: package database is empty (nothing installed)");
        return 0;
    }
    outn("Installed packages:");
    int count = 0;
    for (uint32_t ei = 0; ei < nents; ei++) {
        char *fname = ents[ei].name;
        int fl = slen(fname);
        if (fl < 5 || !seq(fname + fl - 4, ".apd")) continue;
        fname[fl - 4] = 0;
        PkgInfo p;
        if (db_load(fname, &p)) {
            out("  "); out(p.name); out("-"); out(p.version);
            out(" ["); out(p.arch); out("] ");
            outn(p.description);
            count++;
        }
        fname[fl - 4] = '.';
    }
    if (count == 0) outn("  (no packages installed)");
    else { outnum(count); outn(" package(s) installed."); }
    return 0;
}

/* ================================================================
 * Command: info
 * ================================================================ */

static int cmd_info(int argc, const char **argv) {
    if (argc < 3) { errn("usage: apm info <package>"); return 1; }
    const char *name = argv[2];
    idx_load_all();

    PkgInfo p;
    int is_inst = db_installed(name);
    int found_in_db  = is_inst && db_load(name, &p);
    int found_in_idx = 0;

    if (!found_in_db) {
        PkgInfo pAvail;
        found_in_idx = idx_find(name, &pAvail);
        if (found_in_idx) p = pAvail;
    }

    if (!found_in_db && !found_in_idx) {
        out("apm: package not found: "); outn(name);
        return 1;
    }

    out("Name:         "); outn(p.name);
    out("Version:      "); outn(p.version);
    out("Architecture: "); outn(p.arch);
    out("Description:  "); outn(p.description);
    out("Size:         "); outsz(p.size); outn(" (archive)");
    out("Installed:    "); outsz(p.installed_size); outn(" (on disk)");
    out("Repository:   "); outn(p.repo[0] ? p.repo : "(unknown)");
    out("Checksum:     "); outn(p.checksum[0] ? p.checksum : "(none)");
    out("Status:       "); outn(is_inst ? "installed" : "not installed");
    if (p.ndepends > 0) {
        out("Depends:      ");
        for (int i = 0; i < p.ndepends; i++) {
            if (i) out(", ");
            out(p.depends[i]);
        }
        asd_write(1, "\n", 1);
    }
    if (is_inst && p.nfiles > 0) {
        outn("Files:");
        for (int fi = 0; fi < p.nfiles; fi++) {
            out("  "); outn(p.files[fi]);
        }
    }
    return 0;
}

/* ================================================================
 * Command: clean
 * ================================================================ */

static int cmd_clean(void) {
    asd_dirent_t ents[256];
    uint32_t n = 0;
    if (asd_readdir(APM_CACHE_DIR, ents, 256, &n) != 0) {
        outn("apm: cache is already empty.");
        return 0;
    }
    long total = 0;
    for (uint32_t i = 0; i < n; i++) {
        char path[PATH_LEN];
        scopy(path, APM_CACHE_DIR, PATH_LEN);
        scat(path, "/", PATH_LEN);
        scat(path, ents[i].name, PATH_LEN);
        asd_stat_t st;
        if (asd_stat(path, &st) == 0) total += st.size;
        asd_unlink(path);
    }
    out("Cleaned cache ("); outsz(total); outn(" freed).");
    return 0;
}

/* ================================================================
 * Command: check (verify installed packages)
 * ================================================================ */

static int cmd_check(void) {
    asd_dirent_t ents[MAX_PKGS];
    uint32_t nents = 0;
    if (asd_readdir(APM_DB_DIR, ents, MAX_PKGS, &nents) != 0) {
        outn("apm: nothing to check.");
        return 0;
    }
    int errors = 0;
    for (uint32_t ei = 0; ei < nents; ei++) {
        char *fname = ents[ei].name;
        int fl = slen(fname);
        if (fl < 5 || !seq(fname + fl - 4, ".apd")) continue;
        fname[fl - 4] = 0;
        PkgInfo p;
        if (!db_load(fname, &p)) { fname[fl-4] = '.'; continue; }
        for (int fi = 0; fi < p.nfiles; fi++) {
            if (!file_exists(p.files[fi])) {
                out("MISSING "); out(p.name); out(": "); outn(p.files[fi]);
                errors++;
            }
        }
        fname[fl - 4] = '.';
    }
    if (errors == 0) outn("All installed packages OK.");
    else { outnum(errors); outn(" missing file(s) detected. Run: apm upgrade"); }
    return errors ? 1 : 0;
}

/* ================================================================
 * First-run init: create directory structure + default config
 * ================================================================ */

static void ensure_dirs(void) {
    mkdir_p("/etc/apm");
    mkdir_p(APM_DB_DIR);
    mkdir_p(APM_LISTS_DIR);
    mkdir_p(APM_CACHE_DIR);
}

static void write_default_conf(void) {
    if (file_exists(APM_CONF)) return;
    const char *conf =
        "# apm.conf - ASD Package Manager " APM_VERSION " configuration\n"
        "#\n"
        "# Repository format:\n"
        "#   repo <name> <url>\n"
        "#\n"
        "# The URL must point to a directory containing:\n"
        "#   index.idx               — package index\n"
        "#   <name>-<ver>-<arch>.apkg — package archives\n"
        "#\n"
        "# Uncomment to enable a repository:\n"
        "# repo official https://github.com/komarufan/OpenASD-packages/releases/latest/download\n"
        "#\n"
        "# Architecture (do not change)\n"
        "arch=" APM_ARCH "\n";
    write_file(APM_CONF, conf, slen(conf));
}

/* ================================================================
 * Help
 * ================================================================ */

static void usage(void) {
    outn("apm " APM_VERSION " - ASD Package Manager");
    outn("");
    outn("Usage: apm <command> [args]");
    outn("");
    outn("Commands:");
    outn("  update              Fetch/refresh repository package indexes");
    outn("  install <pkg>...    Install one or more packages");
    outn("  del <pkg>...        Remove installed packages");
    outn("  upgrade             Upgrade all installed packages to latest versions");
    outn("  search <query>      Search packages by name or description");
    outn("  list                List all installed packages");
    outn("  info <pkg>          Show detailed package information");
    outn("  clean               Remove cached package archives");
    outn("  check               Verify integrity of installed packages");
    outn("  help                Show this message");
    outn("");
    outn("Config: /etc/apm/apm.conf");
    outn("DB:     /var/apm/db/installed/");
    outn("Cache:  /var/apm/cache/");
    outn("");
    outn("To enable package downloads, add a 'repo' line to /etc/apm/apm.conf");
}

/* ================================================================
 * main
 * ================================================================ */

int main(int argc, const char **argv) {
    ensure_dirs();
    write_default_conf();
    cfg_load(&g_cfg);

    if (argc < 2 || seq(argv[1], "help") || seq(argv[1], "--help") || seq(argv[1], "-h")) {
        usage();
        asd_exit(0);
    }

    const char *cmd = argv[1];
    int rc = 0;

    if (seq(cmd, "update")) {
        rc = cmd_update();
    } else if (seq(cmd, "install") || seq(cmd, "add")) {
        idx_load_all();
        rc = cmd_install(argc, argv);
    } else if (seq(cmd, "del") || seq(cmd, "remove") || seq(cmd, "rm")) {
        rc = cmd_del(argc, argv);
    } else if (seq(cmd, "upgrade")) {
        rc = cmd_upgrade();
    } else if (seq(cmd, "search")) {
        rc = cmd_search(argc, argv);
    } else if (seq(cmd, "list") || seq(cmd, "ls")) {
        rc = cmd_list();
    } else if (seq(cmd, "info") || seq(cmd, "show")) {
        rc = cmd_info(argc, argv);
    } else if (seq(cmd, "clean")) {
        rc = cmd_clean();
    } else if (seq(cmd, "check") || seq(cmd, "verify")) {
        rc = cmd_check();
    } else {
        out("apm: unknown command: "); outn(cmd);
        outn("Run 'apm help' for usage.");
        rc = 1;
    }

    asd_exit(rc);
    return rc;
}
