"""Independent integer/hashlib model of minikey tiles and overflow acceptance."""
import hashlib

class RandomWindow:
    def __init__(self,gaps,seed=0,window=64):
        if not 0<=seed<2**256 or not 1<=window<=256:raise ValueError('invalid seed/window')
        self.gaps=list(gaps);self.seed=seed;self.window=window;self.gap=0
        self.cursor=gaps[0][0] if gaps else 0;self.counter=0;self.rejections=0
        self.pending=[];self.planned=None;self.windows=0
    def below(self,bound):
        mask=(1<<(bound-1).bit_length())-1
        for _ in range(1024):
            digest=hashlib.sha256(b'khminikey-window-v1\0'+self.seed.to_bytes(32,'big')+self.counter.to_bytes(32,'big')).digest()
            self.counter+=1;value=digest[0]&mask
            if value<bound:return value
            self.rejections+=1
        raise ValueError('sampler exhausted')
    def plan(self,span,steps):
        if not span or not 1<=steps<2**64:raise ValueError('invalid size')
        if not self.pending:
            # Form geometric tiles first, then assign their contiguous owners.
            tiles=[]
            for _ in range(self.window):
                if self.gap==len(self.gaps):break
                lo=self.cursor;hi=min(lo+steps,self.gaps[self.gap][1])
                tiles.append((lo,hi,self.gap));self.cursor=hi
                if hi==self.gaps[self.gap][1]:
                    self.gap+=1
                    if self.gap<len(self.gaps):self.cursor=self.gaps[self.gap][0]
            if not tiles:return None
            self.windows+=1;per_work=max(1,min(span,2**64-1)//steps);owners=[];start=0
            while start<len(tiles):
                end=start+1
                while end<len(tiles) and end-start<per_work and tiles[end][2]==tiles[start][2]:end+=1
                owners.extend([(tiles[start][0],tiles[end-1][1])]*(end-start));start=end
            order=list(range(len(tiles)))
            for i in range(len(order)-1,0,-1):
                j=self.below(i+1);order[i],order[j]=order[j],order[i]
            first={};last={}
            for turn,index in enumerate(order):first.setdefault(owners[index],turn);last[owners[index]]=turn
            self.pending=[[tiles[index][0],tiles[index][1],*owners[index],int(first[owners[index]]==turn),int(last[owners[index]]==turn)] for turn,index in enumerate(order)]
        tile=self.pending[0];lo,hi,left,right,first,last=tile
        self.planned=(lo,min(lo+steps,hi),left,right,first,int(last and lo+steps>=hi),0)
        tile[4]=0 # Planning announces ownership even if the attempt overflows.
        return self.planned
    def accept(self):
        if self.planned is None:raise ValueError('no plan')
        self.pending[0][0]=self.planned[1]
        if self.pending[0][0]==self.pending[0][1]:self.pending.pop(0)
        self.planned=None
