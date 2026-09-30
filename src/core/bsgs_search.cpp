#include "keyhunt/core/bsgs_search.h"
#include "keyhunt/crypto/hash/sha256.h"
#include "common/point.h"
#include <algorithm>
#include <fstream>
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
ScalarInterval bsgs_tile(const ScalarInterval& remaining,uint64_t m,uint64_t max_giants) {
    if (!m || !max_giants || max_giants>1048576) throw std::invalid_argument("invalid BSGS tile limit");
    const auto width=UInt256(m).multiply(UInt256(max_giants));
    return ScalarInterval(remaining.begin(),remaining.begin().add(std::min(width,remaining.size())));
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
