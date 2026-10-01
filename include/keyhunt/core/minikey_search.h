#pragma once
#include "keyhunt/core/hash160_search.h"
#include <map>

namespace keyhunt::core {
// Length is repeated in each canonical target so the existing immutable target
// binding also defines the ordinal space. Mixed lengths within a job are invalid.
using MinikeyTarget=std::array<uint8_t,22>;
enum class MinikeyOrder { Forward, Reverse, BothEnds };
MinikeyOrder parse_minikey_order(const std::string& value);
const char* minikey_order_name(MinikeyOrder order);

struct MinikeyPlannedBatch {
    scheduler::KernelBatch batch;
    bool starts_work,finishes_work;
};
// Select batches over canonical missing ordinal intervals. Planning reserves
// work but does not consume coverage. Overflow may re-plan with a smaller bound;
// only accept() consumes the last plan and advances the alternating phase.
class MinikeyBatchPlanner {
public:
    MinikeyBatchPlanner(scheduler::BlockGrid grid,UInt256 block,
        const std::vector<ScalarInterval>& gaps,scheduler::ExecutionIdentity identity,MinikeyOrder order);
    std::optional<MinikeyPlannedBatch> plan(const UInt256& work_span,uint64_t max_steps);
    void accept();
private:
    struct Remaining { ScalarInterval interval; std::optional<ScalarInterval> work; };
    scheduler::BlockGrid grid_;
    UInt256 block_;
    scheduler::ExecutionIdentity identity_;
    MinikeyOrder order_;
    bool high_=false;
    std::map<UInt256,Remaining> remaining_;
    std::optional<MinikeyPlannedBatch> pending_;
    UInt256 selected_;
};
UInt256 minikey_space_end(unsigned length);
std::string minikey_text(const UInt256& ordinal,unsigned length);
UInt256 minikey_ordinal(const std::string& text);
std::optional<UInt256> minikey_scalar(const std::string& text);
MinikeyTarget minikey_target(unsigned length,const Hash160Target& target);
class MinikeyTargets {
public:
    explicit MinikeyTargets(std::vector<MinikeyTarget> values);
    static MinikeyTargets load(const std::string&,unsigned length,Hash160Input=Hash160Input::BitcoinAddress,
                               Hash160Encoding=Hash160Encoding::Both);
    const std::vector<MinikeyTarget>& values()const{return values_;}
    const scheduler::Digest& digest()const{return digest_;}
    unsigned length()const{return values_[0][0];}
    uint8_t encodings()const{return encodings_;}
    unsigned max_matches_per_scalar()const{return encodings_==3?2:1;}
    void validate_interval(const ScalarInterval&)const;
private:
    std::vector<MinikeyTarget> values_;
    alignas(uint64_t) scheduler::Digest digest_{};
    uint8_t encodings_=0;
};
// Receipt coordinates are ordinals, not derived private scalars. Retaining an
// ordinal lets every verifier reconstruct the exact candidate without trusting GPU text.
std::vector<XPointMatch> verify_minikeys(const scheduler::KernelBatch&,const MinikeyTargets&,
    std::vector<XPointCandidate>,const XPointVerifier&);
} // namespace keyhunt::core
