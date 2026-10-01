#pragma once
#include "hash160.h"

namespace keyhunt::gpu::hash {
// A mainnet P2PKH payload is exactly 25 bytes: zero version, 20 hash bytes,
// then the first four bytes of double SHA-256. Output includes a terminator.
KEYHUNT_HD inline unsigned p2pkh_address(const uint8_t* hash,char* output) {
    uint8_t payload[25]{},first[32],second[32];
    for(unsigned i=0;i<20;++i)payload[i+1]=hash[i];
    sha256_short(payload,21,first);sha256_short(first,32,second);
    for(unsigned i=0;i<4;++i)payload[i+21]=second[i];
    unsigned zeroes=0;
    while(zeroes<25 && payload[zeroes]==0)++zeroes;
    // Schoolbook division of a big-endian byte array uses a bounded remainder.
    // Unlike integer-only conversion, each leading zero gets its own '1'.
    char reversed[34];unsigned count=0,begin=zeroes;
    const char alphabet[]="123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    while(begin<25) {
        unsigned remainder=0;
        for(unsigned i=begin;i<25;++i) {
            const unsigned value=256*remainder+payload[i];
            payload[i]=uint8_t(value/58);remainder=value%58;
        }
        if(count+zeroes>=34)return 0; // Defensive storage bound, never truncate.
        reversed[count++]=alphabet[remainder];
        while(begin<25 && payload[begin]==0)++begin;
    }
    unsigned length=0;
    for(unsigned i=0;i<zeroes;++i)output[length++]='1';
    while(count)output[length++]=reversed[--count];
    output[length]='\0';return length;
}
} // namespace keyhunt::gpu::hash
