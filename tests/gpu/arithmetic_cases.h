#pragma once
#include "common/field.h"

namespace keyhunt::gpu::test {
enum class Op : uint32_t { Normalize, Add, Sub, Mul, Square, Negate, Inverse, BatchInverse, Bytes };
struct Request { Op op{}; uint32_t count = 0; Field values[32]{}; };
struct Result { Field values[32]{}; uint32_t flags = 0; };

// Both host and device probes use this adapter. Expected results come from
// independent Python/native oracles, never from this shared evaluation code.
KEYHUNT_HD inline Result evaluate(const Request& request) {
    Result result{};
    const auto a = normalize(request.values[0]), b = normalize(request.values[1]);
    auto left = a, right = b;
    switch (request.op) {
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
    }
    return result;
}
}
