#!/usr/bin/env python3
"""
N-slots-box-obj 参数对比计算器 (v2)。
模型：
  - L0 = 8B，无 slot（原子分配单元）
  - L_k = N^k × 8B，含 N 个 slot（k≥1），每 slot = N^(k-1) × 8B
  - 每个 box 用 N-bit freebitmap 追踪 slot 占用状态
  - obj 占 [1, N-1] 个连续 slot；子 box 占 1 个 slot

输出：CSV 文件 + 终端摘要
"""

import math, csv, sys, os

def fmt_bytes(b):
    """人类可读字节数。"""
    if b < 1024:
        return f"{b}B"
    elif b < 1024 * 1024:
        return f"{b/1024:.0f}KB"
    elif b < 1024 * 1024 * 1024:
        return f"{b/(1024*1024):.1f}MB"
    else:
        return f"{b/(1024**3):.2f}GB"

def obj_level(N, size_bytes):
    """返回 size_bytes 对象应分配的 level k（k≥0）。
    L0 仅用于 8B 对象。k≥1 时找最小的 k 使得 ceil(S/8) ≤ (N-1) × N^(k-1)。"""
    if size_bytes <= 8:
        return 0
    L0_need = math.ceil(size_bytes / 8)
    for k in range(1, 64):  # safe upper bound
        slot_units = N ** (k - 1)
        max_units = (N - 1) * slot_units
        if L0_need <= max_units:
            return k
    return 64  # fallback

def obj_slots(N, size_bytes):
    """返回对象需要的连续 slot 数和所在 level。"""
    k = obj_level(N, size_bytes)
    if k == 0:
        return 1, 0  # L0: 特殊，1 个 8B 单元
    slot_units = N ** (k - 1)
    L0_need = math.ceil(size_bytes / 8)
    m = math.ceil(L0_need / slot_units)
    return m, k

def freebitmap_scan_ops(N):
    """估算在 N-bit bitmap 中找连续 m 个 0 的操作数。
    保守估计：需要遍历 bitmap 找到足够长的连续 0 段。"""
    if N <= 64:
        # tzcnt on each 64-bit word, plus check for consecutive across words
        words = math.ceil(N / 64)
        return f"tzcnt×{words}"
    elif N <= 256:
        # AVX2: load 256-bit, compare, movemask, leading-zero count
        chunks = math.ceil(N / 256)
        return f"AVX2×{chunks}"
    else:
        # AVX-512: 512-bit chunks
        chunks = math.ceil(N / 512)
        return f"AVX512×{chunks}"

def analyze(N, mem_bytes, label):
    L0_units = mem_bytes // 8
    if L0_units < 1:
        return None

    # max_level: 最大 k 满足 N^k ≤ L0_units
    if N == 1:
        max_level = 0
    else:
        max_level = int(math.floor(math.log(L0_units, N)))
    # clamp
    while (N ** max_level) > L0_units and max_level > 0:
        max_level -= 1
    while max_level < 60 and (N ** (max_level + 1)) <= L0_units:
        max_level += 1

    depth = max_level + 1
    root_level = max_level
    root_slots = L0_units // (N ** max_level) if max_level >= 0 else L0_units

    # ---- freebitmap ----
    bitmap_bits = N
    bitmap_bytes = math.ceil(N / 8)
    bitmap_cl = math.ceil(bitmap_bytes / 64)
    scan_method = freebitmap_scan_ops(N)

    # ---- 小对象 (8B–256B) ----
    SMALL_SIZES = [8, 16, 24, 32, 48, 64, 96, 128, 192, 256]
    small_levels = sorted(set(obj_level(N, s) for s in SMALL_SIZES))
    small_min_L, small_max_L = small_levels[0], small_levels[-1]
    # 从 root 降到小对象最高层的步数
    descent_small = max(0, root_level - small_max_L)
    # 小对象典型 alloc 操作: descent + freebitmap 扫描
    # 小对象典型 free 操作: descent (find_obj_node) + bitmap bit-clear
    alloc_ops_small = descent_small + math.ceil(N / 64)
    free_ops_small = descent_small + 1  # find + 1 bit clear

    # ---- 大对象 (1MB–100MB) ----
    LARGE_SIZES = [1024*1024, 2*1024*1024, 4*1024*1024,
                   8*1024*1024, 16*1024*1024, 32*1024*1024,
                   64*1024*1024, 100*1024*1024]
    large_levels = sorted(set(obj_level(N, s) for s in LARGE_SIZES))
    if large_levels:
        large_min_L, large_max_L = large_levels[0], large_levels[-1]
        descent_large = max(0, root_level - large_max_L)
    else:
        large_min_L = large_max_L = -1
        descent_large = -1

    # ---- GC 搬运量（最坏：搬 N-1 个 slot 的数据）----
    def gc_worst(level):
        """移动 level k box 的全部 N-1 slot 的数据量（bytes）。"""
        if level == 0:
            return 8  # L0: 仅 8B
        slot_bytes = 8 * (N ** (level - 1))
        return slot_bytes * (N - 1)

    # 小对象 GC 安全：小对象最高层 GC 是否 ≤10MB
    small_gc_safe = gc_worst(small_max_L) <= 10 * 1024 * 1024
    # 大对象 GC 安全
    large_gc_safe = large_max_L >= 0 and gc_worst(large_max_L) <= 10 * 1024 * 1024

    # ---- box_head_t 推算（仅 freebitmap + childs_blockid，不含 v2p/p2v 等 GC 字段）----
    base_fields = 20  # state, level, parent, slot_id, boundary fields...
    arrays_no_gc = bitmap_bytes + N * 4  # freebitmap + childs_blockid (int32_t × N)
    box_head_no_gc = base_fields + arrays_no_gc
    # 含 GC 字段（v2p + p2v）
    box_head_with_gc = box_head_no_gc + 2 * N  # +virtual_to_phys[N] + phys_to_virtual[N]

    return {
        # 基础参数
        "N": N,
        "N_bits": int(math.log2(N)) if N & (N-1) == 0 else f"~{math.log2(N):.1f}",
        "L0": "8B (无slot)",
        "L1_slot": "8B",
        "L1_box": fmt_bytes(N * 8),
        "L2_slot": fmt_bytes(8 * N),
        # freebitmap
        "freebitmap_bits": N,
        "freebitmap_B": bitmap_bytes,
        "freebitmap_CL": bitmap_cl,
        "freebitmap_scan": scan_method,
        # 树结构
        f"{label}_max_level": max_level,
        f"{label}_depth": depth,
        f"{label}_root_slots": root_slots,
        # 小对象
        "small_range": "8B-256B",
        "small_levels": f"L{small_min_L}-L{small_max_L}",
        "small_level_count": len(small_levels),
        f"small_descent_{label}": descent_small,
        f"small_alloc_ops_{label}": alloc_ops_small,
        f"small_free_ops_{label}": free_ops_small,
        "small_gc_worst": fmt_bytes(gc_worst(small_max_L)),
        "small_gc_safe": "Y" if small_gc_safe else "N",
        # 大对象
        "large_range": "1MB-100MB",
        "large_levels": f"L{large_min_L}-L{large_max_L}" if large_min_L >= 0 else "-",
        "large_level_count": len(large_levels) if large_levels else 0,
        f"large_descent_{label}": descent_large,
        "large_gc_worst": fmt_bytes(gc_worst(large_max_L)) if large_max_L >= 0 else "-",
        "large_gc_safe": "Y" if large_gc_safe else "N",
        # GC 代价详表
        "GC_L0": fmt_bytes(gc_worst(0)),
        "GC_L1": fmt_bytes(gc_worst(1)),
        "GC_L2": fmt_bytes(gc_worst(2)),
        "GC_L3": fmt_bytes(gc_worst(3)),
        # box_head_t
        "box_head_no_gc_B": box_head_no_gc,
        "box_head_with_gc_B": box_head_with_gc,
        "box_head_no_gc_CL": math.ceil(box_head_no_gc / 64),
        "box_head_with_gc_CL": math.ceil(box_head_with_gc / 64),
    }


def main():
    Ns = [2, 16, 64, 256, 1024]
    mem_configs = [(1 * 1024**3, "1GB"), (32 * 1024**3, "32GB")]

    results = []
    for N in Ns:
        row = {}
        for mem_bytes, label in mem_configs:
            r = analyze(N, mem_bytes, label)
            if r:
                row.update(r)
        results.append(row)

    # ---- 终端摘要表 ----
    header = (
        f"{'N':<5} {'1G深':<5} {'32G深':<5} "
        f"{'位图':<6} {'小obj层':<8} {'小降(1G)':<8} {'小降(32G)':<8} "
        f"{'小alloc':<8} {'小free':<7} {'小GC':<8} "
        f"{'大降(1G)':<8} {'大降(32G)':<8} {'大GC':<8} "
        f"{'boxH(B)':<8} {'boxH+GC':<8}"
    )
    print(header)
    print("-" * len(header))

    for r in results:
        line = (
            f"{r['N']:<5} {r['1GB_depth']:<5} {r['32GB_depth']:<5} "
            f"{r['freebitmap_B']:<6} {r['small_levels']:<8} "
            f"{r['small_descent_1GB']:<8} {r['small_descent_32GB']:<8} "
            f"{r['small_alloc_ops_1GB']:<8} {r['small_free_ops_1GB']:<7} "
            f"{r['small_gc_worst']:<8} "
            f"{str(r['large_descent_1GB']):<8} {str(r['large_descent_32GB']):<8} "
            f"{r['large_gc_worst']:<8} "
            f"{r['box_head_no_gc_B']:<8} {r['box_head_with_gc_B']:<8}"
        )
        print(line)

    print(f"\n列说明:")
    print(f"  N        = 每 box 的 slot 数")
    print(f"  1G深/32G深 = 总层数 (L0..L_max)")
    print(f"  位图      = freebitmap 字节数")
    print(f"  小obj层   = 8-256B 对象覆盖的 level 范围")
    print(f"  小降(1G/32G) = 小对象从 root 下降到目标层的步数")
    print(f"  小alloc   = 下降 + freebitmap 扫描 (典型操作数)")
    print(f"  小free    = 下降 + bitmap 标记 (典型操作数)")
    print(f"  小GC/大GC = 该范围内最坏 GC 数据搬运量")
    print(f"  大降(1G/32G) = 大对象(1-100MB)下降步数")
    print(f"  boxH(B)   = box_head_t 大小 (无 GC 字段)")
    print(f"  boxH+GC   = box_head_t 大小 (含 v2p+p2v)")

    # ---- 输出 CSV ----
    script_dir = os.path.dirname(os.path.abspath(__file__))
    csv_path = os.path.join(script_dir, "n_analysis.csv")
    fieldnames = [
        "N", "N_bits",
        "L0", "L1_slot", "L1_box", "L2_slot",
        "freebitmap_bits", "freebitmap_B", "freebitmap_CL", "freebitmap_scan",
        "1GB_max_level", "1GB_depth", "1GB_root_slots",
        "32GB_max_level", "32GB_depth", "32GB_root_slots",
        "small_range", "small_levels", "small_level_count",
        "small_descent_1GB", "small_descent_32GB",
        "small_alloc_ops_1GB", "small_free_ops_1GB",
        "small_alloc_ops_32GB", "small_free_ops_32GB",
        "small_gc_worst", "small_gc_safe",
        "large_range", "large_levels", "large_level_count",
        "large_descent_1GB", "large_descent_32GB",
        "large_gc_worst", "large_gc_safe",
        "GC_L0", "GC_L1", "GC_L2", "GC_L3",
        "box_head_no_gc_B", "box_head_with_gc_B",
        "box_head_no_gc_CL", "box_head_with_gc_CL",
    ]
    with open(csv_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames, extrasaction='ignore')
        writer.writeheader()
        for r in results:
            writer.writerow(r)
    print(f"\nCSV 已保存: {csv_path}")


if __name__ == "__main__":
    main()
