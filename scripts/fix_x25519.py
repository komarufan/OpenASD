#!/usr/bin/env python3
"""Replace X25519 (Curve25519) implementation with ref10 version."""
TLS = "C:/Users/Администратор/Downloads/OpenASD-main/kernel/net/tls.c"

NEW_X25519 = """/* X25519 (Curve25519) — ref10 representation (D.J. Bernstein, public domain)
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
    h[4]=(int32_t)(s12>>6&0x3FFFFFF);
    h[5]=(int32_t)(s16&0x1FFFFFF);
    h[6]=(int32_t)((s16>>25|(s20<<7))&0x3FFFFFF);
    h[7]=(int32_t)((s20>>18|(s24<<14))&0x1FFFFFF);
    h[8]=(int32_t)((s24>>11|(s28<<21))&0x3FFFFFF);
    h[9]=(int32_t)(s28>>4&0x1FFFFFF);
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
    uint32_t t5=(uint32_t)(h6>>7)|((uint32_t)h7<<18);
    uint32_t t6=(uint32_t)(h7>>14)|((uint32_t)h8<<11);
    uint32_t t7=(uint32_t)(h8>>21)|((uint32_t)h9<<4);
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
    fe t0,t1,t2,t3; int i;
    (void)t3;
    fe_sq(t0,z);fe_sq(t1,t0);fe_sq(t1,t1);fe_mul(t1,z,t1);fe_mul(t0,t0,t1);
    fe_sq(t0,t0);fe_mul(t0,t1,t0);
    fe_sq(t1,t0);for(i=1;i<5;i++)fe_sq(t1,t1);fe_mul(t0,t1,t0);
    fe_sq(t1,t0);for(i=1;i<10;i++)fe_sq(t1,t1);fe_mul(t1,t1,t0);
    fe_sq(t2,t1);for(i=1;i<20;i++)fe_sq(t2,t2);fe_mul(t1,t2,t1);
    fe_sq(t1,t1);for(i=1;i<10;i++)fe_sq(t1,t1);fe_mul(t0,t1,t0);
    fe_sq(t1,t0);for(i=1;i<50;i++)fe_sq(t1,t1);fe_mul(t1,t1,t0);
    fe_sq(t2,t1);for(i=1;i<100;i++)fe_sq(t2,t2);fe_mul(t1,t2,t1);
    fe_sq(t1,t1);for(i=1;i<50;i++)fe_sq(t1,t1);fe_mul(t0,t1,t0);
    fe_sq(t0,t0);for(i=1;i<5;i++)fe_sq(t0,t0);fe_mul(out,t0,z);
}
static const uint8_t X25519_BASE[32]={9,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
static void x25519(uint8_t out[32],const uint8_t k[32],const uint8_t u[32]){
    uint8_t ks[32],us[32]; memcpy(ks,k,32); memcpy(us,u,32);
    ks[0]&=248; ks[31]&=127; ks[31]|=64; us[31]&=127;
    fe x1,x2,z2,x3,z3,tmp0,tmp1;
    fe_load(x1,us);
    {int i; for(i=0;i<10;i++){x2[i]=(i==0);z2[i]=0;x3[i]=x1[i];z3[i]=(i==0);}}
    uint32_t swap=0;
    for(int t=254;t>=0;t--){
        uint32_t kt=(ks[t/8]>>(t%8))&1;
        swap^=kt; fe_cswap(x2,x3,swap); fe_cswap(z2,z3,swap); swap=kt;
        fe_sub(tmp0,x3,z3); fe_sub(tmp1,x2,z2);
        fe_add(x2,x2,z2); fe_add(z2,x3,z3);
        fe_mul(z3,tmp0,x2); fe_mul(z2,z2,tmp1);
        fe_sq(tmp0,tmp1); fe_sq(tmp1,x2);
        fe_add(x3,z3,z2); fe_sub(z2,z3,z2);
        fe_mul(x2,tmp1,tmp0); fe_sub(tmp1,tmp1,tmp0);
        fe_sq(z2,z2);
        /* tmp0 = a24*E = 121665 * (AA-BB); z2 = E*(AA + a24*E) */
        {fe a24_E;
         int i; for(i=0;i<10;i++) a24_E[i]=0;
         /* multiply tmp1 by 121665: use int64 */
         int64_t v; for(i=0;i<10;i++){v=(int64_t)tmp1[i]*121665LL;a24_E[i]=(int32_t)v;}
         /* carry */
         {int64_t cc;
          cc=((int64_t)a24_E[0]+(1<<25))>>26;a24_E[1]+=cc;a24_E[0]-=(int32_t)(cc<<26);
          cc=((int64_t)a24_E[1]+(1<<24))>>25;a24_E[2]+=cc;a24_E[1]-=(int32_t)(cc<<25);
          cc=((int64_t)a24_E[2]+(1<<25))>>26;a24_E[3]+=cc;a24_E[2]-=(int32_t)(cc<<26);
          cc=((int64_t)a24_E[3]+(1<<24))>>25;a24_E[4]+=cc;a24_E[3]-=(int32_t)(cc<<25);
          cc=((int64_t)a24_E[4]+(1<<25))>>26;a24_E[5]+=cc;a24_E[4]-=(int32_t)(cc<<26);
          cc=((int64_t)a24_E[5]+(1<<24))>>25;a24_E[6]+=cc;a24_E[5]-=(int32_t)(cc<<25);
          cc=((int64_t)a24_E[6]+(1<<25))>>26;a24_E[7]+=cc;a24_E[6]-=(int32_t)(cc<<26);
          cc=((int64_t)a24_E[7]+(1<<24))>>25;a24_E[8]+=cc;a24_E[7]-=(int32_t)(cc<<25);
          cc=((int64_t)a24_E[8]+(1<<25))>>26;a24_E[9]+=cc;a24_E[8]-=(int32_t)(cc<<26);
          cc=((int64_t)a24_E[9]+(1<<24))>>25;a24_E[0]+=(int32_t)(cc*19);a24_E[9]-=(int32_t)(cc<<25);}
         fe_add(tmp0,tmp1,a24_E); /* AA + a24*E */
         fe_mul(z2,tmp1,tmp0);}
        fe_sq(x3,x3); fe_mul(z3,x1,z2);
    }
    fe_cswap(x2,x3,swap); fe_cswap(z2,z3,swap);
    fe_invert(z2,z2); fe_mul(x2,x2,z2);
    fe_store(out,x2);
}

"""

with open(TLS, encoding='utf-8') as f:
    src = f.read()

# Find old X25519 section
i_start = src.find("typedef uint64_t fe64[5];")
i_end   = src.find("\n/* ================================================================\n * TLS 1.3 state machine", i_start)
if i_start < 0 or i_end < 0:
    # try alternate
    i_start = src.find("/* X25519 (Curve25519) — scalar multiplication")
    i_end   = src.find("/* ================================================================\n * TLS 1.3 state machine", i_start)
    if i_start < 0:
        # find the fe/x25519 section by fe_frombytes or fe_load
        i_start = src.find("typedef int32_t fe[10];")
        if i_start < 0:
            raise RuntimeError("Cannot find X25519 section start")

print(f"Replacing X25519: {i_start}..{i_end}")
src2 = src[:i_start] + NEW_X25519 + src[i_end+1:]
with open(TLS, 'w', encoding='utf-8') as f:
    f.write(src2)
print("Done")
