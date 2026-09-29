#include "keyhunt/core/result_verifier.h"
#include "keyhunt/core/cpu_result_adapter.h"
#include "keyhunt/core/cpu_targets.h"
#include "keyhunt/crypto/secp256k1/SECP256k1.h"
#include "sha3/sha3.h"
#include "keyhunt/crypto/hash/sha256.h"
#include "keyhunt/crypto/hash/ripemd160.h"

#include <algorithm>
#include <cstring>

namespace keyhunt::core {
namespace {
bool valid_scalar(Secp256K1& curve, Int& scalar) {
    return scalar.IsStrictPositive() && scalar.IsLower(&curve.order);
}

bool compute(Secp256K1& curve, const ScalarBytes& scalar, Point& output) {
    alignas(uint64_t) auto bytes = scalar; // The existing Int importer accepts mutable bytes.
    Int key;
    key.Set32Bytes(bytes.data());
    if (!valid_scalar(curve, key)) return false;
    output = curve.ComputePublicKey(&key);
    return true;
}

// Int's legacy byte exporter casts its output to uint64_t*. Keep that alignment
// requirement inside the adapter; callers may use arbitrarily aligned byte arrays.
void export32(Int& value, uint8_t* output) {
    alignas(uint64_t) uint8_t bytes[32];
    value.Get32Bytes(bytes);
    std::memcpy(output, bytes, sizeof(bytes));
}

void serialize(Point& point, UncompressedPublicKey& output) {
    output[0] = 0x04;
    export32(point.x, output.data() + 1);
    export32(point.y, output.data() + 33);
}
// Use the existing general hash APIs on serialized bytes. The search engine's
// specialized GetHash160 path writes Int limbs at unaligned SEC1 offsets.
void hash160(Point& point, bool compressed, uint8_t* output) {
    alignas(uint64_t) uint8_t bytes[65], digest[32];
    bytes[0] = compressed ? (point.y.IsEven() ? 0x02 : 0x03) : 0x04;
    export32(point.x, bytes + 1);
    if (!compressed) export32(point.y, bytes + 33);
    sha256(bytes, compressed ? 33 : 65, digest);
    ripemd160(digest, 32, output);
}
} // namespace

bool CpuResultVerifier::derive(const ScalarBytes& scalar, UncompressedPublicKey& output) const {
    Point point;
    if (!compute(curve, scalar, point)) return false;
    serialize(point, output);
    return true;
}

bool CpuResultVerifier::matches_xpoint(const ScalarBytes& scalar, const XPointBytes& target) const {
    Point point;
    if (!compute(curve, scalar, point)) return false;
    XPointBytes x;
    export32(point.x, x.data());
    return x == target;
}

bool CpuResultVerifier::matches_public_key(const ScalarBytes& scalar,
                                          const CompressedPublicKey& target) const {
    Point point;
    if (!compute(curve, scalar, point)) return false;
    CompressedPublicKey actual;
    actual[0] = point.y.IsEven() ? 0x02 : 0x03;
    export32(point.x, actual.data() + 1);
    return actual == target;
}

bool CpuResultVerifier::matches_public_key(const ScalarBytes& scalar,
                                          const UncompressedPublicKey& target) const {
    UncompressedPublicKey actual;
    return derive(scalar, actual) && actual == target;
}

bool CpuResultVerifier::matches_hash160(const ScalarBytes& scalar, const Hash160Bytes& target,
                                       bool compressed) const {
    Point point;
    if (!compute(curve, scalar, point)) return false;
    Hash160Bytes actual;
    hash160(point, compressed, actual.data());
    return actual == target;
}

// Extracted from the CPU application's Ethereum address derivation.
void generate_binaddress_eth(Point& publickey, unsigned char* dst_address) {
    alignas(uint64_t) unsigned char bytes[64], digest[32];
    publickey.x.Get32Bytes(bytes);
    publickey.y.Get32Bytes(bytes + 32);
    SHA3_256_CTX context;
    SHA3_256_Init(&context);
    SHA3_256_Update(&context, bytes, sizeof(bytes));
    KECCAK_256_Final(digest, &context);
    std::memcpy(dst_address, digest + 12, 20);
}

bool CpuResultVerifier::matches_ethereum(const ScalarBytes& scalar, const Hash160Bytes& target) const {
    Point point;
    if (!compute(curve, scalar, point)) return false;
    Hash160Bytes actual;
    generate_binaddress_eth(point, actual.data());
    return actual == target;
}

bool verifyCpuPublicKey(Secp256K1& curve, Int& scalar, Point& target) {
    if (!valid_scalar(curve, scalar) || !target.z.IsOne()) return false;
    Point actual = curve.ComputePublicKey(&scalar);
    return actual.x.IsEqual(&target.x) && actual.y.IsEqual(&target.y);
}

bool verifyCpuTableCandidate(Secp256K1& curve, Int& scalar, bool compressed,
                             const SearchConfig& config, const CpuTargetTable& table) {
    if (!valid_scalar(curve, scalar) || table.count == 0 || table.entries == nullptr) return false;
    Point point = curve.ComputePublicKey(&scalar);
    unsigned char value[32];
    if (config.mode == MODE_XPOINT) {
        export32(point.x, value);
    } else if (config.mode == MODE_ADDRESS && config.crypto == CRYPTO_ETH) {
        generate_binaddress_eth(point, value);
    } else if (config.mode == MODE_ADDRESS || config.mode == MODE_RMD160 || config.mode == MODE_MINIKEYS) {
        hash160(point, compressed, value);
    } else {
        return false;
    }
    const auto* end = table.entries + table.count;
    const auto* entry = std::lower_bound(table.entries, table.entries + table.count, value,
        [](const address_value& entry, const unsigned char* candidate) {
            return std::memcmp(entry.value, candidate, sizeof(entry.value)) < 0;
        });
    return entry != end && std::memcmp(entry->value, value, sizeof(entry->value)) == 0;
}

} // namespace keyhunt::core
