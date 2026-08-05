# N-slots-box-obj 模型：带 GC 整理的 N-ary 树内存分配器

[上一章](./n-slots-box-obj-model.md) 定义了被动分配器的静态模型。本章在其上引入 **GC 整理（compaction）**：当 box 内碎片度超过阈值，对 slot[N] 进行重排——左对象、右子 box、中间空闲——消除气泡，恢复连续空闲。

---

## 1. 问题：碎片化的代价

### 1.1 碎片场景

N=16 的 box，经过若干轮 malloc/free：

```
物理 slot 布局（●=obj, ■=box, ○=free）:
[0] [1] [2] [3] [4] [5] [6] [7] [8] [9] [10] [11] [12] [13] [14] [15]
 ●   ○   ■   ○   ●   ●   ○   ■   ○   ●   ○   ■   ○   ■   ○   ○

总空闲 = 7 个 slot，最大连续空闲 = 2，无法分配 3-slot 对象。
```

根本原因：**被动分配器不移动数据，free 只是原地标记 UNUSED**。空闲 slot 被已占用 slot 割裂成碎片。

### 1.2 GC 整理后的理想布局

```
整理后（左对象 ○→○→○→○→○ | 空闲 ○→○→○→○→○→○→○ | 右子box ■→■→■→■）:
[0] [1] [2] [3] [4] [5] [6] [7] [8] [9] [10] [11] [12] [13] [14] [15]
 ●   ●   ●   ●   ●   ○   ○   ○   ○   ○   ○   ○   ■   ■   ■   ■

总空闲不变 = 7 个 slot，最大连续空闲 = 7，可分配任意 ≤7 连续 slot 的对象。
```

---

## 2. 核心设计抉择：偏移量重算 vs slotidx[N] 间接层

GC 整理后，obj 和 box 的物理位置变了。head 区（跳表 freelist）记录的引用必须反映新位置。两种策略：

### 方案 A：偏移量重算（eager）

每次 GC 后，递归遍历整棵子树，更新所有引用（freelist 指针、parent/child 关系、外部 offset）。

| 维度 | 评价 |
|------|------|
| 读路径 | 快——offset 直接寻址，零间接 |
| GC 写开销 | **O(全部节点)**，级联到整棵子树 |
| 外部 API | **破坏性**——用户持有的 `obj_offset` 全部失效 |
| 实现复杂度 | 高——更新逻辑需覆盖所有引用类型，遗漏=corruption |
| GC 粒度 | 不可局部化——一个 box 的整理触发全树遍历 |

### 方案 B：slotidx[N] 间接层（lazy）

每个 box 维护 `virtual_to_phys[N]` 映射表。外部世界永远使用 **virtual slot index**，物理访问时查表。

| 维度 | 评价 |
|------|------|
| 读路径 | 多 1 次单字节查表——表在 box_head_t 同一 cache line 内，实际零开销 |
| GC 写开销 | **O(N)**，仅更新本 box 的映射表（N ≤ 16 → 16 bytes） |
| 外部 API | **稳定**——用户持有的 virtual offset 在 GC 前后不变 |
| 实现复杂度 | 低——GC 只改写一个数组，无需遍历树 |
| GC 粒度 | 完全局部化——每个 box 独立整理，不影响父子节点 |

### 结论：slotidx[N] 胜出

根本原因三条：

1. **外部引用稳定性**。`box_alloc` 返回的 offset 编码的是 virtual slot index，GC 只改变 virtual→physical 映射，不改变 virtual identity。用户的 offset 在 GC 前后始终有效。

2. **O(N) vs O(树节点数)**。N=16 时 GC 一个 box 只需改写 16 bytes 的映射表 + 移动 ≤16 个 slot 的数据。偏移量重算需要遍历整棵子树的所有节点，最深可达 8 层、数千个节点。

3. **查表开销被 cache 吸收**。N=16 时 `virtual_to_phys[16]` = 16 bytes，与 `used_slots[16]`、`childs_blockid[64]`（每个 int32_t 4B × 16）等在同一 `box_head_t` 内。访问 box_head_t 时整个 cache line 已在 L1，额外一次 byte 查表是寄存器操作。

---

## 3. 数据结构

### 3.1 新增字段

在 `box_head_t` 中新增 GC 相关字段：

```
box_head_t（更新后）:
├─ ... 现有字段 ...
├─ virtual_to_phys[N]    — virtual slot index → physical slot index（GC 时改写）
├─ phys_to_virtual[N]    — physical slot index → virtual slot index（逆映射，free 时快速定位）
├─ obj_boundary          — 对象区右边界（物理 slot）：objects ∈ [0, obj_boundary)
├─ box_boundary          — 子 box 区左边界（物理 slot）：boxes   ∈ [box_boundary, N)
│                          free 区 = [obj_boundary, box_boundary)
└─ gc_epoch              — GC 版本号（可选，用于 debug/验证）
```

- `virtual_to_phys` 和 `phys_to_virtual` 互为逆置换。GC 后两者同时更新。
- `obj_boundary` / `box_boundary` 是 alloc 的 O(1) 快速路径：对象从 `obj_boundary` 向右分配，子 box 从 `box_boundary - 1` 向左分配。省去线性扫描。
- 三个区间保证：`0 ≤ obj_boundary ≤ box_boundary ≤ N`。

### 3.2 外部 offset 编码

`box_alloc` 返回的 offset 编码 **virtual slot 路径**（与 GC 前完全相同）：

```
offset = Σ (virtual_slot_i × 8 × N^level_i)
       = bit-concatenation of virtual_slot at each level
```

`find_obj_node` 解码时，每层提取 virtual slot index，查 `virtual_to_phys[virtual_slot]` 得到物理位置：

```c
// 解码：virtual → physical
uint8_t vslot = (unit_offset >> (current_level << 2)) & 0xF;
uint8_t pslot = node->virtual_to_phys[vslot];
// 后续用 pslot 访问 used_slots[pslot]、childs_blockid[pslot]
```

---

## 4. GC 整理算法

### 4.1 触发条件

整理在 **alloc 路径**上触发（惰性——只在需要时整理）：

```
box_find_alloc 进入 node：
  need = objsize.multiple   // 需要连续 slot 数
  free_total = box_boundary - obj_boundary   // 物理空闲区大小
  
  if free_total >= need:
      // 空闲区够用，直接分配（O(1)）
      allocate from [obj_boundary, obj_boundary+need)
      return
  
  // 空闲区不够 → 可能有碎片散布在对象区或 box 区
  // 扫描全表统计实际空闲
  actual_free = count BOX_UNUSED across all physical slots
  max_contiguous = box_continuous_max(node)  // 当前最大连续空闲
  
  if actual_free >= need AND max_contiguous < need:
      // 碎片化：总空闲够但不够连续 → 触发 GC
      box_compact(meta, node)
      // GC 后重试分配
      goto retry
```

触发判据：**总空闲 ≥ 需求 且 最大连续 < 需求**。这意味着"有能力满足，但碎片阻碍了"——恰是 GC 的用武之地。

### 4.2 整理过程：`box_compact`

```
box_compact(meta, node):
  输入：一个已碎片化的 box_head_t
  输出：物理布局重整为 [objects...][free...][boxes...]

  Phase 1 — 分类计数
  ─────────────────
  obj_slots = []   // (phys_slot, length) 列表
  box_slots = []   // phys_slot 列表
  free_count = 0
  
  for p = 0 to N-1:
      case used_slots[p].state:
          OBJ_START:     obj_slots.append((p, count OBJ_CONTINUED after p))
          BOX_FORMATTED: box_slots.append(p)
          BOX_UNUSED:    free_count++

  Phase 2 — 计算目标布局
  ──────────────────────
  total_obj_slots = sum(length for (_, length) in obj_slots)
  total_box_slots = len(box_slots)
  
  目标：
  obj 区:   physical [0, total_obj_slots - 1]
  free 区:  physical [total_obj_slots, N - total_box_slots - 1]
  box 区:   physical [N - total_box_slots, N - 1]

  Phase 3 — 构造新的 virtual_to_phys 映射
  ───────────────────────────────────────
  新建 new_v2p[N]，初始全部 = 0xFF（未映射）
  新建 new_p2v[N]，初始全部 = 0xFF

  // 注意：virtual identity 不变。每个 obj/box 保留其原有 virtual slot index。
  // 只改变 physical 位置。

  obj_cursor = 0
  for each (old_phys, length) in obj_slots:
      // 找到这个 obj 的 virtual slot index
      vslot = phys_to_virtual[old_phys]
      
      // 分配新的物理位置
      new_phys = obj_cursor
      for j = 0 to length-1:
          // 移动数据（见 Phase 4）
          move_obj_data(node, old_phys + j, new_phys + j)
          // 更新 used_slots
          used_slots[new_phys + j] = used_slots[old_phys + j]
          // 清除旧位置
          used_slots[old_phys + j].state = BOX_UNUSED
      
      // 更新映射
      new_v2p[vslot] = new_phys
      new_p2v[new_phys] = vslot
      
      obj_cursor += length
  
  box_cursor = N - 1
  for each old_phys in box_slots (逆序):
      vslot = phys_to_virtual[old_phys]
      new_phys = box_cursor
      
      // child box 的 box_head_t 在 meta 区，不存在于 data 区
      // 只需更新 childs_blockid 数组的索引
      childs_blockid[new_phys] = childs_blockid[old_phys]
      childs_blockid[old_phys] = -1
      used_slots[new_phys] = used_slots[old_phys]
      used_slots[old_phys].state = BOX_UNUSED
      
      new_v2p[vslot] = new_phys
      new_p2v[new_phys] = vslot
      
      box_cursor--

  // 标记空闲区
  for p = obj_cursor to box_cursor:
      used_slots[p].state = BOX_UNUSED
      used_slots[p].continue_max = N

  Phase 4 — 数据搬运
  ─────────────────
  // move_obj_data 移动 data 区的实际对象数据
  // 每个 physical slot 对应 8 × N^objlevel bytes 的 data 区空间
  
  move_obj_data(node, old_phys, new_phys):
      if old_phys == new_phys: return
      slot_size = 8 * powN(node->objlevel)   // N^objlevel × 8 bytes
      src = data_base(node) + old_phys * slot_size
      dst = data_base(node) + new_phys * slot_size
      memmove(dst, src, slot_size)

  Phase 5 — 收尾
  ──────────────
  // 替换映射表
  node->virtual_to_phys = new_v2p
  node->phys_to_virtual = new_p2v
  
  // 更新边界
  node->obj_boundary = obj_cursor
  node->box_boundary = box_cursor + 1
  
  // 更新容量 hint
  node->max_obj_capacity = box_boundary - obj_boundary  // 连续空闲区大小
  // 向上传播
  if node->parent > 0:
      update_parent(meta, SC(meta, node, node->parent), false, true)
```

### 4.3 数据搬运的代价

- **对象数据**：每个 slot = `8 × N^objlevel` bytes。Level 越高，搬动代价越大。
  - Level 0: 8 B/slot，搬一个 obj 最高 15×8 = 120 B —— 微不足道
  - Level 4: 8 × 16^4 = 512 KB/slot，搬一个 15-slot 对象 = 7.5 MB —— 显著

- **子 box 数据**：子 box 的元数据在 meta 区，不受 data 区 GC 影响。只需更新 `childs_blockid` 数组的索引（O(1) 写）。

- **总体**：GC 一个 box 的写开销 = `O(N) metadata + O(移动的 data 量)`。对于低层 box（level 0-2），数据量很小；高层 box 很少需要 GC（分配少、碎片积累慢）。

---

## 5. Head 区（跳表 freelist）集成

### 5.1 freelist 记录 slot index，不是偏移量

理论模型中 head 区是跳表，每层维护 freelist。关键设计：

```
level[n].freelist 节点记录: (box_id, virtual_slot_index)
而非: (box_id, data_offset)
```

- `box_id`：标识哪个 box_head_t（跨 box 的跳表需要）
- `virtual_slot_index`：在该 box 内的 virtual slot index

### 5.2 GC 对 freelist 的影响

GC 整理一个 box 后：

1. **virtual_slot_index 不变**——外部持有的 freelist 节点无需更新
2. **物理位置变了**——但 freelist 不关心物理位置，只关心 virtual identity
3. **新的空闲区**——GC 后 `[obj_boundary, box_boundary)` 全为空闲，需将这些 virtual slot 插入 freelist

```
box_compact 收尾 — 重建本 box 在 freelist 中的条目：
1. 从所有 level 的 freelist 中摘除本 box 的旧空闲条目
2. 为新的空闲区 [obj_boundary, box_boundary) 中每个 virtual slot 插入 freelist
```

### 5.3 跨 box 跳表的 slot 定位

当跳表需要在不同 box 间跳跃时：

```
跳表节点: (box_id, virtual_slot_idx)
  → 找到 box_head_t (by box_id)
  → physical_slot = box->virtual_to_phys[virtual_slot_idx]
  → 物理偏移 = obj_offset_raw(box->objlevel, physical_slot)
  → data 区地址 = data_base + 物理偏移
```

始终经过 virtual→physical 查表，保证 GC 安全。

---

## 6. 外部 API 调整

### 6.1 当前 API（无 GC）

```c
uint64_t box_alloc(void *metaptr, size_t size);     // 返回 data 区偏移
void     box_free(void *metaptr, uint64_t offset);   // 传入偏移
uint64_t box_allocated_size(void *metaptr, uint64_t);
```

用户拿到 offset 后直接 `data_base + offset` 访问数据。

### 6.2 GC 后的 API 语义

`box_alloc` 返回的是 **virtual offset**（编码 virtual slot 路径）。GC 前后 virtual offset 不变，但对应物理位置可能变了。

用户**不能**缓存物理指针跨 alloc/free 调用。安全用法：

```c
// 正确用法
uint64_t off = box_alloc(meta, 1024);
void *ptr = box_deref(meta, off);   // 每次访问前解析
memcpy(ptr, src, 1024);
box_free(meta, off);

// 危险用法（GC 后 ptr 可能悬空）
uint64_t off = box_alloc(meta, 1024);
void *ptr = box_deref(meta, off);
box_alloc(meta, 2048);  // 触发 GC → ptr 失效
memcpy(ptr, src, 1024); // ← 悬空指针！
```

### 6.3 新增 API

```c
// box_deref: 将 virtual offset 解析为当前物理地址
// GC 后返回不同值，但 offset 参数本身不变
void* box_deref(void *metaptr, uint64_t virtual_offset);

// box_is_compacted: 查询一个 box 自上次 deref 以来是否被整理过
// 用于用户侧缓存失效检测（可选）
bool box_is_compacted(void *metaptr, uint64_t virtual_offset);
```

`box_deref` 的实现：走 `find_obj_node` 解码 virtual offset → 查 virtual_to_phys → 计算物理地址。与 `find_obj_node` 共用同一逻辑。

---

## 7. 并发语义

### 7.1 GC 在 slot 锁内执行

`box_compact` 在 `box_find_alloc` 内部触发，此时**已持有当前 slot 的 spinlock**。整个 GC 过程在锁保护下进行：

```
box_alloc:
  lock(slot)
  ...
  box_find_alloc:
      if 碎片触发:
          box_compact(node)   // ← 锁内，独占该 slot
      ...
  unlock(slot)
```

### 7.2 对并发 alloc 的影响

- **同 slot**：GC 期间该 slot 被锁，其他线程的 alloc/free 会 trylock 失败轮询到下一 slot。对吞吐影响可控——GC 是低频事件。
- **跨 slot**：完全无影响，per-slot 独立锁保证。
- **free 路径**：`box_free` 可能阻塞等待 GC 完成释放锁。阻塞时间 = 数据搬运时间，对低 level box 可忽略。

### 7.3 SHM 兼容性

virtual_to_phys 和 phys_to_virtual 数组存在于 `box_head_t` 内（meta 区，SHM 中）。GC 的 memmove 作用于 data 区（也是 SHM）。所有状态在共享内存中，无进程私有数据——fork 兼容性不变。

---

## 8. GC 触发时机补充讨论

### 当前设计：alloc 路径惰性触发

- 优点：不做无用功。只在碎片阻碍分配时才整理。
- 缺点：malloc 延迟不可预测（偶发 GC spike）。

### 备选策略

| 策略 | 触发点 | 优点 | 缺点 |
|------|--------|------|------|
| **惰性（当前）** | alloc 失败但总空闲够 | 无无用 GC | malloc 偶发 spike |
| **free 时阈值触发** | free 后碎片率 > 阈值 | 分摊延迟到 free | free 变重 |
| **后台 GC** | 独立整理线程扫描 | malloc/free 均轻 | 需要并发 GC 协议（复杂） |
| **混合** | 碎片率低时惰性，高时 free 触发 | 平衡 | 实现稍复杂 |

对于被动分配器场景（OS kernel、block 设备），**惰性触发**最优：不需要后台线程，不需要复杂的并发 GC 协议，整理只在必要时发生，且锁内持有时间可预测。

---

## 9. 复杂度总结

| 操作 | 无 GC | 有 GC（无触发） | 有 GC（触发） |
|------|-------|----------------|--------------|
| alloc | O(N) 线性扫描 | O(1) 边界分配 | O(1) + O(N·data_move) |
| free | O(N) 标记 + O(log 树) 传播 | 同左 | 同左 |
| deref | O(log 树) | O(log 树) + 1 次 byte 查表 | 同左 |

- N ≤ 16，O(N) = O(1) 常数。边界分配将 alloc 从扫描 16 次降为 2 次比较。
- 数据搬运的常数因子随 level 指数增长，但高层 box 极少触发 GC。
- 查表开销：同在 box_head_t cache line 内，可忽略。

---

## A. 附录：为什么是"左 obj、右 box"

obj 和 box 的分配方向相反——obj 从左向右扩展，box 从右向左收缩——使得空闲区始终在中间自然合并。

```
alloc obj(3 slots):    obj_boundary += 3
alloc box:             box_boundary -= 1
free obj:              标记 UNUSED（不调整边界，留待下次 GC）
free box:              标记 UNUSED

        obj_boundary →        ← box_boundary
[●●●○○○○○○○○○○■■■■]
 对象区    空闲区     子box区
```

边界分配保证 alloc 时无需扫描，直接取边界位置。只有 GC 时才需要扫描全数组做重排。

---

## B. 附录：N=64、N=256 适用性分析

前文以 N=16 为基准展开设计。本节将 GC 整理模型外推到 N=64 和 N=256，从数据结构、cache 行为、GC 代价三个维度做定量对比。

### B.1 GC 代价随 N 和 Level 的爆炸

GC 最坏情况需移动 box 内**全部 slot** 的数据。每个 slot = `8 × N^level` bytes，全部 N-1 个 slot 的总搬运量：

| Level | N=16 (15 slots) | N=64 (63 slots) | N=256 (255 slots) |
|-------|-----------------|-----------------|--------------------|
| L0 | 120 B ✅ | 504 B ✅ | 2 KB ✅ |
| L1 | 1.9 KB ✅ | 31.5 KB ✅ | 510 KB ✅ |
| L2 | 30 KB ✅ | **2 MB** ⚠️ | **127.5 MB** ❌ |
| L3 | 480 KB ✅ | **126 MB** ❌ | **32 GB** ❌ |
| L4 | **7.5 MB** ⚠️ | **8 GB** ❌ | — |
| L5 | **120 MB** ❌ | — | — |

**规律**：GC 搬运量 = `8 × N^level × (N-1)` = `O(N^(level+1))`。N 每增大 4 倍（16→64），同一 level 的搬运量增大 **16 倍**。N 每增大 16 倍（16→256），同一 level 的搬运量增大 **256 倍**。

### B.2 树深度 vs GC 可用层数

以 32GB 地址空间为例：

| | N=16 | N=64 | N=256 |
|---|------|------|-------|
| 树深度 | 8 层 (L0-L7) | 6 层 (L0-L5) | 4 层 (L0-L3) |
| GC 可用的最高 level | **L4** (≤7.5 MB) | **L2** (≤2 MB) | **L1** (≤510 KB) |
| GC 可用层数 | **5 层** (L0-L4) | **3 层** (L0-L2) | **2 层** (L0-L1) |
| GC 不可用层数 | 3 层 (L5-L7) | 3 层 (L3-L5) | 2 层 (L2-L3) |

**关键发现**：增大 N 减少的总层数，几乎全被 GC 不可用层数的增加所抵消。N=16 时 5/8 层可 GC，N=64 时 3/6 层可 GC，N=256 时仅 2/4 层可 GC。

### B.3 典型分配覆盖

8B–64KB 是内存分配器最常见的请求范围：

| | N=16 | N=64 | N=256 |
|---|------|------|-------|
| 覆盖 8B-64KB 的 level | L0-L3 (4 层) | L0-L2 (3 层) | L0-L1 (2 层) |
| 这些 level 的 GC 代价 | ✅ 全部廉价 | ✅ 全部 OK (≤2 MB) | ✅ 全部 OK (≤510 KB) |

**结论**：对于典型分配范围，三种 N 的 GC 都在安全区。差异体现在大对象上——N=16 可以对 512KB 级对象做 GC，N=64/256 不可以。

### B.4 数据结构膨胀

| 字段 | N=16 | N=64 | N=256 |
|------|------|------|-------|
| `virtual_to_phys[N]` | 16 B | 64 B | 256 B |
| `phys_to_virtual[N]` | 16 B | 64 B | 256 B |
| `used_slots[N]` | 16 B | 64 B | 256 B |
| `childs_blockid[N]` | 64 B | 256 B | 1024 B |
| `box_head_t` 估算总量 | **~140 B** | **~480 B** | **~1820 B** |
| cache line 数 (64B/line) | **3** | **8** | **29** |

**N=256 的 box_head_t 接近 2KB**。这是严重问题：

- 29 条 cache line，`box_deref` 所需的 `virtual_to_phys` 查表大概率 L1 miss。
- Per-slot blockmalloc 池需要 ≥2KB 的 block size，meta 区内存开销巨大。
- Level 0 box 管理的数据量仅 8B × 256 = 2KB。meta/data 比接近 **1:1**——元数据开销不可接受。

**N=64 的 box_head_t 约 480B（8 cache lines）**——尚可接受，但相较 N=16（3 CL，全部在 L1 的一次 miss 内可达）退步明显。

### B.5 obj_usage 编码

| | N=16 | N=64 | N=256 |
|---|------|------|-------|
| multiple 范围 | [1, 15] | [1, 63] | [1, 255] |
| multiple 需 bit 数 | 4 | 6 | 8 |
| level 需 bit 数 | 4 | 3 | 2 |
| 总 bit 数 | 8 → **uint8_t** | 9 → **uint16_t** | 10 → **uint16_t** |
| `compare_obj_usage` | 8-bit sub | 16-bit sub | 16-bit sub |

N≥64 时 `obj_usage` 无法放入单字节，`box_child_t.continue_max`（6 bit）也无法表达 N=64 的 63。这些编码膨胀会级联增加所有相关结构体的体积。

### B.6 SIMD 扫描

GC Phase 1 需要对 N 个 slot 做分类扫描：

| N | 最优方案 | 迭代次数 |
|---|---------|---------|
| 16 | 标量循环（16 次，分支预测器完全吸收） | 16 |
| 64 | `tzcnt` × 1 在 64-bit free bitmap 上循环 | 1–2 |
| 256 | AVX2 `_mm256_cmpeq_epi8` + `_mm256_movemask` 分组扫描 | 8 |

N=16 的标量循环已经足够快——16 次迭代在 L1 内完成，延迟 < 64 cycles。N=64/256 才需要 SIMD，但增加的代码复杂度换来的收益有限。

### B.7 并发度

`root_slots` 决定最大并行 alloc 路数。对给定内存大小，root_slots 取决于 `box_bytessize` 的 N-幂对齐：

| 内存大小 | N=16 | N=64 | N=256 |
|---------|------|------|-------|
| 1 GB | **8 slots** | **8 slots** | **8 slots** |
| 4 GB | 2 slots ⚠️ | **32 slots** | **32 slots** |
| 16 GB | 8 slots | 2 slots ⚠️ | 128 slots |
| 32 GB | 1 slot ❌ | 4 slots | 1 slot ❌ |

**N=16 和 N=256 在 32GB 时都退化为单 slot**（32GB = 2^32 × 8B = 16^8 × 8B = 256^4 × 8B）——恰好对齐到整数幂。这种"对齐退化"在实践中可通过略微调整 `box_bytessize` 规避（例如分配 30GB 而非 32GB）。但 N=256 的幂间隔（256×）远大于 N=16（16×），对齐风险窗口更窄。

### B.8 综合评判

```
         GC安全性   meta开销   树深度   并发友好   实现简单   推荐度
N=16      ████      ████      ███      ███       ████     ★★★ 首选
N=64      ███       ███       ████     ████      ███      ★★  可用
N=256     ██        █         █████    ██        ██       ★   不推荐
```

**N=16 是 GC 整理模型的最优参数**。理由：

1. **GC 代价可控**。5 个 level 可安全整理，覆盖所有常见分配大小。N=64 只有 3 个，N=256 只有 2 个。

2. **box_head_t 紧凑**。~140 bytes ≈ 2-3 cache lines。`virtual_to_phys` 查表与 `used_slots` 在同一 cache line，零额外延迟。N=256 膨胀到 29 条 cache line，meta/data 比失控。

3. **obj_usage 单字节编码**。比较退化为 8-bit 减法，一个 cycle。N≥64 需要 uint16_t。

4. **并发退化可控**。16 的幂间隔小，通过微调内存大小即可获得理想的 root_slots 数。256 的幂间隔大，调整幅度也大。

**N=64 在特定场景下可接受**（如内存极大、分配以大对象为主），但 GC 的收益被数据结构膨胀部分抵消。

**N=256 不适合 GC 整理模型**。浅树（4 层）本身已降低碎片风险，建议采用纯惰性策略：不做 GC，依赖 N=256 的大容量在统计上吸收碎片（256 slots 中出现连续碎片满足需求的概率远高于 16 slots）。

---

## C. 附录：全参数对比矩阵（N=2/16/64/256 × 1GB/32GB）

### C.1 结构参数

| N | slotidx 位宽 | obj_usage 编码 | box_head_t 大小 | cache lines |
|---|-------------|---------------|----------------|-------------|
| 2 | 1 bit/level | `level:5, mult:1` → uint8_t | **44 B** | 1 |
| 16 | 4 bit/level | `level:4, mult:4` → uint8_t | **142 B** | 3 |
| 64 | 6 bit/level | `level:3, mult:6` → uint16_t | **478 B** | 8 |
| 256 | 8 bit/level | `level:2, mult:8` → uint16_t | **1822 B** | 29 |

### C.2 树深度与并发度

| N | 1GB 深度 | 1GB root_slots | 32GB 深度 | 32GB root_slots |
|---|---------|---------------|----------|----------------|
| 2 | 27 层 (L0-L26) | 1 ❌ | 32 层 (L0-L31) | 1 ❌ |
| 16 | 7 层 (L0-L6) | **8** ✅ | 8 层 (L0-L7) | 1 ⚠️ |
| 64 | 5 层 (L0-L4) | **8** ✅ | 6 层 (L0-L5) | 4 ✅ |
| 256 | 4 层 (L0-L3) | **8** ✅ | 4 层 (L0-L3) | 1 ⚠️ |

> N=2 深度过深，且二叉导致 root_slots 恒为 1——零并发。
> N=16/64/256 在 32GB 的 root_slots 退化可通过微调内存大小规避（如 30GB 替代 32GB）。

### C.3 小对象（8B–64KB）malloc/free 路径

| N | 内存 | 覆盖 level | 下降层数 | 每层 slot 扫描 | 寻址迭代 | GC 安全性 |
|---|------|----------|---------|---------------|---------|----------|
| 2 | 1GB | L0-L13 (14层) | **13** | ≤2 次（无扫描） | 13 | ✅ 全部安全（≤8KB/层） |
| 2 | 32GB | L0-L13 (14层) | **18** | ≤2 次（无扫描） | 18 | ✅ 同上 |
| 16 | 1GB | L0-L3 (4层) | **3** | ≤16 次标量扫描 | 4 | ✅ L0-L3 全部 ≤480KB |
| 16 | 32GB | L0-L3 (4层) | **4** | ≤16 次标量扫描 | 4 | ✅ 同上 |
| 64 | 1GB | L0-L2 (3层) | **2** | O(1) 边界分配 | 3 | ✅ L0-L2 全部 ≤2MB |
| 64 | 32GB | L0-L2 (3层) | **3** | O(1) 边界分配 | 3 | ✅ 同上 |
| 256 | 1GB | L0-L1 (2层) | **2** | O(1) 边界分配 | 2 | ✅ L0-L1 全部 ≤510KB |
| 256 | 32GB | L0-L1 (2层) | **2** | O(1) 边界分配 | 2 | ✅ 同上 |

> **下降层数** = root_level − target_max_level。寻址迭代 = free 时 `find_obj_node` 的 level 数。
> N=2 虽然每次只要查 2 个 slot，但 13–18 层遍历导致 **13–18 次指针追踪**（每次可能 cache miss），延迟远超 N=16 的 3–4 次。

### C.4 大对象（1MB–100MB）malloc/free 路径

| N | 内存 | 覆盖 level | 下降层数 | GC 搬运量（最坏/层） |
|---|------|----------|---------|-------------------|
| 2 | 1GB | L17-L23 (7层) | **9** | L17=1MB, L18=2MB, L19=4MB, L20=8MB, L21=**16MB**, L22=**32MB**, L23=**64MB** |
| 2 | 32GB | L17-L23 (7层) | **14** | 同上 |
| 16 | 1GB | L4-L5 (2层) | **2** | L4=**7.5MB**, L5=**120MB** ❌ |
| 16 | 32GB | L4-L5 (2层) | **3** | 同上 |
| 64 | 1GB | L2-L3 (2层) | **2** | L2=**2MB**, L3=**126MB** ❌ |
| 64 | 32GB | L2-L3 (2层) | **3** | 同上 |
| 256 | 1GB | L2 (1层) | **1** | L2=**127.5MB** ❌ |
| 256 | 32GB | L2 (1层) | **1** | 同上 |

> 大对象在所有 N 下的 GC 均有痛点。N=16 在 L4（7.5MB）仍可 GC，L5+ 不可。N=256 的大对象层（L2）单一层就不可 GC。

### C.5 综合评判

```
              树深度   小对象延迟   大对象延迟   小对象GC   大对象GC   meta开销   并发度   总分
N=2  1GB      █ (27)   █ (13跳)    ██ (9跳)    ████      ███       ████     █ (1)   ★ 不适合
N=2  32GB     █ (32)   █ (18跳)    █ (14跳)    ████      ███       ████     █ (1)   ★ 不适合
N=16 1GB      ████     ████ (3跳)  ████ (2跳)  ████      ███       ████     ████    ★★★ 首选
N=16 32GB     ████     ████ (4跳)  ████ (3跳)  ████      ███       ████     ███     ★★★ 首选
N=64 1GB      █████    █████ (2跳) ████ (2跳)  ████      ██        ███      ████    ★★  可用
N=64 32GB     █████    █████ (3跳) ████ (3跳)  ████      ██        ███      ████    ★★  可用
N=256 1GB     █████    █████ (2跳) █████ (1跳) ████      █         █        ████    ★★  大对象慎用
N=256 32GB    █████    █████ (2跳) █████ (1跳) ████      █         █        ███     ★★  大对象慎用
```

### C.6 N=16 为什么是最优解

四条硬理由交织在一起形成了 N=16 的不可替代性：

**1. 小对象路径的 cache 友好性**。3–4 次树下降，每次访问的 `box_head_t`（142B，2-3 CL）大概率在 L2 cache 内。对比 N=2 的 13–18 次下降——每次跟踪一个 44B 节点，节点虽小但跳跃次数过多，cache miss 累积延迟远超 N=16。

**2. GC 安全覆盖的 level 数最多**。N=16 有 L0-L4 共 5 层可安全 GC——这正好覆盖到 512KB/对象。几乎所有"需要连续 slot"的场景都落在这个范围内。N=64 只能覆盖到 L2（32KB），N=256 只能到 L1（2KB）。

**3. slot 扫描开销被边界分配消除，不再是大 N 的优势**。原始担忧是 N=16 需要扫描 16 个 slot 找空闲。引入 GC 边界分配后，空闲区始终在 `[obj_boundary, box_boundary)`，alloc 只需 2 次比较。N=16 的"扫描劣势"消失，而大 N 的"box_head_t 膨胀劣势"凸显。

**4. 并发退化窗口窄**。N=16 的幂间隔只有 16×（例如 16MB → 256MB → 4GB → 64GB）。只需将内存池微调几个百分点就能获得理想的 root_slots 数。N=256 的幂间隔是 256×（2KB → 512KB → 128MB → 32GB），跨度巨大，对齐退化的概率更高。

### C.7 选型决策树

```
需要并发 > 1？
├─ 否 → N=2（二叉 buddy，无并发，无 slot 扫描，深度不可接受则排除）
└─ 是 → 需要大对象 GC？
        ├─ 是 → N=16（唯一能在大对象层安全 GC 的选项）
        └─ 否 → 内存 < 4GB？
                ├─ 是 → N=16（box_head_t 紧凑，meta 开销低）
                └─ 否 → 可考虑 N=64（树浅 2 层，小对象全 GC-safe）
                        但注意 box_head_t 膨胀至 478B，meta 区需相应放大
```
