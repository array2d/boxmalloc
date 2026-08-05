# Slots-Box：16 叉树内存分配器模型

## 1. 核心定义

给定一段 `8 × 16^n` 字节的连续内存（`n ≥ 0`）。

- **slot**：最小分配单元，1 slot = **8 字节**，定义为 **level 0**。
- **box**：16 个连续 slot 组成的容器。一个 level L 的 box 覆盖 `16 × (8 × 16^L) = 8 × 16^(L+1)` 字节。
- **level L 的 1 个 slot** = level L-1 的 1 个完整 box = `8 × 16^L` 字节。

```
level 2  ████████████████████████████████████████████████████  1 slot = 2048B = 1 个 level-1 box
         ┌──────────────────────────────────────────────────┐
level 1  │ slot0 │ slot1 │ ... │ slot15 │                   │  16 slots, each = 128B
         │ 128B  │ 128B  │     │ 128B   │                   │
         └──────────────────────────────────────────────────┘
                         ↓ 展开 1 个 slot
         ┌──────────────────────────────────────────────────┐
level 0  │ s0 │ s1 │ ... │ s15 │                            │  16 slots, each = 8B
         │ 8B │ 8B │     │ 8B  │                            │
         └──────────────────────────────────────────────────┘
```

## 2. 层级递推

| level | 1 slot 大小 | 1 box (16 slots) 大小 |
|-------|------------|----------------------|
| 0 | 8 B | 128 B |
| 1 | 128 B | 2 KB |
| 2 | 2 KB | 32 KB |
| 3 | 32 KB | 512 KB |
| 4 | 512 KB | 8 MB |
| L | `8 × 16^L` | `8 × 16^(L+1)` |

总内存 `8 × 16^n` = level n 的 **1 个 slot**，也就是 level n-1 的 **1 个 box**。

## 3. 核心规则

### 规则 1：分配粒度

一个 obj 在 level L 上占用 **[1, 15] 个连续 slot**。

```
obj 大小 → align_to(ceil(size/8)) → (level, multiple)
                                            ↑       ↑
                                    目标层级    连续 slot 数
```

### 规则 2：16 进位（禁止占满）

> **obj 绝不允许占满一个 box 的全部 16 个 slot。**

如果 `multiple == 16`，则进位：`(level+1, multiple=1)`。

| obj 大小 | naive | 进位后 |
|----------|-------|--------|
| 128 B (16 × 8B) | level=0, multiple=16 | **level=1, multiple=1** |
| 2048 B (16 × 128B) | level=1, multiple=16 | **level=2, multiple=1** |

**推论**：在任意 level L，一个 obj 最多占用 15 个 slot。第 16 个 slot 永远不是 obj——它是上一层的一个 slot。

### 规则 3：递归细分

当一个 slot 需要被细分为更小粒度时，它变成一个新的 box（子 box，level-1），包含 16 个子 slot：

```
父 level L 的一个 slot                   子 level L-1 的 box
┌──────────────────────┐               ┌──┬──┬──┬──┬──┬──┬──┬──┐
│   一个 slot           │  ──细分──→    │  │  │  │  │  │  │  │  │  16 个子 slot
│   8 × 16^L bytes     │               └──┴──┴──┴──┴──┴──┴──┴──┘
└──────────────────────┘               each = 8 × 16^(L-1) bytes
```

## 4. Slot 状态机

```
                  ┌── alloc obj (1~15 slots) ──→ OBJ_START + OBJ_CONTINUED
BOX_UNUSED (0) ───┤
                  └── create child box ────────→ BOX_FORMATTED
```

- `BOX_UNUSED (0)`：空闲，可被分配为 obj 或细分为子 box。
- `BOX_FORMATTED (1)`：已细分为子 box，指向一个子节点。
- `OBJ_START (2)`：obj 的起始 slot。
- `OBJ_CONTINUED (3)`：obj 的后续 slot。

## 5. 分配算法

```
box_alloc(size):
  1. n = ceil(size / 8)                    // 转为 8 字节单位
  2. (level, multiple) = align_to(n)       // 计算目标层级和 slot 数
  3. 从 root box 开始递归下降：
     a. if node.level == target_level:
          扫描连续 BOX_UNUSED slot，占 multiple 个
          → 标记 OBJ_START + OBJ_CONTINUED
          → 返回 offset
     b. if node.level > target_level:
          遍历 16 个 slot：
            - 已有子 box (BOX_FORMATTED)：检查容量 hint，够则递归进入
            - 空闲 slot (BOX_UNUSED)：创建子 box → 递归进入
     c. offset = Σ(每层 slot_index × 8 × 16^level)
```

### offset 计算公式

```
offset = 8 × (slot_L × 16^L + slot_(L-1) × 16^(L-1) + ... + slot_target × 16^target)
```

即：从 root 到目标 level，每层选择 1 个 slot 向下，offset 是所有选择的加权和。

## 6. 释放算法

```
box_free(offset):
  1. offset / 8 → unit_offset
  2. 从 root 开始，逐层用位运算定位：
     level L: slot_index = (unit_offset >> (4×L)) & 0xF
  3. 找到 OBJ_START → 释放该 slot + 后续 OBJ_CONTINUED → BOX_UNUSED
  4. 向上传播容量变化到所有祖先节点
```

## 7. 关键性质

### 位运算友好

16 = 2^4，所有乘除取模化为移位和掩码：

```
16^L              = 1 << (4×L)
offset / (8×16^L) = unit_offset >> (4×L)
offset % (16^L)   = unit_offset & ((1 << (4×L)) - 1)
slot_index at L   = (unit_offset >> (4×L)) & 0xF
```

### 树深度

总内存 `8 × 16^n`，最大 level = n。树深 ≤ n+1 层。

| 内存大小 | n | 最大树深 |
|----------|---|---------|
| 128 B | 1 | 2 |
| 2 KB | 2 | 3 |
| 32 KB | 3 | 4 |
| 8 MB | 5 | 6 |
| 32 GB | 9 | 10 |

分配一次 object 只需遍历 ≤ 10 层，每层检查 ≤ 16 个 slot。

### 内部碎片

```
最坏情况：申请 1B → align_to(1) → level=0, multiple=1 → 8B, 碎片率 87.5%
大对象：  申请 16^L×8+1B → align_to(16^L+1) → level=L, multiple=2 → 16^L×16B, 碎片率趋近 50%
```

### 禁止占满的深层原因

"永远不分配 16 个 slot"这一规则保证了：

1. **每个 box 要么全是 obj slot，要么有子 box**——不会出现一个 box 16 个 slot 全是同一个 obj 而失去层级信息。
2. **offset 到树的映射是确定性的**——`find_obj_node` 用位运算逐层定位，前提是 obj 不会"吞掉"整层。
3. **alloc 和 free 的路径一致**——free 时按 offset 反查的 slot_index 序列与 alloc 时的分配路径完全对应。

## 8. 内存布局

```
┌────────────────────────────────────────────┐
│ meta 区：box_meta_t + box_head_t 节点树     │ ← 调用者提供，blockmalloc 管理
│                                            │
│   root box (level=n, m slots, m∈[1,15])   │
│   ├── slot 0 → child box (level=n-1)      │
│   │   ├── slot 0 → OBJ_START (256B obj)   │
│   │   ├── slot 1 → OBJ_CONTINUED          │
│   │   ├── slot 2 → child box (level=n-2)  │
│   │   └── ...                             │
│   ├── slot 1 → OBJ_START (512KB obj)      │
│   └── ...                                 │
├────────────────────────────────────────────┤
│ data 区：纯数据，无元信息                    │ ← 调用者管理
│                                            │
│   box_alloc 返回此区内的字节偏移              │
└────────────────────────────────────────────┘
```
