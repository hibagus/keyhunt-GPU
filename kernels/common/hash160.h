#pragma once
#include "field.h"

namespace keyhunt::gpu::hash {
// Fixed-storage short-message hashes for SEC1 keys (33 or 65 bytes). These are
// device-safe implementations of FIPS 180-4 and the RIPEMD-160 specification;
// the retained CPU verifier is separate. All byte order is explicit.
KEYHUNT_HD inline uint32_t rol(uint32_t x,unsigned n) { return (x<<n)|(x>>(32-n)); }
KEYHUNT_HD inline uint32_t ror(uint32_t x,unsigned n) { return (x>>n)|(x<<(32-n)); }
KEYHUNT_HD inline bool sha256_short(const uint8_t* data,unsigned length,uint8_t* out) {
    if(length>119)return false; // At most two padded 64-byte blocks.
    const uint32_t constants[64]={
        0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
        0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
        0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
        0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
        0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
        0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
        0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
        0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U
    };
    uint32_t state[8]={0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,
                       0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U};
    const unsigned blocks=length<56?1:2;
    for(unsigned block=0;block<blocks;++block) {
        uint32_t words[16]{};
        for(unsigned i=0;i<64;++i) {
            const unsigned position=block*64+i;
            uint8_t byte=position<length?data[position]:(position==length?0x80:0);
            // The supported bit length fits in 16 bits; preceding length bytes
            // are already zero. SHA-256 encodes both words and length big-endian.
            if(position==blocks*64-2)byte=uint8_t((length*8)>>8);
            if(position==blocks*64-1)byte=uint8_t(length*8);
            words[i/4]|=uint32_t(byte)<<(24-8*(i%4));
        }
        uint32_t a=state[0],b=state[1],c=state[2],d=state[3];
        uint32_t e=state[4],f=state[5],g=state[6],h=state[7];
        for(unsigned i=0;i<64;++i) {
            if(i>=16) {
                const uint32_t x=words[(i-15)&15],y=words[(i-2)&15];
                words[i&15]+=(ror(x,7)^ror(x,18)^(x>>3))+words[(i-7)&15]
                            +(ror(y,17)^ror(y,19)^(y>>10));
            }
            const uint32_t t1=h+(ror(e,6)^ror(e,11)^ror(e,25))
                +((e&f)^(~e&g))+constants[i]+words[i&15];
            const uint32_t t2=(ror(a,2)^ror(a,13)^ror(a,22))+((a&b)^(a&c)^(b&c));
            h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;
        state[4]+=e;state[5]+=f;state[6]+=g;state[7]+=h;
    }
    for(unsigned i=0;i<32;++i)out[i]=uint8_t(state[i/4]>>(24-8*(i%4)));
    return true;
}
KEYHUNT_HD inline uint32_t ripemd_function(unsigned round,uint32_t x,uint32_t y,uint32_t z) {
    switch(round) {
    case 0:return x^y^z;
    case 1:return (x&y)|(~x&z);
    case 2:return (x|~y)^z;
    case 3:return (x&z)|(y&~z);
    default:return x^(y|~z);
    }
}
KEYHUNT_HD inline bool ripemd160_short(const uint8_t* data,unsigned length,uint8_t* out) {
    if(length>55)return false; // HASH160 always feeds a 32-byte SHA-256 digest.
    const uint8_t left_index[80]={
        0,1,2,3,4,5,6,7,
        8,9,10,11,12,13,14,15,
        7,4,13,1,10,6,15,3,
        12,0,9,5,2,14,11,8,
        3,10,14,4,9,15,8,1,
        2,7,0,6,13,11,5,12,
        1,9,11,10,0,8,12,4,
        13,3,7,15,14,5,6,2,
        4,0,5,9,7,12,2,10,
        14,1,3,8,11,6,15,13
    };
    const uint8_t left_shift[80]={
        11,14,15,12,5,8,7,9,
        11,13,14,15,6,7,9,8,
        7,6,8,13,11,9,7,15,
        7,12,15,9,11,7,13,12,
        11,13,6,7,14,9,13,15,
        14,8,13,6,5,12,7,5,
        11,12,14,15,14,15,9,8,
        9,14,5,6,8,6,5,12,
        9,15,5,11,6,8,13,12,
        5,12,13,14,11,8,5,6
    };
    const uint8_t right_index[80]={
        5,14,7,0,9,2,11,4,
        13,6,15,8,1,10,3,12,
        6,11,3,7,0,13,5,10,
        14,15,8,12,4,9,1,2,
        15,5,1,3,7,14,6,9,
        11,8,12,2,10,0,4,13,
        8,6,4,1,3,11,15,0,
        5,12,2,13,9,7,10,14,
        12,15,10,4,1,5,8,7,
        6,2,13,14,0,3,9,11
    };
    const uint8_t right_shift[80]={
        8,9,9,11,13,15,15,5,
        7,7,8,11,14,14,12,6,
        9,13,15,7,12,8,9,11,
        7,7,12,7,6,15,13,11,
        9,7,15,11,8,6,6,14,
        12,13,5,14,13,13,7,5,
        15,5,8,11,14,14,6,14,
        6,9,12,9,12,5,15,8,
        8,5,12,9,12,5,14,6,
        8,13,6,5,15,13,11,11
    };
    const uint32_t left_constant[5]={0,0x5a827999U,0x6ed9eba1U,0x8f1bbcdcU,0xa953fd4eU};
    const uint32_t right_constant[5]={0x50a28be6U,0x5c4dd124U,0x6d703ef3U,0x7a6d76e9U,0};
    uint32_t words[16]{};
    for(unsigned i=0;i<length;++i)words[i/4]|=uint32_t(data[i])<<(8*(i%4));
    words[length/4]|=uint32_t(0x80)<<(8*(length%4));words[14]=length*8;
    const uint32_t initial[5]={0x67452301U,0xefcdab89U,0x98badcfeU,0x10325476U,0xc3d2e1f0U};
    uint32_t a=initial[0],b=initial[1],c=initial[2],d=initial[3],e=initial[4];
    uint32_t aa=a,bb=b,cc=c,dd=d,ee=e;
    for(unsigned i=0;i<80;++i) {
        const unsigned round=i/16;
        const uint32_t left=rol(a+ripemd_function(round,b,c,d)+words[left_index[i]]
                               +left_constant[round],left_shift[i])+e;
        a=e;e=d;d=rol(c,10);c=b;b=left;
        const uint32_t right=rol(aa+ripemd_function(4-round,bb,cc,dd)+words[right_index[i]]
                                +right_constant[round],right_shift[i])+ee;
        aa=ee;ee=dd;dd=rol(cc,10);cc=bb;bb=right;
    }
    // RIPEMD combines the parallel lanes across different state positions.
    const uint32_t result[5]={initial[1]+c+dd,initial[2]+d+ee,initial[3]+e+aa,
                              initial[4]+a+bb,initial[0]+b+cc};
    for(unsigned i=0;i<20;++i)out[i]=uint8_t(result[i/4]>>(8*(i%4)));
    return true;
}
KEYHUNT_HD inline bool public_key(const uint8_t* serialized,unsigned length,uint8_t* out) {
    if(length!=33&&length!=65)return false;
    uint8_t sha[32];sha256_short(serialized,length,sha);
    return ripemd160_short(sha,32,out);
}
} // namespace keyhunt::gpu::hash
