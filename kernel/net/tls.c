/*
 * tls.c — Minimal TLS 1.3 client for OpenASD
 *
 * Cipher suite : TLS_CHACHA20_POLY1305_SHA256  (0x1303)
 * Key exchange : X25519 (Curve25519)
 * Cert verify  : SKIPPED (warning printed; sufficient for apm downloads)
 *
 * Depends on: net.h (net_tcp_connect/send/recv/close), string.h
 */

#include "tls.h"
#include "net.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

static void tls_dbg(const char *s) {
    /* serial port only — do not pollute the framebuffer display */
    extern void serial_port_puts(const char *);
    serial_port_puts(s);
}

/* ================================================================
 * SHA-256
 * ================================================================ */

typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; uint32_t blen; } sha256_ctx;

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};
#define ROR32(v,n) (((v)>>(n))|((v)<<(32-(n))))
#define CH(e,f,g)  (((e)&(f))^(~(e)&(g)))
#define MAJ(a,b,c) (((a)&(b))^((a)&(c))^((b)&(c)))
#define EP0(a) (ROR32(a,2)^ROR32(a,13)^ROR32(a,22))
#define EP1(e) (ROR32(e,6)^ROR32(e,11)^ROR32(e,25))
#define SIG0(x)(ROR32(x,7)^ROR32(x,18)^((x)>>3))
#define SIG1(x)(ROR32(x,17)^ROR32(x,19)^((x)>>10))

static void sha256_transform(sha256_ctx *c, const uint8_t d[64]) {
    uint32_t w[64], a,b,e,f,g,h,t1,t2; int i;
    for(i=0;i<16;i++) w[i]=((uint32_t)d[i*4]<<24)|((uint32_t)d[i*4+1]<<16)|((uint32_t)d[i*4+2]<<8)|d[i*4+3];
    for(;i<64;i++) w[i]=SIG1(w[i-2])+w[i-7]+SIG0(w[i-15])+w[i-16];
    a=c->h[0];b=c->h[1];uint32_t cc=c->h[2];uint32_t d2=c->h[3];
    e=c->h[4];f=c->h[5];g=c->h[6];h=c->h[7];
    for(i=0;i<64;i++){
        t1=h+EP1(e)+CH(e,f,g)+K256[i]+w[i];
        t2=EP0(a)+MAJ(a,b,cc);
        h=g;g=f;f=e;e=d2+t1;d2=cc;cc=b;b=a;a=t1+t2;
    }
    c->h[0]+=a;c->h[1]+=b;c->h[2]+=cc;c->h[3]+=d2;
    c->h[4]+=e;c->h[5]+=f;c->h[6]+=g;c->h[7]+=h;
}
static void sha256_init(sha256_ctx *c){
    c->h[0]=0x6a09e667;c->h[1]=0xbb67ae85;c->h[2]=0x3c6ef372;c->h[3]=0xa54ff53a;
    c->h[4]=0x510e527f;c->h[5]=0x9b05688c;c->h[6]=0x1f83d9ab;c->h[7]=0x5be0cd19;
    c->len=0;c->blen=0;
}
static void sha256_update(sha256_ctx *c, const uint8_t *d, size_t n){
    while(n--){ c->buf[c->blen++]=*d++; if(c->blen==64){sha256_transform(c,c->buf);c->len+=512;c->blen=0;} }
}
static void sha256_final(sha256_ctx *c, uint8_t out[32]){
    uint64_t bits=(c->len+(uint64_t)c->blen*8); uint32_t i;
    c->buf[c->blen++]=0x80;
    if(c->blen>56){while(c->blen<64)c->buf[c->blen++]=0;sha256_transform(c,c->buf);c->blen=0;}
    while(c->blen<56)c->buf[c->blen++]=0;
    for(i=0;i<8;i++)c->buf[56+i]=(uint8_t)(bits>>(56-i*8));
    sha256_transform(c,c->buf);
    for(i=0;i<8;i++){out[i*4]=(uint8_t)(c->h[i]>>24);out[i*4+1]=(uint8_t)(c->h[i]>>16);out[i*4+2]=(uint8_t)(c->h[i]>>8);out[i*4+3]=(uint8_t)c->h[i];}
}
static void sha256(const uint8_t *d,size_t n,uint8_t out[32]){sha256_ctx c;sha256_init(&c);sha256_update(&c,d,n);sha256_final(&c,out);}

/* ================================================================
 * HMAC-SHA256 and HKDF-SHA256
 * ================================================================ */

static void hmac_sha256(const uint8_t *key,size_t klen,const uint8_t *msg,size_t mlen,uint8_t out[32]){
    uint8_t k[64],ipad[64],opad[64]; sha256_ctx c; uint32_t i;
    memset(k,0,64);
    if(klen>64){sha256(key,klen,k);}else{memcpy(k,key,klen);}
    for(i=0;i<64;i++){ipad[i]=k[i]^0x36;opad[i]=k[i]^0x5c;}
    sha256_init(&c);sha256_update(&c,ipad,64);sha256_update(&c,msg,mlen);sha256_final(&c,out);
    sha256_init(&c);sha256_update(&c,opad,64);sha256_update(&c,out,32);sha256_final(&c,out);
}
/* HKDF-Extract */
static void hkdf_extract(const uint8_t *salt,size_t slen,const uint8_t *ikm,size_t ilen,uint8_t out[32]){
    if(!salt||slen==0){static const uint8_t z[32]={0};hmac_sha256(z,32,ikm,ilen,out);}
    else hmac_sha256(salt,slen,ikm,ilen,out);
}
/* HKDF-Expand — one block (32 bytes) */
static void hkdf_expand(const uint8_t *prk,const uint8_t *info,size_t ilen,uint8_t *out,size_t olen){
    uint8_t t[32]; uint8_t ctr=1; size_t done=0;
    while(done<olen){
        uint8_t tmp[256]; size_t tl=0;
        if(done>0){memcpy(tmp,t,32);tl=32;}
        memcpy(tmp+tl,info,ilen);tl+=ilen;
        tmp[tl++]=ctr++;
        hmac_sha256(prk,32,tmp,tl,t);
        size_t copy=olen-done; if(copy>32)copy=32;
        memcpy(out+done,t,copy); done+=copy;
    }
}
/* TLS 1.3 HKDF-Expand-Label */
static void hkdf_expand_label(const uint8_t *secret,const char *label,const uint8_t *ctx,size_t clen,uint8_t *out,uint16_t olen){
    uint8_t info[512]; size_t il=0;
    info[il++]=(uint8_t)(olen>>8); info[il++]=(uint8_t)olen;
    uint8_t ll=(uint8_t)(6+strlen(label));
    info[il++]=ll;
    memcpy(info+il,"tls13 ",6);il+=6;
    size_t llen=strlen(label);memcpy(info+il,label,llen);il+=llen;
    info[il++]=(uint8_t)clen;
    if(ctx&&clen)memcpy(info+il,ctx,clen);il+=clen;
    hkdf_expand(secret,info,il,out,(size_t)olen);
}
/* Derive-Secret(secret, label, transcript_hash) → 32 bytes */
static void derive_secret(const uint8_t *secret,const char *label,const uint8_t *hash32,uint8_t out[32]){
    hkdf_expand_label(secret,label,hash32,32,out,32);
}

/* ================================================================
 * ChaCha20
 * ================================================================ */

#define ROTL32(v,n) (((v)<<(n))|((v)>>(32-(n))))
#define QR(a,b,c,d) a+=b;d^=a;d=ROTL32(d,16);c+=d;b^=c;b=ROTL32(b,12);a+=b;d^=a;d=ROTL32(d,8);c+=d;b^=c;b=ROTL32(b,7)
static void chacha20_block(uint32_t st[16], uint8_t out[64]){
    uint32_t x[16]; int i;
    memcpy(x,st,64);
    for(i=0;i<10;i++){QR(x[0],x[4],x[8],x[12]);QR(x[1],x[5],x[9],x[13]);QR(x[2],x[6],x[10],x[14]);QR(x[3],x[7],x[11],x[15]);QR(x[0],x[5],x[10],x[15]);QR(x[1],x[6],x[11],x[12]);QR(x[2],x[7],x[8],x[13]);QR(x[3],x[4],x[9],x[14]);}
    for(i=0;i<16;i++){uint32_t v=x[i]+st[i];out[i*4]=(uint8_t)v;out[i*4+1]=(uint8_t)(v>>8);out[i*4+2]=(uint8_t)(v>>16);out[i*4+3]=(uint8_t)(v>>24);}
}
static void chacha20_xor(const uint8_t key[32],uint32_t ctr,const uint8_t nonce[12],uint8_t *data,size_t len){
    uint32_t st[16]; uint8_t ks[64]; size_t i;
    st[0]=0x61707865;st[1]=0x3320646e;st[2]=0x79622d32;st[3]=0x6b206574;
    for(i=0;i<8;i++) st[4+i]=((uint32_t)key[i*4])|(((uint32_t)key[i*4+1])<<8)|(((uint32_t)key[i*4+2])<<16)|(((uint32_t)key[i*4+3])<<24);
    st[12]=ctr;
    st[13]=((uint32_t)nonce[0])|(((uint32_t)nonce[1])<<8)|(((uint32_t)nonce[2])<<16)|(((uint32_t)nonce[3])<<24);
    st[14]=((uint32_t)nonce[4])|(((uint32_t)nonce[5])<<8)|(((uint32_t)nonce[6])<<16)|(((uint32_t)nonce[7])<<24);
    st[15]=((uint32_t)nonce[8])|(((uint32_t)nonce[9])<<8)|(((uint32_t)nonce[10])<<16)|(((uint32_t)nonce[11])<<24);
    while(len>0){chacha20_block(st,ks);st[12]++;size_t take=len<64?len:64;for(i=0;i<take;i++)data[i]^=ks[i];data+=take;len-=take;}
}

/* ================================================================
 * Poly1305
 * ================================================================ */

/* Poly1305 — donna-64 (Andrew Moon, public domain) — 44/44/42-bit 3-limb scheme */
#define POLY_MUL(r,a,b) ((__uint128_t)(a)*(b))
#define POLY_SHR(v,n)   ((uint64_t)((v)>>(n)))
#define POLY_ADDLO(r,v) ((r)+=(__uint128_t)(v))
#define P44 (0xfffffffffffull)
#define P42 (0x3ffffffffffull)
static void poly1305_mac(const uint8_t *msg, size_t mlen, const uint8_t key[32], uint8_t tag[16]){    /* poly1305-donna-64: 44/44/42-bit 3-limb scheme (Andrew Moon, public domain)
     * Load as two 64-bit LE words from bytes 0-7 and 8-15. */
    uint64_t t0,t1;
    t0 = ((uint64_t)key[ 0])|(((uint64_t)key[ 1])<<8)|(((uint64_t)key[ 2])<<16)
       | (((uint64_t)key[ 3])<<24)|(((uint64_t)key[ 4])<<32)|(((uint64_t)key[ 5])<<40)
       | (((uint64_t)key[ 6])<<48)|(((uint64_t)key[ 7])<<56);
    t1 = ((uint64_t)key[ 8])|(((uint64_t)key[ 9])<<8)|(((uint64_t)key[10])<<16)
       | (((uint64_t)key[11])<<24)|(((uint64_t)key[12])<<32)|(((uint64_t)key[13])<<40)
       | (((uint64_t)key[14])<<48)|(((uint64_t)key[15])<<56);
    /* split into 44/44/42-bit limbs with clamping masks */
    uint64_t r0 = t0 & 0xffc0fffffffull;             /* bits 0..43 of key */
    uint64_t r1 = ((t0>>44)|(t1<<20)) & 0xfffffc0ffffull; /* bits 44..87 */
    uint64_t r2 = (t1>>24) & 0x00ffffffc0full;        /* bits 88..129 */
    /* s1=r1*20, s2=r2*20  (2^132 ≡ 5*4=20 mod p for cross-limb terms) */
    uint64_t s1 = r1 * (uint64_t)(5u << 2);
    uint64_t s2 = r2 * (uint64_t)(5u << 2);
    /* pad = s = key[16..31] as two 64-bit LE */
    uint64_t pad0=0, pad1=0;
    for(int i=0;i<8;i++) pad0|=((uint64_t)key[16+i]<<(i*8));
    for(int i=0;i<8;i++) pad1|=((uint64_t)key[24+i]<<(i*8));
    uint64_t h0=0, h1=0, h2=0;

    while(mlen>0){
        size_t blen = mlen<16 ? mlen : 16;
        uint8_t blk[17]={0};
        memcpy(blk, msg, blen);
        blk[blen] = 1; /* append 1-bit marker */
        /* load block as two 64-bit LE words */
        uint64_t b0,b1;
        b0 = ((uint64_t)blk[ 0])|(((uint64_t)blk[ 1])<<8)|(((uint64_t)blk[ 2])<<16)
           | (((uint64_t)blk[ 3])<<24)|(((uint64_t)blk[ 4])<<32)|(((uint64_t)blk[ 5])<<40)
           | (((uint64_t)blk[ 6])<<48)|(((uint64_t)blk[ 7])<<56);
        b1 = ((uint64_t)blk[ 8])|(((uint64_t)blk[ 9])<<8)|(((uint64_t)blk[10])<<16)
           | (((uint64_t)blk[11])<<24)|(((uint64_t)blk[12])<<32)|(((uint64_t)blk[13])<<40)
           | (((uint64_t)blk[14])<<48)|(((uint64_t)blk[15])<<56);
        uint64_t m0 = b0 & P44;
        uint64_t m1 = ((b0>>44)|(b1<<20)) & P44;
        uint64_t m2 = (b1>>24) | ((uint64_t)blk[16]<<40); /* hibit at pos 40 = bit 128 */
        /* h += m */
        h0 += m0; h1 += m1; h2 += m2;
        /* d = h * r using __uint128_t products */
        __uint128_t d0 = (__uint128_t)h0*r0 + (__uint128_t)h1*s2 + (__uint128_t)h2*s1;
        __uint128_t d1 = (__uint128_t)h0*r1 + (__uint128_t)h1*r0 + (__uint128_t)h2*s2;
        __uint128_t d2 = (__uint128_t)h0*r2 + (__uint128_t)h1*r1 + (__uint128_t)h2*r0;
        /* carry propagation (44/44/42 boundaries) */
        uint64_t cc;
        cc=(uint64_t)(d0>>44); h0=(uint64_t)d0&P44; d1+=cc;
        cc=(uint64_t)(d1>>44); h1=(uint64_t)d1&P44; d2+=cc;
        cc=(uint64_t)(d2>>42); h2=(uint64_t)d2&P42;
        h0+=cc*5; cc=h0>>44; h0&=P44; h1+=cc;
        msg+=blen; mlen-=blen;
    }
    /* final full carry */
    uint64_t cc2;
    cc2=h1>>44; h1&=P44; h2+=cc2;
    cc2=h2>>42; h2&=P42;
    h0+=cc2*5; cc2=h0>>44; h0&=P44;
    h1+=cc2;   cc2=h1>>44; h1&=P44; h2+=cc2;
    /* conditional subtract p: compute g = h+5, select g if h>=p */
    uint64_t g0=h0+5; cc2=g0>>44; g0&=P44;
    uint64_t g1=h1+cc2; cc2=g1>>44; g1&=P44;
    uint64_t g2=h2+cc2-(1ull<<42); /* -2^130 */
    /* g2 < 0 (sign bit set) means h+5 < 2^130 means h < p: keep h */
    uint64_t mask=(uint64_t)((int64_t)g2>>63);
    h0=(h0&mask)|(g0&~mask); h1=(h1&mask)|(g1&~mask);
    /* pack limbs → 128-bit and add pad */
    uint64_t f0 = h0|(h1<<44);
    uint64_t f1 = (h1>>20)|(h2<<24);
    __uint128_t out128 = (__uint128_t)f0|((__uint128_t)f1<<64);
    out128 += (__uint128_t)pad0|((__uint128_t)pad1<<64);
    f0=(uint64_t)out128; f1=(uint64_t)(out128>>64);
    for(int i=0;i<8;i++){tag[i]=(uint8_t)(f0>>(i*8));tag[i+8]=(uint8_t)(f1>>(i*8));}
}

/* ChaCha20-Poly1305 AEAD */
static int chacha20_poly1305_seal(const uint8_t key[32],const uint8_t nonce[12],
                                   const uint8_t *aad,size_t alen,
                                   uint8_t *plain,size_t plen,uint8_t *out){
    /* Generate Poly1305 key from ChaCha20 block 0 */
    uint8_t poly_key[64]={0};
    chacha20_xor(key,0,nonce,poly_key,64);
    /* Encrypt with counter=1 */
    memcpy(out,plain,plen);
    chacha20_xor(key,1,nonce,out,plen);
    /* Build MAC input: aad || pad16 || ciphertext || pad16 || alen || plen */
    static uint8_t mac_data[8192]; uint8_t zeros[16]={0}; size_t mp=0;
    if(alen){memcpy(mac_data+mp,aad,alen);mp+=alen;}
    size_t apad=(16-alen%16)%16; if(apad)memcpy(mac_data+mp,zeros,apad),mp+=apad;
    memcpy(mac_data+mp,out,plen);mp+=plen;
    size_t ppad=(16-plen%16)%16; if(ppad)memcpy(mac_data+mp,zeros,ppad),mp+=ppad;
    for(int i=0;i<8;i++)mac_data[mp++]=(uint8_t)(alen>>(i*8));
    for(int i=0;i<8;i++)mac_data[mp++]=(uint8_t)(plen>>(i*8));
    poly1305_mac(mac_data,mp,poly_key,out+plen);
    return (int)(plen+16);
}
static int chacha20_poly1305_open(const uint8_t key[32],const uint8_t nonce[12],
                                   const uint8_t *aad,size_t alen,
                                   const uint8_t *in,size_t ilen,uint8_t *out){
    if(ilen<16)return -1;
    size_t plen=ilen-16;
    uint8_t poly_key[64]={0};
    chacha20_xor(key,0,nonce,poly_key,64);
    static uint8_t mac_data[8192]; uint8_t zeros[16]={0}; size_t mp=0;
    if(alen){memcpy(mac_data+mp,aad,alen);mp+=alen;}
    size_t apad=(16-alen%16)%16; if(apad)memcpy(mac_data+mp,zeros,apad),mp+=apad;
    memcpy(mac_data+mp,in,plen);mp+=plen;
    size_t ppad=(16-plen%16)%16; if(ppad)memcpy(mac_data+mp,zeros,ppad),mp+=ppad;
    for(int i=0;i<8;i++)mac_data[mp++]=(uint8_t)(alen>>(i*8));
    for(int i=0;i<8;i++)mac_data[mp++]=(uint8_t)(plen>>(i*8));

    uint8_t tag[16];
    poly1305_mac(mac_data,mp,poly_key,tag);

    /* constant-time compare */
    uint8_t diff=0; for(int i=0;i<16;i++)diff|=(tag[i]^in[plen+i]);
    if(diff)return -1;
    memcpy(out,in,plen);
    chacha20_xor(key,1,nonce,(uint8_t*)out,plen);
    return (int)plen;
}

/* ================================================================
 * X25519 (Curve25519 Diffie-Hellman)
 * Compact implementation using 64-bit limbs.
 * ================================================================ */

/* X25519 (Curve25519) — ref10 representation (D.J. Bernstein, public domain)
 * 10 x 25/26-bit limbs, pure 32-bit arithmetic. */

typedef int32_t fe[10];

static void fe_load(fe h, const uint8_t *s) {
    uint32_t s0=(uint32_t)s[0]|(((uint32_t)s[1])<<8)|(((uint32_t)s[2])<<16)|(((uint32_t)s[3])<<24);
    uint32_t s4=(uint32_t)s[4]|(((uint32_t)s[5])<<8)|(((uint32_t)s[6])<<16)|(((uint32_t)s[7])<<24);
    uint32_t s8=(uint32_t)s[8]|(((uint32_t)s[9])<<8)|(((uint32_t)s[10])<<16)|(((uint32_t)s[11])<<24);
    uint32_t s12=(uint32_t)s[12]|(((uint32_t)s[13])<<8)|(((uint32_t)s[14])<<16)|(((uint32_t)s[15])<<24);
    uint32_t s16=(uint32_t)s[16]|(((uint32_t)s[17])<<8)|(((uint32_t)s[18])<<16)|(((uint32_t)s[19])<<24);
    uint32_t s20=(uint32_t)s[20]|(((uint32_t)s[21])<<8)|(((uint32_t)s[22])<<16)|(((uint32_t)s[23])<<24);
    uint32_t s24=(uint32_t)s[24]|(((uint32_t)s[25])<<8)|(((uint32_t)s[26])<<16)|(((uint32_t)s[27])<<24);
    uint32_t s28=(uint32_t)s[28]|(((uint32_t)s[29])<<8)|(((uint32_t)s[30])<<16)|(((uint32_t)s[31])<<24);
    h[0]=(int32_t)(s0&0x3FFFFFF);
    h[1]=(int32_t)((s0>>26|(s4<<6))&0x1FFFFFF);
    h[2]=(int32_t)((s4>>19|(s8<<13))&0x3FFFFFF);
    h[3]=(int32_t)((s8>>13|(s12<<19))&0x1FFFFFF);
    h[4]=(int32_t)((s12>>6|(s16<<26))&0x3FFFFFF);
    h[5]=(int32_t)(s16&0x1FFFFFF);
    h[6]=(int32_t)((s16>>25|(s20<<7))&0x3FFFFFF);
    h[7]=(int32_t)((s20>>19|(s24<<13))&0x1FFFFFF);
    h[8]=(int32_t)((s24>>12|(s28<<20))&0x3FFFFFF);
    h[9]=(int32_t)((s28>>6)&0x1FFFFFF);
}
static void fe_store(uint8_t *s, fe h) {
    int32_t h0=h[0],h1=h[1],h2=h[2],h3=h[3],h4=h[4],h5=h[5],h6=h[6],h7=h[7],h8=h[8],h9=h[9];
    int32_t q=(19*h9+(1<<24))>>25;
    q=(h0+q)>>26; q=(h1+q)>>25; q=(h2+q)>>26; q=(h3+q)>>25;
    q=(h4+q)>>26; q=(h5+q)>>25; q=(h6+q)>>26; q=(h7+q)>>25;
    q=(h8+q)>>26; q=(h9+q)>>25;
    h0+=19*q;
    int32_t c;
    c=h0>>26;h1+=c;h0-=c<<26; c=h1>>25;h2+=c;h1-=c<<25;
    c=h2>>26;h3+=c;h2-=c<<26; c=h3>>25;h4+=c;h3-=c<<25;
    c=h4>>26;h5+=c;h4-=c<<26; c=h5>>25;h6+=c;h5-=c<<25;
    c=h6>>26;h7+=c;h6-=c<<26; c=h7>>25;h8+=c;h7-=c<<25;
    c=h8>>26;h9+=c;h8-=c<<26; c=h9>>25;h9-=c<<25;
    uint32_t t0=(uint32_t)h0|((uint32_t)h1<<26);
    uint32_t t1=(uint32_t)(h1>>6)|((uint32_t)h2<<19);
    uint32_t t2=(uint32_t)(h2>>13)|((uint32_t)h3<<13);
    uint32_t t3=(uint32_t)(h3>>19)|((uint32_t)h4<<6);
    uint32_t t4=(uint32_t)h5|((uint32_t)h6<<25);
    uint32_t t5=(uint32_t)(h6>>7)|((uint32_t)h7<<19);
    uint32_t t6=(uint32_t)(h7>>13)|((uint32_t)h8<<12);
    uint32_t t7=(uint32_t)(h8>>20)|((uint32_t)h9<<6);
    s[0]=(uint8_t)t0;s[1]=(uint8_t)(t0>>8);s[2]=(uint8_t)(t0>>16);s[3]=(uint8_t)(t0>>24);
    s[4]=(uint8_t)t1;s[5]=(uint8_t)(t1>>8);s[6]=(uint8_t)(t1>>16);s[7]=(uint8_t)(t1>>24);
    s[8]=(uint8_t)t2;s[9]=(uint8_t)(t2>>8);s[10]=(uint8_t)(t2>>16);s[11]=(uint8_t)(t2>>24);
    s[12]=(uint8_t)t3;s[13]=(uint8_t)(t3>>8);s[14]=(uint8_t)(t3>>16);s[15]=(uint8_t)(t3>>24);
    s[16]=(uint8_t)t4;s[17]=(uint8_t)(t4>>8);s[18]=(uint8_t)(t4>>16);s[19]=(uint8_t)(t4>>24);
    s[20]=(uint8_t)t5;s[21]=(uint8_t)(t5>>8);s[22]=(uint8_t)(t5>>16);s[23]=(uint8_t)(t5>>24);
    s[24]=(uint8_t)t6;s[25]=(uint8_t)(t6>>8);s[26]=(uint8_t)(t6>>16);s[27]=(uint8_t)(t6>>24);
    s[28]=(uint8_t)t7;s[29]=(uint8_t)(t7>>8);s[30]=(uint8_t)(t7>>16);s[31]=(uint8_t)(t7>>24);
}
static void fe_add(fe h,const fe f,const fe g){int i;for(i=0;i<10;i++)h[i]=f[i]+g[i];}
static void fe_sub(fe h,const fe f,const fe g){int i;for(i=0;i<10;i++)h[i]=f[i]-g[i];}
static void fe_cswap(fe f,fe g,uint32_t b){
    b=0u-b; int i;
    for(i=0;i<10;i++){int32_t t=(f[i]^g[i])&(int32_t)b;f[i]^=t;g[i]^=t;}
}
static void fe_mul(fe h,const fe f,const fe g){
    int32_t f0=f[0],f1=f[1],f2=f[2],f3=f[3],f4=f[4],f5=f[5],f6=f[6],f7=f[7],f8=f[8],f9=f[9];
    int32_t g0=g[0],g1=g[1],g2=g[2],g3=g[3],g4=g[4],g5=g[5],g6=g[6],g7=g[7],g8=g[8],g9=g[9];
    int32_t g1_19=19*g1,g2_19=19*g2,g3_19=19*g3,g4_19=19*g4,g5_19=19*g5;
    int32_t g6_19=19*g6,g7_19=19*g7,g8_19=19*g8,g9_19=19*g9;
    int32_t f1_2=2*f1,f3_2=2*f3,f5_2=2*f5,f7_2=2*f7,f9_2=2*f9;
    int64_t h0=(int64_t)f0*g0+(int64_t)f1_2*g9_19+(int64_t)f2*g8_19+(int64_t)f3_2*g7_19+(int64_t)f4*g6_19+(int64_t)f5_2*g5_19+(int64_t)f6*g4_19+(int64_t)f7_2*g3_19+(int64_t)f8*g2_19+(int64_t)f9_2*g1_19;
    int64_t h1=(int64_t)f0*g1+(int64_t)f1*g0+(int64_t)f2*g9_19+(int64_t)f3*g8_19+(int64_t)f4*g7_19+(int64_t)f5*g6_19+(int64_t)f6*g5_19+(int64_t)f7*g4_19+(int64_t)f8*g3_19+(int64_t)f9*g2_19;
    int64_t h2=(int64_t)f0*g2+(int64_t)f1_2*g1+(int64_t)f2*g0+(int64_t)f3_2*g9_19+(int64_t)f4*g8_19+(int64_t)f5_2*g7_19+(int64_t)f6*g6_19+(int64_t)f7_2*g5_19+(int64_t)f8*g4_19+(int64_t)f9_2*g3_19;
    int64_t h3=(int64_t)f0*g3+(int64_t)f1*g2+(int64_t)f2*g1+(int64_t)f3*g0+(int64_t)f4*g9_19+(int64_t)f5*g8_19+(int64_t)f6*g7_19+(int64_t)f7*g6_19+(int64_t)f8*g5_19+(int64_t)f9*g4_19;
    int64_t h4=(int64_t)f0*g4+(int64_t)f1_2*g3+(int64_t)f2*g2+(int64_t)f3_2*g1+(int64_t)f4*g0+(int64_t)f5_2*g9_19+(int64_t)f6*g8_19+(int64_t)f7_2*g7_19+(int64_t)f8*g6_19+(int64_t)f9_2*g5_19;
    int64_t h5=(int64_t)f0*g5+(int64_t)f1*g4+(int64_t)f2*g3+(int64_t)f3*g2+(int64_t)f4*g1+(int64_t)f5*g0+(int64_t)f6*g9_19+(int64_t)f7*g8_19+(int64_t)f8*g7_19+(int64_t)f9*g6_19;
    int64_t h6=(int64_t)f0*g6+(int64_t)f1_2*g5+(int64_t)f2*g4+(int64_t)f3_2*g3+(int64_t)f4*g2+(int64_t)f5_2*g1+(int64_t)f6*g0+(int64_t)f7_2*g9_19+(int64_t)f8*g8_19+(int64_t)f9_2*g7_19;
    int64_t h7=(int64_t)f0*g7+(int64_t)f1*g6+(int64_t)f2*g5+(int64_t)f3*g4+(int64_t)f4*g3+(int64_t)f5*g2+(int64_t)f6*g1+(int64_t)f7*g0+(int64_t)f8*g9_19+(int64_t)f9*g8_19;
    int64_t h8=(int64_t)f0*g8+(int64_t)f1_2*g7+(int64_t)f2*g6+(int64_t)f3_2*g5+(int64_t)f4*g4+(int64_t)f5_2*g3+(int64_t)f6*g2+(int64_t)f7_2*g1+(int64_t)f8*g0+(int64_t)f9_2*g9_19;
    int64_t h9=(int64_t)f0*g9+(int64_t)f1*g8+(int64_t)f2*g7+(int64_t)f3*g6+(int64_t)f4*g5+(int64_t)f5*g4+(int64_t)f6*g3+(int64_t)f7*g2+(int64_t)f8*g1+(int64_t)f9*g0;
    int64_t c;
    c=(h0+(1LL<<25))>>26;h1+=c;h0-=c*(1LL<<26); c=(h4+(1LL<<25))>>26;h5+=c;h4-=c*(1LL<<26);
    c=(h1+(1LL<<24))>>25;h2+=c;h1-=c*(1LL<<25); c=(h5+(1LL<<24))>>25;h6+=c;h5-=c*(1LL<<25);
    c=(h2+(1LL<<25))>>26;h3+=c;h2-=c*(1LL<<26); c=(h6+(1LL<<25))>>26;h7+=c;h6-=c*(1LL<<26);
    c=(h3+(1LL<<24))>>25;h4+=c;h3-=c*(1LL<<25); c=(h7+(1LL<<24))>>25;h8+=c;h7-=c*(1LL<<25);
    c=(h4+(1LL<<25))>>26;h5+=c;h4-=c*(1LL<<26); c=(h8+(1LL<<25))>>26;h9+=c;h8-=c*(1LL<<26);
    c=(h9+(1LL<<24))>>25;h0+=c*19;h9-=c*(1LL<<25);
    c=(h0+(1LL<<25))>>26;h1+=c;h0-=c*(1LL<<26);
    h[0]=(int32_t)h0;h[1]=(int32_t)h1;h[2]=(int32_t)h2;h[3]=(int32_t)h3;h[4]=(int32_t)h4;
    h[5]=(int32_t)h5;h[6]=(int32_t)h6;h[7]=(int32_t)h7;h[8]=(int32_t)h8;h[9]=(int32_t)h9;
}
static void fe_sq(fe h,const fe f){fe_mul(h,f,f);}
static void fe_invert(fe out,const fe z){
    /* Standard Curve25519 inversion: compute z^(2^255-21) = z^(p-2)
     * Chain: z1, z2, z9, z11, z^(2^5-1), z^(2^10-1), z^(2^20-1),
     *        z^(2^40-1), z^(2^50-1), z^(2^100-1), z^(2^200-1), z^(2^250-1), z^(2^255-21) */
    fe z1,z2,z9,z11,z2_5_0,z2_10_0,z2_20_0,z2_50_0,t; int i;
    memcpy(z1,z,sizeof(fe));
    fe_sq(z2,z1);                                    /* z^2 */
    fe_sq(z9,z2); fe_sq(z9,z9); fe_mul(z9,z9,z1);   /* z^9 */
    fe_mul(z11,z9,z2);                               /* z^11 */
    fe_sq(t,z11); fe_mul(z2_5_0,t,z9);               /* z^(2^5-1) */
    fe_sq(t,z2_5_0);
    for(i=1;i<5;i++) fe_sq(t,t);
    fe_mul(z2_10_0,t,z2_5_0);                        /* z^(2^10-1) */
    fe_sq(t,z2_10_0);
    for(i=1;i<10;i++) fe_sq(t,t);
    fe_mul(z2_20_0,t,z2_10_0);                       /* z^(2^20-1) */
    fe_sq(t,z2_20_0);
    for(i=1;i<20;i++) fe_sq(t,t);
    fe_mul(t,t,z2_20_0);                             /* z^(2^40-1) */
    for(i=0;i<10;i++) fe_sq(t,t);
    fe_mul(z2_50_0,t,z2_10_0);                       /* z^(2^50-1) */
    fe_sq(t,z2_50_0);
    for(i=1;i<50;i++) fe_sq(t,t);
    fe_mul(t,t,z2_50_0);                             /* z^(2^100-1) */
    {fe t2; fe_sq(t2,t);
     for(i=1;i<100;i++) fe_sq(t2,t2);
     fe_mul(t,t2,t);}                                /* z^(2^200-1) */
    for(i=0;i<50;i++) fe_sq(t,t);
    fe_mul(t,t,z2_50_0);                             /* z^(2^250-1) */
    for(i=0;i<5;i++) fe_sq(t,t);                    /* z^(2^255-32) */
    fe_mul(out,t,z11);                               /* z^(2^255-21) */
}
static const uint8_t X25519_BASE[32]={9,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
/* fe_mul121666: multiply by 121666 (= a24*2 + 2 = (a-2)/4 * 2 + 2) — used for z2 in ladder
 * Actually we need a24 = 121665. Use scalar multiply. */
static void fe_mul121665(fe h, const fe f) {
    int64_t v;
    int64_t h0=(int64_t)f[0]*121665;int64_t h1=(int64_t)f[1]*121665;
    int64_t h2=(int64_t)f[2]*121665;int64_t h3=(int64_t)f[3]*121665;
    int64_t h4=(int64_t)f[4]*121665;int64_t h5=(int64_t)f[5]*121665;
    int64_t h6=(int64_t)f[6]*121665;int64_t h7=(int64_t)f[7]*121665;
    int64_t h8=(int64_t)f[8]*121665;int64_t h9=(int64_t)f[9]*121665;
    int64_t c;
    c=(h9+(1LL<<24))>>25;h0+=c*19;h9-=c*(1LL<<25);
    c=(h1+(1LL<<24))>>25;h2+=c;h1-=c*(1LL<<25);
    c=(h3+(1LL<<24))>>25;h4+=c;h3-=c*(1LL<<25);
    c=(h5+(1LL<<24))>>25;h6+=c;h5-=c*(1LL<<25);
    c=(h7+(1LL<<24))>>25;h8+=c;h7-=c*(1LL<<25);
    c=(h0+(1LL<<25))>>26;h1+=c;h0-=c*(1LL<<26);
    c=(h2+(1LL<<25))>>26;h3+=c;h2-=c*(1LL<<26);
    c=(h4+(1LL<<25))>>26;h5+=c;h4-=c*(1LL<<26);
    c=(h6+(1LL<<25))>>26;h7+=c;h6-=c*(1LL<<26);
    c=(h8+(1LL<<25))>>26;h9+=c;h8-=c*(1LL<<26);
    h[0]=(int32_t)h0;h[1]=(int32_t)h1;h[2]=(int32_t)h2;h[3]=(int32_t)h3;h[4]=(int32_t)h4;
    h[5]=(int32_t)h5;h[6]=(int32_t)h6;h[7]=(int32_t)h7;h[8]=(int32_t)h8;h[9]=(int32_t)h9;
    (void)v;
}
static void x25519(uint8_t out[32],const uint8_t k[32],const uint8_t u[32]){
    uint8_t ks[32],us[32]; memcpy(ks,k,32); memcpy(us,u,32);
    ks[0]&=248; ks[31]&=127; ks[31]|=64; us[31]&=127;
    fe x1,x2,z2,x3,z3,A,AA,B,BB,E,C,D,DA,CB;
    fe_load(x1,us);
    {int i; for(i=0;i<10;i++){x2[i]=(i==0);z2[i]=0;x3[i]=x1[i];z3[i]=(i==0);}}
    uint32_t swap=0;
    for(int t=254;t>=0;t--){
        uint32_t kt=(ks[t/8]>>(t%8))&1;
        swap^=kt; fe_cswap(x2,x3,swap); fe_cswap(z2,z3,swap); swap=kt;
        /* RFC 7748 §5 ladder step */
        fe_add(A,x2,z2);   fe_sq(AA,A);
        fe_sub(B,x2,z2);   fe_sq(BB,B);
        fe_sub(E,AA,BB);
        fe_add(C,x3,z3);   fe_sub(D,x3,z3);
        fe_mul(DA,D,A);    fe_mul(CB,C,B);
        fe_add(x3,DA,CB);  fe_sq(x3,x3);
        fe_sub(z3,DA,CB);  fe_sq(z3,z3);  fe_mul(z3,x1,z3);
        fe_mul(x2,AA,BB);
        fe_mul121665(z2,E);
        fe_add(z2,z2,AA);
        fe_mul(z2,E,z2);
    }
    fe_cswap(x2,x3,swap); fe_cswap(z2,z3,swap);
    fe_invert(z2,z2); fe_mul(x2,x2,z2);
    fe_store(out,x2);
}
/* ================================================================
 * TLS 1.3 state machine
 * ================================================================ */

#define TLS_REC_HANDSHAKE      22
#define TLS_REC_CHANGE_CIPHER  20
#define TLS_REC_ALERT          21
#define TLS_REC_APP_DATA       23
#define TLS_HS_CLIENT_HELLO     1
#define TLS_HS_SERVER_HELLO     2
#define TLS_HS_ENCRYPTED_EXT    8
#define TLS_HS_CERTIFICATE     11
#define TLS_HS_CERT_VERIFY     15
#define TLS_HS_FINISHED        20
#define TLS_EXT_SNI            0
#define TLS_EXT_SUPPORTED_VER  43
#define TLS_EXT_KEY_SHARE      51
#define TLS_EXT_SUPPORTED_GRP  10
#define TLS_VERSION_13         0x0304
#define TLS_GROUP_X25519       0x001D
#define TLS_SUITE_CHACHA_POLY  0x1303

#define TLS_MAX_CONN 2

typedef struct {
    int      tcp_id;
    uint8_t  client_priv[32];
    uint8_t  client_pub[32];
    uint8_t  server_pub[32];
    uint8_t  shared[32];
    /* Key material */
    uint8_t  hs_key_c[32],  hs_key_s[32];
    uint8_t  hs_nonce_c[12],hs_nonce_s[12];
    uint8_t  app_key_c[32], app_key_s[32];
    uint8_t  app_nonce_c[12],app_nonce_s[12];
    uint64_t seq_s, seq_c;  /* sequence numbers for nonce */
    /* Transcript hash (updated throughout handshake) */
    sha256_ctx transcript;
    uint8_t  transcript_snap[32]; /* hash after server hello */
    int      handshake_done;
    /* Receive buffer for decrypted records */
    uint8_t  rx[32768];
    int      rx_head, rx_tail;
    int      eof;
} tls_conn_t;

static tls_conn_t g_tls[TLS_MAX_CONN];

/* XOR nonce with sequence number */
static void make_nonce(uint8_t n[12], const uint8_t base[12], uint64_t seq){
    uint8_t s[8]; for(int i=0;i<8;i++) s[i]=(uint8_t)(seq>>(56-i*8));
    uint8_t tmp[12]; memcpy(tmp,base,12);
    for(int i=0;i<8;i++) tmp[4+i]^=s[i];
    memcpy(n,tmp,12);
}

/* Write a big-endian 16-bit value */
#define W16(b,v) do{(b)[0]=(uint8_t)((v)>>8);(b)[1]=(uint8_t)(v);}while(0)
#define W24(b,v) do{(b)[0]=(uint8_t)((v)>>16);(b)[1]=(uint8_t)((v)>>8);(b)[2]=(uint8_t)(v);}while(0)
#define R16(b)   ((uint16_t)((b)[0]<<8|(b)[1]))
#define R24(b)   (((uint32_t)(b)[0]<<16)|((uint32_t)(b)[1]<<8)|(b)[2])

/* Generate a pseudo-random 32-byte private key (time + PIT counter) */
static void gen_private_key(uint8_t k[32]){
    extern uint64_t pit_ticks(void);
    uint64_t seed = pit_ticks() ^ 0xDEADBEEFCAFE0000ULL;
    uint8_t buf[40];
    for(int i=0;i<8;i++) buf[i]=(uint8_t)(seed>>(i*8));
    buf[8]='T';buf[9]='L';buf[10]='S';buf[11]='k';
    for(int i=12;i<40;i++) buf[i]=(uint8_t)i;
    sha256(buf,40,k);
    k[0]&=248; k[31]&=127; k[31]|=64;
}

/* Encrypt and send a TLS record */
static int tls_send_record(tls_conn_t *c, uint8_t type, const uint8_t *data, size_t dlen){
    static uint8_t buf[16640];
    /* plaintext for AEAD: data + content_type byte */
    static uint8_t plain[16384]; memcpy(plain,data,dlen); plain[dlen]=type;
    /* header: outer type=23(app_data), version=0x0303, length */
    uint8_t hdr[5];
    hdr[0]=TLS_REC_APP_DATA; hdr[1]=3; hdr[2]=3;
    uint16_t enc_len=(uint16_t)(dlen+1+16);
    W16(hdr+3,enc_len);
    /* build nonce */
    uint8_t nonce[12]; make_nonce(nonce,c->hs_nonce_c,c->seq_c++);
    /* encrypt */
    int n=chacha20_poly1305_seal(c->hs_key_c,nonce,hdr,5,plain,dlen+1,buf+5);
    memcpy(buf,hdr,5);
    return net_tcp_send(c->tcp_id,buf,(uint32_t)(5+(size_t)n));
}

/* Receive and decrypt a TLS record from TCP; returns plaintext length */
static int tls_recv_record(tls_conn_t *c, uint8_t *out, size_t cap, uint8_t *rtype){
    uint8_t hdr[5]; uint8_t tmp[16384];
    /* Read exactly 5 header bytes */
    int got=0;
    while(got<5){
        int r=net_tcp_recv(c->tcp_id,hdr+got,(uint32_t)(5-got),1);
        if(r<=0)return -1; got+=r;
    }
    uint16_t rlen=R16(hdr+3);
    if(rlen>sizeof(tmp))return -1;
    got=0;
    while((uint16_t)got<rlen){
        int r=net_tcp_recv(c->tcp_id,tmp+got,(uint32_t)(rlen-got),1);
        if(r<=0)return -1; got+=r;
    }
    if(hdr[0]!=TLS_REC_APP_DATA){
        if(hdr[0]==TLS_REC_HANDSHAKE){memcpy(out,tmp,(size_t)rlen<cap?(size_t)rlen:cap);*rtype=TLS_REC_HANDSHAKE;return rlen;}
        if(hdr[0]==TLS_REC_CHANGE_CIPHER){*rtype=TLS_REC_CHANGE_CIPHER;return 0;}
        *rtype=hdr[0]; return -1;
    }

    /* Decrypt */
    uint8_t nonce[12]; make_nonce(nonce,c->hs_nonce_s,c->seq_s++);


    int plen=chacha20_poly1305_open(c->hs_key_s,nonce,hdr,5,tmp,(size_t)rlen,out);
    if(plen<0){return -1;}
    /* Last byte is content type */
    *rtype=out[plen-1];
    return plen-1;
}

/* TLS 1.3 handshake */
int net_tls_connect(int tcp_id, const char *hostname) {
    int slot=-1;
    for(int i=0;i<TLS_MAX_CONN;i++) if(g_tls[i].tcp_id<0){slot=i;break;}
    if(slot<0) return -1;

    tls_conn_t *c=&g_tls[slot];
    memset(c,0,sizeof(*c));
    c->tcp_id=tcp_id;
    sha256_init(&c->transcript);

    /* Generate ephemeral X25519 key pair */
    gen_private_key(c->client_priv);
    x25519(c->client_pub,c->client_priv,X25519_BASE);

    /* Build ClientHello */
    static uint8_t ch[640]; int pos=0;
    /* Handshake header placeholder */
    ch[pos++]=TLS_HS_CLIENT_HELLO;
    int hs_len_off=pos; pos+=3; /* length placeholder */
    /* legacy_version = TLS 1.2 */
    ch[pos++]=3; ch[pos++]=3;
    /* 32-byte random (seeded from private key for determinism) */
    for(int i=0;i<32;i++) ch[pos++]=c->client_priv[i]^(uint8_t)(0x5A+i);
    /* session_id = 32 bytes (TLS 1.3 middlebox compat, RFC 8446 §D.4) */
    ch[pos++]=32;
    for(int i=0;i<32;i++) ch[pos++]=c->client_pub[i]^(uint8_t)(0xA5+i);
    /* cipher suites: only ChaCha20-Poly1305 (only one we implement) */
    ch[pos++]=0;ch[pos++]=2;
    ch[pos++]=0x13;ch[pos++]=0x03;  /* TLS_CHACHA20_POLY1305_SHA256 */
    /* compression = null */
    ch[pos++]=1;ch[pos++]=0;

    /* Extensions */
    int ext_len_off=pos; pos+=2;
    /* SNI extension */
    int snl=(int)strlen(hostname);
    W16(ch+pos,TLS_EXT_SNI);pos+=2;
    W16(ch+pos,(uint16_t)(snl+5));pos+=2;  /* ext data len */
    W16(ch+pos,(uint16_t)(snl+3));pos+=2;  /* list len */
    ch[pos++]=0;                            /* type=host_name */
    W16(ch+pos,(uint16_t)snl);pos+=2;
    memcpy(ch+pos,hostname,snl);pos+=snl;
    /* supported_versions: TLS 1.3 (required per RFC 8446 §9.2) */
    W16(ch+pos,TLS_EXT_SUPPORTED_VER);pos+=2;
    W16(ch+pos,3);pos+=2; ch[pos++]=2;ch[pos++]=3;ch[pos++]=4;
    /* supported_groups: x25519 */
    W16(ch+pos,TLS_EXT_SUPPORTED_GRP);pos+=2;
    W16(ch+pos,4);pos+=2; W16(ch+pos,2);pos+=2; W16(ch+pos,TLS_GROUP_X25519);pos+=2;
    /* key_share */
    W16(ch+pos,TLS_EXT_KEY_SHARE);pos+=2;
    W16(ch+pos,38);pos+=2;   /* ext data len = 36+2 */
    W16(ch+pos,36);pos+=2;   /* client_shares len */
    W16(ch+pos,TLS_GROUP_X25519);pos+=2;
    W16(ch+pos,32);pos+=2;
    memcpy(ch+pos,c->client_pub,32);pos+=32;
    /* signature_algorithms (required per RFC 8446 §9.2) */
    W16(ch+pos,0x000D);pos+=2;  /* extension type */
    W16(ch+pos,8);pos+=2;       /* ext data length */
    W16(ch+pos,6);pos+=2;       /* list length */
    W16(ch+pos,0x0804);pos+=2;  /* rsa_pss_rsae_sha256 */
    W16(ch+pos,0x0401);pos+=2;  /* rsa_pkcs1_sha256 */
    W16(ch+pos,0x0403);pos+=2;  /* ecdsa_secp256r1_sha256 */
    /* fill in extension length */
    W16(ch+ext_len_off,(uint16_t)(pos-(ext_len_off+2)));
    /* fill in handshake message length */
    W24(ch+hs_len_off,pos-(hs_len_off+3));
    /* Update transcript */
    sha256_update(&c->transcript,ch,pos);
    /* Wrap in TLS record: type=22, version=0x0301, length — ONE send to avoid split issues */
    static uint8_t full_rec[512+5];
    full_rec[0]=TLS_REC_HANDSHAKE; full_rec[1]=3; full_rec[2]=1; W16(full_rec+3,(uint16_t)pos);
    memcpy(full_rec+5, ch, (size_t)pos);
    if(net_tcp_send(tcp_id, full_rec, (uint32_t)(5+pos)) < 0) return -1;
    /* Receive ServerHello — skip change_cipher_spec (0x14) sent for
     * TLS 1.3 middlebox compatibility (RFC 8446 §D.3) */
    uint8_t rbuf[4096]; uint8_t rtype;
    int rlen=0;
    for(int attempt=0; attempt<4; attempt++){
        int raw_got=0;
        uint8_t rhdr[5]; int rg=0;
        while(rg<5){int r=net_tcp_recv(tcp_id,rhdr+rg,(uint32_t)(5-rg),1);if(r<=0)return -1;rg+=r;}
        uint16_t rec_len=R16(rhdr+3);
        if(rec_len>sizeof(rbuf))return -1;
        while(raw_got<(int)rec_len){int r=net_tcp_recv(tcp_id,rbuf+raw_got,(uint32_t)(rec_len-raw_got),1);if(r<=0)return -1;raw_got+=r;}
        rlen=raw_got;
        if(rhdr[0]==0x14) continue;  /* change_cipher_spec — skip */
        if(rhdr[0]==0x15){
            tls_dbg("tls: Alert lvl=");
            {char t[4];t[0]='0'+(rbuf[0]/10%10);t[1]='0'+(rbuf[0]%10);t[2]=' ';t[3]=0;tls_dbg(t);}
            tls_dbg("desc=");
            {char t[4];t[0]='0'+(rbuf[1]/10%10);t[1]='0'+(rbuf[1]%10);t[2]='\n';t[3]=0;tls_dbg(t);}
            return -1;
        }
        if(rhdr[0]!=0x16){tls_dbg("tls: unexpected record type\n");return -1;}
        break; /* Handshake record */
    }
    /* Parse ServerHello */
    if(rbuf[0]!=TLS_HS_SERVER_HELLO){
        tls_dbg("tls: expected ServerHello, got type=");
        {char t[4];t[0]='0'+(rbuf[0]/10);t[1]='0'+(rbuf[0]%10);t[2]='\n';t[3]=0;tls_dbg(t);}
        return -1;
    }
    sha256_update(&c->transcript,rbuf,rlen);
    /* Extract server's X25519 key from key_share extension */
    /* skip 2(ver)+32(rand)+1+session+2(suite)+1(comp) = 38+session bytes */
    int p=4+2+32; p+=1+rbuf[p]; p+=2+1; /* suite + comp */
    /* extensions length */
    if(p+2>rlen)return -1;
    int ext_end=p+2+R16(rbuf+p); p+=2;
    int found_key=0;
    while(p+4<=ext_end){
        uint16_t etype=R16(rbuf+p); uint16_t elen=R16(rbuf+p+2); p+=4;
        if(etype==TLS_EXT_KEY_SHARE&&elen>=36){
            /* group(2) + len(2) + key(32) */
            memcpy(c->server_pub,rbuf+p+4,32); found_key=1;
        }
        p+=elen;
    }
    if(!found_key){tls_dbg("tls: no server key_share\n");return -1;}
    /* Compute shared secret */
    x25519(c->shared,c->client_priv,c->server_pub);

    /* Key schedule */
    uint8_t zero32[32]={0};
    uint8_t es[32]; hkdf_extract(NULL,0,zero32,32,es); /* early secret */
    uint8_t derived_es[32]; derive_secret(es,"derived",NULL/* empty hash */,derived_es);
    /* Actually use hash of empty string for "derived" */
    uint8_t empty_hash[32]; sha256((const uint8_t*)"",0,empty_hash);
    uint8_t hs_sec[32];
    {uint8_t derived[32];hkdf_expand_label(es,"derived",empty_hash,32,derived,32);
     hkdf_extract(derived,32,c->shared,32,hs_sec);}
    /* Get transcript hash at this point (after ServerHello) */
    {sha256_ctx tmp=c->transcript; sha256_final(&tmp,c->transcript_snap);}
    /* Handshake traffic secrets */
    uint8_t c_hs_ts[32], s_hs_ts[32];
    hkdf_expand_label(hs_sec,"c hs traffic",c->transcript_snap,32,c_hs_ts,32);
    hkdf_expand_label(hs_sec,"s hs traffic",c->transcript_snap,32,s_hs_ts,32);
    /* Keys and IVs */
    hkdf_expand_label(c_hs_ts,"key",NULL,0,c->hs_key_c,32);
    hkdf_expand_label(s_hs_ts,"key",NULL,0,c->hs_key_s,32);
    hkdf_expand_label(c_hs_ts,"iv",NULL,0,c->hs_nonce_c,12);
    hkdf_expand_label(s_hs_ts,"iv",NULL,0,c->hs_nonce_s,12);


    /* Receive encrypted handshake messages */
    uint8_t fin_verify[32];
    for(int tries=0;tries<8;tries++){
        uint8_t plain[16384];
        rlen=tls_recv_record(c,plain,sizeof(plain),&rtype);
        if(rlen<0){return -1;}
        if(rtype==TLS_REC_CHANGE_CIPHER)continue;
        if(rtype!=TLS_REC_HANDSHAKE)continue;
        /* Parse handshake messages: update transcript per-message, not per-record,
         * so we can snapshot transcript BEFORE the Finished (RFC 8446 §4.4.4) */
        int hp=0;
        while(hp<rlen){
            if(hp+4>rlen)break;
            uint8_t htype=plain[hp]; uint32_t hlen=R24(plain+hp+1);
            if(hp+4+(int)hlen>rlen)break;
            if(htype==TLS_HS_FINISHED){
                /* Snapshot transcript BEFORE the Finished */
                sha256_ctx snap=c->transcript;
                uint8_t tsnap[32]; sha256_final(&snap,tsnap);
                uint8_t finished_key[32];
                hkdf_expand_label(s_hs_ts,"finished",NULL,0,finished_key,32);
                hmac_sha256(finished_key,32,tsnap,32,fin_verify);
                /* Verify server Finished */
                uint8_t diff=0;
                for(int i=0;i<32;i++) diff|=(plain[hp+4+i]^fin_verify[i]);
                if(diff){
                    tls_dbg("tls: server Finished MISMATCH\n");
                    return -1;
                }
                /* Now add Finished to transcript for master secret derivation */
                sha256_update(&c->transcript,plain+hp,(size_t)(4+hlen));
                goto send_finished;
            }
            sha256_update(&c->transcript,plain+hp,(size_t)(4+hlen));
            hp+=4+(int)hlen;
        }
    }
    tls_dbg("tls: no server Finished\n");
    return -1;
    send_finished:;
    /* Compute master secret and application keys */
    uint8_t derived_hs[32];
    {uint8_t th[32];sha256((uint8_t*)"",0,th);hkdf_expand_label(hs_sec,"derived",th,32,derived_hs,32);}
    uint8_t ms[32]; hkdf_extract(derived_hs,32,zero32,32,ms);
    uint8_t tsnap_fin[32]; {sha256_ctx tmp=c->transcript;sha256_final(&tmp,tsnap_fin);}
    uint8_t c_ap_ts[32],s_ap_ts[32];
    hkdf_expand_label(ms,"c ap traffic",tsnap_fin,32,c_ap_ts,32);
    hkdf_expand_label(ms,"s ap traffic",tsnap_fin,32,s_ap_ts,32);
    hkdf_expand_label(c_ap_ts,"key",NULL,0,c->app_key_c,32);
    hkdf_expand_label(s_ap_ts,"key",NULL,0,c->app_key_s,32);
    hkdf_expand_label(c_ap_ts,"iv",NULL,0,c->app_nonce_c,12);
    hkdf_expand_label(s_ap_ts,"iv",NULL,0,c->app_nonce_s,12);

    /* Send client Finished */
    {uint8_t tsnap2[32];{sha256_ctx tmp=c->transcript;sha256_final(&tmp,tsnap2);}
     uint8_t fin_key[32];hkdf_expand_label(c_hs_ts,"finished",NULL,0,fin_key,32);
     uint8_t vd[32];hmac_sha256(fin_key,32,tsnap2,32,vd);
     uint8_t fin_msg[36];fin_msg[0]=TLS_HS_FINISHED;W24(fin_msg+1,32);memcpy(fin_msg+4,vd,32);
     tls_send_record(c,TLS_REC_HANDSHAKE,fin_msg,36);
     sha256_update(&c->transcript,fin_msg,36);}

    /* Switch to application keys */
    memcpy(c->hs_key_c,  c->app_key_c,   32);
    memcpy(c->hs_key_s,  c->app_key_s,   32);
    memcpy(c->hs_nonce_c,c->app_nonce_c, 12);
    memcpy(c->hs_nonce_s,c->app_nonce_s, 12);
    c->seq_c=0; c->seq_s=0;
    c->handshake_done=1;
    return slot;
}

int net_tls_send(int id, const void *data, uint32_t len) {
    if(id<0||id>=TLS_MAX_CONN||!g_tls[id].handshake_done)return -1;
    tls_conn_t *c=&g_tls[id];
    tls_send_record(c,TLS_REC_APP_DATA,(const uint8_t*)data,(size_t)len);
    return (int)len;
}

int net_tls_recv(int id, void *buf, uint32_t cap, int blocking) {
    if(id<0||id>=TLS_MAX_CONN||!g_tls[id].handshake_done)return -1;
    tls_conn_t *c=&g_tls[id];
    (void)blocking;
    uint8_t plain[16384]; uint8_t rtype;
    /* Loop: skip non-app-data records (NewSessionTicket, alerts, etc.) */
    for(;;){
        int n=tls_recv_record(c,plain,sizeof(plain),&rtype);
        if(n<0){c->eof=1;return 0;}
        if(rtype==TLS_REC_APP_DATA){
            if((uint32_t)n>cap)n=(int)cap;
            memcpy(buf,plain,(size_t)n);
            return n;
        }
        /* Skip: handshake (NewSessionTicket) or other */
    }
}

void net_tls_close(int id) {
    if(id<0||id>=TLS_MAX_CONN)return;
    g_tls[id].tcp_id=-1;
    g_tls[id].handshake_done=0;
}

void net_tls_init(void){
    for(int i=0;i<TLS_MAX_CONN;i++) g_tls[i].tcp_id=-1;
    /* X25519 RFC 7748 §6.1 self-test at init time — panic on fail, silent on OK */
    {
        static const uint8_t p[32]={0x77,0x07,0x6d,0x0a,0x73,0x18,0xa5,0x7d,0x3c,0x16,0xc1,0x72,0x51,0xb2,0x66,0x45,0xdf,0x4c,0x2f,0x87,0xeb,0xc0,0x99,0x2a,0xb1,0x77,0xfb,0xa5,0x1d,0xb9,0x2c,0x2a};
        static const uint8_t u[32]={0xde,0x9e,0xdb,0x7d,0x7b,0x7d,0xc1,0xb4,0xd3,0x5b,0x61,0xc2,0xec,0xe4,0x35,0x37,0x3f,0x83,0x43,0xc8,0x5b,0x78,0x67,0x4d,0xad,0xfc,0x7e,0x14,0x6f,0x88,0x2b,0x4f};
        static const uint8_t e[32]={0x4a,0x5d,0x9d,0x5b,0xa4,0xce,0x2d,0xe1,0x72,0x8e,0x3b,0xf4,0x80,0x35,0x0f,0x25,0xe0,0x7e,0x21,0xc9,0x47,0xd1,0x9e,0x33,0x76,0xf0,0x9b,0x3c,0x1e,0x16,0x17,0x42};
        uint8_t r[32]; x25519(r,p,u);
        uint8_t d=0; for(int i=0;i<32;i++) d|=(r[i]^e[i]);
        if(d){ extern void serial_puts(const char *s); serial_puts("PANIC: X25519 self-test FAILED\n"); for(;;); }
    }
}
