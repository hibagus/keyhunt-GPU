#include "keyhunt/core/xpoint_search.h"
#include "keyhunt/crypto/secp256k1/SECP256k1.h"
#include "keyhunt/crypto/hash/sha256.h"
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace keyhunt::core {
namespace {
constexpr size_t max_targets = 1048576;
int digit(char c) {
    if (c >= '0' && c <= '9') return c-'0';
    if (c >= 'a' && c <= 'f') return c-'a'+10;
    if (c >= 'A' && c <= 'F') return c-'A'+10;
    throw std::invalid_argument("xpoint targets must contain exactly 64 hexadecimal digits per nonempty line");
}
}
XPointTargets::XPointTargets(std::vector<XPointBytes> values) : values_(std::move(values)) {
    if (values_.empty() || values_.size() > max_targets)
        throw std::invalid_argument("xpoint target count must be in [1, 1048576]");
    const auto prime = UInt256::from_hex("fffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2f").bytes();
    for (const auto& value : values_)
        if (value >= prime) throw std::invalid_argument("xpoint target must be smaller than the field prime");
    std::sort(values_.begin(), values_.end());
    values_.erase(std::unique(values_.begin(), values_.end()), values_.end());
    // The format is a nonempty concatenation of fixed-width big-endian values.
    // Include a domain/version tag so later target formats cannot collide by encoding.
    std::vector<uint8_t> encoded{'x','p','o','i','n','t','-','v','1',0};
    encoded.reserve(encoded.size()+32*values_.size());
    for (const auto& value : values_) encoded.insert(encoded.end(),value.begin(),value.end());
    sha256(encoded.data(),encoded.size(),digest_.data());
}
XPointTargets XPointTargets::load(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open xpoint targets: " + path);
    std::vector<XPointBytes> values;
    // A fixed line buffer also bounds memory for a malformed enormous line.
    char line[67];
    while (input.getline(line,sizeof(line))) {
        // gcount includes the delimiter when present; preserve embedded NULs
        // so they are rejected instead of silently truncating a valid-looking line.
        const size_t length = size_t(input.gcount())-(input.eof() ? 0 : 1);
        std::string text(line,length);
        if (!text.empty() && text.back() == '\r') text.pop_back();
        if (text.empty()) continue;
        if (text.size() != 64) throw std::invalid_argument("xpoint target line must have 64 hexadecimal digits");
        XPointBytes value{};
        for (size_t i=0;i<32;++i) value[i] = uint8_t(16*digit(text[2*i])+digit(text[2*i+1]));
        if (values.size() == max_targets) throw std::invalid_argument("too many xpoint target lines");
        values.push_back(value);
    }
    if (input.bad() || !input.eof()) throw std::runtime_error("failed to read xpoint target file or line too long");
    return XPointTargets(std::move(values));
}
XPointVerifier::XPointVerifier() : curve_(std::make_unique<Secp256K1>()) { curve_->Init(); }
XPointVerifier::~XPointVerifier() = default;
UncompressedPublicKey XPointVerifier::derive(const UInt256& scalar) const {
    UncompressedPublicKey result{};
    if (!CpuResultVerifier(*curve_).derive(scalar.bytes(),result))
        throw std::invalid_argument("invalid xpoint scalar");
    return result;
}
std::vector<XPointMatch> XPointVerifier::verify(const scheduler::KernelBatch& batch,
    const XPointTargets& targets, std::vector<XPointCandidate> candidates) const {
    // The scalar planner now supports multiple search families. A matching
    // digest alone must not let an xpoint verifier accept another mode's label.
    if (batch.work().identity().algorithm != scheduler::WorkAlgorithm::DirectXPointV1 ||
        batch.work().identity().target_digest != targets.digest())
        throw std::invalid_argument("xpoint target digest does not match the plan");
    std::sort(candidates.begin(), candidates.end(), [](const auto& a,const auto& b){return a.offset < b.offset;});
    std::vector<XPointMatch> matches;
    matches.reserve(candidates.size());
    const CpuResultVerifier verifier(*curve_);
    uint64_t previous = 0;
    for (const auto& candidate : candidates) {
        if (candidate.offset >= batch.step_count() || candidate.target >= targets.values().size() || candidate.reserved)
            throw std::runtime_error("invalid HIP xpoint candidate record");
        if (!matches.empty() && previous == candidate.offset)
            throw std::runtime_error("duplicate HIP xpoint candidate offset");
        const auto scalar = batch.scalar_at(candidate.offset);
        if (!verifier.matches_xpoint(scalar.bytes(),targets.values()[candidate.target]))
            throw std::runtime_error("HIP xpoint candidate failed CPU verification");
        matches.push_back({scalar,candidate.target});
        previous = candidate.offset;
    }
    return matches;
}
} // namespace keyhunt::core
