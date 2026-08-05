/*
 * doc.h — N-slots-box-obj 模型伪代码
 *
 * 严格遵循 left-box / right-obj 布局以避免碎片化。
 * N 可配置 (#define SBO_N 64)，以下用 N 代指。
 *
 *   physical slot 布局（单 box 内）:
 *   ┌──────────────┬────────────────────────────┬──────────────────┐
 *   │  子 box 区    │         空闲区              │    对象区         │
 *   │  [0, box_b)  │  [box_b, obj_b]            │  (obj_b, N-1]    │
 *   │  box→ 向右扩展 │  连续空闲，可合并            │  obj← 向左扩展    │
 *   └──────────────┴────────────────────────────┴──────────────────┘
 *
 *   - 分配子 box: 取 slot[box_boundary], box_boundary++
 *   - 分配对象:   取 slot[obj_boundary-m+1 .. obj_boundary], obj_boundary -= m
 *   - 释放:       仅标记 FREE，不调边界。N=64 统计吸收碎片，不做 compact。
 *
 * 约定: physical_slot ∈ [0,N-1]。无 compact → physical == virtual 始终成立。
 *       外部 offset 直接编码 physical slot index。
 *
 * 模型: L0=8B(无slot), L_k = N^k × 8B (k≥1), 每 box 固定 N 个 slot.
 * N_BITS = log2(N), offset 每层占 N_BITS bit.
 *
 * N=64 时各层 data 大小:
 *   L0 = 8B            (原子, 无 slot)
 *   L1 = 512B          (64 个obj，每个 8B)
 *   L2 = 32KB          ()
 *   L3 = 2MB           ()
 *   L4 = 128MB         (对普通小程序，足够)
 *   L5 = 8GB
 *   L6 = 512GB
 * 低层数带来的好处:malloc时，
 */

#ifndef SBO_DOC_H
#define SBO_DOC_H

/*
 * 假设 #define:
 *   SBO_N         = 64       // 每 box 的 slot 数
 *   SBO_N_BITS    = 6        // log2(SBO_N)
 *   SBO_N_MASK    = 0x3F     // SBO_N - 1
 *   SBO_BITMAP_B  = SBO_N/8  // free_bitmap 字节数
 */

/* ================================================================
 * 1. 数据结构
 * ================================================================ */

/*
 * sbo_usage_t  (2 bytes)
 *   容量编码: 8 × N^(level-1) × multiple bytes (level≥1)
 *   level ∈ [1,15], multiple ∈ [1,N-1], level=0 为 sentinel
 */

/*
 * sbo_slot_t  (1 byte)
 *   state:2  — FREE(0) | BOX(1) | OBJ_START(2) | OBJ_CONT(3)
 *   _rsv:6
 */

/*
 * sbo_box_t  (每个 box 固定 N 个 slot)
 *
 *   // --- 身份 ---
 *   uint8_t  state;                       // BOX (formatted)
 *   uint8_t  objlevel;                    // 本 box 所在 level (≥1)
 *   uint8_t  slot_id;                     // 所属 root slot
 *   int32_t  parent;                      // 父节点 block id (0=root, -1=无)
 *
 *   // --- 物理布局 ---
 *   uint8_t  free_bitmap[SBO_BITMAP_B];   // bit=1→FREE, bit=0→占用
 *   sbo_slot_t slots[N];                  // 按物理 slot 索引
 *   int32_t   children[N];                // 子 box 的 block id (-1=无)
 *
 *   // --- left-box / right-obj 边界 ---
 *   uint8_t  box_boundary;                // boxes ∈ [0, box_boundary)
 *   uint8_t  obj_boundary;                // objs  ∈ (obj_boundary, N-1]
 *                                         // free  ∈ [box_boundary, obj_boundary]
 *
 *   // --- 恒等映射 (无 compact, physical==virtual) ---

 *   // --- 容量 hint ---
 *   sbo_usage_t child_max_cap;            // 子树最大可用容量 (快速剪枝)
 *   uint8_t  max_obj_cap;                 // 本 box 最大连续空闲 slot 数 [0,N]
 */

/*
 * sbo_meta_t
 *   magic[16], head_size, data_size
 *   slot_pools[MAX_ROOT_SLOTS]   — per-slot blockmalloc
 *   slot_locks[MAX_ROOT_SLOTS]   — per-slot spinlock (64B padding)
 *   slot_bytes, per_slot_meta, root_slots
 *   block_stride, sizeof_block_head
 */


/* ================================================================
 * 2. free_bitmap 辅助操作 (byte-oriented)
 * ================================================================ */

/*
 * 由于 free_bitmap 是 uint8_t[N/8]，所有位操作通过字节索引:
 *
 *   bitmap_get(fb, i):
 *       return (fb[i >> 3] >> (i & 7)) & 1
 *
 *   bitmap_set(fb, i):
 *       fb[i >> 3] |= (1 << (i & 7))
 *
 *   bitmap_clear(fb, i):
 *       fb[i >> 3] &= ~(1 << (i & 7))
 *
 *   bitmap_test_mask(fb, start, count):
 *       // 检查 [start, start+count) 是否全为 1
 *       for i in start .. start+count-1:
 *           if !bitmap_get(fb, i): return false
 *       return true
 *
 *   bitmap_fill_all(fb):
 *       memset(fb, 0xFF, SBO_BITMAP_B)
 *
 *   bitmap_clear_range(fb, start, count):
 *       for i in start .. start+count-1:
 *           bitmap_clear(fb, i)
 *
 *   bitmap_set_range(fb, start, count):
 *       for i in start .. start+count-1:
 *           bitmap_set(fb, i)
 *
 *   优化: 若 SBO_N 是 64 的倍数，可将 8 字节 reinterpret 为 uint64_t 做批量操作。
 */


/* ================================================================
 * 3. 核心操作伪代码
 * ================================================================ */

/* --- 3.1 box_format: 初始化一个 box ---
 *
 * sbo_box_format(box, objlevel, parent_id):
 *   box.state       = BOX
 *   box.objlevel    = objlevel
 *   box.parent      = parent_id
 *   bitmap_fill_all(box.free_bitmap)       // N 个 1
 *
 *   box.box_boundary = 0
 *   box.obj_boundary = N - 1               // free 区 = [0, N-1] 全范围
 *
 *   for i in 0..N-1:
 *       box.slots[i].state = FREE
 *       box.children[i] = -1
 *       box.v2p[i] = box.p2v[i] = i     // 恒等映射 (预留)
 *
 *   box.child_max_cap = {level: objlevel+1, mult: 1}  // 满 box
 */

/* --- 3.2 alloc_box_slot: 分配一个子 box slot（从左侧） ---
 *
 * sbo_alloc_box_slot(meta, box):
 *   for p = box.box_boundary to box.obj_boundary:
 *       if bitmap_get(box.free_bitmap, p):
 *           bitmap_clear(box.free_bitmap, p)
 *           box.slots[p].state = BOX
 *           box.box_boundary = p + 1
 *           return (OK, slot=p)
 *   return FAIL   // 空闲区无 slot → 向上传播失败
 */

/* --- 3.3 alloc_obj_slots: 分配 m 个连续对象 slot（从右侧） ---
 *
 * sbo_alloc_obj_slots(meta, box, m):   // m ∈ [1, N-1]
 *   for p = box.obj_boundary down to box.box_boundary:
 *       start = p - m + 1
 *       if start < box.box_boundary: break
 *       if bitmap_test_mask(box.free_bitmap, start, m):
 *           bitmap_clear_range(box.free_bitmap, start, m)
 *           box.slots[start].state = OBJ_START
 *           for j = start+1 to p:
 *               box.slots[j].state = OBJ_CONT
 *           box.obj_boundary = start - 1
 *           return (OK, slot=start)
 *   return FAIL   // 边界无连续空闲 → 向上传播失败
 */

/* --- 3.4 free_obj_slots: 释放对象的连续 slot ---
 *
 * sbo_free_obj_slots(meta, box, physical_start):
 *   p = physical_start
 *   assert box.slots[p].state == OBJ_START
 *   box.slots[p].state = FREE
 *   bitmap_set(box.free_bitmap, p)
 *   p++
 *   while p < N and box.slots[p].state == OBJ_CONT:
 *       box.slots[p].state = FREE
 *       bitmap_set(box.free_bitmap, p)
 *       p++
 *   // 不调边界 — 碎片由后续 alloc 扫描吸收
 */

/* --- 3.5 free_box_slot: 释放子 box slot ---
 *
 * sbo_free_box_slot(meta, box, physical_slot):
 *   assert box.slots[physical_slot].state == BOX
 *   box.slots[physical_slot].state = FREE
 *   bitmap_set(box.free_bitmap, physical_slot)
 *   box.children[physical_slot] = -1
 *   // 不调边界, 不回收 child box_head_t (被动分配器)
 */

/* --- 3.6 sbo_find_alloc: 递归树下降分配 ---
 *
 * sbo_find_alloc(meta, box, parent, usage):
 *   if usage.level == box.objlevel:
 *       r = sbo_alloc_obj_slots(meta, box, usage.multiple)
 *       if r == FAIL: return FAIL
 *       return sbo_offset_raw(box.objlevel, r.start_physical)
 *
 *   if usage.level < box.objlevel:
 *       for p in 0..N-1:
 *           if children[p] >= 0:
 *               ch = sbo_child(meta, box, children[p])
 *               if ch 子树容量 < usage: continue
 *               sub = sbo_find_alloc(meta, ch, box, usage)
 *               if sub != FAIL:
 *                   return sbo_offset_raw(box.objlevel, p) + sub
 *               continue
 *
 *           if slots[p] == FREE:
 *               r = sbo_alloc_box_slot(meta, box)
 *               if r == FAIL: continue
 *               child_id = blocks_alloc(...)
 *               box.children[r.physical] = child_id
 *               ch = sbo_child(meta, box, child_id)
 *               sbo_box_format(ch, box.objlevel-1, sbo_self_id(meta, box))
 *               ch.slot_id = box.slot_id
 *               sub = sbo_find_alloc(meta, ch, box, usage)
 *               if sub != FAIL:
 *                   return sbo_offset_raw(box.objlevel, r.physical) + sub
 *   return FAIL
 */

/* --- 3.7 sbo_find_obj_node: offset → (box, slot) ---
 *
 * offset 分解: 每层占 N_BITS bit（无 compact → physical==virtual）。
 *   unit = obj_offset / 8
 *   第 L 层的 slot = (unit >> ((L-1)*N_BITS)) & (N-1)
 *
 * sbo_find_obj_node(meta, offset) → (box, slot):
 *   unit = offset / 8
 *   root_si = offset / slot_bytes
 *   node = root box of that slot
 *   cur_level = node.objlevel
 *   while cur_level >= 1:
 *       slot = (unit >> ((cur_level-1)*N_BITS)) & (N-1)
 *       if node.slots[slot].state == OBJ_START:
 *           return (node, slot)
 *       if node.slots[slot].state == BOX:
 *           node = sbo_child(meta, node, node.children[slot])
 *           cur_level--
 *           continue
 *       return NULL
 *   return NULL
 */

/* --- 3.8 offset 编码 ---
 *
 * sbo_offset_raw(level, slot):
 *   // slot 在 level-k box 内的字节偏移 (k≥1)
 *   return slot << ((level-1) * N_BITS + 3)
 *
 * sbo_offset_of(usage):
 *   // 总字节数 = multiple × 8 × N^(level-1)
 *   if usage.level == 0: return 8
 *   return usage.multiple << ((usage.level-1) * N_BITS + 3)
 */

/* --- 3.9 公共 API ---
 *
 * sbo_align_to(n_slots):
 *   // slot 数 → (level≥1, multiple∈[1,N-1])
 *   // [1, N-1] → L1, [N, N(N-1)] → L2, ...
 *   // 找最小 k≥1 使 n_slots ≤ (N-1) × N^(k-1)
 *
 * sbo_init(metaptr, head_size, data_size):
 *   // data_size 必须 64-幂对齐
 *
 * sbo_alloc(metaptr, size):
 *   1. usage = sbo_align_to(ceil(size/8))
 *   2. hash → root_slot → trylock 轮询
 *   3. root box 子树容量 >= usage → sbo_find_alloc
 *   4. total = si * slot_bytes + offset
 *   5. 返回 total
 *
 * sbo_free(metaptr, obj_offset):
 *   1. lock → sbo_find_obj_node → (box, slot)
 *   2. sbo_free_obj_slots(box, slot)
 *   3. unlock
 *
 * sbo_data_ptr(metaptr, data_base, obj_offset):
 *   // meta/data 分离, data_base = data 区起始
 *   unit = obj_offset / 8
 *   node = root; cur_level = node.objlevel; phys_off = 0
 *   while cur_level >= 1:
 *       slot = (unit >> ((cur_level-1)*N_BITS)) & (N-1)
 *       phys_off += slot * 8 * N^(node.objlevel - 1)
 *       if node.slots[slot].state == OBJ_START: break
 *       node = sbo_child(meta, node, node.children[slot])
 *       cur_level--
 *   return data_base + phys_off
 */


/* ================================================================
 * 4. 碎片化处理（无 compact）
 * ================================================================
 *
 * alloc 路径:
 *   1. 边界分配 (box_boundary / obj_boundary)  ← O(1) 首选
 *   2. 失败 → 从边界向外扫描 free_bitmap       ← O(N) 次选
 *   3. 仍失败 → 向上传播容量不足
 *
 * free 路径:
 *   1. 标记 FREE, bitmap_set
 *   2. 不调整边界 (碎片留待后续 alloc 扫描吸收)
 *
 * N=64 的 64-slot 容量使 left-box/right-obj 的边界分配命中率很高，
 * 短暂碎片被统计稀释，无需 compact。
 */


/* ================================================================
 * 5. 关键不变量
 * ================================================================
 *
 * 1. 左 box 右 obj:
 *    alloc_box_slot 始终向右扩展 box_boundary
 *    alloc_obj_slots 始终向左收缩 obj_boundary
 *    新分配不会在空闲区中间"挖洞"
 *
 * 2. free_bitmap 与 slots 一致:
 *    bitmap_get(fb, i) == 1  ⇔  slots[i].state == FREE
 *
 * 3. 容量 hint 正确:
 *    sbo_box_and_child_max_cap 在 alloc/free 后更新并向上传播
 */

#endif /* SBO_DOC_H */
