#include "keyhunt/core/minikey_search.h"
#include "keyhunt/crypto/hash/sha256.h"
#include <algorithm>
#include <charconv>
#include <tuple>
#include <stdexcept>

namespace keyhunt::core {
namespace {
const std::string alphabet="123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
void check_length(unsigned length){if(length!=22&&length!=30)throw std::invalid_argument("minikey length must be 22 or 30");}
}
MinikeyOrder parse_minikey_order(const std::string& value){
    if(value=="forward")return MinikeyOrder::Forward;
    if(value=="reverse")return MinikeyOrder::Reverse;
    if(value=="both-ends")return MinikeyOrder::BothEnds;
    if(value=="dance")return MinikeyOrder::Dance;
    if(value=="random-window")return MinikeyOrder::RandomWindow;
    throw std::invalid_argument("ordinal-order must be forward, reverse, both-ends, dance or random-window");
}
const char* minikey_order_name(MinikeyOrder order){
    switch(order){
    case MinikeyOrder::Forward:return "forward";
    case MinikeyOrder::Reverse:return "reverse";
    case MinikeyOrder::BothEnds:return "both-ends";
    case MinikeyOrder::Dance:return "dance";
    case MinikeyOrder::RandomWindow:return "random-window";
    }
    throw std::invalid_argument("invalid minikey ordinal order");
}
MinikeyRandomWindow parse_minikey_random_window(const std::string& seed,const std::string& window){
    MinikeyRandomWindow result;result.seed=UInt256::from_hex(seed);
    const auto parsed=std::from_chars(window.data(),window.data()+window.size(),result.tiles);
    if(parsed.ec!=std::errc{} || parsed.ptr!=window.data()+window.size() || !result.tiles || result.tiles>256)
        throw std::invalid_argument("ordinal-window must be 1..256");
    return result;
}
void validate_minikey_random_window(MinikeyOrder order,const std::optional<MinikeyRandomWindow>& settings){
    (void)minikey_order_name(order);
    if(settings && order!=MinikeyOrder::RandomWindow)
        throw std::invalid_argument("ordinal-seed/ordinal-window require ordinal-order random-window");
    if(settings && (!settings->tiles || settings->tiles>256))throw std::invalid_argument("ordinal-window must be 1..256");
}
MinikeyBatchPlanner::MinikeyBatchPlanner(scheduler::BlockGrid grid,UInt256 block,
    const std::vector<ScalarInterval>& gaps,scheduler::ExecutionIdentity identity,MinikeyOrder order,std::optional<MinikeyRandomWindow> random)
    :grid_(std::move(grid)),block_(block),identity_(std::move(identity)),order_(order),random_(random.value_or(MinikeyRandomWindow{})){
    validate_minikey_random_window(order,random);
    if(!scheduler::is_minikeys(identity_.algorithm) || identity_.stride_mapping)
        throw std::invalid_argument("ordinal planner requires unmapped minikey work");
    const auto parent=grid_.block(block_);
    for(size_t i=0;i<gaps.size();++i){
        if(!parent.contains(gaps[i]) || (i && gaps[i-1].end()>gaps[i].begin()))
            throw std::invalid_argument("minikey gaps must be sorted, disjoint and inside the block");
    }
    if(order_==MinikeyOrder::Dance && !gaps.empty()){
        // Subtract before adding to avoid endpoint overflow. Fixing the pivot
        // once adds only one gap, even across an enormous ordinal domain.
        const auto low=gaps.front().begin();
        pivot_=low.add(gaps.back().end().subtract(low).divmod(UInt256(2)).first);
    }
    for(const auto& gap:gaps){
        if(pivot_ && gap.begin()<*pivot_ && *pivot_<gap.end()){
            remaining_.emplace(gap.begin(),Remaining{{gap.begin(),*pivot_},std::nullopt});
            remaining_.emplace(*pivot_,Remaining{{*pivot_,gap.end()},std::nullopt});
        }else remaining_.emplace(gap.begin(),Remaining{gap,std::nullopt});
    }
}
unsigned MinikeyBatchPlanner::random_below(unsigned bound){
    // Specify byte layout and rejection sampling instead of depending on a
    // standard-library shuffle. This matches the independent hashlib oracle.
    constexpr char domain[]="khminikey-window-v1";
    alignas(uint64_t) uint8_t input[sizeof(domain)+64]{},hash[32]{};
    std::copy_n(domain,sizeof(domain),input);
    const auto seed=random_.seed.bytes();std::copy(seed.begin(),seed.end(),input+sizeof(domain));
    unsigned mask=1;while(mask<bound-1)mask=(mask<<1)|1;
    for(unsigned attempt=0;attempt<1024;++attempt){
        const auto counter=random_counter_.bytes();
        std::copy(counter.begin(),counter.end(),input+sizeof(domain)+32);
        random_counter_=random_counter_.add(UInt256(1));
        sha256(input,sizeof(input),hash);
        const unsigned value=hash[0]&mask;if(value<bound)return value;
    }
    throw std::runtime_error("minikey shuffle sampler did not converge");
}
void MinikeyBatchPlanner::fill_window(const UInt256& work_span,uint64_t max_steps){
    window_.clear();window_next_=0;
    const auto width=UInt256(max_steps);
    const auto span=std::min(work_span,UInt256(UINT64_MAX));
    const auto per_work=std::max(UInt256(1),std::min(UInt256(random_.tiles),span.divmod(width).first)).to_uint64();
    std::map<UInt256,unsigned> totals,visited;
    // Freeze tile geometry and contiguous work ownership before shuffling.
    // Later overflow changes sub-batches, never this window's tile boundaries.
    while(window_.size()<random_.tiles && !remaining_.empty()){
        const auto first=remaining_.begin();const auto gap=first->second.interval;
        const auto count=std::min<uint64_t>(per_work,random_.tiles-window_.size());
        const ScalarInterval work(gap.begin(),gap.begin().add(std::min(gap.size(),width.multiply(UInt256(count)))));
        remaining_.erase(first);
        if(work.end()<gap.end())remaining_.emplace(work.end(),Remaining{{work.end(),gap.end()},std::nullopt});
        for(auto cursor=work.begin();cursor<work.end();){
            const auto end=cursor.add(std::min(width,work.end().subtract(cursor)));
            window_.push_back({{cursor,end},work,false,false});++totals[work.begin()];cursor=end;
        }
    }
    for(size_t i=window_.size();i>1;--i)std::swap(window_[i-1],window_[random_below(unsigned(i))]);
    for(auto& tile:window_){const auto count=++visited[tile.work.begin()];
        tile.starts_work=count==1;tile.finishes_work=count==totals.at(tile.work.begin());}
}
std::optional<MinikeyPlannedBatch> MinikeyBatchPlanner::plan(const UInt256& work_span,uint64_t max_steps){
    if(work_span.is_zero() || !max_steps)throw std::invalid_argument("zero minikey work or batch bound");
    if(order_==MinikeyOrder::RandomWindow){
        if(window_next_==window_.size())fill_window(work_span,max_steps);
        if(window_.empty())return std::nullopt;
        auto& tile=window_[window_next_];auto identity=identity_;
        identity.algorithm=scheduler::WorkAlgorithm::DirectMinikeysV1;
        const auto work=*scheduler::WorkUnit::plan(grid_,block_,tile.work.begin(),tile.work.size().to_uint64(),identity);
        const auto steps=std::min(UInt256(max_steps),tile.remaining.size()).to_uint64();
        const auto batch=*scheduler::KernelBatch::plan(work,tile.remaining.begin(),steps);
        pending_=MinikeyPlannedBatch{batch,tile.starts_work,tile.finishes_work&&batch.interval().end()==tile.remaining.end()};
        // Announce a reservation once, including when this attempt overflows.
        tile.starts_work=false;
        return pending_;
    }
    if(remaining_.empty())return std::nullopt;
    const bool reverse=order_==MinikeyOrder::Reverse ||
        ((order_==MinikeyOrder::BothEnds || order_==MinikeyOrder::Dance) && phase_==1);
    auto chosen=reverse?std::prev(remaining_.end()):remaining_.begin();
    if(order_==MinikeyOrder::Dance && phase_==2){
        // The pivot split ensures a middle lookup always selects an endpoint.
        chosen=remaining_.lower_bound(*pivot_);
        if(chosen==remaining_.end())chosen=remaining_.begin();
    }
    const bool starts_work=!chosen->second.work;
    if(starts_work){
        const auto gap=chosen->second.interval;
        const auto span=std::min({work_span,gap.size(),UInt256(UINT64_MAX)});
        const ScalarInterval work=reverse?ScalarInterval(gap.end().subtract(span),gap.end()):
            ScalarInterval(gap.begin(),gap.begin().add(span));
        remaining_.erase(chosen);
        if(work.size()!=gap.size()){
            const ScalarInterval free=reverse?ScalarInterval(gap.begin(),work.begin()):ScalarInterval(work.end(),gap.end());
            remaining_.emplace(free.begin(),Remaining{free,std::nullopt});
        }
        chosen=remaining_.emplace(work.begin(),Remaining{work,work}).first;
    }
    // Different fronts may meet in one reservation. Keep its original bounds for
    // accounting, but clip this batch to the still-unconsumed middle interval.
    const auto active=chosen->second.interval, reserved=*chosen->second.work;
    auto identity=identity_;
    identity.algorithm=reverse?scheduler::WorkAlgorithm::ReverseMinikeysV1:scheduler::WorkAlgorithm::DirectMinikeysV1;
    const auto work=*scheduler::WorkUnit::plan(grid_,block_,reverse?reserved.end():reserved.begin(),reserved.size().to_uint64(),identity);
    const auto steps=std::min(UInt256(max_steps),active.size()).to_uint64();
    const auto batch=*scheduler::KernelBatch::plan(work,reverse?active.end():active.begin(),steps);
    selected_=chosen->first;
    pending_=MinikeyPlannedBatch{batch,starts_work,batch.interval().size()==active.size()};
    return pending_;
}
void MinikeyBatchPlanner::accept(){
    if(!pending_)throw std::logic_error("no minikey batch to accept");
    if(order_==MinikeyOrder::RandomWindow){
        auto& tile=window_[window_next_];const auto end=pending_->batch.interval().end();
        if(end==tile.remaining.end())++window_next_;
        else tile.remaining=ScalarInterval(end,tile.remaining.end());
        pending_.reset();return;
    }
    const auto chosen=remaining_.find(selected_);
    const auto entry=chosen->second;
    const auto& batch=pending_->batch;
    remaining_.erase(chosen);
    if(!pending_->finishes_work){
        const ScalarInterval rest=batch.ordinal_reverse()?ScalarInterval(entry.interval.begin(),batch.interval().begin()):
            ScalarInterval(batch.interval().end(),entry.interval.end());
        remaining_.emplace(rest.begin(),Remaining{rest,entry.work});
    }
    if(order_==MinikeyOrder::BothEnds)phase_=(phase_+1)%2;
    else if(order_==MinikeyOrder::Dance)phase_=(phase_+1)%3;
    pending_.reset();
}
UInt256 minikey_space_end(unsigned length){
    check_length(length);UInt256 size(1);
    for(unsigned i=1;i<length;++i)size=size.multiply(UInt256(58));
    return size.add(UInt256(1));
}
std::string minikey_text(const UInt256& ordinal,unsigned length){
    if(ordinal.is_zero()||ordinal>=minikey_space_end(length))throw std::out_of_range("minikey ordinal outside candidate space");
    auto suffix=ordinal.subtract(UInt256(1));std::string text(length,'1');text[0]='S';
    for(unsigned i=length-1;i>0;--i){const auto digit=suffix.divmod(UInt256(58));suffix=digit.first;text[i]=alphabet[digit.second.to_uint64()];}
    return text;
}
UInt256 minikey_ordinal(const std::string& text){
    check_length(text.size());if(text[0]!='S')throw std::invalid_argument("minikey must begin with S");
    UInt256 ordinal;
    for(size_t i=1;i<text.size();++i){const auto digit=alphabet.find(text[i]);
        if(digit==std::string::npos)throw std::invalid_argument("invalid minikey Base58 character");
        ordinal=ordinal.multiply(UInt256(58)).add(UInt256(digit));}
    return ordinal.add(UInt256(1));
}
std::optional<UInt256> minikey_scalar(const std::string& text){
    minikey_ordinal(text); // Syntax is an error; an invalid check byte is ordinary rejected work.
    alignas(uint64_t) uint8_t message[32]{},hash[32];std::copy(text.begin(),text.end(),message);message[text.size()]='?';
    sha256(message,text.size()+1,hash);if(hash[0])return std::nullopt;
    sha256(message,text.size(),hash);UInt256::Bytes bytes{};std::copy_n(hash,32,bytes.begin());
    const auto scalar=UInt256::from_bytes(bytes);
    if(scalar.is_zero()||scalar>=scalar_order())return std::nullopt;
    return scalar;
}
MinikeyTarget minikey_target(unsigned length,const Hash160Target& hash){
    check_length(length);hash160_encoding_name(hash[0]);MinikeyTarget result{};result[0]=uint8_t(length);
    std::copy(hash.begin(),hash.end(),result.begin()+1);return result;
}
MinikeyTargets::MinikeyTargets(std::vector<MinikeyTarget> values):values_(std::move(values)){
    if(values_.empty()||values_.size()>1048576)throw std::invalid_argument("minikey target count must be in [1,1048576]");
    check_length(length());
    for(const auto& target:values_){
        if(target[0]!=length())throw std::invalid_argument("mixed minikey lengths in one job");
        hash160_encoding_name(target[1]);encodings_|=target[1];
    }
    std::sort(values_.begin(),values_.end());values_.erase(std::unique(values_.begin(),values_.end()),values_.end());
    std::vector<uint8_t> bytes{'m','i','n','i','k','e','y','s','-','v','1',0};
    for(const auto& target:values_)bytes.insert(bytes.end(),target.begin(),target.end());
    sha256(bytes.data(),bytes.size(),digest_.data());
}
MinikeyTargets MinikeyTargets::load(const std::string& path,unsigned length,Hash160Input input,Hash160Encoding encoding){
    check_length(length);const auto hashes=Hash160Targets::load(path,input,encoding);std::vector<MinikeyTarget> targets;
    for(const auto& hash:hashes.values())targets.push_back(minikey_target(length,hash));return MinikeyTargets(std::move(targets));
}
void MinikeyTargets::validate_interval(const ScalarInterval& interval)const{
    if(interval.end()>minikey_space_end(length()))throw std::out_of_range("range exceeds minikey ordinal space");
}
std::vector<XPointMatch> verify_minikeys(const scheduler::KernelBatch& batch,const MinikeyTargets& targets,
    std::vector<XPointCandidate> candidates,const XPointVerifier& verifier){
    if(!scheduler::is_minikeys(batch.work().identity().algorithm) ||
       batch.work().identity().target_digest!=targets.digest())throw std::invalid_argument("minikey identity does not match plan");
    targets.validate_interval(batch.interval());
    std::sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b){return std::tie(a.offset,a.target)<std::tie(b.offset,b.target);});
    std::vector<XPointMatch> matches;matches.reserve(candidates.size());uint64_t previous=0;uint32_t prior_target=0;
    UncompressedPublicKey pub{};
    for(const auto& candidate:candidates){
        if(candidate.offset>=batch.step_count()||candidate.target>=targets.values().size()||candidate.reserved)
            throw std::runtime_error("invalid GPU minikey candidate record");
        if(!matches.empty()&&previous==candidate.offset&&prior_target==candidate.target)throw std::runtime_error("duplicate GPU minikey relation");
        const auto ordinal=batch.ordinal_at(candidate.offset);
        if(matches.empty()||previous!=candidate.offset){
            const auto scalar=minikey_scalar(minikey_text(ordinal,targets.length()));
            if(!scalar)throw std::runtime_error("GPU minikey candidate failed validity check");
            pub=verifier.derive(*scalar);
        }
        const auto& target=targets.values()[candidate.target];const auto hash=hash160_target(pub,target[1]);
        if(!std::equal(hash.begin(),hash.end(),target.begin()+1))throw std::runtime_error("GPU minikey candidate failed CPU verification");
        matches.push_back({ordinal,candidate.target});previous=candidate.offset;prior_target=candidate.target;
    }
    return matches;
}
} // namespace keyhunt::core
