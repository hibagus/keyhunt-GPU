#include "keyhunt/core/bsgs_search.h"
#include "keyhunt/crypto/hash/sha256.h"
#include "common/point.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <tuple>

namespace keyhunt::core {
namespace {
constexpr size_t max_targets=65536;
gpu::Affine decode(const uint8_t* bytes,size_t length) {
    gpu::Affine point; point.infinity=false;
    if (!gpu::from_bytes_checked(point.x,bytes+1)) throw std::invalid_argument("noncanonical BSGS target X");
    if (length==65 && bytes[0]==4) {
        if (!gpu::from_bytes_checked(point.y,bytes+33)) throw std::invalid_argument("noncanonical BSGS target Y");
    } else if (length==33 && (bytes[0]==2 || bytes[0]==3)) {
        // p = 3 mod 4: y = (x^3+7)^((p+1)/4). Check the square afterwards;
        // nonresidues and noncanonical SEC1 encodings are rejected, never reduced.
        gpu::Field rhs,seven{{7}},square;
        gpu::square(rhs,point.x); gpu::mul(rhs,rhs,point.x); gpu::add(rhs,rhs,seven);
        const auto exponent=UInt256::from_hex("3fffffffffffffffffffffffffffffffffffffffffffffffffffffffbfffff0c").bytes();
        point.y=gpu::one();
        for (uint8_t byte : exponent) for (int bit=7;bit>=0;--bit) {
            gpu::square(point.y,point.y);
            if ((byte>>bit)&1) gpu::mul(point.y,point.y,rhs);
        }
        gpu::square(square,point.y);
        if (!gpu::equal(square,rhs)) throw std::invalid_argument("BSGS target X has no curve point");
        if ((point.y.limb[0]&1)!=(bytes[0]&1)) gpu::neg(point.y,point.y);
    } else throw std::invalid_argument("BSGS target must be finite compressed or uncompressed SEC1");
    if (!gpu::on_curve(point)) throw std::invalid_argument("BSGS target is off curve");
    return point;
}
UncompressedPublicKey encode(const gpu::Affine& point) {
    UncompressedPublicKey bytes{}; bytes[0]=4;
    gpu::to_bytes(bytes.data()+1,point.x); gpu::to_bytes(bytes.data()+33,point.y);
    return bytes;
}
unsigned digit(char c) {
    if (c>='0' && c<='9') return unsigned(c-'0');
    if (c>='a' && c<='f') return unsigned(c-'a'+10);
    if (c>='A' && c<='F') return unsigned(c-'A'+10);
    throw std::invalid_argument("nonhexadecimal BSGS target");
}
}
BsgsPublicKeyTargets::BsgsPublicKeyTargets(std::vector<UncompressedPublicKey> points) : points_(std::move(points)) {
    if (points_.empty() || points_.size()>max_targets) throw std::invalid_argument("BSGS targets must be in [1,65536]");
    for (const auto& point : points_) (void)decode(point.data(),point.size());
    std::sort(points_.begin(),points_.end());
    points_.erase(std::unique(points_.begin(),points_.end()),points_.end());
    std::vector<uint8_t> encoded{'b','s','g','s','-','t','a','r','g','e','t','s','-','v','1',0};
    for (const auto& point : points_) encoded.insert(encoded.end(),point.begin(),point.end());
    alignas(uint64_t) uint8_t digest[32];
    sha256(encoded.data(),encoded.size(),digest);
    std::copy_n(digest,32,digest_.begin());
}
BsgsPublicKeyTargets BsgsPublicKeyTargets::load(const std::string& path) {
    std::ifstream input(path,std::ios::binary);
    if (!input) throw std::runtime_error("cannot open BSGS targets: "+path);
    std::vector<UncompressedPublicKey> points;
    char line[133]; // fixed bound; embedded NUL and overlong lines remain errors
    while (input.getline(line,sizeof(line))) {
        std::string text(line,size_t(input.gcount())-(input.eof()?0:1));
        if (!text.empty() && text.back()=='\r') text.pop_back();
        if (text.empty()) continue;
        if (text.size()!=66 && text.size()!=130) throw std::invalid_argument("BSGS target line must have 66 or 130 hex digits");
        uint8_t bytes[65]{};
        for (size_t i=0;i<text.size()/2;++i) bytes[i]=uint8_t(16*digit(text[2*i])+digit(text[2*i+1]));
        if (points.size()==max_targets) throw std::invalid_argument("too many BSGS target lines");
        points.push_back(encode(decode(bytes,text.size()/2)));
    }
    if (input.bad() || !input.eof()) throw std::runtime_error("failed to read BSGS targets or line too long");
    return BsgsPublicKeyTargets(std::move(points));
}
BsgsBatch::BsgsBatch(ScalarInterval interval,uint64_t m,uint32_t first,uint32_t count,
    scheduler::Digest targets,scheduler::Digest table) : interval_(interval),m_(m),first_(first),count_(count),
    target_digest_(targets),table_checksum_(table) {
    if (!m || !count || count>64 || first>=max_targets || count>max_targets-first)
        throw std::invalid_argument("invalid BSGS table size or target subset");
    const auto qr=interval.size().divmod(UInt256(m));
    auto giants=qr.first;
    if (!qr.second.is_zero()) giants=giants.add(UInt256(1));
    giants_=giants.to_uint64();
    if (!giants_ || giants_>1048576/count) throw std::invalid_argument("BSGS batch exceeds 1048576 target giant steps");
    last_babies_=qr.second.is_zero()?m:qr.second.to_uint64();
}
UInt256 BsgsBatch::scalar_at(uint64_t giant,uint64_t baby) const {
    if (giant>=giants_ || baby>=m_ || (giant==giants_-1 && baby>=last_babies_))
        throw std::invalid_argument("BSGS candidate outside tile");
    const auto scalar=interval_.begin().add(UInt256(giant).multiply(UInt256(m_))).add(UInt256(baby));
    if (!interval_.contains(scalar)) throw std::logic_error("BSGS reconstruction escaped interval");
    return scalar;
}
ScalarInterval bsgs_tile(const ScalarInterval& remaining,uint64_t m,uint64_t max_giants,bool reverse) {
    if (!m || !max_giants || max_giants>1048576) throw std::invalid_argument("invalid BSGS tile limit");
    const auto width=UInt256(m).multiply(UInt256(max_giants));
    const auto span=std::min(width,remaining.size());
    return reverse?ScalarInterval(remaining.end().subtract(span),remaining.end()):
        ScalarInterval(remaining.begin(),remaining.begin().add(span));
}
BsgsTileOrder parse_bsgs_tile_order(const std::string& value) {
    if(value=="forward")return BsgsTileOrder::Forward;
    if(value=="reverse")return BsgsTileOrder::Reverse;
    if(value=="both-ends")return BsgsTileOrder::BothEnds;
    if(value=="dance")return BsgsTileOrder::Dance;
    if(value=="random-window")return BsgsTileOrder::RandomWindow;
    throw std::invalid_argument("tile-order must be forward, reverse, both-ends, dance or random-window");
}
const char* bsgs_tile_order_name(BsgsTileOrder order) {
    switch(order){
        case BsgsTileOrder::Forward:return "forward";
        case BsgsTileOrder::Reverse:return "reverse";
        case BsgsTileOrder::BothEnds:return "both-ends";
        case BsgsTileOrder::Dance:return "dance";
        case BsgsTileOrder::RandomWindow:return "random-window";
    }
    throw std::invalid_argument("invalid BSGS tile order");
}
BsgsRandomWindow parse_bsgs_random_window(const std::string& seed,const std::string& window){
    BsgsRandomWindow result;result.seed=UInt256::from_hex(seed);
    const auto parsed=std::from_chars(window.data(),window.data()+window.size(),result.tiles);
    if(parsed.ec!=std::errc{} || parsed.ptr!=window.data()+window.size() || !result.tiles || result.tiles>256)
        throw std::invalid_argument("tile-window must be 1..256");
    return result;
}
void validate_bsgs_random_window(BsgsTileOrder order,const std::optional<BsgsRandomWindow>& settings){
    (void)bsgs_tile_order_name(order);
    if(settings && order!=BsgsTileOrder::RandomWindow)
        throw std::invalid_argument("tile-seed/tile-window require tile-order random-window");
    if(settings && (!settings->tiles || settings->tiles>256))throw std::invalid_argument("tile-window must be 1..256");
}
BsgsTilePlanner::BsgsTilePlanner(const std::vector<ScalarInterval>& gaps,uint64_t m,uint64_t max_giants,BsgsTileOrder order,std::optional<BsgsRandomWindow> random)
    :m_(m),giants_(max_giants),order_(order),random_(random.value_or(BsgsRandomWindow{})) {
    validate_bsgs_random_window(order,random);
    if(!m || !max_giants || max_giants>1048576)throw std::invalid_argument("invalid BSGS tile limit");
    for(size_t i=1;i<gaps.size();++i)
        if(gaps[i-1].end()>gaps[i].begin())throw std::invalid_argument("BSGS gaps must be sorted and disjoint");
    if(order==BsgsTileOrder::Dance && !gaps.empty()){
        // Subtract before adding: endpoints near n must not overflow. A fixed
        // pivot adds only one gap; repeatedly choosing a new middle would
        // fragment huge searches in proportion to the number of tiles.
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
unsigned BsgsTilePlanner::random_below(unsigned bound){
    // The SHA stream and byte mask are specified explicitly so CPU/GPU owners
    // and independent oracles reproduce the same shuffle on every platform.
    constexpr char domain[]="khbsgs-window-v1";
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
    throw std::runtime_error("BSGS shuffle sampler did not converge");
}
void BsgsTilePlanner::fill_window(const UInt256& work_span){
    window_.clear();window_next_=0;
    const auto tile_width=UInt256(m_).multiply(UInt256(giants_));
    const auto work_tiles=std::max(UInt256(1),std::min(UInt256(random_.tiles),work_span.divmod(tile_width).first)).to_uint64();
    std::map<UInt256,unsigned> totals,visited;
    // Freeze accounting partitions for this window before shuffling. This
    // allows fixed one-tile units and adaptive multi-tile units to share the
    // same random policy without accumulating unbounded live reservations.
    while(window_.size()<random_.tiles && !remaining_.empty()){
        const auto first=remaining_.begin();const auto gap=first->second.interval;
        const auto count=std::min<uint64_t>(work_tiles,random_.tiles-window_.size());
        const auto span=std::min(gap.size(),tile_width.multiply(UInt256(count)));
        const ScalarInterval work(gap.begin(),gap.begin().add(span));
        remaining_.erase(first);
        if(work.end()<gap.end())remaining_.emplace(work.end(),Remaining{{work.end(),gap.end()},std::nullopt});
        for(auto cursor=work.begin();cursor<work.end();){
            const auto tile=bsgs_tile({cursor,work.end()},m_,giants_);
            window_.push_back({tile,work,false,false});++totals[work.begin()];cursor=tile.end();
        }
    }
    for(size_t i=window_.size();i>1;--i)std::swap(window_[i-1],window_[random_below(unsigned(i))]);
    // First/last refer to shuffled submission order, not scalar coordinates.
    // Callers finish every target subgroup before requesting the next tile.
    for(auto& tile:window_){const auto count=++visited[tile.work.begin()];
        tile.starts_work=count==1;tile.finishes_work=count==totals.at(tile.work.begin());}
}
std::optional<BsgsPlannedTile> BsgsTilePlanner::next(const UInt256& work_span) {
    if(work_span.is_zero())throw std::invalid_argument("zero BSGS work span");
    if(order_==BsgsTileOrder::RandomWindow){
        if(window_next_==window_.size())fill_window(work_span);
        if(window_.empty())return std::nullopt;
        return window_[window_next_++];
    }
    if(remaining_.empty())return std::nullopt;
    const bool high=order_==BsgsTileOrder::Reverse ||
        ((order_==BsgsTileOrder::BothEnds || order_==BsgsTileOrder::Dance) && phase_==1);
    auto chosen=high?std::prev(remaining_.end()):remaining_.begin();
    if(order_==BsgsTileOrder::Dance && phase_==2){
        chosen=remaining_.lower_bound(*pivot_);
        if(chosen==remaining_.end())chosen=remaining_.begin();
    }
    // Every front consumes an interval endpoint. Reservations never overlap;
    // fronts that meet share the existing owner, including a short final tile.
    const auto entry=chosen->second;
    const bool starts_work=!entry.work;
    const auto span=std::min(work_span,entry.interval.size());
    const auto work=entry.work.value_or(high?
        ScalarInterval(entry.interval.end().subtract(span),entry.interval.end()):
        ScalarInterval(entry.interval.begin(),entry.interval.begin().add(span)));
    const auto active=starts_work?work:entry.interval;
    const auto tile=bsgs_tile(active,m_,giants_,high);
    const BsgsPlannedTile result{tile,work,starts_work,tile.size()==active.size()};
    remaining_.erase(chosen);
    if(starts_work && work.size()!=entry.interval.size()){
        const ScalarInterval free=high?ScalarInterval(entry.interval.begin(),work.begin()):
            ScalarInterval(work.end(),entry.interval.end());
        remaining_.emplace(free.begin(),Remaining{free,std::nullopt});
    }
    if(!result.finishes_work){
        const ScalarInterval rest=high?ScalarInterval(active.begin(),tile.begin()):
            ScalarInterval(tile.end(),active.end());
        remaining_.emplace(rest.begin(),Remaining{rest,work});
    }
    if(order_==BsgsTileOrder::BothEnds)phase_=(phase_+1)%2;
    if(order_==BsgsTileOrder::Dance)phase_=(phase_+1)%3;
    return result;
}
std::vector<BsgsMatch> verify_bsgs(const BsgsBatch& batch,const BsgsPublicKeyTargets& targets,
    const XPointVerifier& verifier,std::vector<BsgsCandidate> candidates) {
    if (batch.target_digest()!=targets.digest() || batch.first_target()+batch.target_count()>targets.values().size())
        throw std::invalid_argument("BSGS target identity mismatch");
    std::sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b) {
        return std::tie(a.target,a.giant,a.baby)<std::tie(b.target,b.giant,b.baby);
    });
    std::vector<BsgsMatch> matches;
    for (const auto& c : candidates) {
        if (c.reserved || c.target<batch.first_target() || c.target>=batch.first_target()+batch.target_count())
            throw std::runtime_error("invalid BSGS candidate record");
        const auto scalar=batch.scalar_at(c.giant,c.baby);
        // A finite full point has exactly one scalar in [1,n). Thus two records
        // for the same canonical target in this interval indicate corruption.
        if (!matches.empty() && matches.back().target==c.target) throw std::runtime_error("duplicate BSGS target candidate");
        if (verifier.derive(scalar)!=targets.values()[c.target]) throw std::runtime_error("BSGS candidate failed CPU verification");
        matches.push_back({scalar,c.target});
    }
    std::sort(matches.begin(),matches.end(),[](const auto& a,const auto& b){return a.scalar<b.scalar;});
    return matches;
}
} // namespace keyhunt::core
