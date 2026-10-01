#pragma once
#include "sqlite.h"
#include "keyhunt/storage/journal.h"
#include "keyhunt/core/bsgs_search.h"
#include "keyhunt/core/hash160_search.h"

namespace keyhunt::storage::detail {
// Versioned encodings are independent of native struct layout and endianness.
struct Binding {
    Mode mode;
    Bytes configuration,targets;
    Digest target_digest{},algorithm_digest{},table_checksum{};
    uint64_t m=0;
    size_t count() const { return targets.size()/target_width(mode); }
    void verify(const core::XPointVerifier& verifier,const UInt256& scalar,uint32_t target) const;
};
Binding binding(const core::XPointTargets& targets);
Binding binding(const core::Hash160Targets& targets);
Binding binding(const core::BsgsPublicKeyTargets& targets,const bsgs::Table& table);
Binding decode_binding(const Manifest& manifest,const Bytes& configuration,const Bytes& targets);
struct CheckpointData {
    UInt256 block;
    uint64_t generation=0,executor=0;
    Bytes epoch;
    std::vector<ScalarInterval> coverage;
    std::vector<core::XPointMatch> matches;
};
Bytes encode_checkpoint(const CheckpointData& data);
CheckpointData decode_checkpoint(const Bytes& data);
std::vector<ScalarInterval> merged(std::vector<ScalarInterval> intervals);
} // namespace keyhunt::storage::detail
