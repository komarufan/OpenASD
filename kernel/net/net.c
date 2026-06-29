/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, ASD Project Contributors
 *
 * Minimal network stack for OpenASD:
 *   Ethernet → ARP (reply + cache) + IPv4 → UDP
 *
 * All state is static (no heap). Packet reception is polled from
 * the virtio-net ISR; transmission is synchronous.
 */

#include "net.h"
#include "../drv/virtio_net.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

extern void sched_yield(void);

static void net_wait_for_io(void) {
    /* Wait ~one PIT tick (10 ms) of REAL time so QEMU/SLIRP can deliver packets,
     * but cooperatively — repeatedly sched_yield() instead of `hlt`.
     *
     * The old code did `sti; hlt; cli`, which HALTS THE WHOLE CPU.  When a
     * network call runs inside a user process's syscall (e.g. `apm update` from
     * the terminal), that froze the GUI and, worse, could wedge if no IRQ woke
     * the hlt — making `apm` appear to hang at "resolving ...".  Yielding lets
     * the window server / other processes run; when nothing else is runnable
     * the idle task halts on its own, still giving QEMU host time.  pit_ticks()
     * advances from the timer IRQ regardless of which task is running. */
    /* Wait for the next interrupt (the PIT @ 100 Hz wakes us within ~10 ms, or
     * the NIC IRQ fires sooner on an incoming packet).
     *
     * Do NOT sched_yield() here.  A network call made from a foreground-reaped
     * child (e.g. `apm`/`nettest` launched by the shell) runs at SCHED_RT and is
     * OFF the run queue; sched_yield would re-enqueue it, immediately re-pick
     * itself (highest priority), and double-insert its rb-node — corrupting the
     * run queue and wedging the whole scheduler.  Just hlt: the timer keeps the
     * wait bounded and the cooperative model is preserved by staying on-CPU.
     * pic_unmask(0) guarantees the PIT can wake us even during early boot, when
     * the global IRQ0 unmask in asdinit has not run yet. */
    extern void pic_unmask(int);
    pic_unmask(0);
    __asm__ volatile("sti; hlt; cli" ::: "memory");
}

/* Minimal serial debug — uses I/O port helpers defined later in this file */
static inline uint8_t net_in8(uint16_t port) {
    uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(port)); return v;
}
static inline void net_out8(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(port));
}
static void net_dbg(const char *s) {
    for (; *s; s++) {
        for (int t = 100000; t && !(net_in8(0x3F8+5) & 0x20); t--)
            __asm__ volatile("pause");
        net_out8(0x3F8, (uint8_t)*s);
    }
}

/* ------------------------------------------------------------------ */
/* Utility                                                              */
/* ------------------------------------------------------------------ */

static uint16_t htons(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}
static uint32_t htonl(uint32_t v) {
    return ((v & 0xFF000000u) >> 24) |
           ((v & 0x00FF0000u) >>  8) |
           ((v & 0x0000FF00u) <<  8) |
           ((v & 0x000000FFu) << 24);
}
#define ntohs htons
#define ntohl htonl

/* ------------------------------------------------------------------ */
/* Ethernet                                                             */
/* ------------------------------------------------------------------ */

#define ETH_ALEN 6
#define ETHERTYPE_ARP  0x0806
#define ETHERTYPE_IP   0x0800

typedef struct {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t type;
} __attribute__((packed)) eth_hdr_t;

static uint8_t g_mac[ETH_ALEN];

static const uint8_t g_bcast[ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

/* ------------------------------------------------------------------ */
/* ARP                                                                  */
/* ------------------------------------------------------------------ */

#define ARP_CACHE_SIZE 8

typedef struct {
    uint8_t  hwtype_hi, hwtype_lo;   /* 0x0001 */
    uint8_t  proto_hi,  proto_lo;    /* 0x0800 */
    uint8_t  hwlen;                  /* 6 */
    uint8_t  prlen;                  /* 4 */
    uint8_t  op_hi, op_lo;
    uint8_t  sha[ETH_ALEN];
    uint8_t  spa[4];
    uint8_t  tha[ETH_ALEN];
    uint8_t  tpa[4];
} __attribute__((packed)) arp_pkt_t;

typedef struct {
    uint32_t ip;
    uint8_t  mac[ETH_ALEN];
    int      valid;
} arp_entry_t;

static arp_entry_t g_arp_cache[ARP_CACHE_SIZE];

static void arp_cache_store(uint32_t ip, const uint8_t *mac) {
    /* Update existing */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_arp_cache[i].valid && g_arp_cache[i].ip == ip) {
            memcpy(g_arp_cache[i].mac, mac, ETH_ALEN);
            return;
        }
    }
    /* Find empty slot */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!g_arp_cache[i].valid) {
            g_arp_cache[i].ip = ip;
            memcpy(g_arp_cache[i].mac, mac, ETH_ALEN);
            g_arp_cache[i].valid = 1;
            return;
        }
    }
    /* Evict slot 0 (simple LRU approximation) */
    g_arp_cache[0].ip = ip;
    memcpy(g_arp_cache[0].mac, mac, ETH_ALEN);
    g_arp_cache[0].valid = 1;
}

static int arp_lookup(uint32_t ip, uint8_t *mac_out) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_arp_cache[i].valid && g_arp_cache[i].ip == ip) {
            memcpy(mac_out, g_arp_cache[i].mac, ETH_ALEN);
            return 0;
        }
    }
    return -1;
}

static uint8_t g_frame_buf[1536];

static void arp_send_request(uint32_t target_ip) {
    uint8_t *p = g_frame_buf;
    eth_hdr_t *eth = (eth_hdr_t *)p;
    memcpy(eth->dst, g_bcast, ETH_ALEN);
    memcpy(eth->src, g_mac,   ETH_ALEN);
    eth->type = htons(ETHERTYPE_ARP);

    arp_pkt_t *arp = (arp_pkt_t *)(p + sizeof(eth_hdr_t));
    arp->hwtype_hi = 0; arp->hwtype_lo = 1;
    arp->proto_hi  = 0x08; arp->proto_lo = 0x00;
    arp->hwlen     = ETH_ALEN;
    arp->prlen     = 4;
    arp->op_hi     = 0; arp->op_lo = 1;  /* request */
    memcpy(arp->sha, g_mac, ETH_ALEN);
    uint32_t src_ip = htonl(NET_IP_ADDR);
    memcpy(arp->spa, &src_ip, 4);
    memset(arp->tha, 0, ETH_ALEN);
    uint32_t tgt = htonl(target_ip);
    memcpy(arp->tpa, &tgt, 4);

    virtio_net_send(g_frame_buf,
                    (uint16_t)(sizeof(eth_hdr_t) + sizeof(arp_pkt_t)));
}

static void arp_handle(const uint8_t *data, uint16_t len) {
    if (len < (uint16_t)sizeof(arp_pkt_t)) return;
    const arp_pkt_t *arp = (const arp_pkt_t *)data;

    uint32_t spa;
    memcpy(&spa, arp->spa, 4);
    spa = ntohl(spa);
    arp_cache_store(spa, arp->sha);

    /* Is it a request for our IP? */
    uint32_t tpa;
    memcpy(&tpa, arp->tpa, 4);
    tpa = ntohl(tpa);
    if (arp->op_lo != 1) return;                /* not a request */
    if (tpa != NET_IP_ADDR) return;

    /* Send ARP reply */
    uint8_t *p = g_frame_buf;
    eth_hdr_t *eth = (eth_hdr_t *)p;
    memcpy(eth->dst, arp->sha, ETH_ALEN);
    memcpy(eth->src, g_mac,    ETH_ALEN);
    eth->type = htons(ETHERTYPE_ARP);

    arp_pkt_t *rep = (arp_pkt_t *)(p + sizeof(eth_hdr_t));
    rep->hwtype_hi = 0; rep->hwtype_lo = 1;
    rep->proto_hi  = 0x08; rep->proto_lo = 0x00;
    rep->hwlen     = ETH_ALEN;
    rep->prlen     = 4;
    rep->op_hi     = 0; rep->op_lo = 2;  /* reply */
    memcpy(rep->sha, g_mac,    ETH_ALEN);
    uint32_t my_ip = htonl(NET_IP_ADDR);
    memcpy(rep->spa, &my_ip, 4);
    memcpy(rep->tha, arp->sha, ETH_ALEN);
    memcpy(rep->tpa, arp->spa, 4);

    virtio_net_send(g_frame_buf,
                    (uint16_t)(sizeof(eth_hdr_t) + sizeof(arp_pkt_t)));
}

/* ------------------------------------------------------------------ */
/* IPv4 / UDP                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  ihl_ver;
    uint8_t  dscp;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} __attribute__((packed)) ip4_hdr_t;

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed)) udp_hdr_t;

#define IP_PROTO_ICMP 1
#define IP_PROTO_UDP  17
#define IP_PROTO_TCP  6

static uint16_t g_ip_id;  /* global IP packet ID counter, shared by UDP and ICMP */

static uint16_t ip_checksum(const void *ptr, size_t len) {
    const uint16_t *p = (const uint16_t *)ptr;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

/* ------------------------------------------------------------------ */
/* UDP receive ring buffer                                              */
/* ------------------------------------------------------------------ */

#define UDP_RING_SIZE   16
#define UDP_PAYLOAD_MAX 1472

typedef struct {
    uint8_t  data[UDP_PAYLOAD_MAX];
    uint16_t len;
    uint32_t src_ip;
    uint16_t src_port;
    uint8_t  valid;
} udp_slot_t;

static udp_slot_t g_udp_ring[UDP_RING_SIZE];
static uint16_t   g_udp_head;   /* next slot to write */
static uint16_t   g_udp_tail;   /* next slot to read */

static void udp_ring_push(uint32_t src_ip, uint16_t src_port,
                           const uint8_t *data, uint16_t len) {
    if (len > UDP_PAYLOAD_MAX) len = UDP_PAYLOAD_MAX;
    udp_slot_t *s = &g_udp_ring[g_udp_head % UDP_RING_SIZE];
    if (s->valid) return; /* overflow — drop */
    memcpy(s->data, data, len);
    s->len      = len;
    s->src_ip   = src_ip;
    s->src_port = src_port;
    s->valid    = 1;
    g_udp_head++;
}

int net_udp_recv(void *buf, uint16_t buf_sz,
                 uint32_t *src_ip, uint16_t *src_port) {
    /* Poll the virtio-net ISR to get fresh packets */
    virtio_net_rx_poll();

    udp_slot_t *s = &g_udp_ring[g_udp_tail % UDP_RING_SIZE];
    if (!s->valid) return -1;

    uint16_t n = s->len < buf_sz ? s->len : buf_sz;
    memcpy(buf, s->data, n);
    if (src_ip)   *src_ip   = s->src_ip;
    if (src_port) *src_port = s->src_port;
    s->valid = 0;
    g_udp_tail++;
    return (int)n;
}

/* ------------------------------------------------------------------ */
/* ICMP echo (ping)                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed)) icmp_hdr_t;

#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY   0

static volatile uint16_t g_ping_reply_id;
static volatile uint16_t g_ping_reply_seq;
static volatile int      g_ping_reply_ready;

static void icmp_handle(const uint8_t *data, uint16_t len) {
    if (len < (uint16_t)sizeof(icmp_hdr_t)) return;
    const icmp_hdr_t *ic = (const icmp_hdr_t *)data;
    if (ic->type != ICMP_ECHO_REPLY) return;
    g_ping_reply_id    = ntohs(ic->id);
    g_ping_reply_seq   = ntohs(ic->seq);
    g_ping_reply_ready = 1;
}

static int icmp_send_echo(uint32_t dst_ip, uint16_t id, uint16_t seq) {
    if (!virtio_net_ready()) return -1;
    uint8_t dst_mac[ETH_ALEN];
    uint32_t nexthop = ((dst_ip & NET_MASK) == (NET_IP_ADDR & NET_MASK))
                       ? dst_ip : NET_GW_ADDR;
    if (arp_lookup(nexthop, dst_mac) != 0) {
        arp_send_request(nexthop);
        /* Yield CPU to QEMU so it can process our ARP request and reply */
        for (int retry = 0; retry < 50; retry++) {
            net_wait_for_io();
            virtio_net_rx_poll();
            if (arp_lookup(nexthop, dst_mac) == 0) break;
        }
        if (arp_lookup(nexthop, dst_mac) != 0) return -1;
    }

    static uint8_t pf[sizeof(eth_hdr_t) + sizeof(ip4_hdr_t) + sizeof(icmp_hdr_t) + 8];
    uint8_t *p = pf;

    eth_hdr_t *eth = (eth_hdr_t *)p;
    memcpy(eth->dst, dst_mac, ETH_ALEN);
    memcpy(eth->src, g_mac,   ETH_ALEN);
    eth->type = htons(ETHERTYPE_IP);

    ip4_hdr_t *ip = (ip4_hdr_t *)(p + sizeof(eth_hdr_t));
    uint16_t ip_total = (uint16_t)(sizeof(ip4_hdr_t) + sizeof(icmp_hdr_t) + 8);
    ip->ihl_ver   = 0x45;
    ip->dscp      = 0;
    ip->total_len = htons(ip_total);
    ip->id        = htons(g_ip_id++);
    ip->frag_off  = 0;
    ip->ttl       = 64;
    ip->proto     = IP_PROTO_ICMP;
    ip->checksum  = 0;
    ip->src_ip    = htonl(NET_IP_ADDR);
    ip->dst_ip    = htonl(dst_ip);
    ip->checksum  = ip_checksum(ip, sizeof(ip4_hdr_t));

    icmp_hdr_t *ic = (icmp_hdr_t *)(p + sizeof(eth_hdr_t) + sizeof(ip4_hdr_t));
    ic->type     = ICMP_ECHO_REQUEST;
    ic->code     = 0;
    ic->checksum = 0;
    ic->id       = htons(id);
    ic->seq      = htons(seq);
    static const uint8_t ping_data[8] = {'O','p','e','n','A','S','D',0};
    memcpy((uint8_t *)ic + sizeof(icmp_hdr_t), ping_data, 8);
    ic->checksum = ip_checksum(ic, sizeof(icmp_hdr_t) + 8);

    return virtio_net_send(pf, (uint16_t)sizeof(pf));
}

int net_icmp_ping(uint32_t dst_ip, uint32_t *rtt_ms) {
    static uint16_t s_id  = 0x4153;
    static uint16_t s_seq = 0;
    uint16_t id = s_id, seq = ++s_seq;

    g_ping_reply_ready = 0;
    /* Resolve ARP. The gateway MAC should already be in cache from net_init().
     * If not (first ping after boot), send ARP request and poll. */
    uint32_t nexthop = ((dst_ip & NET_MASK) == (NET_IP_ADDR & NET_MASK))
                       ? dst_ip : NET_GW_ADDR;
    uint8_t probe_mac[6];

    for (int attempt = 0; attempt < 30; attempt++) {
        if (arp_lookup(nexthop, probe_mac) == 0) break;
            arp_send_request(nexthop);
        /* Poll RX: give QEMU time to process ARP and send reply */
        for (int h = 0; h < 10; h++) {
            net_wait_for_io();
            virtio_net_rx_poll();
            if (arp_lookup(nexthop, probe_mac) == 0) goto arp_ok;
        }
    }
    if (arp_lookup(nexthop, probe_mac) != 0) return -1;
arp_ok:
    if (icmp_send_echo(dst_ip, id, seq) != 0) return -1;

    /*
     * Wait for reply.  Use "sti; hlt; cli" to yield the CPU to QEMU
     * so it can process our ICMP echo and deliver the reply.
     * The PIT fires at 100 Hz, so each hlt sleeps at most 10 ms.
     * 100 iterations = ~1 second timeout per ping.
     */
    for (uint32_t i = 0; i < 100; i++) {
        net_wait_for_io();
        virtio_net_rx_poll();
        if (g_ping_reply_ready &&
            g_ping_reply_id == id && g_ping_reply_seq == seq) {
            g_ping_reply_ready = 0;
            /* Each iteration ≈ 1 timer tick = 10 ms at 100 Hz */
            if (rtt_ms) *rtt_ms = i * 10 < 1 ? 1 : i * 10;
            return 0;
        }
    }
    g_ping_reply_ready = 0;
    return -1;
}

/* ------------------------------------------------------------------ */
/* IPv4 dispatch                                                        */
/* ------------------------------------------------------------------ */

static void tcp_handle(uint32_t src_ip, const uint8_t *data, uint16_t len);

static void ip4_handle(const uint8_t *data, uint16_t len) {
    if (len < (uint16_t)sizeof(ip4_hdr_t)) return;
    const ip4_hdr_t *ip = (const ip4_hdr_t *)data;
    uint8_t ihl = (ip->ihl_ver & 0x0F) << 2;
    if (ihl < sizeof(ip4_hdr_t) || ihl > len) return;

    /* Use IP total_len to exclude Ethernet zero-padding on short frames */
    uint16_t ip_total = ntohs(ip->total_len);
    if (ip_total < ihl || ip_total > len) ip_total = len;

    uint32_t dst = ntohl(ip->dst_ip);
    if (dst != NET_IP_ADDR && dst != 0xFFFFFFFFu) return;

    const uint8_t *pl = data + ihl;
    uint16_t plen = (uint16_t)(ip_total - ihl);

    if (ip->proto == IP_PROTO_ICMP) {
        icmp_handle(pl, plen);
        return;
    }
    if (ip->proto == IP_PROTO_TCP) {
        tcp_handle(ntohl(ip->src_ip), pl, plen);
        return;
    }
    if (ip->proto != IP_PROTO_UDP) return;

    if (plen < (uint16_t)sizeof(udp_hdr_t)) return;
    const udp_hdr_t *udp = (const udp_hdr_t *)pl;
    uint16_t payload_len = (uint16_t)(ntohs(udp->length) - sizeof(udp_hdr_t));
    if (payload_len > plen - sizeof(udp_hdr_t))
        payload_len = (uint16_t)(plen - sizeof(udp_hdr_t));
    udp_ring_push(ntohl(ip->src_ip), ntohs(udp->src_port),
                  pl + sizeof(udp_hdr_t), payload_len);
}

/* ------------------------------------------------------------------ */
/* Ethernet dispatch (called from virtio_net_rx_poll)                  */
/* ------------------------------------------------------------------ */

void net_rx_dispatch(const void *frame, uint16_t len) {
    if (len < (uint16_t)sizeof(eth_hdr_t)) return;
    const eth_hdr_t *eth = (const eth_hdr_t *)frame;
    uint16_t type = ntohs(eth->type);
    const uint8_t *payload = (const uint8_t *)frame + sizeof(eth_hdr_t);
    uint16_t plen  = (uint16_t)(len - sizeof(eth_hdr_t));

    if (type == ETHERTYPE_ARP) {
        arp_handle(payload, plen);
    } else if (type == ETHERTYPE_IP) {
        ip4_handle(payload, plen);
    }
}

/* ------------------------------------------------------------------ */
/* UDP send                                                             */
/* ------------------------------------------------------------------ */

int net_udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                 const void *data, uint16_t len) {
    if (!virtio_net_ready()) return -1;
    if (len > UDP_PAYLOAD_MAX) return -1;

    /* Resolve destination MAC */
    uint8_t dst_mac[ETH_ALEN];
    uint32_t nexthop = dst_ip;
    /* If dst is not on our subnet, use gateway */
    if ((dst_ip & NET_MASK) != (NET_IP_ADDR & NET_MASK))
        nexthop = NET_GW_ADDR;

    if (arp_lookup(nexthop, dst_mac) != 0) {
        for (int a = 0; a < 30; a++) {
            arp_send_request(nexthop);
            for (int p = 0; p < 10; p++) {
                net_wait_for_io();
                virtio_net_rx_poll();
                if (arp_lookup(nexthop, dst_mac) == 0) goto udp_arp_ok;
            }
        }
        net_dbg("[UDP] ARP FAIL: no reply from gateway\n");
        return -1;
    }
udp_arp_ok:;

    /* Build frame */
    uint8_t *p = g_frame_buf;
    eth_hdr_t *eth = (eth_hdr_t *)p;
    memcpy(eth->dst, dst_mac, ETH_ALEN);
    memcpy(eth->src, g_mac,   ETH_ALEN);
    eth->type = htons(ETHERTYPE_IP);

    ip4_hdr_t *ip = (ip4_hdr_t *)(p + sizeof(eth_hdr_t));
    uint16_t ip_total = (uint16_t)(sizeof(ip4_hdr_t) + sizeof(udp_hdr_t) + len);
    ip->ihl_ver   = 0x45;
    ip->dscp      = 0;
    ip->total_len = htons(ip_total);
    ip->id        = htons(g_ip_id++);
    ip->frag_off  = 0;
    ip->ttl       = 64;
    ip->proto     = IP_PROTO_UDP;
    ip->checksum  = 0;
    ip->src_ip    = htonl(NET_IP_ADDR);
    ip->dst_ip    = htonl(dst_ip);
    ip->checksum  = ip_checksum(ip, sizeof(ip4_hdr_t));

    udp_hdr_t *udp = (udp_hdr_t *)(p + sizeof(eth_hdr_t) + sizeof(ip4_hdr_t));
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length   = htons((uint16_t)(sizeof(udp_hdr_t) + len));
    udp->checksum = 0;  /* optional for UDP/IPv4 */
    memcpy((uint8_t *)udp + sizeof(udp_hdr_t), data, len);

    uint16_t frame_len = (uint16_t)(sizeof(eth_hdr_t) + ip_total);
    return virtio_net_send(g_frame_buf, frame_len);
}

/* ------------------------------------------------------------------ */
/* IRQ routing hook (called from virtio_net_init)                       */
/* ------------------------------------------------------------------ */

static uint8_t g_net_irq = 0xFF;

void net_set_irq(uint8_t irq) {
    g_net_irq = irq;
}

uint8_t net_get_irq(void) {
    return g_net_irq;
}

/* ------------------------------------------------------------------ */
/* Init                                                                 */
/* ------------------------------------------------------------------ */

void net_init(void) {
    virtio_net_get_mac(g_mac);
    memset(g_arp_cache, 0, sizeof(g_arp_cache));
    memset(g_udp_ring,  0, sizeof(g_udp_ring));
    g_udp_head = 0;
    g_udp_tail = 0;
    g_ip_id    = 1;

    /* Kick off ARP for gateway — reply arrives asynchronously.
     * Do NOT hlt here: IRQ0 (PIT) is still masked at this point in boot. */
    if (virtio_net_ready())
        arp_send_request(NET_GW_ADDR);
}

/* ================================================================
 * TCP client — minimal state machine for HTTP downloads.
 *
 * Supports up to NET_TCP_MAX_CONN simultaneous connections.
 * Only active-open (client) role is implemented.  Each connection
 * uses a separate TX buffer so it doesn't clobber g_frame_buf during
 * the RX-poll spin-wait loops.
 * ================================================================ */

#define NET_TCP_MAX_CONN   4
#define NET_TCP_RX_BUF     32768   /* 32 KiB per connection  */
#define NET_TCP_EPHEM_BASE 49152   /* ephemeral port base    */
#define NET_TCP_MSS        1460    /* max segment size       */

/* TCP states */
#define TCP_ST_CLOSED      0
#define TCP_ST_SYN_SENT    1
#define TCP_ST_ESTABLISHED 2
#define TCP_ST_FIN_WAIT_1  3
#define TCP_ST_FIN_WAIT_2  4
#define TCP_ST_CLOSE_WAIT  5

/* TCP flags */
#define TCP_FL_FIN  0x01
#define TCP_FL_SYN  0x02
#define TCP_FL_RST  0x04
#define TCP_FL_PSH  0x08
#define TCP_FL_ACK  0x10

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off;   /* (hdr_len/4) << 4 */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed)) tcp_hdr_t;

typedef struct {
    uint32_t remote_ip;
    uint16_t remote_port;
    uint16_t local_port;
    uint32_t snd_nxt;       /* next seq to send        */
    uint32_t snd_una;       /* oldest un-ACKed seq     */
    uint32_t rcv_nxt;       /* next expected from peer */
    uint8_t  state;
    uint8_t  fin_rcvd;
    uint8_t  rx_buf[NET_TCP_RX_BUF];
    uint32_t rx_rd;         /* consumer (reader) index */
    uint32_t rx_wr;         /* producer (network) index */
    uint8_t  tx_buf[1536];  /* dedicated TX frame buffer */
} tcp_conn_t;

static tcp_conn_t g_tcp[NET_TCP_MAX_CONN];

/* ── TCP checksum (pseudo-header over src/dst IP, proto=6, length) ── */

static uint16_t tcp_cksum(uint32_t sip, uint32_t dip,
                           const void *tcp_data, uint16_t tcp_len) {
    uint32_t sum = 0;
    /* Pseudo-header: IPs must be in network byte order for LE-read consistency.
     * htonl converts host-order IP to network order; splitting gives correct LE words. */
    uint32_t nsip = htonl(sip);
    uint32_t ndip = htonl(dip);
    sum += (nsip >> 16) & 0xFFFF;
    sum += nsip & 0xFFFF;
    sum += (ndip >> 16) & 0xFFFF;
    sum += ndip & 0xFFFF;
    sum += htons(6);
    sum += htons(tcp_len);
    const uint16_t *p = (const uint16_t *)tcp_data;
    uint16_t len = tcp_len;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

/* ── Send one TCP segment ── */

static int tcp_tx(tcp_conn_t *c, uint8_t flags,
                  const void *data, uint16_t dlen) {
    uint8_t dst_mac[ETH_ALEN];
    uint32_t nexthop = c->remote_ip;
    if ((nexthop & NET_MASK) != (NET_IP_ADDR & NET_MASK))
        nexthop = NET_GW_ADDR;

    /* ARP with retries */
    for (int a = 0; a < 30; a++) {
        if (arp_lookup(nexthop, dst_mac) == 0) break;
        arp_send_request(nexthop);
        for (int p = 0; p < 10; p++) virtio_net_rx_poll();
        if (a == 29) return -1;
    }

    uint16_t tcp_len = (uint16_t)(sizeof(tcp_hdr_t) + dlen);
    uint16_t ip_tot  = (uint16_t)(sizeof(ip4_hdr_t)  + tcp_len);
    uint8_t *f = c->tx_buf;

    eth_hdr_t *eth = (eth_hdr_t *)f;
    memcpy(eth->dst, dst_mac, ETH_ALEN);
    memcpy(eth->src, g_mac,   ETH_ALEN);
    eth->type = htons(ETHERTYPE_IP);

    ip4_hdr_t *ip = (ip4_hdr_t *)(f + sizeof(eth_hdr_t));
    ip->ihl_ver   = 0x45;
    ip->dscp      = 0;
    ip->total_len = htons(ip_tot);
    ip->id        = htons(g_ip_id++);
    ip->frag_off  = 0;
    ip->ttl       = 64;
    ip->proto     = IP_PROTO_TCP;
    ip->checksum  = 0;
    ip->src_ip    = htonl(NET_IP_ADDR);
    ip->dst_ip    = htonl(c->remote_ip);
    ip->checksum  = ip_checksum(ip, sizeof(ip4_hdr_t));

    tcp_hdr_t *tcp = (tcp_hdr_t *)(f + sizeof(eth_hdr_t) + sizeof(ip4_hdr_t));
    tcp->src_port = htons(c->local_port);
    tcp->dst_port = htons(c->remote_port);
    tcp->seq      = htonl(c->snd_nxt);
    tcp->ack      = (flags & TCP_FL_ACK) ? htonl(c->rcv_nxt) : 0;
    tcp->data_off = (uint8_t)((sizeof(tcp_hdr_t) / 4) << 4);
    tcp->flags    = flags;
    tcp->window   = htons(16384);
    tcp->checksum = 0;
    tcp->urgent   = 0;
    if (data && dlen > 0)
        memcpy((uint8_t *)tcp + sizeof(tcp_hdr_t), data, dlen);
    tcp->checksum = tcp_cksum(NET_IP_ADDR, c->remote_ip, tcp, tcp_len);

    return virtio_net_send(f, (uint16_t)(sizeof(eth_hdr_t) + ip_tot));
}

/* ── Process one incoming TCP packet for a known connection ── */

static void tcp_rx_conn(tcp_conn_t *c, uint8_t flags,
                         uint32_t seq, uint32_t ack,
                         const uint8_t *payload, uint16_t plen) {
    if (flags & TCP_FL_RST) { c->state = TCP_ST_CLOSED; return; }

    switch (c->state) {

    case TCP_ST_SYN_SENT:
        if ((flags & (TCP_FL_SYN | TCP_FL_ACK)) == (TCP_FL_SYN | TCP_FL_ACK)) {
            c->rcv_nxt = seq + 1;
            c->snd_una = ack;
            c->state   = TCP_ST_ESTABLISHED;
            tcp_tx(c, TCP_FL_ACK, NULL, 0);
        }
        break;

    case TCP_ST_ESTABLISHED:
    case TCP_ST_FIN_WAIT_1:
    case TCP_ST_FIN_WAIT_2:
        /* ACK advances snd_una */
        if (flags & TCP_FL_ACK) {
            c->snd_una = ack;
            if (c->state == TCP_ST_FIN_WAIT_1 && ack == c->snd_nxt)
                c->state = TCP_ST_FIN_WAIT_2;
        }
        /* Buffer incoming data (in-order only, no reordering) */
        if (plen > 0 && seq == c->rcv_nxt) {
            for (uint16_t i = 0; i < plen; i++) {
                uint32_t next = (c->rx_wr + 1) % NET_TCP_RX_BUF;
                if (next == c->rx_rd) break; /* rx overflow */
                c->rx_buf[c->rx_wr] = payload[i];
                c->rx_wr = next;
            }
            c->rcv_nxt += plen;
            tcp_tx(c, TCP_FL_ACK, NULL, 0);
        } else if (plen > 0) {
            /* Out-of-order / duplicate data segment: re-ACK what we have so the
             * peer can fast-retransmit the missing segment. */
            tcp_tx(c, TCP_FL_ACK, NULL, 0);
        }
        /* Remote FIN */
        if ((flags & TCP_FL_FIN) && c->state == TCP_ST_ESTABLISHED) {
            c->rcv_nxt++;
            c->fin_rcvd = 1;
            tcp_tx(c, TCP_FL_ACK, NULL, 0);
            c->state = TCP_ST_CLOSE_WAIT;
        }
        if ((flags & TCP_FL_FIN) &&
            (c->state == TCP_ST_FIN_WAIT_1 || c->state == TCP_ST_FIN_WAIT_2)) {
            c->rcv_nxt++;
            tcp_tx(c, TCP_FL_ACK, NULL, 0);
            c->state = TCP_ST_CLOSED;
        }
        break;
    }
}

/* ── Handle incoming TCP packets (called from ip4_handle) ── */

static void tcp_handle(uint32_t src_ip, const uint8_t *data, uint16_t len) {
    if (len < (uint16_t)sizeof(tcp_hdr_t)) return;
    const tcp_hdr_t *tcp = (const tcp_hdr_t *)data;
    uint16_t src_port = ntohs(tcp->src_port);
    uint16_t dst_port = ntohs(tcp->dst_port);
    uint32_t seq = ntohl(tcp->seq);
    uint32_t ack = ntohl(tcp->ack);
    uint8_t  hlen = (uint8_t)(((tcp->data_off >> 4) & 0xF) << 2);
    if (hlen < (uint8_t)sizeof(tcp_hdr_t) || hlen > len) return;
    const uint8_t *payload = data + hlen;
    uint16_t plen = (uint16_t)(len - hlen);

    for (int i = 0; i < NET_TCP_MAX_CONN; i++) {
        tcp_conn_t *c = &g_tcp[i];
        if (c->state == TCP_ST_CLOSED) continue;
        if (c->local_port != dst_port) continue;
        if (c->remote_port != src_port) {
            net_dbg("[TCP] rx: rport mismatch\n"); continue;
        }
        if (c->remote_ip != src_ip)
            c->remote_ip = src_ip;  /* accept SLIRP NAT */
        tcp_rx_conn(c, tcp->flags, seq, ack, payload, plen);
        return;
    }
}

/* ── Public TCP API ── */

static uint16_t g_tcp_port_counter = 0;

int net_tcp_connect(uint32_t dst_ip, uint16_t dst_port, int *conn_out) {
    if (!virtio_net_ready()) return -1;
    /* Seed port counter from PIT on first use to avoid reusing ports
     * across QEMU reboots (SLIRP TIME_WAIT on host side).
     * Use full 14-bit range (16384 ports) so consecutive boots pick
     * very different starting ports. */
    if (g_tcp_port_counter == 0) {
        extern uint64_t pit_ticks(void);
        g_tcp_port_counter = (uint16_t)(pit_ticks() & 0x3FFFu) + 1;
    }
    if (!virtio_net_ready()) return -1;
    int slot = -1;
    for (int i = 0; i < NET_TCP_MAX_CONN; i++) {
        if (g_tcp[i].state == TCP_ST_CLOSED) { slot = i; break; }
    }
    if (slot < 0) return -1;

    tcp_conn_t *c = &g_tcp[slot];
    memset(c, 0, sizeof(*c));
    c->remote_ip   = dst_ip;
    c->remote_port = dst_port;
    c->local_port  = (uint16_t)(NET_TCP_EPHEM_BASE + (g_tcp_port_counter++ % 16384));
    c->snd_nxt     = (uint32_t)(0xC0DE0000u + (uint32_t)slot);
    c->state       = TCP_ST_SYN_SENT;

    if (tcp_tx(c, TCP_FL_SYN, NULL, 0) != 0) {
        c->state = TCP_ST_CLOSED; return -1;
    }
    c->snd_nxt++;   /* SYN consumes one sequence number */

    /* Wait for SYN-ACK (up to ~10s) */
    for (int t = 0; t < 1000; t++) {
        net_wait_for_io();
        virtio_net_rx_poll();
        if (c->state == TCP_ST_ESTABLISHED) { *conn_out = slot; return 0; }
        if (c->state == TCP_ST_CLOSED) return -1;
    }
    c->state = TCP_ST_CLOSED;
    return -1;
}

int net_tcp_send(int id, const void *data, uint32_t len) {
    if (id < 0 || id >= NET_TCP_MAX_CONN) return -1;
    tcp_conn_t *c = &g_tcp[id];
    if (c->state != TCP_ST_ESTABLISHED) return -1;

    const uint8_t *p = (const uint8_t *)data;
    uint32_t left = len;
    while (left > 0) {
        uint16_t chunk = (uint16_t)(left > NET_TCP_MSS ? NET_TCP_MSS : left);
        if (tcp_tx(c, TCP_FL_PSH | TCP_FL_ACK, p, chunk) != 0) return -1;
        c->snd_nxt += chunk;
        /* Wait for ACK (~2s timeout) */
        for (int t = 0; t < 200; t++) {
            net_wait_for_io();
            virtio_net_rx_poll();
            if (c->snd_una >= c->snd_nxt) break;
            if (c->state != TCP_ST_ESTABLISHED) return -1;
        }
        p    += chunk;
        left -= chunk;
    }
    return (int)len;
}

int net_tcp_recv(int id, void *buf, uint32_t cap, int blocking) {
    if (id < 0 || id >= NET_TCP_MAX_CONN) return -1;
    tcp_conn_t *c = &g_tcp[id];

    if (blocking) {
        /* Wait up to ~30s for data or FIN */
        for (int t = 0; t < 3000; t++) {
            if (c->rx_rd != c->rx_wr) break;
            if (c->state == TCP_ST_CLOSE_WAIT || c->state == TCP_ST_CLOSED) break;
            net_wait_for_io();
            virtio_net_rx_poll();
        }
    }

    uint8_t *out = (uint8_t *)buf;
    uint32_t got = 0;
    while (got < cap && c->rx_rd != c->rx_wr) {
        out[got++] = c->rx_buf[c->rx_rd];
        c->rx_rd = (c->rx_rd + 1) % NET_TCP_RX_BUF;
    }
    if (got == 0 && c->fin_rcvd) return 0;   /* EOF */
    return (int)got;
}

void net_tcp_close(int id) {
    if (id < 0 || id >= NET_TCP_MAX_CONN) return;
    tcp_conn_t *c = &g_tcp[id];
    if (c->state == TCP_ST_ESTABLISHED || c->state == TCP_ST_CLOSE_WAIT) {
        c->state = TCP_ST_FIN_WAIT_1;
        tcp_tx(c, TCP_FL_FIN | TCP_FL_ACK, NULL, 0);
        c->snd_nxt++;
        for (int t = 0; t < 100; t++) {
            net_wait_for_io();
            virtio_net_rx_poll();
            if (c->state == TCP_ST_CLOSED) break;
        }
    }
    c->state = TCP_ST_CLOSED;
}

int net_tcp_alive(int id) {
    if (id < 0 || id >= NET_TCP_MAX_CONN) return 0;
    uint8_t s = g_tcp[id].state;
    return s == TCP_ST_ESTABLISHED || s == TCP_ST_CLOSE_WAIT;
}

/* ================================================================
 * DNS — resolve hostname to IPv4 via UDP to 8.8.8.8:53.
 * Implements a minimal DNS A-record query (no AAAA, no CNAME follow).
 * ================================================================ */

#define DNS_SERVER_IP  0x0A000203u  /* 10.0.2.3 — QEMU SLIRP built-in DNS proxy */
#define DNS_PORT       53
#define DNS_SRC_PORT   1053
#define DNS_QUERY_ID   0xABCDu

static int dns_build_query(const char *host, uint8_t *buf, int cap) {
    /* Fixed 12-byte DNS header */
    if (cap < 12) return -1;
    buf[0]  = (DNS_QUERY_ID >> 8) & 0xFF;  /* ID hi */
    buf[1]  = DNS_QUERY_ID & 0xFF;          /* ID lo */
    buf[2]  = 0x01; buf[3]  = 0x00;         /* flags: recursion desired */
    buf[4]  = 0x00; buf[5]  = 0x01;         /* QDCOUNT = 1 */
    buf[6]  = 0x00; buf[7]  = 0x00;         /* ANCOUNT = 0 */
    buf[8]  = 0x00; buf[9]  = 0x00;         /* NSCOUNT = 0 */
    buf[10] = 0x00; buf[11] = 0x00;         /* ARCOUNT = 0 */

    /* QNAME: encode "www.example.com" as \3www\7example\3com\0 */
    int pos = 12;
    const char *p = host;
    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        int llen = (int)(dot - p);
        if (pos + 1 + llen + 4 >= cap) return -1;
        buf[pos++] = (uint8_t)llen;
        for (int i = 0; i < llen; i++) buf[pos++] = (uint8_t)p[i];
        p = dot;
        if (*p == '.') p++;
    }
    buf[pos++] = 0;          /* root label */
    buf[pos++] = 0x00; buf[pos++] = 0x01;  /* QTYPE  = A  */
    buf[pos++] = 0x00; buf[pos++] = 0x01;  /* QCLASS = IN */
    return pos;
}

int net_dns_resolve(const char *hostname, uint32_t *ip_out) {
    if (!virtio_net_ready()) {
        net_dbg("[DNS] FAIL: virtio_net not ready\n");
        return -1;
    }

    /* Check if it's already a dotted-decimal IP */
    uint32_t ip = 0; int dots = 0; const char *s = hostname;
    while (*s) {
        if (*s >= '0' && *s <= '9') { ip = (ip & 0xFFFFFF00u) | (uint8_t)((ip & 0xFF)*10 + (*s-'0')); }
        else if (*s == '.') { ip <<= 8; dots++; }
        else { dots = -1; break; }
        s++;
    }
    if (dots == 3) { *ip_out = ip; return 0; }

    static uint8_t qbuf[256];
    int qlen = dns_build_query(hostname, qbuf, (int)sizeof(qbuf));
    if (qlen < 0) return -1;

    /* Send DNS query (UDP) */
    if (net_udp_send(DNS_SERVER_IP, DNS_SRC_PORT, DNS_PORT,
                     qbuf, (uint16_t)qlen) != 0) {
        net_dbg("[DNS] FAIL: UDP send failed\n");
        return -1;
    }

    /* Wait for response (up to ~3s, checking UDP ring) */
    static uint8_t rbuf[512];
    for (int t = 0; t < 300; t++) {
        net_wait_for_io();
        virtio_net_rx_poll();

        uint32_t src_ip = 0; uint16_t src_port = 0;
        int rlen = net_udp_recv(rbuf, sizeof(rbuf), &src_ip, &src_port);
        if (rlen < 12) continue;
        /* Check ID and QR bit (response) */
        uint16_t rid = ((uint16_t)rbuf[0] << 8) | rbuf[1];
        if (rid != DNS_QUERY_ID) continue;
        if (!(rbuf[2] & 0x80)) continue; /* not a response */

        uint16_t ancount = ((uint16_t)rbuf[6] << 8) | rbuf[7];
        if (ancount == 0) return -1;

        /* Skip question section */
        int pos = 12;
        while (pos < rlen && rbuf[pos]) {
            if ((rbuf[pos] & 0xC0) == 0xC0) { pos += 2; break; }
            pos += rbuf[pos] + 1;
        }
        if (pos < rlen && rbuf[pos] == 0) pos++;
        pos += 4; /* skip QTYPE + QCLASS */

        /* Parse first answer record */
        while (ancount-- > 0 && pos + 10 < rlen) {
            /* Skip name (may be pointer or label) */
            if ((rbuf[pos] & 0xC0) == 0xC0) pos += 2;
            else { while (pos < rlen && rbuf[pos]) pos += rbuf[pos]+1; pos++; }

            if (pos + 10 > rlen) break;
            uint16_t rtype  = ((uint16_t)rbuf[pos]<<8)|rbuf[pos+1]; pos += 2;
            pos += 2; /* class */
            pos += 4; /* ttl   */
            uint16_t rdlen = ((uint16_t)rbuf[pos]<<8)|rbuf[pos+1]; pos += 2;

            if (rtype == 1 && rdlen == 4 && pos + 4 <= rlen) {
                *ip_out = ((uint32_t)rbuf[pos]   << 24)
                        | ((uint32_t)rbuf[pos+1] << 16)
                        | ((uint32_t)rbuf[pos+2] <<  8)
                        | ((uint32_t)rbuf[pos+3]);
                return 0;
            }
            pos += rdlen;
        }
        net_dbg("[DNS] FAIL: no A record in response\n");
        return -1;
    }
    net_dbg("[DNS] FAIL: timeout waiting for UDP reply\n");
    return -1;
}
