/*
 * slotsboxobj.h — N=64 left-box/right-obj 内存分配器 (header-only)
 *
 * 严格遵循 doc.h 模型：子 box 从左侧分配，对象从右侧分配，空闲居中。
 * 无 compact — N=64 容量统计吸收碎片。
 *
 * 模型: L0=8B(无slot), L1=512B(64×8B), L2=32KB, L3=2MB, L4=128MB, ...
 * 每 box 固定 64 个 slot。freebitmap = uint8_t[8]。
 *
 * SHM 兼容：零指针，纯 block-id + offset。
 * 并发：per-slot spinlock (64B padded)，trylock 轮询。
 */

#ifndef SLOTSBOXOBJ_H
#define SLOTSBOXOBJ_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>

/* ================================================================
 * 常量
 * ================================================================ */

#define SBO_N         64
#define SBO_N_BITS    6
#define SBO_N_MASK    0x3F
#define SBO_MULT_MAX  63
#define SBO_BITMAP_B  (SBO_N / 8)
#define SBO_MAX_ROOT  64
#define SBO_MAGIC     "slotsboxobj64v2"

/* ================================================================
 * obj_usage — 8 × 64^(level-1) × multiple bytes (level≥1)
 * ================================================================ */

typedef struct {
    uint8_t level;
    uint8_t multiple;
} __attribute__((packed)) sbo_usage_t;

static inline uint64_t sbo_pow64(uint32_t lvl) {
    return (uint64_t)1 << (lvl * SBO_N_BITS);
}

static inline uint32_t sbo_log64(uint64_t n) {
    return (uint32_t)((63 - __builtin_clzll(n)) / SBO_N_BITS);
}

static inline uint64_t sbo_offset_raw(uint8_t level, uint8_t slot) {
    return (uint64_t)slot << ((level - 1) * SBO_N_BITS + 3);
}

static inline uint64_t sbo_offset_of(sbo_usage_t u) {
    if (u.level == 0) return 8;
    return (uint64_t)u.multiple << ((u.level - 1) * SBO_N_BITS + 3);
}

static inline int sbo_usage_cmp(sbo_usage_t a, sbo_usage_t b) {
    uint16_t pa = (uint16_t)((a.level << 6) | a.multiple);
    uint16_t pb = (uint16_t)((b.level << 6) | b.multiple);
    return (int)pa - (int)pb;
}

static inline sbo_usage_t sbo_align_to(uint64_t n_slots) {
    sbo_usage_t r = {0, 0};
    if (n_slots == 0) return r;
    if (n_slots <= SBO_MULT_MAX) { r.level = 1; r.multiple = (uint8_t)n_slots; return r; }
    uint8_t k = 1;
    uint64_t limit = SBO_MULT_MAX;
    while (n_slots > limit) { k++; limit = (uint64_t)SBO_MULT_MAX * sbo_pow64(k - 1); }
    uint64_t base = sbo_pow64(k - 1);
    r.level = k;
    r.multiple = (uint8_t)((n_slots + base - 1) / base);
    return r;
}

/* ================================================================
 * Spinlock (cache-line padded)
 * ================================================================ */

typedef struct { atomic_int lock; uint8_t _pad[60]; } sbo_lock_t;

static inline void sbo_lock(sbo_lock_t *lk) {
    int e = 0;
    while (!atomic_compare_exchange_weak(&lk->lock, &e, 1)) {
        e = 0;
#ifdef __x86_64__
        __asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__)
        __asm__ __volatile__("yield" ::: "memory");
#endif
    }
}
static inline bool sbo_trylock(sbo_lock_t *lk) { int e = 0; return atomic_compare_exchange_weak(&lk->lock, &e, 1); }
static inline void sbo_unlock(sbo_lock_t *lk) { atomic_store(&lk->lock, 0); }

/* ================================================================
 * Slot / Box 状态
 * ================================================================ */

typedef enum { SBO_FREE = 0, SBO_BOX = 1, SBO_OBJ_START = 2, SBO_OBJ_CONT = 3 } sbo_state_e;

typedef struct { uint8_t state : 2; uint8_t _rsv : 6; } __attribute__((packed)) sbo_slot_t;

/* ================================================================
 * box_head_t — N=64 树节点 (packed, ~600B with bitmap+children)
 * ================================================================ */

typedef struct {
    uint8_t  state;               // SBO_BOX
    uint8_t  max_obj_cap;         // 最大连续空闲 [0,64]
    uint8_t  slot_id;             // root slot
    int32_t  parent;              // 父 block id (0=root, -1=无)
    uint8_t  objlevel;            // box level (≥1)
    uint8_t  _pad0;

    uint8_t  box_boundary;        // 子 box 右边界: boxes ∈ [0, box_b)
    uint8_t  obj_boundary;        // 对象左边界:   objs  ∈ (obj_b, 63]
    uint8_t  _pad1[2];

    sbo_usage_t child_max_cap;    // 子树容量 hint
    uint8_t  free_bitmap[SBO_BITMAP_B]; // bit=1→FREE
    sbo_slot_t slots[64];
    int32_t    children[64];       // 子 box block id (-1=无)
} __attribute__((packed)) sbo_box_t;

/* ================================================================
 * box_meta_t — 全局元信息（base + 后跟 slot_pools + slot_locks）
 * ================================================================ */

typedef struct {
    uint8_t  magic[16];
    uint64_t head_size;
    uint64_t data_size;
    uint64_t slot_bytes;
    uint64_t per_slot_meta;
    uint8_t  root_slots;
    uint8_t  _pad[7];
    uint16_t block_stride;
    uint8_t  sizeof_block_head;
    uint8_t  _pad2[5];
    // 后跟: blocks_meta_t slot_pools[root_slots] + sbo_lock_t slot_locks[root_slots]
} sbo_meta_t;

#define SBO_META_BASE sizeof(sbo_meta_t)

/* ================================================================
 * Public API
 * ================================================================ */

int      sbo_init(void *metaptr, size_t head_size, size_t data_size);
uint64_t sbo_alloc(void *metaptr, size_t size);
void     sbo_free(void *metaptr, uint64_t obj_offset);
uint64_t sbo_allocated_size(void *metaptr, uint64_t obj_offset);
void    *sbo_data_ptr(void *metaptr, void *data_base, uint64_t obj_offset);
size_t   sbo_meta_size(size_t data_size, size_t per_slot_pool);

#endif

#ifdef SLOTSBOXMALLOC_IMPLEMENTATION

#include <blockmalloc/blockmalloc.h>

/* ================================================================
 * free_bitmap helpers (byte-oriented)
 * ================================================================ */

static inline bool sbo_bm_get(uint8_t *bm, int i) {
    return (bm[i >> 3] >> (i & 7)) & 1;
}
static inline void sbo_bm_set(uint8_t *bm, int i) {
    bm[i >> 3] |= (uint8_t)(1 << (i & 7));
}
static inline void sbo_bm_clear(uint8_t *bm, int i) {
    bm[i >> 3] &= (uint8_t)~(1 << (i & 7));
}
static inline bool sbo_bm_test_range(uint8_t *bm, int start, int count) {
    for (int i = start; i < start + count; i++)
        if (!sbo_bm_get(bm, i)) return false;
    return true;
}
static inline void sbo_bm_fill(uint8_t *bm) {
    memset(bm, 0xFF, SBO_BITMAP_B);
}
static inline void sbo_bm_clear_range(uint8_t *bm, int start, int count) {
    for (int i = start; i < start + count; i++) sbo_bm_clear(bm, i);
}
static inline void sbo_bm_set_range(uint8_t *bm, int start, int count) {
    for (int i = start; i < start + count; i++) sbo_bm_set(bm, i);
}
static inline int sbo_bm_popcount(uint8_t *bm) {
    int n = 0;
    for (int i = 0; i < SBO_N; i++) if (sbo_bm_get(bm, i)) n++;
    return n;
}

/* ================================================================
 * 内部 helper — meta 区布局:
 *   [sbo_meta_t][N×blocks_meta_t][N×sbo_lock_t][slot0_pool][slot1_pool]...
 * ================================================================ */

static inline blocks_meta_t *sbo_pool_meta(sbo_meta_t *meta, uint8_t si) {
    return (blocks_meta_t *)((uint8_t *)(meta + 1)) + si;
}
static inline sbo_lock_t *sbo_pool_lock(sbo_meta_t *meta, uint8_t si) {
    return (sbo_lock_t *)((uint8_t *)(meta + 1) + meta->root_slots * sizeof(blocks_meta_t)) + si;
}
static inline void *sbo_pool_mem(sbo_meta_t *meta, uint8_t si) {
    return (uint8_t *)(meta + 1)
           + meta->root_slots * (sizeof(blocks_meta_t) + sizeof(sbo_lock_t))
           + si * meta->per_slot_meta;
}
static inline sbo_box_t *sbo_child(sbo_meta_t *meta, sbo_box_t *box, int32_t cid) {
    if (cid < 0) return NULL;
    return (sbo_box_t *)((uint8_t *)sbo_pool_mem(meta, box->slot_id)
           + cid * meta->block_stride + meta->sizeof_block_head);
}
static inline int32_t sbo_self_id(sbo_meta_t *meta, sbo_box_t *box) {
    return blockid_bydataoffset(sbo_pool_meta(meta, box->slot_id),
           (uint8_t *)box - (uint8_t *)sbo_pool_mem(meta, box->slot_id));
}

/* ================================================================
 * 容量查询
 * ================================================================ */

static inline uint8_t sbo_continuous_max(sbo_box_t *box) {
    uint8_t cur = 0, best = 0;
    for (int i = 0; i < SBO_N; i++) {
        if (sbo_bm_get(box->free_bitmap, i)) cur++;
        else { if (cur > best) best = cur; cur = 0; }
    }
    return cur > best ? cur : best;
}

static inline sbo_usage_t sbo_box_max_cap(sbo_box_t *box) {
    if (box->max_obj_cap == 0) return (sbo_usage_t){0, 0};
    if (box->max_obj_cap == 64) return (sbo_usage_t){box->objlevel + 1, 1};
    return (sbo_usage_t){box->objlevel, box->max_obj_cap};
}

static inline sbo_usage_t sbo_box_and_child_max_cap(sbo_box_t *box) {
    sbo_usage_t own = sbo_box_max_cap(box);
    return sbo_usage_cmp(own, box->child_max_cap) >= 0 ? own : box->child_max_cap;
}

/* ================================================================
 * 容量向上传播
 * ================================================================ */

static inline void sbo_update_parent(sbo_meta_t *meta, sbo_box_t *box,
                                      bool slot_c, bool child_c) {
    if (slot_c) {
        uint8_t nc = sbo_continuous_max(box);
        if (box->max_obj_cap != nc) box->max_obj_cap = nc; else slot_c = false;
    }
    if (child_c) {
        sbo_usage_t nm = {0, 0};
        for (int i = 0; i < SBO_N; i++) {
            if (box->slots[i].state != SBO_BOX) continue;
            sbo_box_t *ch = sbo_child(meta, box, box->children[i]);
            if (!ch) continue;
            sbo_usage_t cm = sbo_box_and_child_max_cap(ch);
            if (sbo_usage_cmp(cm, nm) > 0) nm = cm;
        }
        if (sbo_usage_cmp(nm, box->child_max_cap) != 0) box->child_max_cap = nm;
        else child_c = false;
    }
    if ((slot_c || child_c) && box->parent > 0) {
        sbo_box_t *p = sbo_child(meta, box, box->parent);
        if (p) sbo_update_parent(meta, p, slot_c, child_c);
    }
}

/* ================================================================
 * box_format — 初始化 box
 * ================================================================ */

static inline void sbo_box_format(sbo_meta_t *meta, sbo_box_t *box,
                                   uint8_t objlevel, int32_t parent_id) {
    box->state = SBO_BOX;
    box->objlevel = objlevel;
    box->parent = parent_id;
    box->slot_id = 0;
    box->box_boundary = 0;
    box->obj_boundary = SBO_N - 1;
    box->max_obj_cap = SBO_N;
    sbo_bm_fill(box->free_bitmap);
    for (int i = 0; i < SBO_N; i++) {
        box->slots[i].state = SBO_FREE;
        box->children[i] = -1;
    }
    box->child_max_cap = (sbo_usage_t){objlevel + 1, 1};
}

/* ================================================================
 * alloc_box_slot — 从左侧分配子 box slot
 * ================================================================ */

static inline int8_t sbo_alloc_box_slot(sbo_meta_t *meta, sbo_box_t *box) {
    for (int p = box->box_boundary; p <= box->obj_boundary; p++) {
        if (sbo_bm_get(box->free_bitmap, p)) {
            sbo_bm_clear(box->free_bitmap, p);
            box->slots[p].state = SBO_BOX;
            box->box_boundary = (uint8_t)(p + 1);
            return (int8_t)p;
        }
    }
    return -1;
}

/* ================================================================
 * alloc_obj_slots — 从右侧分配 m 个连续对象 slot
 * ================================================================ */

static inline int8_t sbo_alloc_obj_slots(sbo_meta_t *meta, sbo_box_t *box, uint8_t m) {
    if (m == 0 || m > SBO_MULT_MAX) return -1;
    // 从 N-1 向左扫描全范围: 覆盖 free 产生的右侧空洞
    for (int p = SBO_N - 1; p >= box->box_boundary; p--) {
        int start = p - m + 1;
        if (start < box->box_boundary) break;
        if (sbo_bm_test_range(box->free_bitmap, start, m)) {
            sbo_bm_clear_range(box->free_bitmap, start, m);
            box->slots[start].state = SBO_OBJ_START;
            for (int j = start + 1; j < start + m; j++)
                box->slots[j].state = SBO_OBJ_CONT;
            box->obj_boundary = (uint8_t)(start - 1);
            uint8_t nc = sbo_continuous_max(box);
            if (box->max_obj_cap != nc) {
                box->max_obj_cap = nc;
                if (box->parent > 0) {
                    sbo_box_t *pp = sbo_child(meta, box, box->parent);
                    if (pp) sbo_update_parent(meta, pp, false, true);
                }
            }
            return (int8_t)start;
        }
    }
    return -1;
}

/* ================================================================
 * free_obj_slots — 释放对象 slot（不调边界）
 * ================================================================ */

static inline void sbo_free_obj_slots(sbo_meta_t *meta, sbo_box_t *box, uint8_t start) {
    int p = start;
    box->slots[p].state = SBO_FREE;
    sbo_bm_set(box->free_bitmap, p);
    p++;
    while (p < SBO_N && box->slots[p].state == SBO_OBJ_CONT) {
        box->slots[p].state = SBO_FREE;
        sbo_bm_set(box->free_bitmap, p);
        p++;
    }
    uint8_t nc = sbo_continuous_max(box);
    if (box->max_obj_cap != nc) {
        box->max_obj_cap = nc;
        if (box->parent > 0) {
            sbo_box_t *parent = sbo_child(meta, box, box->parent);
            if (parent) sbo_update_parent(meta, parent, false, true);
        }
    }
}

/* ================================================================
 * sbo_find_alloc — 递归树下降分配
 * ================================================================ */

#define SBO_ALLOC_FAIL ((uint64_t)-1)

static inline uint64_t sbo_find_alloc(sbo_meta_t *meta, sbo_box_t *box,
                                       sbo_box_t *parent, sbo_usage_t usage) {
    if (!box || box->state != SBO_BOX) return SBO_ALLOC_FAIL;

    if (usage.level == box->objlevel) {
        int8_t slot = sbo_alloc_obj_slots(meta, box, usage.multiple);
        if (slot < 0) return SBO_ALLOC_FAIL;
        return sbo_offset_raw(box->objlevel, (uint8_t)slot);
    }

    if (usage.level < box->objlevel) {
        // 先遍历已有子 box
        for (int i = 0; i < SBO_N; i++) {
            if (box->children[i] < 0) continue;
            sbo_box_t *ch = sbo_child(meta, box, box->children[i]);
            if (!ch) continue;
            if (sbo_usage_cmp(sbo_box_and_child_max_cap(ch), usage) < 0) continue;
            uint64_t sub = sbo_find_alloc(meta, ch, box, usage);
            if (sub != SBO_ALLOC_FAIL)
                return sbo_offset_raw(box->objlevel, (uint8_t)i) + sub;
        }
        // 无已有子 box 满足 → 从左侧分配新子 box slot
        int8_t slot = sbo_alloc_box_slot(meta, box);
        if (slot < 0) return SBO_ALLOC_FAIL;

        int64_t cid = blocks_alloc(sbo_pool_meta(meta, box->slot_id),
                                   sbo_pool_mem(meta, box->slot_id));
        if (cid < 0) {
            box->slots[slot].state = SBO_FREE;
            sbo_bm_set(box->free_bitmap, slot);
            return SBO_ALLOC_FAIL;
        }
        box->children[slot] = (int32_t)cid;
        sbo_box_t *ch = sbo_child(meta, box, cid);
        sbo_box_format(meta, ch, box->objlevel - 1, sbo_self_id(meta, box));
        ch->slot_id = box->slot_id;

        uint8_t nc = sbo_continuous_max(box);
        if (box->max_obj_cap != nc) {
            box->max_obj_cap = nc;
            if (parent) sbo_update_parent(meta, parent, true, false);
        }

        sbo_usage_t ch_max = {ch->objlevel + 1, 1};
        if (sbo_usage_cmp(ch_max, usage) >= 0) {
            uint64_t sub = sbo_find_alloc(meta, ch, box, usage);
            if (sub != SBO_ALLOC_FAIL)
                return sbo_offset_raw(box->objlevel, (uint8_t)slot) + sub;
        }
    }
    return SBO_ALLOC_FAIL;
}

/* ================================================================
 * sbo_find_obj_node — offset → (box, slot_index)
 * 无 compact: physical == virtual, 直接 6-bit 解码
 * ================================================================ */

static inline sbo_box_t *sbo_find_obj_node(sbo_meta_t *meta, uint64_t obj_offset,
                                            uint8_t *out_slot, uint8_t *out_root) {
    uint64_t unit = obj_offset >> 3;
    uint8_t si = (uint8_t)(obj_offset / meta->slot_bytes);
    if (si >= meta->root_slots) return NULL;
    *out_root = si;

    sbo_box_t *node = (sbo_box_t *)((uint8_t *)sbo_pool_mem(meta, si)
                     + meta->sizeof_block_head);
    if (!node || node->state != SBO_BOX) return NULL;

    int level = node->objlevel;
    while (level >= 1) {
        uint8_t slot = (uint8_t)((unit >> ((level - 1) * SBO_N_BITS)) & SBO_N_MASK);
        if (node->slots[slot].state == SBO_OBJ_START) { *out_slot = slot; return node; }
        if (node->slots[slot].state == SBO_BOX && node->children[slot] >= 0) {
            node = sbo_child(meta, node, node->children[slot]);
            level--;
            continue;
        }
        return NULL;
    }
    return NULL;
}

/* ================================================================
 * Public API
 * ================================================================ */

int sbo_init(void *metaptr, size_t head_size, size_t data_size) {
    if (!metaptr || data_size % 8 != 0) return -1;
    sbo_meta_t *meta = (sbo_meta_t *)metaptr;
    if (memcmp(meta->magic, SBO_MAGIC, sizeof(SBO_MAGIC) - 1) == 0) return -1;

    // data_size 必须 = 8 × 64^k (k ≥ 0)
    uint64_t slots = data_size / 8;
    if (slots == 0 || (slots & (slots - 1)) != 0) return -1;       // 非 2 的幂
    if (__builtin_ctzll(slots) % SBO_N_BITS != 0) return -1;        // 非 64 的幂

    sbo_usage_t rounded = sbo_align_to(slots);

    meta->head_size = head_size;
    meta->data_size = data_size;

    uint8_t root_slots = rounded.multiple;
    size_t header_base = sizeof(sbo_meta_t)
                       + (size_t)root_slots * (sizeof(blocks_meta_t) + sizeof(sbo_lock_t));
    size_t per_slot_meta = (head_size - header_base) / root_slots;
    meta->root_slots = root_slots;
    meta->slot_bytes = data_size / root_slots;
    meta->per_slot_meta = per_slot_meta;

    for (uint8_t si = 0; si < root_slots; si++) {
        void *pool_mem = sbo_pool_mem(meta, si);
        blocks_init(sbo_pool_meta(meta, si), per_slot_meta, sizeof(sbo_box_t));

        int64_t rid = blocks_alloc(sbo_pool_meta(meta, si), pool_mem);
        if (rid < 0) return -1;

        sbo_box_t *root = (sbo_box_t *)((uint8_t *)pool_mem
                          + blockdata_offset(sbo_pool_meta(meta, si), rid));
        sbo_box_format(meta, root, rounded.level - 1, 0);
        root->slot_id = si;
    }

    meta->sizeof_block_head = (uint8_t)sbo_pool_meta(meta, 0)->sizeof_block_head;
    meta->block_stride = (uint16_t)(meta->sizeof_block_head + sizeof(sbo_box_t));

    memset(meta->magic, 0, sizeof(meta->magic));
    memcpy(meta->magic, SBO_MAGIC, sizeof(SBO_MAGIC) - 1);
    return 0;
}

uint64_t sbo_alloc(void *metaptr, size_t size) {
    if (!metaptr || size == 0) return SBO_ALLOC_FAIL;
    sbo_meta_t *meta = (sbo_meta_t *)metaptr;
    if (memcmp(meta->magic, SBO_MAGIC, sizeof(SBO_MAGIC) - 1) != 0)
        return SBO_ALLOC_FAIL;

    sbo_usage_t usage = sbo_align_to((size + 7) / 8);
    int start = (int)((size ^ ((uint64_t)&size >> 4)) % meta->root_slots);

    for (int t = 0; t < meta->root_slots; t++) {
        int si = (start + t) % meta->root_slots;
        if (!sbo_trylock(sbo_pool_lock(meta, si))) continue;

        sbo_box_t *root = (sbo_box_t *)((uint8_t *)sbo_pool_mem(meta, si)
                          + meta->sizeof_block_head);
        if (sbo_usage_cmp(sbo_box_and_child_max_cap(root), usage) >= 0) {
            uint64_t off = sbo_find_alloc(meta, root, NULL, usage);
            if (off != SBO_ALLOC_FAIL) {
                uint64_t total = (uint64_t)si * meta->slot_bytes + off;
                if (total + sbo_offset_of(usage) <= meta->data_size) {
                    sbo_unlock(sbo_pool_lock(meta, si));
                    return total;
                }
                uint8_t us = 0, ur = 0;
                sbo_box_t *n = sbo_find_obj_node(meta, total, &us, &ur);
                if (n) { n->slots[us].state = SBO_FREE; sbo_bm_set(n->free_bitmap, us); }
            }
        }
        sbo_unlock(sbo_pool_lock(meta, si));
    }
    return SBO_ALLOC_FAIL;
}

void sbo_free(void *metaptr, uint64_t obj_offset) {
    if (!metaptr) return;
    sbo_meta_t *meta = (sbo_meta_t *)metaptr;
    uint8_t root_si = (uint8_t)(obj_offset / meta->slot_bytes);
    if (root_si >= meta->root_slots) return;

    sbo_lock(sbo_pool_lock(meta, root_si));
    uint8_t slot = 0, out_r = 0;
    sbo_box_t *node = sbo_find_obj_node(meta, obj_offset, &slot, &out_r);
    if (!node) { sbo_unlock(sbo_pool_lock(meta, root_si)); return; }
    sbo_free_obj_slots(meta, node, slot);
    sbo_unlock(sbo_pool_lock(meta, root_si));
}

uint64_t sbo_allocated_size(void *metaptr, uint64_t obj_offset) {
    if (!metaptr) return 0;
    sbo_meta_t *meta = (sbo_meta_t *)metaptr;
    uint8_t root_si = (uint8_t)(obj_offset / meta->slot_bytes);
    if (root_si >= meta->root_slots) return 0;

    sbo_lock(sbo_pool_lock(meta, root_si));
    uint8_t slot = 0, out_r = 0;
    sbo_box_t *node = sbo_find_obj_node(meta, obj_offset, &slot, &out_r);
    if (!node) { sbo_unlock(sbo_pool_lock(meta, root_si)); return 0; }

    uint8_t count = 1;
    for (int i = slot + 1; i < SBO_N && node->slots[i].state == SBO_OBJ_CONT; i++) count++;

    sbo_usage_t u;
    if (count == 64) { u.level = node->objlevel + 1; u.multiple = 1; }
    else             { u.level = node->objlevel;     u.multiple = count; }
    uint64_t r = sbo_offset_of(u);
    sbo_unlock(sbo_pool_lock(meta, root_si));
    return r;
}

/* sbo_data_ptr: meta/data 分离。data_base = data 区起始地址。
 * 无 compact → physical==virtual, 直接 data_base + offset。 */
void *sbo_data_ptr(void *metaptr, void *data_base, uint64_t obj_offset) {
    (void)metaptr;
    if (!data_base) return NULL;
    return (uint8_t *)data_base + obj_offset;
}

size_t sbo_meta_size(size_t data_size, size_t per_slot_pool) {
    sbo_usage_t r = sbo_align_to(data_size / 8);
    return sizeof(sbo_meta_t)
           + (size_t)r.multiple * (sizeof(blocks_meta_t) + sizeof(sbo_lock_t) + per_slot_pool);
}

#endif /* SLOTSBOXMALLOC_IMPLEMENTATION */
