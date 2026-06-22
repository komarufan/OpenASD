/*
 * nettest — Network regression test for OpenASD.
 * Tests: ICMP ping, DNS resolve, TCP connect.
 * Output lines are parsed by scripts/test_net.py.
 */
#include <asd/syscall.h>
#include <stdint.h>

static void out(const char *s) {
    int n = 0; while (s[n]) n++;
    asd_write(1, s, (size_t)n);
}

static void outn(const char *s) { out(s); asd_write(1, "\n", 1); }

static void out_u32(uint32_t n) {
    char buf[16]; int i = 15;
    buf[i] = '\0';
    if (n == 0) { buf[--i] = '0'; }
    while (n) { buf[--i] = (char)('0' + n % 10); n /= 10; }
    out(&buf[i]);
}

static void out_ip(uint32_t ip) {
    out_u32((ip >> 24) & 0xFF); asd_write(1, ".", 1);
    out_u32((ip >> 16) & 0xFF); asd_write(1, ".", 1);
    out_u32((ip >>  8) & 0xFF); asd_write(1, ".", 1);
    out_u32( ip        & 0xFF);
}

int main(void) {
    int pass = 0, fail = 0;

    /* ── Test 1: ICMP ping to QEMU gateway ─────────────────────────── */
    outn("NETTEST [1/3] ping 10.0.2.2 (QEMU gateway)...");
    {
        /* parse 10.0.2.2 = 0x0A000202 */
        uint32_t gw = (10u << 24) | (0u << 16) | (2u << 8) | 2u;
        uint32_t rtt = 0;
        int r = asd_ping(gw, &rtt);
        if (r == 0) {
            out("NETTEST PING OK rtt="); out_u32(rtt); outn("ms");
            pass++;
        } else {
            outn("NETTEST PING FAIL");
            fail++;
        }
    }

    /* ── Test 2: DNS resolve github.com ────────────────────────────── */
    outn("NETTEST [2/3] DNS resolve github.com...");
    {
        uint32_t ip = 0;
        int r = asd_dns_resolve("github.com", &ip);
        if (r == 0 && ip != 0) {
            out("NETTEST DNS OK ip="); out_ip(ip); asd_write(1, "\n", 1);
            pass++;
        } else {
            outn("NETTEST DNS FAIL");
            fail++;
        }
    }

    /* ── Test 3: TCP connect to github.com:443 (external HTTPS) ────── */
    outn("NETTEST [3/3] TCP connect github.com:443...");
    {
        uint32_t ip = 0;
        asd_dns_resolve("github.com", &ip);
        if (ip == 0) ip = (140u<<24)|(82u<<16)|(121u<<8)|4u;

        int conn = -1;
        int r = asd_tcp_connect(ip, 443, &conn);
        if (r == 0 && conn >= 0) {
            outn("NETTEST TCP OK");
            asd_tcp_close(conn);
            pass++;
        } else {
            outn("NETTEST TCP FAIL");
            fail++;
        }
    }

    /* ── Summary ────────────────────────────────────────────────────── */
    out("NETTEST SUMMARY pass="); out_u32((uint32_t)pass);
    out(" fail="); out_u32((uint32_t)fail);
    asd_write(1, "\n", 1);

    if (fail == 0) outn("NETTEST ALL OK");
    else           outn("NETTEST SOME FAILED");

    asd_exit(fail > 0 ? 1 : 0);
    return 0;
}
