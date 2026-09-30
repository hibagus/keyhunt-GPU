#pragma once
#include "common/point.h"
#include "multiply16.h"

namespace keyhunt::gpu::test {
enum class Op : uint32_t { Normalize, Add, Sub, Mul, Mul16, Square, Negate, Inverse, BatchInverse, Bytes, Limits, WordAdd, WordSub, Public, PointAdd, PointMixed, PointDouble, PointNegate, PointReduce, PointMultiply, PointValid };
struct Request { Op op{}; uint32_t count = 0; Field values[32]{}; };
struct Result { Field values[32]{}; uint32_t flags = 0; };

KEYHUNT_HD inline Point input_point(const Request& request, unsigned offset, unsigned scale) {
    Point point{request.values[offset],request.values[offset+1],request.values[offset+2]};
    if (is_infinity(point)) return Point{};
    const Field z = normalize(request.values[scale]);
    Field zz, zzz;
    square(zz,z); mul(zzz,zz,z);
    mul(point.x,point.x,zz); mul(point.y,point.y,zzz); point.z = z;
    return point;
}
KEYHUNT_HD inline void output_point(Result& result, unsigned index, const Point& point) {
    if (!less(point.x,prime()) || !less(point.y,prime()) || !less(point.z,prime()) ||
        (is_infinity(point) && (!is_zero(point.x) || !is_zero(point.y)))) result.flags |= 0x40000000U;
    const auto affine = to_affine(point);
    if (affine.infinity) result.flags |= 1U << index;
    result.values[2*index] = affine.x; result.values[2*index+1] = affine.y;
}
KEYHUNT_HD inline Result evaluate_point(const Request& request) {
    Result result{};
    Scalar scalar;
    uint8_t bytes[32]; to_bytes(bytes,request.values[8]); scalar = scalar_from_bytes(bytes);
    if (request.op == Op::PointValid) {
        result.flags = on_curve(Affine{request.values[0],request.values[1],false}); return result;
    }
    Point a = input_point(request,0,6), b = input_point(request,3,7), out;
    auto left = a, right = b;
    switch (request.op) {
    case Op::Public:
        if (!public_key(out,scalar)) result.flags = 0x80000000U;
        else output_point(result,0,out);
        break;
    case Op::PointValid:
        result.flags = on_curve(Affine{request.values[0],request.values[1],false}); break;
    case Op::PointReduce: output_point(result,0,a); break;
    case Op::PointDouble:
        point_double(out,a); point_double(left,left);
        output_point(result,0,out); output_point(result,1,left); break;
    case Op::PointNegate:
        point_negate(out,a); point_negate(left,left);
        output_point(result,0,out); output_point(result,1,left); break;
    case Op::PointMultiply:
        point_multiply(out,a,scalar); point_multiply(left,left,scalar);
        output_point(result,0,out); output_point(result,1,left); break;
    case Op::PointAdd:
        point_add(out,a,b); point_add(left,left,b); point_add(right,a,right);
        output_point(result,0,out); output_point(result,1,left); output_point(result,2,right); break;
    case Op::PointMixed: {
        const auto affine = to_affine(b);
        point_add_mixed(out,a,affine); point_add_mixed(left,left,affine);
        output_point(result,0,out); output_point(result,1,left); break;
    }
    default: break;
    }
    return result;
}

// Raw 256-bit words test the final carry/borrow before modular reduction can
// hide it. Expected values come from Python integers, including both aliases.
KEYHUNT_HD inline uint32_t evaluate_words(Field& out, const Field& a, const Field& b, bool subtract) {
    uint32_t flag = 0;
#if defined(KEYHUNT_USE_GFX942_CARRY)
    if (subtract) gfx942::subtract_words(out.limb, a.limb, b.limb, flag);
    else gfx942::add_words(out.limb, a.limb, b.limb, flag);
#else
    if (subtract) out = subtract_words(a, b, flag);
    else {
        for (unsigned i = 0; i < 8; ++i) {
            const uint64_t sum = uint64_t(a.limb[i]) + b.limb[i] + flag;
            out.limb[i] = uint32_t(sum);
            flag = uint32_t(sum >> 32);
        }
    }
#endif
    return flag;
}

// Both host and device probes use this adapter. Expected results come from
// independent Python/native oracles, never from this shared evaluation code.
KEYHUNT_HD inline Result evaluate(const Request& request) {
    if (request.op >= Op::Public) return evaluate_point(request);
    Result result{};
    const auto a = normalize(request.values[0]), b = normalize(request.values[1]);
    auto left = a, right = b;
    switch (request.op) {
    case Op::WordAdd: case Op::WordSub: {
        const auto& x = request.values[0]; const auto& y = request.values[1];
        Field left_words = x, right_words = y;
        const bool subtract = request.op == Op::WordSub;
        result.flags = evaluate_words(result.values[0], x, y, subtract);
        const auto left_flag = evaluate_words(left_words, left_words, y, subtract);
        const auto right_flag = evaluate_words(right_words, x, right_words, subtract);
        if (left_flag != result.flags || right_flag != result.flags) result.flags |= 0x80000000U;
        result.values[1] = left_words; result.values[2] = right_words;
        break;
    }
    case Op::Normalize: result.values[0] = a; break;
    case Op::Add:
        add(result.values[0], a, b); add(left, left, b); add(right, a, right);
        result.values[1] = left; result.values[2] = right; break;
    case Op::Sub:
        sub(result.values[0], a, b); sub(left, left, b); sub(right, a, right);
        result.values[1] = left; result.values[2] = right; break;
    case Op::Mul:
        mul(result.values[0], a, b); mul(left, left, b); mul(right, a, right);
        result.values[1] = left; result.values[2] = right; break;
    case Op::Mul16:
        multiply16(result.values[0],a,b); multiply16(left,left,b); multiply16(right,a,right);
        result.values[1] = left; result.values[2] = right; break;
    case Op::Square:
        square(result.values[0], a); square(left, left); result.values[1] = left; break;
    case Op::Negate:
        neg(result.values[0], a); neg(left, left); result.values[1] = left; break;
    case Op::Inverse:
        result.flags = inverse(result.values[0], a);
        if (inverse(left, left) != bool(result.flags)) result.flags = 2;
        result.values[1] = left; break;
    case Op::Bytes: {
        uint8_t bytes[32];
        to_bytes(bytes, request.values[0]);
        result.flags = from_bytes_checked(result.values[0], bytes);
        result.values[1] = from_bytes_reduced(bytes); break;
    }
    case Op::Limits: {
        Field output[2]{one(),one()}, input[2]{one(),Field{}};
        const bool rejected = !batch_inverse<1>(output,input,2);
        const bool empty = batch_inverse<1>(output,input,0);
        result.values[0].limb[0] = !(rejected && empty && equal(output[0],one()) && equal(output[1],one()));
        break;
    }
    case Op::BatchInverse: {
        Field values[32];
        for (unsigned i = 0; i < 32; ++i) values[i] = normalize(request.values[i]);
        result.flags = batch_inverse<32>(result.values, values, request.count);
        if (request.count <= 32) {
            batch_inverse<32>(values, values, request.count);
            for (unsigned i = 0; i < request.count; ++i)
                if (!equal(values[i], result.values[i])) result.flags = 2;
        }
        break;
    }
    default: break;
    }
    return result;
}
}
