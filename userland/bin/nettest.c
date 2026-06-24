/*
 * nettest — Network regression test for OpenASD.
 * Tests: ICMP ping, DNS resolve, TCP connect, TLS handshake, TLS HTTP GET.
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

static int slen(const char *s) { int n=0; while(s[n])n++; return n; }

int main(void) {
    int pass = 0, fail = 0;

    /* ── Test 1: ICMP ping to QEMU gateway ─────────────────────────── */
    outn("NETTEST [1/4] ping 10.0.2.2 (QEMU gateway)...");
    {
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
    outn("NETTEST [2/4] DNS resolve github.com...");
    uint32_t gh_ip = 0;
    {
        int r = asd_dns_resolve("github.com", &gh_ip);
        if (r == 0 && gh_ip != 0) {
            out("NETTEST DNS OK ip="); out_ip(gh_ip); asd_write(1, "\n", 1);
            pass++;
        } else {
            outn("NETTEST DNS FAIL");
            gh_ip = (140u<<24)|(82u<<16)|(121u<<8)|4u; /* fallback */
            fail++;
        }
    }

    /* ── Test 3: TLS handshake github.com:443 ──────────────────────── */
    outn("NETTEST [3/4] TLS handshake github.com:443...");
    {
        int tcp_conn = -1;
        if (asd_tcp_connect(gh_ip, 443, &tcp_conn) != 0) {
            outn("NETTEST TLS FAIL (tcp connect)");
            fail++;
        } else {
            outn("NETTEST TCP OK");
            int tls_id = asd_tls_connect(tcp_conn, "github.com");
            if (tls_id >= 0) {
                outn("NETTEST TLS OK");
                asd_tls_close(tls_id);
                pass++;
            } else {
                outn("NETTEST TLS FAIL (handshake)");
                fail++;
            }
            asd_tcp_close(tcp_conn);
        }
    }

    /* ── Test 4: TLS HTTP GET — send request, verify response ──────── */
    outn("NETTEST [4/4] TLS HTTP GET github.com:443...");
    {
        int tcp_conn = -1;
        if (asd_tcp_connect(gh_ip, 443, &tcp_conn) != 0) {
            outn("NETTEST TLS HTTP FAIL (tcp connect)");
            fail++;
        } else {
            int tls_id = asd_tls_connect(tcp_conn, "github.com");
            if (tls_id < 0) {
                outn("NETTEST TLS HTTP FAIL (handshake)");
                fail++;
                asd_tcp_close(tcp_conn);
            } else {
                /* Send HTTP/1.0 GET */
                const char *req =
                    "GET / HTTP/1.0\r\nHost: github.com\r\n"
                    "User-Agent: nettest/1.0\r\nConnection: close\r\n\r\n";
                int slen_req = slen(req);
                long sent = asd_tls_send(tls_id, req, (size_t)slen_req);
                if (sent < 0) {
                    outn("NETTEST TLS HTTP FAIL (send)");
                    fail++;
                } else {
                    /* Receive response — expect "HTTP/" as first bytes */
                    static char rbuf[4096];
                    long n = asd_tls_recv(tls_id, rbuf, sizeof(rbuf) - 1, 1);
                    if (n > 4 &&
                        rbuf[0]=='H' && rbuf[1]=='T' &&
                        rbuf[2]=='T' && rbuf[3]=='P' && rbuf[4]=='/') {
                        /* Extract status line for debug (first 32 chars) */
                        char sl[33]; int si=0;
                        while (si < 32 && si < (int)n && rbuf[si] != '\r') {
                            sl[si] = rbuf[si]; si++;
                        }
                        sl[si] = 0;
                        out("NETTEST TLS HTTP OK status="); outn(sl);
                        pass++;
                    } else if (n <= 0) {
                        out("NETTEST TLS HTTP FAIL (recv=");
                        out_u32((uint32_t)(n < 0 ? 0 : n));
                        outn(")");
                        fail++;
                    } else {
                        /* Got data but not HTTP — print first 16 bytes as hex */
                        static const char *hx="0123456789ABCDEF";
                        char dbg[48]; int di=0;
                        int show = n < 16 ? (int)n : 16;
                        for(int i=0;i<show;i++){
                            dbg[di++]=hx[((unsigned char)rbuf[i])>>4];
                            dbg[di++]=hx[((unsigned char)rbuf[i])&0xF];
                            dbg[di++]=' ';
                        }
                        dbg[di]=0;
                        out("NETTEST TLS HTTP FAIL (bad response: "); out(dbg); outn(")");
                        fail++;
                    }
                }
                asd_tls_close(tls_id);
                asd_tcp_close(tcp_conn);
            }
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
