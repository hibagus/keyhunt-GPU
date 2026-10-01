"""Integer model: immutable reservations separate from the uncovered intervals."""
U64=(1<<64)-1
class Planner:
    def __init__(self,gaps,order='both-ends',seeds=None):
        self.gaps=list(gaps);self.owners=[];self.high=False;self.order=order;self.seeds=seeds
    def plan(self,work,batch):
        assert work>0 and batch>0
        if not self.gaps:return None
        low,high=self.gaps[-1 if self.high else 0];at=high-1 if self.high else low
        owner=next((o for o in self.owners if o[0]<=at<o[1]),None);starts=owner is None
        if starts:
            # Find the free region without splitting gaps at owner boundaries.
            floor=max([low]+[o[1] for o in self.owners if low<o[1]<=at])
            ceiling=min([high]+[o[0] for o in self.owners if at<o[0]<high])
            span=min(work,U64)
            owner=(max(floor,high-span),high) if self.high else (low,min(ceiling,low+span))
            self.owners.append(owner)
        if self.high:
            left,right=max(low,owner[0],high-batch),high
            if self.seeds:left=max(left,1+((right-2)//self.seeds)*self.seeds)
        else:
            left,right=low,min(high,owner[1],low+batch)
            if self.seeds:right=min(right,1+((left-1)//self.seeds+1)*self.seeds)
        finishes=max(low,owner[0])==left and min(high,owner[1])==right
        self.pending=(left,right,owner,finishes)
        return left,right,*owner,int(starts),int(finishes)
    def accept(self):
        left,right,owner,finishes=self.pending
        self.gaps=[part for lo,hi in self.gaps for part in
                   ([(lo,hi)] if right<=lo or left>=hi else [(lo,left),(right,hi)]) if part[0]<part[1]]
        if finishes:self.owners.remove(owner)
        if self.order=='both-ends':self.high=not self.high
