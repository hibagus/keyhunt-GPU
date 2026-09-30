#pragma once
#include "sqlite.h"
#include "keyhunt/storage/journal.h"

namespace keyhunt::storage::detail {
// Sparse binary range tree over [0, block_count). Missing nodes mean wholly
// free; a zero count collapses a wholly occupied subtree. Partial nodes retain
// exact free counts, allowing unbiased rank selection without scanning rows or
// repeatedly guessing IDs when almost all blocks are occupied.
class FreeTree {
public:
    FreeTree(Database& db,const Scope& scope,UInt256 count):db_(db),scope_(scope),count_(count){}
    UInt256 free() const { return value(UInt256(),count_,0); }
    UInt256 select(UInt256 rank) const;
    UInt256 count(UInt256 begin,UInt256 end) const;
    void occupy(UInt256 id){change(UInt256(),count_,0,id,false);}
    void release(UInt256 id){change(UInt256(),count_,0,id,true);}
private:
    Database& db_; const Scope& scope_; UInt256 count_;
    UInt256 value(UInt256 lo,UInt256 hi,unsigned depth) const;
    void store(UInt256 lo,unsigned depth,UInt256 free);
    void erase(UInt256 lo,UInt256 hi,unsigned depth);
    void change(UInt256 lo,UInt256 hi,unsigned depth,UInt256 id,bool available);
    UInt256 count(UInt256 lo,UInt256 hi,unsigned depth,UInt256 begin,UInt256 end) const;
};
} // namespace keyhunt::storage::detail
