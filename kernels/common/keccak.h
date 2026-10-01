#pragma once
#include "field.h"

namespace keyhunt::gpu::hash {
KEYHUNT_HD inline uint64_t keccak_rotate(uint64_t value,unsigned bits) {
    return bits ? (value<<bits)|(value>>(64-bits)) : value;
}
KEYHUNT_HD inline void keccak_permute(uint64_t* state) {
    // Lane index is x + 5*y. Explicit rho/pi coordinates make the byte/lane
    // convention visible; no host-endian casts or external hash state is used.
    const unsigned rotations[25]={0,1,62,28,27,36,44,6,55,20,3,10,43,25,39,41,45,15,21,8,18,2,61,56,14};
    const uint64_t constants[24]={
        0x0000000000000001ULL,0x0000000000008082ULL,0x800000000000808aULL,0x8000000080008000ULL,
        0x000000000000808bULL,0x0000000080000001ULL,0x8000000080008081ULL,0x8000000000008009ULL,
        0x000000000000008aULL,0x0000000000000088ULL,0x0000000080008009ULL,0x000000008000000aULL,
        0x000000008000808bULL,0x800000000000008bULL,0x8000000000008089ULL,0x8000000000008003ULL,
        0x8000000000008002ULL,0x8000000000000080ULL,0x000000000000800aULL,0x800000008000000aULL,
        0x8000000080008081ULL,0x8000000000008080ULL,0x0000000080000001ULL,0x8000000080008008ULL};
    for(unsigned round=0;round<24;++round) {
        uint64_t columns[5],moved[25];
        for(unsigned x=0;x<5;++x)columns[x]=state[x]^state[x+5]^state[x+10]^state[x+15]^state[x+20];
        for(unsigned x=0;x<5;++x) {
            const uint64_t delta=columns[(x+4)%5]^keccak_rotate(columns[(x+1)%5],1);
            for(unsigned y=0;y<5;++y) {
                const unsigned index=x+5*y;
                moved[y+5*((2*x+3*y)%5)]=keccak_rotate(state[index]^delta,rotations[index]);
            }
        }
        for(unsigned y=0;y<5;++y)for(unsigned x=0;x<5;++x)
            state[x+5*y]=moved[x+5*y]^((~moved[(x+1)%5+5*y])&moved[(x+2)%5+5*y]);
        state[0]^=constants[round];
    }
}
// Ethereum's 64-byte public point and ERC-55's 40-byte text both fit one rate
// block. Refuse longer inputs before touching memory. Keccak suffix 0x01 is
// essential: SHA3-256 uses 0x06 and produces unrelated addresses.
KEYHUNT_HD inline bool keccak256_short(const uint8_t* input,unsigned length,uint8_t* output) {
    if(length>135)return false;
    uint64_t state[25]{};
    for(unsigned i=0;i<length;++i)state[i/8]^=uint64_t(input[i])<<(8*(i%8));
    state[length/8]^=uint64_t(1)<<(8*(length%8));
    state[16]^=0x8000000000000000ULL;
    keccak_permute(state);
    for(unsigned i=0;i<32;++i)output[i]=uint8_t(state[i/8]>>(8*(i%8)));
    return true;
}
} // namespace keyhunt::gpu::hash
