/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, ASD Project Contributors
 * All rights reserved.
 */

#include "vfs.h"
#include "../mm/mm.h"
#include "../ipc/ringbuf.h"
#include "../ipc/port.h"
#include "../sched/sched.h"
#include <string.h>
#include <stddef.h>

static void **pcb_fd_table(struct pcb *p);

typedef uint32_t spinlock_t;

static inline void spin_lock(spinlock_t *l) {
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
}

static inline void spin_unlock(spinlock_t *l) {
    __atomic_store_n(l, 0, __ATOMIC_RELEASE);
}

static vfsnode_t    g_nodes[MAX_VFSNODES];
static vfs_backend_t g_backends[MAX_BACKENDS];
static spinlock_t   g_vfs_lock;
static uint32_t     g_next_node_id = 1;

static uint32_t alloc_node_id(void) {
    uint32_t id = g_next_node_id;
    for (uint32_t i = 0; i < MAX_VFSNODES; i++) {
        uint32_t candidate = (id + i) % MAX_VFSNODES;
        if (candidate == 0) continue;
        if (g_nodes[candidate].node_id == 0) {
            g_next_node_id = (candidate + 1) % MAX_VFSNODES;
            if (g_next_node_id == 0) g_next_node_id = 1;
            return candidate;
        }
    }
    return 0; /* all slots full */
}

static vfs_backend_t *find_backend(const char *path, const char **rel_path) {
    vfs_backend_t *best = NULL;
    size_t best_len = 0;

    for (uint32_t i = 0; i < MAX_BACKENDS; i++) {
        vfs_backend_t *be = &g_backends[i];
        if (be->id == 0) continue;

        size_t mlen = strlen(be->mount_pt);
        /* Special case: root mount point "/" matches any absolute path */
        if (mlen == 1 && be->mount_pt[0] == '/') {
            if (path[0] != '/') continue; /* must be absolute */
            /* Root is a valid match; keep best_len = 1 */
        } else {
            /* Check if path starts with mount_pt */
            if (strncmp(path, be->mount_pt, mlen) != 0) continue;
            /* Ensure it's a proper prefix: either exact match or followed by '/' */
            if (path[mlen] != '\0' && path[mlen] != '/') continue;
        }

        /* >= so a later-mounted backend at the same path overrides an earlier one */
        if (mlen >= best_len) {
            best = be;
            best_len = mlen;
        }
    }

    if (best) {
        /* Root mount "/" strips one char → "bin/foo"; ramfs stores "/bin/foo". */
        if (best_len == 1 && best->mount_pt[0] == '/') {
            *rel_path = path;
        } else {
            const char *rp = path + best_len;
            if (*rp == '\0') rp = "/";
            *rel_path = rp;
        }
    }
    return best;
}

static void base_name(const char *path, char *out, size_t out_sz) {
    const char *sep = strrchr(path, '/');
    const char *name = sep ? sep + 1 : path;
    strncpy(out, name, out_sz - 1);
    out[out_sz - 1] = '\0';
}

/*
 * SECURITY FIX: vfs_canonicalize — resolve "." and ".." components
 * in a path to prevent path traversal attacks (e.g. /boot/../etc/passwd).
 *
 * Algorithm:
 *   - Split path by '/'.
 *   - For each component:
 *       "."  -> skip (current dir)
 *       ".." -> pop last component (but never go above root)
 *       else -> push component
 *   - Reconstruct the canonical absolute path.
 *
 * The output is always an absolute path starting with '/'.
 * Returns 0 on success, -1 if the path is too long.
 */
int vfs_canonicalize(const char *path, char *out, size_t out_sz) {
    /* Stack of path components (pointers into a working copy) */
    static char work[VFS_PATH_MAX];
    static const char *stk[VFS_PATH_MAX / 2];
    int top = 0;

    if (!path || out_sz == 0) return -1;

    /* Work on a mutable copy */
    size_t plen = strlen(path);
    if (plen >= VFS_PATH_MAX) return -1;
    memcpy(work, path, plen + 1);

    char *p = work;
    /* Skip leading slash(es) */
    while (*p == '/') p++;

    while (*p) {
        char *seg = p;
        /* Find end of component */
        while (*p && *p != '/') p++;
        if (*p == '/') { *p = '\0'; p++; }

        if (seg[0] == '\0' || (seg[0] == '.' && seg[1] == '\0')) {
            /* Empty segment or "." — skip */
            continue;
        } else if (seg[0] == '.' && seg[1] == '.' && seg[2] == '\0') {
            /* ".." — pop one level (never above root) */
            if (top > 0) top--;
        } else {
            if (top >= (int)(VFS_PATH_MAX / 2 - 1)) return -1;
            stk[top++] = seg;
        }
    }

    /* Reconstruct */
    out[0] = '/';
    size_t pos = 1;
    for (int i = 0; i < top; i++) {
        size_t slen = strlen(stk[i]);
        if (pos + slen + 2 > out_sz) return -1;  /* too long */
        if (i > 0) out[pos++] = '/';
        memcpy(out + pos, stk[i], slen);
        pos += slen;
    }
    out[pos] = '\0';
    return 0;
}

void vfs_init(void) {
    memset(g_nodes, 0, sizeof(g_nodes));
    memset(g_backends, 0, sizeof(g_backends));
    g_vfs_lock = 0;
    g_next_node_id = 1;
}

uint32_t vfs_mount(const char *mount_pt, vfs_ops_t ops, void *ctx) {
    if (!mount_pt || !mount_pt[0]) return 0;

    spin_lock(&g_vfs_lock);

    uint32_t be_id = 0;
    for (uint32_t i = 0; i < MAX_BACKENDS; i++) {
        if (g_backends[i].id == 0) { be_id = i + 1; break; }
    }
    if (be_id == 0) { spin_unlock(&g_vfs_lock); return 0; }

    vfs_backend_t *be = &g_backends[be_id - 1];
    memset(be, 0, sizeof(*be));
    be->id = be_id;
    strncpy(be->name, mount_pt, VFS_BE_NAME_LEN - 1);
    be->name[VFS_BE_NAME_LEN - 1] = '\0';
    strncpy(be->mount_pt, mount_pt, VFS_PATH_MAX - 1);
    be->mount_pt[VFS_PATH_MAX - 1] = '\0';
    be->ops = ops;
    be->ctx = ctx;

    spin_unlock(&g_vfs_lock);
    return be_id;
}

fd_t vfs_open(const char *path, int flags, struct pcb *caller) {
    if (!path || !path[0]) return (fd_t)(VFS_EINVAL);

    /* SECURITY FIX: canonicalize path to prevent traversal attacks */
    char canon[VFS_PATH_MAX];
    if (vfs_canonicalize(path, canon, sizeof(canon)) != 0)
        return (fd_t)(VFS_EINVAL);
    path = canon;

    /* Validate flags */
    if (!(flags & (VFS_O_READ | VFS_O_WRITE)))
        flags |= VFS_O_READ; /* default to read if nothing specified */
    spin_lock(&g_vfs_lock);
    /* Try to find a backend for this path */
    const char *rel_path = NULL;
    vfs_backend_t *be = find_backend(path, &rel_path);

    vfs_bctx_t bctx = NULL;
    vfs_backend_t *used_be = NULL;

    if (be && be->ops.open) {
        bctx = be->ops.open(be, rel_path, flags);
        if (bctx) used_be = be;
    }

    if (!bctx) {
        /* TODO: synthetic node handling for /drv/, /sys/, /shm/ */
        spin_unlock(&g_vfs_lock);
        return (fd_t)(VFS_ENOENT);
    }

    /* Allocate a vfsnode */
    uint32_t nid = alloc_node_id();
    if (nid == 0) {
        if (used_be && used_be->ops.close)
            used_be->ops.close(used_be, bctx);
        spin_unlock(&g_vfs_lock);
        return (fd_t)(VFS_EMFILE);
    }

    vfsnode_t *node = &g_nodes[nid];
    memset(node, 0, sizeof(*node));
    node->node_id  = nid;
    node->refcount = 1;
    node->flags    = (uint32_t)flags;
    node->offset   = 0;
    node->size     = -1; /* unknown until stat */
    strncpy(node->path, path, VFS_PATH_MAX - 1);
    node->path[VFS_PATH_MAX - 1] = '\0';
    node->backend  = used_be;
    node->bctx     = bctx;
    node->pcb      = caller;
    node->ring     = NULL;
    node->port_id  = 0;

    /* Get file size if backend supports stat */
    if (used_be && used_be->ops.stat) {
        stat_info_t info;
        if (used_be->ops.stat(used_be, rel_path, &info) == 0)
            node->size = info.size;
    }

    /* Install in caller's fd table (if caller is a process).
     * Start from 3 to avoid conflict with stdin(0)/stdout(1)/stderr(2)
     * which sys_read/sys_write handle specially without touching fd_table. */
    fd_t fd = 0;
    if (caller) {
        void **fdt = pcb_fd_table(caller);
        for (fd_t i = 3; i < MAX_FDS; i++) {
            if (fdt[i] == NULL) { fdt[i] = (void *)node; fd = i; break; }
        }
    } else {
        /* Kernel context: return a pseudo-fd (node_id offset) */
        fd = (fd_t)(nid + MAX_FDS);
    }

    spin_unlock(&g_vfs_lock);

    if (fd == 0) {
        /* No fd slot available — release the node */
        node->node_id = 0;
        if (used_be && used_be->ops.close)
            used_be->ops.close(used_be, bctx);
        return (fd_t)(VFS_EMFILE);
    }

    return fd;
}

int vfs_close(fd_t fd) {
    /* 0/1/2 are stdin/stdout/stderr handled by syscall layer, not VFS */
    if (fd <= 2) return 0;

    spin_lock(&g_vfs_lock);

    vfsnode_t *node = NULL;

    if (fd >= MAX_FDS) {
        /* Kernel pseudo-fd: look up by node_id */
        uint32_t nid = (uint32_t)(fd - MAX_FDS);
        if (nid >= MAX_VFSNODES || g_nodes[nid].node_id != nid) {
            spin_unlock(&g_vfs_lock);
            return VFS_EINVAL;
        }
        node = &g_nodes[nid];
    } else {
        /* Process fd: need PCB to look up */
        pcb_t *cur = sched_current();
        if (!cur) { spin_unlock(&g_vfs_lock); return VFS_EINVAL; }

        void **fdt = pcb_fd_table(cur);
        node = (vfsnode_t *)fdt[fd];
        if (!node) { spin_unlock(&g_vfs_lock); return VFS_EINVAL; }
        fdt[fd] = NULL;
    }

    node->refcount--;
    if (node->refcount == 0) {
        if (node->backend && node->backend->ops.close)
            node->backend->ops.close(node->backend, node->bctx);
        node->node_id = 0;
    }

    spin_unlock(&g_vfs_lock);
    return 0;
}

void vfs_close_all(struct pcb *pcb) {
    if (!pcb) return;
    void **fdt = pcb_fd_table(pcb);
    spin_lock(&g_vfs_lock);
    for (fd_t i = 3; i < MAX_FDS; i++) {
        vfsnode_t *node = (vfsnode_t *)fdt[i];
        if (!node) continue;
        fdt[i] = NULL;
        node->refcount--;
        if (node->refcount == 0) {
            if (node->backend && node->backend->ops.close)
                node->backend->ops.close(node->backend, node->bctx);
            node->node_id = 0;
        }
    }
    spin_unlock(&g_vfs_lock);
}

int vfs_read(fd_t fd, void *buf, size_t n, size_t *got) {
    /* See vfs_write: honor a redirected fd 0/1/2 (pipe in fd_table) rather than
     * rejecting — the fd_table lookup below decides validity. */
    if (!buf) return VFS_EINVAL;

    spin_lock(&g_vfs_lock);

    vfsnode_t *node = NULL;

    if (fd >= MAX_FDS) {
        uint32_t nid = (uint32_t)(fd - MAX_FDS);
        if (nid >= MAX_VFSNODES || g_nodes[nid].node_id != nid) {
            spin_unlock(&g_vfs_lock);
            return VFS_EINVAL;
        }
        node = &g_nodes[nid];
    } else {
        pcb_t *cur = sched_current();
        if (!cur) { spin_unlock(&g_vfs_lock); return VFS_EINVAL; }
        void **fdt = pcb_fd_table(cur);
        node = (vfsnode_t *)fdt[fd];
        if (!node) { spin_unlock(&g_vfs_lock); return VFS_EINVAL; }
    }

    /* Ring buffer synthetic read */
    if (node->ring) {
        size_t total = 0;
        uint8_t *bp = (uint8_t *)buf;
        while (total < n) {
            size_t chunk = n - total;
            if (chunk > node->ring->slot_size) chunk = node->ring->slot_size;
            if (ringbuf_pop(node->ring, bp + total, (uint32_t)chunk) != 0)
                break; /* ring empty */
            total += chunk;
        }
        if (got) *got = total;
        spin_unlock(&g_vfs_lock);
        return (total > 0) ? 0 : VFS_ERR;
    }

    /* Backend read */
    if (!node->backend || !node->backend->ops.read) {
        spin_unlock(&g_vfs_lock);
        return VFS_EPERM;
    }

    int rc = node->backend->ops.read(node->backend, node->bctx, buf, n, got);
    if (rc == 0 && got)
        node->offset += *got;

    spin_unlock(&g_vfs_lock);
    return rc;
}

int vfs_write(fd_t fd, const void *buf, size_t n) {
    /* fd 0/1/2 are normally console (handled in the syscall layer), but when a
     * process has redirected them (pipe in fd_table[fd], e.g. a shell under the
     * terminal), the syscall layer routes here — so honor the fd_table entry
     * instead of rejecting.  Only the lookup below decides validity. */
    if (!buf || n == 0) return VFS_EINVAL;

    spin_lock(&g_vfs_lock);

    vfsnode_t *node = NULL;

    if (fd >= MAX_FDS) {
        uint32_t nid = (uint32_t)(fd - MAX_FDS);
        if (nid >= MAX_VFSNODES || g_nodes[nid].node_id != nid) {
            spin_unlock(&g_vfs_lock);
            return VFS_EINVAL;
        }
        node = &g_nodes[nid];
    } else {
        pcb_t *cur = sched_current();
        if (!cur) { spin_unlock(&g_vfs_lock); return VFS_EINVAL; }
        void **fdt = pcb_fd_table(cur);
        node = (vfsnode_t *)fdt[fd];
        if (!node) { spin_unlock(&g_vfs_lock); return VFS_EINVAL; }
    }

    /* Ring buffer synthetic write */
    if (node->ring) {
        size_t total = 0;
        const uint8_t *bp = (const uint8_t *)buf;
        while (total < n) {
            size_t chunk = n - total;
            if (chunk > node->ring->slot_size) chunk = node->ring->slot_size;
            if (ringbuf_push(node->ring, bp + total, (uint32_t)chunk) != 0)
                break; /* ring full */
            total += chunk;
        }
        spin_unlock(&g_vfs_lock);
        return (total > 0) ? 0 : VFS_ERR;
    }

    /* Port synthetic write */
    if (node->port_id) {
        int rc = port_send(node->port_id, buf, n);
        spin_unlock(&g_vfs_lock);
        return rc;
    }

    /* Backend write */
    if (!node->backend || !node->backend->ops.write) {
        spin_unlock(&g_vfs_lock);
        return VFS_EPERM;
    }

    int rc = node->backend->ops.write(node->backend, node->bctx, buf, n);
    if (rc == 0)
        node->offset += n;

    spin_unlock(&g_vfs_lock);
    return rc;
}

int64_t vfs_seek(fd_t fd, int64_t off, int whence) {
    if (fd <= 2) return (int64_t)(VFS_EINVAL);

    spin_lock(&g_vfs_lock);

    vfsnode_t *node = NULL;

    if (fd >= MAX_FDS) {
        uint32_t nid = (uint32_t)(fd - MAX_FDS);
        if (nid >= MAX_VFSNODES || g_nodes[nid].node_id != nid) {
            spin_unlock(&g_vfs_lock);
            return (int64_t)(VFS_EINVAL);
        }
        node = &g_nodes[nid];
    } else {
        pcb_t *cur = sched_current();
        if (!cur) { spin_unlock(&g_vfs_lock); return (int64_t)(VFS_EINVAL); }
        void **fdt = pcb_fd_table(cur);
        node = (vfsnode_t *)fdt[fd];
        if (!node) { spin_unlock(&g_vfs_lock); return (int64_t)(VFS_EINVAL); }
    }

    /* Backend seek */
    if (node->backend && node->backend->ops.seek) {
        int64_t rc = node->backend->ops.seek(node->backend, node->bctx, off, whence);
        if (rc >= 0) node->offset = (uint64_t)rc;
        spin_unlock(&g_vfs_lock);
        return rc;
    }

    /* Simple offset-based seek (for synthetic nodes or simple backends) */
    int64_t new_off;
    switch (whence) {
    case VFS_SEEK_SET: new_off = off; break;
    case VFS_SEEK_CUR: new_off = (int64_t)node->offset + off; break;
    case VFS_SEEK_END:
        if (node->size < 0) { spin_unlock(&g_vfs_lock); return VFS_ERR; }
        new_off = node->size + off;
        break;
    default:
        spin_unlock(&g_vfs_lock);
        return VFS_EINVAL;
    }

    if (new_off < 0) { spin_unlock(&g_vfs_lock); return VFS_EINVAL; }
    node->offset = (uint64_t)new_off;
    spin_unlock(&g_vfs_lock);
    return new_off;
}

int vfs_stat(const char *path, stat_info_t *out) {
    if (!path || !out) return VFS_EINVAL;

    /* SECURITY FIX: canonicalize path */
    char canon[VFS_PATH_MAX];
    if (vfs_canonicalize(path, canon, sizeof(canon)) != 0) return VFS_EINVAL;
    path = canon;

    spin_lock(&g_vfs_lock);

    const char *rel_path = NULL;
    vfs_backend_t *be = find_backend(path, &rel_path);

    if (be && be->ops.stat) {
        int rc = be->ops.stat(be, rel_path, out);
        spin_unlock(&g_vfs_lock);
        return rc;
    }

    spin_unlock(&g_vfs_lock);
    return VFS_ENOENT;
}

int vfs_unlink(const char *path) {
    if (!path || !path[0]) return VFS_EINVAL;

    /* SECURITY FIX: canonicalize path */
    char canon[VFS_PATH_MAX];
    if (vfs_canonicalize(path, canon, sizeof(canon)) != 0) return VFS_EINVAL;
    path = canon;

    spin_lock(&g_vfs_lock);

    const char *rel_path = NULL;
    vfs_backend_t *be = find_backend(path, &rel_path);

    if (be && be->ops.unlink) {
        int rc = be->ops.unlink(be, rel_path);
        spin_unlock(&g_vfs_lock);
        return rc;
    }

    spin_unlock(&g_vfs_lock);
    return VFS_ENODEV;
}

int vfs_mkdir(const char *path) {
    if (!path || !path[0]) return VFS_EINVAL;

    /* SECURITY FIX: canonicalize path */
    char canon[VFS_PATH_MAX];
    if (vfs_canonicalize(path, canon, sizeof(canon)) != 0) return VFS_EINVAL;
    path = canon;

    spin_lock(&g_vfs_lock);

    const char *rel_path = NULL;
    vfs_backend_t *be = find_backend(path, &rel_path);

    if (be && be->ops.mkdir) {
        int rc = be->ops.mkdir(be, rel_path);
        spin_unlock(&g_vfs_lock);
        return rc;
    }

    spin_unlock(&g_vfs_lock);
    return VFS_ENODEV;
}

int vfs_readdir(const char *path, vfs_dirent_t *buf,
                uint32_t cap, uint32_t *n_out)
{
    if (!path || !buf) return VFS_EINVAL;

    spin_lock(&g_vfs_lock);

    const char *rel_path = NULL;
    vfs_backend_t *be = find_backend(path, &rel_path);

    if (be && be->ops.readdir) {
        /* Open dir, read, close */
        vfs_bctx_t dctx = be->ops.open(be, rel_path, VFS_O_READ | VFS_O_DIR);
        if (!dctx) { spin_unlock(&g_vfs_lock); return VFS_ENOENT; }

        int rc = be->ops.readdir(be, dctx, buf, cap, n_out);
        be->ops.close(be, dctx);
        spin_unlock(&g_vfs_lock);
        return rc;
    }

    spin_unlock(&g_vfs_lock);
    return VFS_ENODEV;
}

int vfs_register_ring(const char *path, ringbuf_t *ring) {
    if (!path || !ring) return VFS_EINVAL;

    spin_lock(&g_vfs_lock);

    uint32_t nid = alloc_node_id();
    if (nid == 0) { spin_unlock(&g_vfs_lock); return VFS_EMFILE; }

    vfsnode_t *node = &g_nodes[nid];
    memset(node, 0, sizeof(*node));
    node->node_id  = nid;
    node->refcount = 1; /* permanent registration */
    node->flags    = VFS_O_READ | VFS_O_WRITE;
    node->size     = -1;
    strncpy(node->path, path, VFS_PATH_MAX - 1);
    node->path[VFS_PATH_MAX - 1] = '\0';
    node->ring     = ring;

    spin_unlock(&g_vfs_lock);
    return 0;
}

int vfs_unregister_node(const char *path) {
    if (!path) return VFS_EINVAL;

    spin_lock(&g_vfs_lock);

    for (uint32_t i = 0; i < MAX_VFSNODES; i++) {
        if (g_nodes[i].node_id != 0 &&
            strcmp(g_nodes[i].path, path) == 0) {
            g_nodes[i].node_id = 0;
            spin_unlock(&g_vfs_lock);
            return 0;
        }
    }

    spin_unlock(&g_vfs_lock);
    return VFS_ENOENT;
}

static void **pcb_fd_table(struct pcb *p) {
    return p ? p->fd_table : NULL;
}

/* Copy parent's fd_table into child (called during spawn).
 * Increments refcount on each borrowed vfsnode so vfs_close_all()
 * on either party doesn't free a node still in use by the other. */
void vfs_fd_inherit(struct pcb *child, struct pcb *parent) {
    if (!child || !parent) return;
    spin_lock(&g_vfs_lock);
    void **pfdt = parent->fd_table;
    void **cfdt = child->fd_table;
    for (fd_t i = 0; i < MAX_FDS; i++) {
        if (pfdt[i]) {
            vfsnode_t *node = (vfsnode_t *)pfdt[i];
            cfdt[i] = pfdt[i];
            node->refcount++;
        }
    }
    spin_unlock(&g_vfs_lock);
}

/* ------------------------------------------------------------------ */
/* vfs_pipe — create anonymous pipe, install in current process fds.   */
/* ------------------------------------------------------------------ */
int vfs_pipe(int *rfd_out, int *wfd_out) {
    struct pcb *cur = sched_current();
    if (!cur) return VFS_ERR;

    size_t rb_sz = ringbuf_required_size(1, 4096);
    ringbuf_t *rb = (ringbuf_t *)kmalloc(rb_sz);
    if (!rb) return VFS_ENOSPC;
    if (ringbuf_init(rb, 1, 4096) != 0) { kfree(rb); return VFS_ERR; }

    spin_lock(&g_vfs_lock);

    uint32_t rid = alloc_node_id();
    uint32_t wid = rid ? alloc_node_id() : 0;
    if (!rid || !wid) { spin_unlock(&g_vfs_lock); kfree(rb); return VFS_ENOSPC; }

    vfsnode_t *rn = &g_nodes[rid];
    vfsnode_t *wn = &g_nodes[wid];
    memset(rn, 0, sizeof(*rn));
    memset(wn, 0, sizeof(*wn));
    rn->node_id = rid; rn->refcount = 1; rn->flags = VFS_O_READ;  rn->ring = rb; rn->size = -1;
    wn->node_id = wid; wn->refcount = 1; wn->flags = VFS_O_WRITE; wn->ring = rb; wn->size = -1;

    void **fdt = cur->fd_table;
    fd_t rfd = 0, wfd = 0;
    for (fd_t i = 3; i < MAX_FDS; i++) {
        if (!fdt[i]) {
            if (!rfd) { fdt[i] = rn; rfd = i; }
            else if (!wfd) { fdt[i] = wn; wfd = i; break; }
        }
    }
    if (!rfd || !wfd) {
        if (rfd) fdt[rfd] = NULL;
        rn->node_id = 0; wn->node_id = 0;
        spin_unlock(&g_vfs_lock); kfree(rb); return VFS_EMFILE;
    }

    spin_unlock(&g_vfs_lock);
    *rfd_out = (int)rfd;
    *wfd_out = (int)wfd;
    return 0;
}

/* ------------------------------------------------------------------ */
/* vfs_dup2 — duplicate oldfd into newfd (newfd may be 0/1/2).         */
/* ------------------------------------------------------------------ */
int vfs_dup2(fd_t oldfd, fd_t newfd) {
    if (newfd >= MAX_FDS) return VFS_EINVAL;
    struct pcb *cur = sched_current();
    if (!cur) return VFS_ERR;
    void **fdt = cur->fd_table;

    spin_lock(&g_vfs_lock);

    vfsnode_t *node = (oldfd < MAX_FDS) ? (vfsnode_t *)fdt[oldfd] : NULL;
    if (!node) { spin_unlock(&g_vfs_lock); return VFS_EINVAL; }

    if (fdt[newfd]) {
        vfsnode_t *old = (vfsnode_t *)fdt[newfd];
        fdt[newfd] = NULL;
        old->refcount--;
        if (old->refcount == 0) {
            if (old->backend && old->backend->ops.close)
                old->backend->ops.close(old->backend, old->bctx);
            old->node_id = 0;
        }
    }

    fdt[newfd] = node;
    node->refcount++;
    spin_unlock(&g_vfs_lock);
    return (int)newfd;
}
