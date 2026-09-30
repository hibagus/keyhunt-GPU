#pragma once
#include "keyhunt/core/result_verifier.h"
#include "keyhunt/scheduler/work_unit.h"
#include <memory>
#include <string>
#include <vector>

namespace keyhunt::core {
// Exact, canonical 32-byte X coordinates. Duplicates are one target: both k and
// n-k still produce distinct scalar matches. The digest binds a submitted plan
// to the immutable sorted target set actually uploaded by its executor.
class XPointTargets {
public:
    explicit XPointTargets(std::vector<XPointBytes> values);
    static XPointTargets load(const std::string& path);
    const std::vector<XPointBytes>& values() const { return values_; }
    const scheduler::Digest& digest() const { return digest_; }
private:
    std::vector<XPointBytes> values_;
    scheduler::Digest digest_{};
};
struct XPointCandidate { uint64_t offset = 0; uint32_t target = 0, reserved = 0; };
struct XPointMatch { UInt256 scalar; uint32_t target; };

// CPU curve types and their x86 headers stay out of HIP translation units.
// Initialize once per search owner, before worker threads; the legacy curve's
// initialization sets process-wide field parameters. Calls use local temporaries.
class XPointVerifier {
public:
    XPointVerifier();
    ~XPointVerifier();
    UncompressedPublicKey derive(const UInt256& scalar) const;
    std::vector<XPointMatch> verify(const scheduler::KernelBatch& batch,
        const XPointTargets& targets, std::vector<XPointCandidate> candidates) const;
private:
    std::unique_ptr<Secp256K1> curve_;
};
} // namespace keyhunt::core
