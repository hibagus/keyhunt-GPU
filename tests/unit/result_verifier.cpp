#include "keyhunt/core/result_verifier.h"
#include "keyhunt/core/cpu_result_adapter.h"
#include "keyhunt/core/cpu_targets.h"
#include "keyhunt/crypto/secp256k1/SECP256k1.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>

using namespace keyhunt::core;

namespace {
int checks = 0;
void require(bool result, const char* message) {
    ++checks;
    if (!result) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

template<size_t N> std::array<uint8_t, N> hex(std::string_view text) {
    if (text.size() != 2 * N) std::abort();
    std::array<uint8_t, N> bytes{};
    auto digit = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        std::abort();
    };
    for (size_t i = 0; i < N; ++i) bytes[i] = digit(text[2*i]) * 16 + digit(text[2*i+1]);
    return bytes;
}
}

int main() {
    // Fixed public vectors from tests/baseline/vectors.json. These are regression
    // fixtures, not a replacement for C06's independently pinned arithmetic oracle.
    auto curve = std::make_unique<Secp256K1>();
    curve->Init();
    CpuResultVerifier verifier(*curve);
    ScalarBytes one{};
    one.back() = 1;
    const auto x = hex<32>("79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");
    const auto compressed = hex<33>("0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");
    const auto uncompressed = hex<65>("0479be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8");
    const auto hash_compressed = hex<20>("751e76e8199196d454941c45d1b3a323f1433bd6");
    const auto hash_uncompressed = hex<20>("91b24bf9f5288532960ac687abb035127b1d28a5");
    const auto eth = hex<20>("7e5f4552091a69125d5dfcb7b8c2659029395bdf");
    UncompressedPublicKey derived{};
    require(verifier.derive(one, derived) && derived == uncompressed, "derive scalar one");
    struct alignas(uint64_t) OffsetBuffers {
        uint8_t prefix;
        ScalarBytes scalar;
        UncompressedPublicKey output;
    } offset{};
    offset.scalar = one;
    require(reinterpret_cast<uintptr_t>(offset.scalar.data()) % 8 != 0
            && reinterpret_cast<uintptr_t>(offset.output.data()) % 8 != 0,
            "exercise intentionally unaligned input and output buffers");
    require(verifier.derive(offset.scalar, offset.output) && offset.output == uncompressed,
            "byte interface supports unaligned buffers");
    require(verifier.matches_xpoint(one, x), "full X match");
    auto wrong_x = x;
    wrong_x.back() ^= 1; // Shares the entire historical 20-byte prefix.
    require(!verifier.matches_xpoint(one, wrong_x), "reject X suffix mismatch");
    require(verifier.matches_public_key(one, compressed), "compressed key match");
    require(verifier.matches_public_key(one, uncompressed), "uncompressed key match");
    auto opposite = compressed;
    opposite.front() = 3;
    require(!verifier.matches_public_key(one, opposite), "reject opposite Y parity");
    auto bad_y = uncompressed;
    bad_y.back() ^= 1;
    require(!verifier.matches_public_key(one, bad_y), "reject changed Y coordinate");
    auto bad_prefix = compressed;
    bad_prefix.front() = 4;
    require(!verifier.matches_public_key(one, bad_prefix), "reject wrong SEC1 prefix");
    require(verifier.matches_hash160(one, hash_compressed, true), "compressed hash160");
    require(verifier.matches_hash160(one, hash_uncompressed, false), "uncompressed hash160");
    require(!verifier.matches_hash160(one, hash_compressed, false), "reject encoding mismatch");
    require(!verifier.matches_hash160(one, hash_uncompressed, true), "reject opposite encoding mismatch");
    require(verifier.matches_ethereum(one, eth), "Ethereum Keccak address");
    auto bad_eth = eth;
    bad_eth.back() ^= 1;
    require(!verifier.matches_ethereum(one, bad_eth), "reject wrong Ethereum address");

    const auto order = hex<32>("fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141");
    auto above_order = order;
    above_order.back()++;
    ScalarBytes maximum;
    maximum.fill(0xff);
    for (const auto& invalid : {ScalarBytes{}, order, above_order, maximum}) {
        derived.fill(0xa5);
        const auto sentinel = derived;
        require(!verifier.derive(invalid, derived) && derived == sentinel, "invalid scalar preserves output");
        require(!verifier.matches_xpoint(invalid, x), "invalid scalar cannot match X");
        require(!verifier.matches_public_key(invalid, compressed), "invalid scalar cannot match point");
        require(!verifier.matches_public_key(invalid, uncompressed), "invalid scalar cannot match full point");
        require(!verifier.matches_hash160(invalid, hash_compressed, true), "invalid scalar cannot match hash");
        require(!verifier.matches_ethereum(invalid, eth), "invalid scalar cannot match Ethereum");
    }
    auto last = order;
    last.back()--;
    require(verifier.matches_public_key(last, opposite), "order-minus-one is negative generator");
    require(verifier.matches_xpoint(last, x), "X-only target intentionally accepts both Y signs");
    require(!verifier.matches_public_key(last, compressed), "order-minus-one rejects positive generator");

    const auto high = hex<32>("0000000000000000000000000000000000000000000100000000000000001000");
    const auto high_pub = hex<33>("037d2fbfd534719d3770d2517bcdf734f9e933356d5476296df34a426076ae0c49");
    require(verifier.matches_public_key(high, high_pub), "scalar above 64 bits");
    require(!verifier.matches_public_key(one, high_pub), "different scalar cannot match target");

    Int cpu_one;
    cpu_one.SetInt32(1);
    Point point = curve->ComputePublicKey(&cpu_one);
    require(verifyCpuPublicKey(*curve, cpu_one, point), "CPU point adapter exact match");
    Point negative = curve->Negation(point);
    require(!verifyCpuPublicKey(*curve, cpu_one, negative), "CPU adapter rejects opposite Y");
    Int wide;
    wide.SetBase16("10000000000000000000000000000000000000000000000000000000000000001");
    require(!verifyCpuPublicKey(*curve, wide, point), "reject over-wide Int without truncation");
    Int negative_scalar;
    negative_scalar.SetInt32(1);
    negative_scalar.Neg();
    require(!verifyCpuPublicKey(*curve, negative_scalar, point), "reject negative Int");
    Point infinity = point;
    infinity.z.SetInt32(0);
    require(!verifyCpuPublicKey(*curve, cpu_one, infinity), "reject non-affine target");

    SearchConfig config;
    config.mode = MODE_RMD160;
    CpuTargetTable table(config);
    require(!verifyCpuTableCandidate(*curve, cpu_one, true, config, table), "empty table rejects candidate");
    table.count = 1;
    table.entries = static_cast<address_value*>(std::malloc(sizeof(address_value)));
    if (!table.entries) std::abort();
    std::copy(hash_compressed.begin(), hash_compressed.end(), table.entries[0].value);
    require(verifyCpuTableCandidate(*curve, cpu_one, true, config, table), "CPU table recomputes matching hash");
    require(!verifyCpuTableCandidate(*curve, cpu_one, false, config, table), "CPU table rejects wrong encoding");
    require(!verifyCpuTableCandidate(*curve, wide, true, config, table), "CPU table rejects over-wide scalar");
    table.entries[0].value[19] ^= 1;
    require(!verifyCpuTableCandidate(*curve, cpu_one, true, config, table), "CPU table rejects hash mismatch");
    std::cout << "PASS " << checks << " CPU verification checks\n";
}
