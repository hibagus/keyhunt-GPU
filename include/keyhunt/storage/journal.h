#pragma once
#include "keyhunt/core/xpoint_search.h"
#include <functional>
#include <stdexcept>
#include <memory>
#include <string>
#include <vector>

namespace keyhunt::coordination { class Repository; class Worker; }
namespace keyhunt::storage {
using core::UInt256;
using core::ScalarInterval;
using Digest=scheduler::Digest;
// Expected lease/control transitions are separate from a failed GPU executor.
// Existing callers can still handle these as invalid_argument.
class ExecutionBlocked : public std::invalid_argument {
public:
    enum class Reason { Fence, Expired, Revalidation, Paused };
    ExecutionBlocked(Reason why,const char* message):std::invalid_argument(message),reason(why){}
    Reason reason;
};
enum class Mode:uint8_t { XPoint=1, Bsgs=2, Hash160=3, Ethereum=4, Vanity=5, Minikeys=6 };
// Explicit switches prevent a future/invalid mode from silently using BSGS
// widths or identity strings in persisted and transported records.
const char* mode_name(Mode mode);
size_t target_width(Mode mode);
struct Manifest {
    Mode mode;
    ScalarInterval root;
    UInt256 block_width;
    Digest targets{},algorithm{};
};
struct Scope { std::string project; Digest job{}; };
struct Grant {
    Scope scope;
    UInt256 block;
    ScalarInterval interval;
    std::string owner;
    int64_t generation=0,expires=0;
    std::vector<uint8_t> epoch;
};
enum class Policy { Sequential, Random, RandomWindow, Manual };
struct Selection {
    Policy policy=Policy::Sequential;
    uint32_t count=1;
    std::optional<UInt256> block;
    UInt256 window=UInt256(4096);
};
struct BlockState {
    std::string state; // unexplored, in_progress, finished
    std::optional<Grant> assignment;
    bool started=false,expired=false;
    std::vector<ScalarInterval> covered,remaining;
};
struct StoredMatch { int64_t id; UInt256 block,scalar; uint32_t target; std::vector<uint8_t> target_bytes; };
class CheckpointRun;
namespace detail { struct Binding; class Database; }
struct Statistics {
    UInt256 blocks,unexplored,finished;
    uint64_t assignments=0,coverage_intervals=0,finished_runs=0,tree_nodes=0,requests=0,events=0;
    uint64_t database_bytes=0,wal_bytes=0,free_pages=0;
    bool quarantined=false;
};
// Trusted local repository API, one connection per host owner. Identity strings
// are NOT authentication: C15 must bind them to authenticated project membership.
// Raw C12 coverage is only for unbound/synthetic jobs. Bound searches accept
// matches and coverage exclusively through the CPU-verifying CheckpointRun.
class Journal {
public:
    using Clock=std::function<int64_t()>;
    explicit Journal(const std::string& state_directory={},Clock clock={});
    ~Journal();
    Journal(const Journal&)=delete;
    Journal& operator=(const Journal&)=delete;
    std::string state_directory() const;
    std::string create_project(const std::string& name);
    Scope create_job(const std::string& project,const Manifest& manifest,std::optional<Digest> seed={});
    Manifest manifest(const Scope& scope) const;
    std::vector<Grant> claim(const Scope& scope,const std::string& owner,const std::string& request,
        Selection selection={},int64_t lifetime_seconds=30*24*60*60);
    void start(const Grant& grant,const std::string& request);
    Grant renew(const Grant& grant,const std::string& request,int64_t lifetime_seconds=30*24*60*60);
    Grant recover(const Scope& scope,const UInt256& block,const std::string& owner,
        const std::string& request,bool previous_executor_stopped=false,int64_t lifetime_seconds=30*24*60*60);
    void return_unstarted(const Grant& grant,const std::string& request);
    void record_coverage(const Grant& grant,const std::vector<ScalarInterval>& intervals,const std::string& request);
    BlockState block(const Scope& scope,const UInt256& id) const;
    Statistics statistics(const Scope& scope) const;
    std::vector<StoredMatch> results(const Scope& scope,int64_t after=0,uint32_t limit=100) const;
    void check() const;
    void compact(); // SQLite checkpoint/VACUUM; does not erase retry receipts
    void backup(const std::string& destination_directory) const;
    static void restore(const std::string& snapshot_directory,const std::string& destination_directory);
private:
    friend class CheckpointRun;
    friend class coordination::Repository;
    friend class coordination::Worker;
    detail::Database& database() const;
    int64_t timestamp() const;
    Grant import_remote(const Grant&,const Manifest&,const detail::Binding&,const std::string& remote,
        const std::string& device,int64_t deadline,int64_t local_expiry,bool paused,const std::vector<ScalarInterval>& accepted);
    void bind_search(const Scope&,const detail::Binding&);
    int64_t begin_search(const Grant&);
    void validate_search(const Grant&,int64_t executor) const;
    void commit_search(const Grant&,int64_t executor,const std::vector<ScalarInterval>&,
        const std::vector<core::XPointMatch>&,const std::string& request);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace keyhunt::storage
