#include "free_tree.h"
#include <algorithm>
#include <stdexcept>
namespace keyhunt::storage::detail {
namespace {
Bytes bytes(const Digest& digest){return Bytes(digest.begin(),digest.end());}
UInt256 midpoint(UInt256 lo,UInt256 hi){return lo.add(hi.subtract(lo).divmod(UInt256(2)).first);}
}
UInt256 FreeTree::value(UInt256 lo,UInt256 hi,unsigned depth)const{
    Statement s(db_.handle(),"SELECT free FROM free_nodes WHERE project=? AND job=? AND lo=? AND depth=?");
    s.bind(1,scope_.project);s.bind(2,bytes(scope_.job));s.bind(3,lo);s.bind(4,int64_t(depth));
    auto span=hi.subtract(lo);if(!s.step())return span;
    const auto result=s.wide(0);if(result>span)throw std::runtime_error("invalid free-tree count");return result;
}
void FreeTree::store(UInt256 lo,unsigned depth,UInt256 free){
    Statement s(db_.handle(),"INSERT INTO free_nodes VALUES(?,?,?,?,?) ON CONFLICT(project,job,lo,depth) DO UPDATE SET free=excluded.free");
    s.bind(1,scope_.project);s.bind(2,bytes(scope_.job));s.bind(3,lo);s.bind(4,int64_t(depth));s.bind(5,free);s.step();
}
void FreeTree::erase(UInt256 lo,UInt256 hi,unsigned depth){
    Statement s(db_.handle(),"DELETE FROM free_nodes WHERE project=? AND job=? AND lo>=? AND lo<? AND depth>=?");
    s.bind(1,scope_.project);s.bind(2,bytes(scope_.job));s.bind(3,lo);s.bind(4,hi);s.bind(5,int64_t(depth));s.step();
}
void FreeTree::change(UInt256 lo,UInt256 hi,unsigned depth,UInt256 id,bool available){
    if(id<lo || id>=hi)throw std::out_of_range("block outside free tree");
    const auto span=hi.subtract(lo),old=value(lo,hi,depth);
    if(span==UInt256(1)){
        if(old==(available?UInt256(1):UInt256()))throw std::runtime_error("free-tree transition conflicts with ownership");
        if(available)erase(lo,hi,depth);else store(lo,depth,UInt256());return;
    }
    const auto mid=midpoint(lo,hi);
    // Releasing from a collapsed occupied subtree must explicitly preserve its
    // occupied sibling; absent nodes ordinarily mean free, not occupied.
    if(old.is_zero()){
        if(!available)throw std::runtime_error("block already occupied");
        store(lo,depth+1,UInt256());store(mid,depth+1,UInt256());
    }
    if(id<mid)change(lo,mid,depth+1,id,available);else change(mid,hi,depth+1,id,available);
    const auto now=available?old.add(UInt256(1)):old.subtract(UInt256(1));
    if(now==span)erase(lo,hi,depth);
    else if(now.is_zero()){erase(lo,hi,depth);store(lo,depth,now);}
    else store(lo,depth,now);
}
UInt256 FreeTree::select(UInt256 rank)const{
    UInt256 lo,hi=count_;unsigned depth=0;
    while(true){
        const auto n=value(lo,hi,depth),span=hi.subtract(lo);
        if(rank>=n)throw std::out_of_range("rank outside unexplored blocks");
        if(n==span)return lo.add(rank); // an untouched enormous range needs no rows
        const auto mid=midpoint(lo,hi),left=value(lo,mid,depth+1);
        if(rank<left)hi=mid;else{lo=mid;rank=rank.subtract(left);}++depth;
    }
}
UInt256 FreeTree::count(UInt256 begin,UInt256 end)const{
    if(end<begin || end>count_)throw std::out_of_range("window outside free tree");
    return count(UInt256(),count_,0,begin,end);
}
UInt256 FreeTree::count(UInt256 lo,UInt256 hi,unsigned depth,UInt256 begin,UInt256 end)const{
    if(begin>=hi || end<=lo || begin==end)return UInt256();
    auto n=value(lo,hi,depth);if(n.is_zero())return n;
    if(begin<=lo && end>=hi)return n;
    if(n==hi.subtract(lo))return std::min(end,hi).subtract(std::max(begin,lo));
    const auto mid=midpoint(lo,hi);
    return count(lo,mid,depth+1,begin,end).add(count(mid,hi,depth+1,begin,end));
}
} // namespace keyhunt::storage::detail
