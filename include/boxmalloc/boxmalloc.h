/*
boxmalloc — 16-ary buddy allocator for shared-memory (SHM) / RDMA workloads.

设计灵感来自于现实世界的包装箱系统：大包装箱（外箱）可嵌套小包装箱（内箱），
形成多层结构。物品按其体积大小选择合适的包装箱存放。

设计目标：OS kernel、block 设备的存储分配器，也适用于 SHM 多进程 / RDMA 场景。
被动分配，不主动整理/移动对象。


=== 16 叉伙伴树模型 ===

将 data 区视为一棵 16 叉树。最小分配单元 8 字节，按 16 的幂对齐：

  16^0 × 8 = 8B      1 个 slot
  16^1 × 8 = 128B    16 个 slot
  16^2 × 8 = 2KB     256 个 slot
  ...
  16^k × m × 8        其中 k ≥ 0，m ∈ [1, 15]

alloc(N) → align_to((N+7)/8) → (level, multiple)，分配连续 multiple 个 slot。

16 叉 vs 二叉对比：
- 16叉深度更低，但每层遍历 1-16 个子节点。以 32GB (level=8) 为例，
  复杂度 O(8 × (1~16)) = O(8~128)。
- 二叉深度更深，但每层只需遍历 1-2 个子节点，复杂度 O(32 × (1~2)) = O(32~64)。

16 = 2^4，所有运算化为位操作：offset → slot_index = (unit_offset >> (level×4)) & 0xF。


=== 内存布局 ===

两块独立区域，地址互不依赖：

  meta 区（连续，调用者分配并传入 metaptr）:
  ┌──────────────────────────────────────┐
  │ box_meta_t                            │
  │   magic[16] = "boxmalloc"            │
  │   slot_block[0..root_slots-1]         │  ← 每 slot 独立 blockmalloc 池
  │   slot_locks[0..root_slots-1]         │  ← 每 slot 一个 spinlock
  │   slot_bytes / per_slot_meta / ...    │
  ├──────────────────────────────────────┤
  │ slot 0 的 box_head_t 池               │  ← 树节点，blockmalloc 管理
  │ slot 1 的 box_head_t 池               │
  │ ...                                   │
  └──────────────────────────────────────┘

  data 区（独立，调用者自行管理）:
  ┌──────────────────────────────────────┐
  │ slot 0 的纯数据区                     │  ← slot_bytes 字节
  │ slot 1 的纯数据区                     │
  │ ...                                   │
  └──────────────────────────────────────┘

- boxmalloc 不持有 data 区指针。box_alloc 返回 data 区内的字节偏移量，
  调用者自行 data + offset 读写。
- 偏移量是整数，可跨进程传递（RDMA remote key 计算）。
- data 区不含任何元数据，meta 区依赖 blockmalloc 管理 box_head_t 节点。


=== Slot 状态机 ===

  BOX_UNUSED (0) ──alloc obj──→ OBJ_START (2) + OBJ_CONTINUED (3)...
  BOX_UNUSED (0) ──create child─→ BOX_FORMATTED (1)


=== 并发模型 ===

Root slot 分区锁：data 区分成 root_slots 个 slot（1-16），每 slot 一把 spinlock +
独立 blockmalloc 池。物理隔离，消除跨槽缓存弹跳。

- box_alloc：trylock 遍历 slot，获取锁失败则尝试下一个 slot。
- box_free：根据 offset 确定 slot → lock 等待。
- 多进程 (fork + mmap MAP_SHARED)：零进程私有变量/指针，开箱即用。


=== obj_usage：size 内部编码 ===

  typedef struct {
      uint8_t level : 4;     // 0-15
      uint8_t multiple : 4;  // 1-15 连续 slot 数；0 = 无可用空间（哨兵）
  } obj_usage;

obj_offset(u) = 8 × 16^level × multiple = multiple << (level×4 + 3)


=== 约束与限制 ===

- box_bytessize 必须 = 16^k × m × 8（k≥0, m∈[1,15]），否则 box_init 失败。
- 每次分配向上对齐到 16^k × m × 8。内部碎片最坏约 8×（申请 1B → 8B），
  大对象趋近 2×（如申请 16^k×8+1 字节时 multiple 进位到 2）。
- 无碎片整理：长期运行可能产生外部碎片。
- meta 区大小 = sizeof(box_meta_t) + root_slots × per_slot_meta。
  小对象过多时 meta 区开销显著。


=== 典型用法（SHM 多进程）===

  // 进程 A（初始化）
  uint8_t *meta = mmap(NULL, META_SZ, PROT_READ|PROT_WRITE,
                       MAP_SHARED|MAP_ANONYMOUS, -1, 0);
  uint8_t *data = mmap(NULL, DATA_SZ, PROT_READ|PROT_WRITE,
                       MAP_SHARED|MAP_ANONYMOUS, -1, 0);
  box_init(meta, META_SZ, DATA_SZ);
  uint64_t off = box_alloc(meta, 1024);
  *(uint64_t*)(data + off) = 42;
  // 将 off 通过 IPC 发给进程 B

  // 进程 B（同一共享内存映射）
  uint64_t val = *(uint64_t*)(data + off);
  box_free(meta, off);
*/

#ifndef BOX_MALLOC_H
#define BOX_MALLOC_H

#include <stddef.h>
#include <stdint.h>

// box_init: 初始化分配器。
// metaptr 为 meta 区起始地址（调用者分配并清零）。
// boxhead_bytessize: meta 区总字节数。
// box_bytessize: data 区总字节数，必须 = 16^k × m × 8。
// 返回 0 成功，-1 失败。
int box_init(void *metaptr, const size_t boxhead_bytessize, const size_t box_bytessize);

// box_alloc: 分配 size 字节对象，返回 data 区内字节偏移。
// 失败返回 (uint64_t)-1。
// 线程安全：slot 分区锁，多线程/多进程并发安全。
uint64_t box_alloc(void *metaptr, const size_t size);

// box_allocated_size: 查询已分配对象的实际占用字节数（向上对齐到 16 幂）。
// 线程安全。
uint64_t box_allocated_size(void *metaptr, const uint64_t obj_offset);

// box_free: 释放对象。obj_offset 为 box_alloc 返回的偏移。
// 线程安全。
void box_free(void *metaptr, const uint64_t obj_offset);

#endif // BOX_MALLOC_H
