# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目定位

slotsboxmalloc 是基于 16 叉伙伴系统（16-ary buddy system）的存储分配器。不是通用 malloc 替代，设计目标覆盖 OS kernel 和 block 设备的存储分配场景。依赖 blockmalloc 管理 meta 区的 box_head_t 节点。

## 构建

```bash
make
```

**注意**：slotsboxmalloc 依赖 blockmalloc 共享库。需要先 `sudo make install` blockmalloc，或设置 `LD_LIBRARY_PATH`。运行时确保 `libblockmalloc.so.1` 在链接路径中。

## 测试

```bash
# Python test runner（含耗时、exit code、CSV 报告，默认并行数=cpu_count）
python3 tutorial/test.py
python3 tutorial/test.py --filter 04          # 按名称过滤
python3 tutorial/test.py --repeat 10          # 每 case 重复 N 轮
python3 tutorial/test.py --skip-stress        # 跳过 03_stress（有无界循环）
python3 tutorial/test.py --jobs 1             # 单线程串行（调试时用）
python3 tutorial/test.py --errorexit          # 遇错即停

# 直接运行单个 case
cd build && ./tutorial/01_basic

# Sanitizer 下运行
ASAN_OPTIONS=detect_leaks=1 ./tutorial/01_basic
TSAN_OPTIONS=history_size=7 ./tutorial/04_multithread
```

tutorial case 清单：

| case | 说明 |
|------|------|
| `01_basic` | 基础 init/alloc/free |
| `02_bench` | 111 次不同大小 alloc/write/read/free |
| `03_stress` | 分配至满，然后无限随机 free/alloc 循环 |
| `04_multithread` | 多线程并发 alloc/free（**当前 FAIL：slotsboxmalloc 无锁**）|
| `05_multiprocess` | 多进程并发，共享内存 mmap（间歇 FAIL）|
| `06_multiprocess_multithread` | 多进程 × 多线程（间歇 FAIL）|

## 调试

```bash
# Valgrind 内存检查
valgrind --leak-check=full --track-origins=yes ./tutorial/01_basic

# GDB 断点调试
gdb --args ./tutorial/01_basic

# TSan 抑制假阳性（如必要的 benign race）
TSAN_OPTIONS="suppressions=tsan_suppress.txt history_size=7" ./tutorial/04_multithread
```

---

## 架构

### 16 叉伙伴树模型

slotsboxmalloc 将数据区视为一棵 16 叉树。最小分配单元 = 8 字节，按 16 的幂对齐：

```
16^0 * 8 = 8B     一个 slot
16^1 * 8 = 128B   16 个 slot
16^2 * 8 = 2KB    256 个 slot
...
16^N * 8          16^N 个 slot
```

- **level**: 树的层级，0 = 最小级别（8B/slot）
- **multiple**: 该层级占用几个连续 slot（1-15，或者占满 16 等价于 level+1, multiple=1）

### 内存布局（两块独立区域）

```
meta 区（连续）:                       data 区（独立）:
┌─────────────────────┐               ┌─────────────────┐
│ box_meta_t           │               │ obj 数据        │
│   magic[16]          │               │ （纯数据,        │
│   boxhead_bytessize  │               │  无任何元信息)   │
│   box_bytessize      │               │                 │
│   blocks_meta_t      │ ← blockmalloc  │                 │
├─────────────────────┤   管理这部分    │                 │
│ box_head_t[0] (root) │ ← block_id 0  │                 │
│ box_head_t[1]        │               │                 │
│ ...                  │               │                 │
└─────────────────────┘               └─────────────────┘
```

- `metaptr` 指向 meta 区开头（即 `&box_meta_t`）
- `boxhead = metaptr + sizeof(box_meta_t)` 是 blockmalloc 管理的 block 区域
- data 区的地址 slotsboxmalloc 不持有——调用者自行管理，box_alloc 返回的是 data 区内的**字节偏移量**

### obj_usage：size 的内部编码

```c
typedef struct {
    uint8_t level : 4;     // 0-15, 16^level * 8 字节
    uint8_t multiple : 4;  // 1-15, 连续 slot 数。multiple==0 表示无可用空间
} obj_usage;
```

关键转换函数：
- `align_to(n)`: 将 8 字节单位数 n 转换为 (level, multiple) 对
- `obj_offset(usage)`: 计算 `8 * 16^level * multiple` 字节偏移
- `compare_obj_usage(a, b)`: 先比 level，再比 multiple

**易错点**：`multiple == 0` 是哨兵值（表示 "无可用空间"），`multiple == 16` 等价于 `(level+1, multiple=1)`。

### box_head_t：树节点结构

```c
typedef struct {
    uint8_t state : 2;             // BOX_UNUSED/BOX_FORMATTED
    int8_t max_obj_capacity : 6;   // 当前节点最大连续空闲 slot 数 [0-16]

    atomic_int_fast64_t rw_lock;   // 预留锁字段（★ 未使用）

    int32_t parent;                // parent 的 block_id（-1=root）
    uint8_t objlevel;              // 当前节点所在 level
    uint8_t avliable_slot;         // 节点可用 slot 数 [2-16]
    obj_usage child_max_obj_capacity;  // 所有子节点中最大可用容量（hint）

    box_child_t used_slots[16];    // 每个 slot 的状态
    int32_t childs_blockid[16];    // 每个 slot 对应的子节点 block_id（-1=无子节点）
} box_head_t;
```

### Slot 状态机

```
BOX_UNUSED (0)  ──alloc obj──→  OBJ_START (2) + OBJ_CONTINUED (3)...
                               （obj 占 1+ 个连续 slot，首 slot=START，后续=CONTINUED）

BOX_UNUSED (0)  ──create child─→ BOX_FORMATTED (1)
                                 （此 slot 指向一个子 box_head_t 节点）
```

- `OBJ_START` 和 `OBJ_CONTINUED` 的 `continue_max` 字段无意义（设为 0）
- `BOX_UNUSED` 的 `continue_max` 是一个 hint，表示从该位置开始的连续空闲 slot 数

### offset ↔ 树位置映射

这是最容易出错的环节。`box_alloc` 返回的 offset 是 data 区内的**字节偏移**。

**offset → node + slot_index**（`find_obj_node`）：
```
unit_offset = obj_offset / 8;           // 转换为 8 字节单位
从 root 开始, current_level = root->objlevel:
  slot_index = (unit_offset / 16^current_level) % 16;
  读 used_slots[slot_index]:
    OBJ_START    → 找到了，返回此 node + slot_index
    BOX_FORMATTED → 进入子节点, current_level--
    其他         → 错误
```

**node + slot_index → offset**（用于分配时返回给调用者）：
```
offset = obj_offset({level=node->objlevel, multiple=slot_index})
       = 8 * 16^level * slot_index
```

此 offset 是相对于 data 区起始的偏移。调用者在 data 区写入：`data + offset`。

### 核心算法

#### box_alloc

```
1. size → 8 字节对齐向上取整 → align_to → obj_usage (level, multiple)
2. 检查 root 的最大容量（child_max_obj_capacity hint），不够则失败
3. box_find_alloc 递归遍历树：
   a. objsize.level == node->objlevel → put_slots 分配连续 slot
   b. objsize.level < node->objlevel → 遍历子节点:
      - 已有子节点 (BOX_FORMATTED): 检查容量 hint，不够则跳过
      - 空闲 slot (BOX_UNUSED): 创建子节点（blocks_alloc），递归进入
4. 返回 offset（可能是多层累加：parent_offset + child_offset）
```

**关键：多层 offset 累加**。当分配在子节点中完成时，返回的 offset = 当前层 slot 偏移 + 子节点内偏移。每个子节点覆盖父节点对应 slot 的地址空间（1/16）。

#### box_free

```
1. find_obj_node: offset → node + slot_index
2. 释放 slot(s): OBJ_START → BOX_UNUSED, OBJ_CONTINUED → BOX_UNUSED
3. 重新计算 max_obj_capacity（box_continuous_max）
4. 如果 max_obj_capacity 变化且 parent 存在 → update_parent 递归上传
```

#### update_parent

从子节点向上传播容量变化：
- `slotstate_changed`: 本节点 max_obj_capacity 变了 → 更新 parent 的 child_max_obj_capacity
- `slot_max_obj_capacity_changed`: 子节点的 child_max 变了 → 当前节点重新扫描所有子节点取 max → 继续上传
- 递归直到 root 或变化消失

**注意**：`update_parent` 涉及 `child_max_obj_capacity` 的重新计算——遍历所有 `BOX_FORMATTED` 的 slot，读子节点的 `box_and_child_max_obj_capacity()` 取 max。

#### box_continuous_max

扫描节点所有 slot，找最长连续 `BOX_UNUSED` 段。时间复杂度 O(avliable_slot) ≤ 16。

---

## 并发

### 当前状态：无锁

`lock.h` 定义了 `rlock`/`runlock`/`lock`/`unlock` 函数，`box_head_t.rw_lock` 字段也存在，但**代码中完全没有调用**。注释中的 "线程安全需求" 是目标设计，尚未实现。

### 并发测试结果

- 04_multithread: 稳定 FAIL，alloc 很快失败（树状态被并发破坏）
- 05_multiprocess: 间歇 FAIL（进程间竞态）
- 06_multiprocess_multithread: 间歇 FAIL

### 修复计划

根节点自旋锁（最小改动方案）：
1. `box_head_t` 已有 `rw_lock` 字段，不需要改数据结构
2. `lock.h` 已有 spinlock 函数（需要从 rwlock 简化为普通 spinlock）
3. `box_alloc` 和 `box_free` 入口处加 `spin_lock(&root->rw_lock)` / `spin_unlock`
4. 临界区极短（16 叉树深 ~7 层，50-100 cycles），一把锁完全够用

---

## 常见易错点

1. **offset 是相对 data 区的字节偏移，不是绝对地址**。调用者自己 `data + offset`。
2. **`multiple == 0` 是哨兵**（无可用空间），`multiple == 16` 应进位到 `(level+1, 1)`。
3. **`box_continuous_max` 只在 `BOX_UNUSED` 上计数**。OBJ_START/CONTINUED 和 BOX_FORMATTED 都会打断连续计数。
4. **`box_and_child_max_obj_capacity` 取自身容量和子树容量的 max**，两者可能不同（一个满但有子节点空间，或全空但无子节点）。
5. **`childs_blockid[i]` 不是指针，是 blockmalloc 的 block_id**。通过 `boxhead + blockdata_offset(&meta->blocks, id)` 获取实际指针。
6. **`avliable_slot` 不是恒定 16**。虽然新格式化的节点初始化为 16，但理论上可以更小（预留字段）。
7. **`__attribute__((packed))` 在多处使用**：`box_child_t`、`box_head_t`、`obj_usage`。修改这些结构体时必须保持 packed 且检查 sizeof。
8. **位域宽度精确匹配**：`obj_usage.level:4` 最大 15，`box_child_t.continue_max:6` 最大 63（但语义上是 0-16）。修改位域宽度时检查所有使用点的取值范围。
