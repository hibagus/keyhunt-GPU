#pragma once
#include "point.h"
#include "hash160.h"

namespace keyhunt::gpu {
// Convert the one-based work coordinate to a fixed-width Base58 suffix. The
// highest limb is divided first; leading zero digits deliberately become '1'.
KEYHUNT_HD inline bool minikey_text(Scalar ordinal,unsigned length,uint8_t* text){
    if(length!=22&&length!=30)return false;
    uint32_t nonzero=0;for(unsigned i=0;i<8;++i)nonzero|=ordinal.limb[i];if(!nonzero)return false;
    for(unsigned i=0;i<8;++i){const auto prior=ordinal.limb[i];--ordinal.limb[i];if(prior)break;}
    const char alphabet[]="123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    text[0]='S';text[length]=0;
    for(unsigned position=length-1;position>0;--position){
        uint64_t remainder=0;
        for(int limb=7;limb>=0;--limb){const uint64_t value=(remainder<<32)|ordinal.limb[limb];
            ordinal.limb[limb]=uint32_t(value/58);remainder=value%58;}
        text[position]=uint8_t(alphabet[remainder]);
    }
    nonzero=0;for(unsigned i=0;i<8;++i)nonzero|=ordinal.limb[i];
    return nonzero==0; // A high suffix must never be truncated into another ordinal.
}
KEYHUNT_HD inline bool minikey_private(const uint8_t* text,unsigned length,Scalar& scalar){
    if(length!=22&&length!=30)return false;
    uint8_t message[32]{},digest[32];for(unsigned i=0;i<length;++i)message[i]=text[i];message[length]='?';
    hash::sha256_short(message,length+1,digest);if(digest[0])return false;
    hash::sha256_short(message,length,digest);const auto value=scalar_from_bytes(digest);
    // SHA-256 is interpreted as an integer, never reduced modulo the curve order.
    if(!valid_scalar(value))return false;
    scalar=value;return true;
}
} // namespace keyhunt::gpu
