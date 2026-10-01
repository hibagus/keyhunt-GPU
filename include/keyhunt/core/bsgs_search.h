#pragma once
#include "keyhunt/core/bsgs_table.h"
#include "keyhunt/core/xpoint_search.h"
#include <map>
#include <optional>

namespace keyhunt::core {
// Distinct from the preserved CPU loader's BsgsTargets aggregate: sharing
// that qualified name would violate the ODR and can select the wrong destructor.
// Finite SEC1 points, canonicalized to uncompressed form and sorted by full
// point. Compressed/uncompressed duplicates coalesce; opposite signs do not.
class BsgsPublicKeyTargets {
public:
    explicit BsgsPublicKeyTargets(std::vector<UncompressedPublicKey> points);
    static BsgsPublicKeyTargets load(const std::string& path);
    const std::vector<UncompressedPublicKey>& values() const { return points_; }
    const scheduler::Digest& digest() const { return digest_; }
private:
    std::vector<UncompressedPublicKey> points_;
    scheduler::Digest digest_{};
};
struct BsgsCandidate { uint64_t giant=0, baby=0; uint32_t target=0, reserved=0; };
struct BsgsMatch { UInt256 scalar; uint32_t target; };

// A bounded tile and contiguous subset of canonical targets. This is a BSGS
// plan, never a DirectXPointV1 work unit or proof of durable coverage. The owner
// must complete every target subset before crediting this interval once.
class BsgsBatch {
public:
    BsgsBatch(ScalarInterval interval,uint64_t m,uint32_t first,uint32_t count,
        scheduler::Digest target_digest,scheduler::Digest table_checksum);
    const ScalarInterval& interval() const { return interval_; }
    uint64_t m() const { return m_; }
    uint64_t giants() const { return giants_; }
    uint64_t last_babies() const { return last_babies_; }
    uint32_t first_target() const { return first_; }
    uint32_t target_count() const { return count_; }
    uint64_t steps() const { return giants_*count_; }
    const scheduler::Digest& target_digest() const { return target_digest_; }
    const scheduler::Digest& table_checksum() const { return table_checksum_; }
    UInt256 scalar_at(uint64_t giant,uint64_t baby) const;
private:
    ScalarInterval interval_;
    uint64_t m_,giants_,last_babies_;
    uint32_t first_,count_;
    scheduler::Digest target_digest_,table_checksum_;
};
// Choose the next exact tile without overflowing near n. Products are checked
// UInt256 integers; no absolute scalar, endpoint or m*i product is truncated.
// Reverse selects the highest remaining tile; arithmetic inside it is unchanged.
ScalarInterval bsgs_tile(const ScalarInterval& remaining,uint64_t m,uint64_t max_giants,bool reverse=false);
enum class BsgsTileOrder { Forward, Reverse, BothEnds, Dance };
BsgsTileOrder parse_bsgs_tile_order(const std::string& value);
const char* bsgs_tile_order_name(BsgsTileOrder order);

struct BsgsPlannedTile {
    ScalarInterval interval,work;
    bool starts_work,finishes_work;
};
// Process-local selection over sorted, disjoint missing intervals. Each next()
// consumes one planned tile; callers must finish all its targets before calling
// again. On failure/restart, reconstruct from durable coverage, never this queue.
// Both-ends alternates low/high; dance cycles low/high/fixed-midpoint-forward.
// Both advance per tile, even inside adaptive work units (see docs/C23_BSGS_DANCE.md).
class BsgsTilePlanner {
public:
    BsgsTilePlanner(const std::vector<ScalarInterval>& gaps,uint64_t m,uint64_t max_giants,BsgsTileOrder order);
    std::optional<BsgsPlannedTile> next(const UInt256& work_span);
private:
    struct Remaining { ScalarInterval interval; std::optional<ScalarInterval> work; };
    std::map<UInt256,Remaining> remaining_;
    uint64_t m_,giants_;
    BsgsTileOrder order_;
    unsigned phase_=0;
    std::optional<UInt256> pivot_;
};
std::vector<BsgsMatch> verify_bsgs(const BsgsBatch& batch,const BsgsPublicKeyTargets& targets,
    const XPointVerifier& verifier,std::vector<BsgsCandidate> candidates);
} // namespace keyhunt::core
