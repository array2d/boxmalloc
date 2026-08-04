/*
boxmalloc: 16-ary buddy allocator for shared-memory concurrent workloads.

设计目标：OS kernel、block 设备存储分配器。被动分配，不主动整理/移动对象。
内存模型：meta 区（box_head_t 树，blockmalloc 管理）+ data 区（纯 obj 数据，无元信息）。
并发模型：root slot 分区锁（最多 16 路），每 slot 独立 blockmalloc 池（物理隔离，零跨槽缓存弹跳）。
SHM 兼容：零进程私有变量/指针，fork+mmap MAP_SHARED 开箱即用。

最小分配单元 8B，16 幂对齐：alloc(N) → 16^k × m × 8, k≥0, m∈[1,15]。
*/

#ifndef BOX_MALLOC_H
#define BOX_MALLOC_H

#include <stddef.h>
#include <stdint.h>

// box_init: 初始化分配器。metaptr 为 meta 区起始地址（调用者分配）。
// boxhead_bytessize: meta 区总字节数（含 sizeof(box_meta_t) + per-slot 池）。
// box_bytessize: data 区总字节数，必须 = 16^k × m × 8。
// 返回 0 成功，-1 失败。
int box_init(void *metaptr, const size_t boxhead_bytessize, const size_t box_bytessize);

// box_alloc: 分配 size 字节对象，返回 data 区内字节偏移。
// 失败返回 (uint64_t)-1。
// 线程安全：多线程/多进程并发安全（slot 分区锁 + per-slot 独立 blockmalloc）。
uint64_t box_alloc(void *metaptr, const size_t size);

// box_allocated_size: 查询已分配对象的实际占用字节数（向上取整到 16 幂对齐）。
uint64_t box_allocated_size(void *metaptr, const uint64_t obj_offset);

// box_free: 释放对象。obj_offset 为 box_alloc 返回的偏移。
void box_free(void *metaptr, const uint64_t obj_offset);

#endif
