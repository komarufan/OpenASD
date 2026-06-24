#!/usr/bin/env python3
"""Replace Poly1305 body with correct donna-64 implementation."""

TLS_PATH = "C:/Users/Администратор/Downloads/OpenASD-main/kernel/net/tls.c"

OLD_START = "    /* donna-64: load r as 3x44-bit limbs with inline clamp masks */"
OLD_END   = "}\n\n/* ChaCha20-Poly1305 AEAD */"

# If old_start not found, try the alternate marker
ALT_START = "    /* donna-64: 44/44/42-bit 3-limb Poly1305 (Andrew Moon, public domain) */"

NEW_BODY = """    /* poly1305-donna-64: 44/44/42-bit 3-limb scheme (Andrew Moon, public domain)
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

"""

with open(TLS_PATH, encoding='utf-8') as f:
    src = f.read()

# Try to find the function body to replace
i_start = src.find(OLD_START)
if i_start < 0:
    i_start = src.find(ALT_START)
    if i_start < 0:
        # Find the static void poly1305_mac line and the body start
        import re
        m = re.search(r'static void poly1305_mac\([^)]+\)\{', src)
        if m:
            i_start = m.end()
        else:
            raise RuntimeError("Cannot find poly1305_mac body start")

i_end = src.find(OLD_END, i_start)
if i_end < 0:
    raise RuntimeError(f"Cannot find end marker OLD_END after offset {i_start}")

new_src = src[:i_start] + NEW_BODY + "/* ChaCha20-Poly1305 AEAD */" + src[i_end+len(OLD_END):]
with open(TLS_PATH, 'w', encoding='utf-8') as f:
    f.write(new_src)
print(f"OK: replaced {i_end-i_start} chars at offset {i_start}")
