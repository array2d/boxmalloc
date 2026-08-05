# N-slots-box-obj模型的N-ary树的内存分配器模型

给定一段8*N^n bytes的内存，这块内存可以定义为box，这块内存可以存放两类，一是obj，二是box，box是obj或childbox的容器，每个box拥有N个slot，可以容纳N个childbox或obj。

## level机制。
8byte是最小的boxslot，定义为level0。意味着无论是bool、byte、char、int32/64都会占有整个8byte的boxslot
8*N是level1的box。
8*N*N是level2的box。
从level0到leveltop，每层都是N个slot。

## head区和data区

slots-box模型中，data区完全只存obj的data，不会存任何ojb的分配器元信息。

## obj分配机制
1. objlevel=log(objsize)，obj按level分配到对应level的[1,N-1]个连续slot。由于log，所以不会分配连续N个slot，只会在[1,N-1]范围内。

obj_usage         
├─ level            — N^k 指数
└─ multiple         — 系数 m ∈ [1,N-1]，连续占用 slot 数
    → 实际字节数 = 8 × N^level × multiple
 
2. 我们按低位（左侧）box，中间为空闲、高位（右侧）obj的机制，组织slot和obj的排布。

3. 由于不断的malloc和free，slot会产生很多的空闲气泡。

## head区
slots-box模型的head区是一个跳表，每层都用freelist对空闲slot进行连接。

levels
├─ level[n]
├─     freelist      
├─      freebitmap   
├─      freelist.pre   
├─      freelist.next          
├─ level[n-1]
├─ level[0] 

head区记录的并不是偏移量，而是slotidx[n]的数组

## N=?

N=2，总内存1g，32G s，变为2叉树，二叉导致深度更深，但是每个深度只需要遍历2个子节点，无需bod的slot查找。
N=256,256叉树，32G内存，深度为4，很浅。但alloc时，每个box中256个slot的查找需要优化。
n=256，深度很浅，小对象位于第四层，只需要4次查找。把header区的ojb/box位置记录为slotidx[n],n=4。
n=1024，深度更浅

## 碎片问题

N=16 的 box，经过若干轮 malloc/free：

```
物理 slot 布局（●=obj, ■=box, ○=free）:
[0] [1] [2] [3] [4] [5] [6] [7] [8] [9] [10] [11] [12] [13] [14] [15]
 ●   ○   ■   ○   ●   ●   ○   ■   ○   ●   ○   ■   ○   ■   ○   ○

总空闲 = 7 个 slot，最大连续空闲 = 2，无法分配 3-slot 对象。
```

当n=64，256，由于slot多，尽管依然存在碎片，但malloc分配时，更容易命中
