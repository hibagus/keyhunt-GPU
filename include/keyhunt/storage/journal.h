#pragma once
#include "keyhunt/scheduler/work_unit.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace keyhunt::storage {
using core::UInt256;
using core::ScalarInterval;
using Digest=scheduler::Digest;
enum class Mode:uint8_t { XPoint=1, Bsgs=2 };
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
struct Statistics {
    UInt256 blocks,unexplored,finished;
    uint64_t assignments=0,coverage_intervals=0,finished_runs=0,tree_nodes=0,requests=0,events=0;
    uint64_t database_bytes=0,wal_bytes=0,free_pages=0;
    bool quarantined=false;
};
// Trusted local repository API, one connection per host owner. Identity strings
// are NOT authentication: C15 must bind them to authenticated project membership.
// C12 has no GPU checkpoint integration; record_coverage is reserved for C13's
// verified commit coordinator and synthetic tests, never exposed by the CLI.
class Journal {
public:
    using Clock=std::function<int64_t()>;
    explicit Journal(const std::string& state_directory={},Clock clock={});
    ~Journal();
    Journal(const Journal&)=delete;
    Journal& operator=(const Journal&)=delete;
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
    void check() const;
    void compact(); // SQLite checkpoint/VACUUM; does not erase retry receipts
    void backup(const std::string& destination_directory) const;
    static void restore(const std::string& snapshot_directory,const std::string& destination_directory);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace keyhunt::storage
