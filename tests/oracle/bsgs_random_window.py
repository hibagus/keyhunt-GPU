"""Independent integer/hashlib model for the specified BSGS window shuffle."""
import hashlib

class RandomWindow:
    def __init__(self,gaps,m,giants,seed=0,window=64):
        if not 0<m<2**64 or not 1<=giants<=1048576 or not 1<=window<=256 or not 0<=seed<2**256:
            raise ValueError('invalid geometry/seed')
        self.gaps=list(gaps);self.width=m*giants;self.seed=seed;self.window=window
        self.gap=0;self.cursor=gaps[0][0] if gaps else 0;self.pending=[]
        self.counter=0;self.rejections=0;self.windows=0
    def below(self,bound):
        mask=(1<<(bound-1).bit_length())-1
        for _ in range(1024):
            digest=hashlib.sha256(b'khbsgs-window-v1\0'+self.seed.to_bytes(32,'big')+self.counter.to_bytes(32,'big')).digest()
            self.counter+=1;value=digest[0]&mask
            if value<bound:return value
            self.rejections+=1
        raise ValueError('sampler exhausted')
    def next(self,span):
        if not span:raise ValueError('zero work')
        if not self.pending:
            # First form the fixed geometric tiles, independently of ownership.
            tiles=[]
            for _ in range(self.window):
                if self.gap==len(self.gaps):break
                lo=self.cursor;hi=min(lo+self.width,self.gaps[self.gap][1])
                tiles.append((lo,hi,self.gap));self.cursor=hi
                if hi==self.gaps[self.gap][1]:
                    self.gap+=1
                    if self.gap<len(self.gaps):self.cursor=self.gaps[self.gap][0]
            if not tiles:return None
            self.windows+=1;per_work=max(1,span//self.width);owned=[];start=0
            while start<len(tiles):
                end=start+1
                while end<len(tiles) and end-start<per_work and tiles[end][2]==tiles[start][2]:end+=1
                work=(tiles[start][0],tiles[end-1][1])
                owned.extend([work]*(end-start));start=end
            permutation=list(range(len(tiles)))
            for i in range(len(tiles)-1,0,-1):
                j=self.below(i+1);permutation[i],permutation[j]=permutation[j],permutation[i]
            first={};last={}
            for turn,index in enumerate(permutation):first.setdefault(owned[index],turn);last[owned[index]]=turn
            self.pending=[(*tiles[index][:2],*owned[index],int(first[owned[index]]==turn),int(last[owned[index]]==turn)) for turn,index in enumerate(permutation)]
        return self.pending.pop(0)
