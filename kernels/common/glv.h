#pragma once
#include "point.h"

namespace keyhunt::gpu {
// The lattice basis and reciprocal constants follow libsecp256k1 v0.6.0,
// scalar_impl.h (MIT, Copyright (c) 2014 Pieter Wuille). See C23_GLV.md and
// third_party/secp256k1-oracle/COPYING. This is a separate 32-bit-limb implementation.
struct GlvSigned128 { uint32_t limb[4]{}; bool negative=false; };
struct GlvSplit { GlvSigned128 first{},second{}; };
namespace glv_detail {
// Full integer products: these words are not field elements modulo p.
// Each multiply-accumulate is at most (2^32-1)^2+2*(2^32-1), fitting uint64_t.
template<unsigned Left,unsigned Right>
KEYHUNT_HD inline void product(uint32_t out[16],const Scalar& a,const Scalar& b) {
    for(unsigned i=0;i<16;++i)out[i]=0;
    for(unsigned i=0;i<Left;++i){
        uint64_t carry=0;
        for(unsigned j=0;j<Right;++j){
            const uint64_t sum=uint64_t(a.limb[i])*b.limb[j]+out[i+j]+carry;
            out[i+j]=uint32_t(sum);carry=sum>>32;
        }
        out[i+Right]=uint32_t(carry);
    }
}
KEYHUNT_HD inline Scalar rounded_quotient(const Scalar& k,const Scalar& reciprocal) {
    uint32_t wide[16];product<8,8>(wide,k,reciprocal);
    Scalar result;
    uint64_t carry=wide[11]>>31; // round the product divided by 2^384
    for(unsigned i=0;i<4;++i){
        const uint64_t sum=uint64_t(wide[i+12])+carry;
        result.limb[i]=uint32_t(sum);carry=sum>>32;
    }
    // With the fixed reciprocals and k<n, the rounded quotient fits 128 bits.
    return result;
}
KEYHUNT_HD inline Scalar subtract(const Scalar& a,const Scalar& b) {
    Scalar result;uint64_t borrow=0;
    for(unsigned i=0;i<8;++i){
        const uint64_t right=uint64_t(b.limb[i])+borrow;
        result.limb[i]=uint32_t(uint64_t(a.limb[i])-right);
        borrow=uint64_t(a.limb[i])<right;
    }
    return result; // two's complement modulo 2^256; not scalar reduction modulo n
}
template<unsigned Right>
KEYHUNT_HD inline Scalar basis_product(const Scalar& coefficient,const Scalar& basis) {
    uint32_t wide[16];product<4,Right>(wide,coefficient,basis);
    Scalar result;for(unsigned i=0;i<8;++i)result.limb[i]=wide[i];
    return result;
}
KEYHUNT_HD inline bool signed_component(GlvSigned128& out,const Scalar& value) {
    out.negative=(value.limb[7]>>31)!=0;
    uint64_t carry=out.negative?1:0;
    for(unsigned i=0;i<8;++i){
        const uint64_t sum=uint64_t(out.negative?~value.limb[i]:value.limb[i])+carry;
        const auto magnitude=uint32_t(sum);carry=sum>>32;
        if(i<4)out.limb[i]=magnitude;
        else if(magnitude)return false; // fail closed if the proven 128-bit bound is violated
    }
    return true;
}
} // namespace glv_detail
KEYHUNT_HD inline bool glv_split(GlvSplit& out,const Scalar& k) {
    if(!valid_scalar(k)){out=GlvSplit{};return false;}
    const Scalar a1={{0x9284eb15U,0xe86c90e4U,0xa7d46bcdU,0x3086d221U,0x00000000U,0x00000000U,0x00000000U,0x00000000U}},minus_b1={{0x0abfe4c3U,0x6f547fa9U,0x010e8828U,0xe4437ed6U,0x00000000U,0x00000000U,0x00000000U,0x00000000U}},a2={{0x9d44cfd8U,0x57c1108dU,0xa8e2f3f6U,0x14ca50f7U,0x00000001U,0x00000000U,0x00000000U,0x00000000U}};
    const Scalar g1={{0x45dbb031U,0xe893209aU,0x71e8ca7fU,0x3daa8a14U,0x9284eb15U,0xe86c90e4U,0xa7d46bcdU,0x3086d221U}},g2={{0x8ac47f71U,0x1571b4aeU,0x9df506c6U,0x221208acU,0x0abfe4c4U,0x6f547fa9U,0x010e8828U,0xe4437ed6U}};
    const auto c1=glv_detail::rounded_quotient(k,g1),c2=glv_detail::rounded_quotient(k,g2);
    // The signed lattice differences are small. Computing their low 256 bits
    // preserves their two's-complement values even if an intermediate carries.
    const auto first=glv_detail::subtract(glv_detail::subtract(k,glv_detail::basis_product<4>(c1,a1)),
                                        glv_detail::basis_product<5>(c2,a2));
    const auto second=glv_detail::subtract(glv_detail::basis_product<4>(c1,minus_b1),
                                         glv_detail::basis_product<4>(c2,a1));
    return glv_detail::signed_component(out.first,first)&&glv_detail::signed_component(out.second,second);
}
KEYHUNT_HD inline void point_endomorphism(Point& out,const Point& point) {
    if(is_infinity(point)){out=Point{};return;}
    const Field beta={{0x719501eeU,0xc1396c28U,0x12f58995U,0x9cf04975U,0xac3434e9U,0x6e64479eU,0x657c0710U,0x7ae96a2bU}};
    Point result=point;mul(result.x,point.x,beta);out=result;
}
KEYHUNT_HD inline bool public_key_glv(Point& out,const Scalar& scalar) {
    GlvSplit split;
    if(!glv_split(split,scalar)){out=Point{};return false;}
    const auto base=generator();Point mapped;point_endomorphism(mapped,base);
    Affine first{base.x,base.y,false},second{mapped.x,mapped.y,false};
    if(split.first.negative)neg(first.y,first.y);
    if(split.second.negative)neg(second.y,second.y);
    Point result{};
    // Share the 128 doublings between both signed components. Mixed additions
    // use finite affine bases, so no extra inversion or per-candidate table is needed.
    for(int bit=127;bit>=0;--bit){
        point_double(result,result);
        if((split.first.limb[bit/32]>>(bit%32))&1U)point_add_mixed(result,result,first);
        if((split.second.limb[bit/32]>>(bit%32))&1U)point_add_mixed(result,result,second);
    }
    out=result;return !is_infinity(result);
}
} // namespace keyhunt::gpu
