#pragma once
#include "field.h"

namespace keyhunt::gpu {
// Jacobian coordinates: affine x=X/Z^2, y=Y/Z^3. Every Z=0 denotes infinity;
// operations emit the canonical all-zero infinity. Finite inputs must be valid
// curve points with canonical field coordinates. These operations are variable time.
struct Point { Field x{}, y{}, z{}; };
struct Affine { Field x{}, y{}; bool infinity = true; };
// Scalars are unsigned 256-bit integers, deliberately distinct from Field.
// Raw multiplication accepts all bit patterns; checked public-key derivation
// requires 1 <= scalar < n. No field reduction is ever applied to scalar bits.
struct Scalar { uint32_t limb[8]{}; };
KEYHUNT_HD inline Scalar scalar_from_bytes(const uint8_t bytes[32]) {
    Scalar scalar{};
    for (unsigned i = 0; i < 32; ++i)
        scalar.limb[(31-i)/4] |= uint32_t(bytes[i]) << (8*((31-i)%4));
    return scalar;
}
KEYHUNT_HD inline bool valid_scalar(const Scalar& scalar) {
    const Scalar order{{0xd0364141U,0xbfd25e8cU,0xaf48a03bU,0xbaaedce6U,
                        0xfffffffeU,0xffffffffU,0xffffffffU,0xffffffffU}};
    uint32_t nonzero = 0;
    for (unsigned i = 0; i < 8; ++i) nonzero |= scalar.limb[i];
    if (!nonzero) return false;
    for (int i = 7; i >= 0; --i)
        if (scalar.limb[i] != order.limb[i]) return scalar.limb[i] < order.limb[i];
    return false;
}
KEYHUNT_HD inline bool is_infinity(const Point& point) { return is_zero(point.z); }
KEYHUNT_HD inline Point from_affine(const Affine& point) {
    return point.infinity ? Point{} : Point{point.x,point.y,one()};
}
KEYHUNT_HD inline bool on_curve(const Affine& point) {
    if (point.infinity) return true;
    if (!less(point.x,prime()) || !less(point.y,prime())) return false;
    Field lhs, rhs, seven{{7,0,0,0,0,0,0,0}};
    square(lhs,point.y); square(rhs,point.x); mul(rhs,rhs,point.x); add(rhs,rhs,seven);
    return equal(lhs,rhs);
}
KEYHUNT_HD inline Affine to_affine(const Point& point) {
    if (is_infinity(point)) return Affine{};
    Field zi, zz;
    inverse(zi,point.z); square(zz,zi);
    Affine result;
    result.infinity = false;
    mul(result.x,point.x,zz);
    mul(zz,zz,zi); mul(result.y,point.y,zz);
    return result;
}
KEYHUNT_HD inline Point generator() {
    return {{{0x16f81798U,0x59f2815bU,0x2dce28d9U,0x029bfcdbU,0xce870b07U,0x55a06295U,0xf9dcbbacU,0x79be667eU}},
            {{0xfb10d4b8U,0x9c47d08fU,0xa6855419U,0xfd17b448U,0x0e1108a8U,0x5da4fbfcU,0x26a3c465U,0x483ada77U}},one()};
}
KEYHUNT_HD inline void point_negate(Point& out, const Point& point) {
    Point result = point;
    if (is_infinity(point)) result = Point{};
    else neg(result.y,point.y);
    out = result;
}
KEYHUNT_HD inline void point_double(Point& out, const Point& point) {
    if (is_infinity(point) || is_zero(point.y)) { out = Point{}; return; }
    // Tangent formula for y^2=x^3+7 (a=0): S=4XY^2, M=3X^2.
    // All writes go to temporaries, preserving the old Y when out aliases point.
    Field xx, yy, yyyy, s, m, twice_s, eight_yyyy, temp;
    square(xx,point.x); square(yy,point.y); square(yyyy,yy);
    mul(s,point.x,yy); add(s,s,s); add(s,s,s);
    add(m,xx,xx); add(m,m,xx);
    Point result;
    square(result.x,m); add(twice_s,s,s); sub(result.x,result.x,twice_s);
    sub(temp,s,result.x); mul(result.y,m,temp);
    add(eight_yyyy,yyyy,yyyy); add(eight_yyyy,eight_yyyy,eight_yyyy); add(eight_yyyy,eight_yyyy,eight_yyyy);
    sub(result.y,result.y,eight_yyyy);
    mul(result.z,point.y,point.z); add(result.z,result.z,result.z);
    out = result;
}
KEYHUNT_HD inline void point_add(Point& out, const Point& a, const Point& b) {
    if (is_infinity(a)) { out = is_infinity(b) ? Point{} : b; return; }
    if (is_infinity(b)) { out = a; return; }
    Field z1z1,z2z2,u1,u2,s1,s2,h,r,hh,hhh,v,temp;
    square(z1z1,a.z); square(z2z2,b.z);
    mul(u1,a.x,z2z2); mul(u2,b.x,z1z1);
    mul(s1,a.y,b.z); mul(s1,s1,z2z2);
    mul(s2,b.y,a.z); mul(s2,s2,z1z1);
    sub(h,u2,u1); sub(r,s2,s1);
    if (is_zero(h)) {
        if (is_zero(r)) point_double(out,a); // equal affine points, even with different Z
        else out = Point{}; // P + (-P)
        return;
    }
    square(hh,h); mul(hhh,hh,h); mul(v,u1,hh);
    Point result;
    square(result.x,r); sub(result.x,result.x,hhh); add(temp,v,v); sub(result.x,result.x,temp);
    sub(temp,v,result.x); mul(result.y,r,temp); mul(temp,s1,hhh); sub(result.y,result.y,temp);
    mul(result.z,a.z,b.z); mul(result.z,result.z,h);
    out = result;
}
KEYHUNT_HD inline void point_add_mixed(Point& out, const Point& a, const Affine& b) {
#if !defined(__CUDACC__) || (defined(__CUDA_ARCH__) && !defined(KEYHUNT_CUDA_PORTABLE_MIXED))
    // b has Z=1: omit its Z square and the multiplications by that Z and Z^2.
    // This is the same Jacobian group law as point_add, including doubling,
    // inverse points, infinity, and an output aliased to a.
    if (b.infinity) { out = is_infinity(a) ? Point{} : a; return; }
    if (is_infinity(a)) { out = from_affine(b); return; }
    Field zz, u2, s2, h, r, hh, hhh, v, temp;
    square(zz, a.z); mul(u2, b.x, zz);
    mul(s2, b.y, a.z); mul(s2, s2, zz);
    sub(h, u2, a.x); sub(r, s2, a.y);
    if (is_zero(h)) {
        if (is_zero(r)) point_double(out, a);
        else out = Point{};
        return;
    }
    square(hh, h); mul(hhh, hh, h); mul(v, a.x, hh);
    Point result;
    square(result.x, r); sub(result.x, result.x, hhh);
    add(temp, v, v); sub(result.x, result.x, temp);
    sub(temp, v, result.x); mul(result.y, r, temp);
    mul(temp, a.y, hhh); sub(result.y, result.y, temp);
    mul(result.z, a.z, h);
    out = result;
#else
    // Retain the general formula for the explicit CUDA reference path.
    const Point right = from_affine(b);
    point_add(out,a,right);
#endif
}
KEYHUNT_HD inline void point_add_cached(Point& out, const Point& a, const Point& b) {
    // Search executors upload only finite affine seed/power points (Z=1).
    // General callers must use point_add or point_add_mixed instead.
#if defined(__CUDA_ARCH__) && !defined(KEYHUNT_CUDA_PORTABLE_MIXED)
    point_add_mixed(out, a, Affine{b.x, b.y, false});
#else
    point_add(out, a, b);
#endif
}
KEYHUNT_HD inline void point_multiply(Point& out, const Point& point, const Scalar& scalar) {
    Point result{};
    for (int bit = 255; bit >= 0; --bit) {
        point_double(result,result);
        if ((scalar.limb[bit/32] >> (bit%32)) & 1) point_add(result,result,point);
    }
    out = result;
}
KEYHUNT_HD inline bool public_key(Point& out, const Scalar& scalar) {
    if (!valid_scalar(scalar)) { out = Point{}; return false; }
    point_multiply(out,generator(),scalar);
    return true;
}
} // namespace keyhunt::gpu
