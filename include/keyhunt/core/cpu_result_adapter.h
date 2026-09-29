#pragma once

class Int;
class Point;
class Secp256K1;

namespace keyhunt::core {
class CpuTargetTable;
struct SearchConfig;

// Adapter for the existing CPU engine. Checks the entire Int before converting
// to 32 bytes, so negative and over-wide inputs cannot be truncated into a key.
bool verifyCpuPublicKey(Secp256K1& curve, Int& scalar, Point& affine_target);

// Preserves the current CPU table representation: hash160/ETH are exact 20-byte
// values, but xpoint entries are only 20-byte prefixes. Future GPU candidates must
// use CpuResultVerifier::matches_xpoint with a full XPointBytes target instead.
// The table must have been sorted by the CPU loader's caller.
bool verifyCpuTableCandidate(Secp256K1& curve, Int& scalar, bool compressed,
                             const SearchConfig& config, const CpuTargetTable& table);

// Shared CPU address derivation, also used by the existing batched search loops.
void generate_binaddress_eth(Point& publickey, unsigned char* dst_address);

} // namespace keyhunt::core
