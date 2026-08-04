#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

#include <blockmalloc/blockmalloc.h>

#include <boxmalloc/boxmalloc.h>
#include "obj_usage.h"
#include "logutil.h"
#include "lock.h"
#include "box.h"

static int check_magic(box_meta_t *meta) {
    if (memcmp(meta->magic, BOX_MAGIC, sizeof(BOX_MAGIC)-1) != 0) {
        return -1;
    }
    return 0;
}

static void box_format(box_meta_t *meta, box_head_t *node, uint8_t objlevel, uint8_t avliable_slot, int32_t parent_id);
static box_head_t *find_obj_node(box_meta_t *meta, const uint64_t obj_offset, uint8_t *out_slot_index);

int box_init(void *metaptr, const size_t boxhead_bytessize, const size_t box_bytessize)
{
    if(check_magic((box_meta_t *)metaptr) == 0) {
        LOG("[ERROR] box_meta_t already initialized");
        return -1;
    }

    if (box_bytessize % 8 != 0)
    {
        LOG("[ERROR] box_bytessize must be aligned to 8. Given size: %zu", box_bytessize);
        return -1;
    }
    obj_usage rounded_size_t = align_to(box_bytessize / 8);
    if (box_bytessize != obj_offset(rounded_size_t))
    {
        LOG("[ERROR] box_bytessize must be aligned to 16. Given size: %zu", box_bytessize);
        return -1;
    }
    box_meta_t *meta = metaptr;
    *meta = (box_meta_t){
        .boxhead_bytessize = boxhead_bytessize,
        .box_bytessize = box_bytessize,
    };

    uint8_t root_slots = rounded_size_t.multiple;
    size_t per_slot_meta_bytes = (boxhead_bytessize - sizeof(box_meta_t)) / root_slots;

    // 用 slot 0 分配 root 节点来计算 avliable_slot 和 slot_bytes
    uint8_t tmp_mem[4096];
    blocks_meta_t tmp_blocks;
    blocks_init(&tmp_blocks, sizeof(tmp_mem), sizeof(box_head_t));
    int64_t tmp_id = blocks_alloc(&tmp_blocks, tmp_mem);
    box_head_t *tmp_root = (box_head_t *)(tmp_mem + blockdata_offset(&tmp_blocks, tmp_id));
    box_format(meta, tmp_root, rounded_size_t.level, root_slots, -1);

    meta->root_slots = tmp_root->avliable_slot;
    meta->slot_bytes = box_bytessize / meta->root_slots;
    meta->per_slot_meta = per_slot_meta_bytes;

    for (uint8_t si = 0; si < meta->root_slots; si++) {
        void *slot_pool = (void*)meta + sizeof(box_meta_t) + si * per_slot_meta_bytes;
        blocks_init(&meta->slot_block[si], per_slot_meta_bytes, sizeof(box_head_t));

        int64_t root_cid = blocks_alloc(&meta->slot_block[si], slot_pool);
        if (root_cid < 0) { LOG("[ERROR] slot %d root alloc failed", si); return -1; }
        box_head_t *child = (box_head_t *)((uint8_t*)slot_pool +
            blockdata_offset(&meta->slot_block[si], root_cid));
        box_format(meta, child, rounded_size_t.level - 1, 16, 0);
        child->slot_id = si;
        child->parent = 0;
    }

    // 缓存 block 布局，消除热路径中 blockdata_offset 外部函数调用
    meta->sizeof_block_head = meta->slot_block[0].sizeof_block_head;
    meta->block_stride = (uint16_t)(meta->sizeof_block_head + sizeof(box_head_t));

    memset(meta->magic, 0, sizeof(meta->magic));
    memcpy(meta->magic, BOX_MAGIC, sizeof(BOX_MAGIC)-1);
    LOG("[INFO] box_init success: %d slots, %zu bytes/slot", meta->root_slots, per_slot_meta_bytes);
    return 0;
}

static uint8_t box_continuous_max(box_head_t *node)
{
    uint8_t continuous_count = 0;
    uint8_t continuous_max = 0;
    for (int i = 0; i < node->avliable_slot; i++)
    {
        if (node->used_slots[i].state == BOX_UNUSED)
        {
            continuous_count++;
        }
        else
        {
            if (continuous_count > continuous_max)
                continuous_max = continuous_count;
            continuous_count = 0;
        }
    }
    if (continuous_count > continuous_max)
        continuous_max = continuous_count;
    return continuous_max;
}

static void box_format(box_meta_t *meta, box_head_t *node, uint8_t objlevel, uint8_t avliable_slot, int32_t parent_id)
{
    node->state = BOX_FORMATTED;
    node->objlevel = objlevel;

    node->avliable_slot = avliable_slot;
    node->max_obj_capacity = avliable_slot;
    for (int i = 0; i < avliable_slot; i++)
    {
        node->used_slots[i] = (box_child_t){
            .continue_max = 16,
            .state = BOX_UNUSED,
        };
    }
    node->child_max_obj_capacity = (obj_usage){
        .level =objlevel,
        .multiple =1,
    };
    for (int i = 0; i < 16; i++)
    {
        node->childs_blockid[i] = -1;
    }

    node->parent = parent_id;
}

static obj_usage box_max_obj_capacity(box_head_t *node)
{
    if (node->max_obj_capacity > 0)
    {
        if (node->max_obj_capacity == 16)
        {
            return (obj_usage){
                .level = node->objlevel + 1,
                .multiple = 1,
            };
        }
        return (obj_usage){
            .level = node->objlevel,
            .multiple = node->max_obj_capacity,
        };
    }
    else
    {
        return node->child_max_obj_capacity;
    }
}

static obj_usage box_and_child_max_obj_capacity(box_head_t *node)
{
    obj_usage own;

    if (node->max_obj_capacity == 16)
    {
        own.level = node->objlevel + 1;
        own.multiple = 1;
    }
    else
    {
        own.level = node->objlevel;
        own.multiple = node->max_obj_capacity;
    }

    obj_usage child = node->child_max_obj_capacity;

    return compare_obj_usage(own, child) >= 0 ? own : child;
}

static void update_parent(box_meta_t *meta, box_head_t *node, bool slotstate_changed, bool slot_max_obj_capacity_changed)
{

    if (slotstate_changed)
    {
        uint8_t newcontinuous_max = box_continuous_max(node);
        if (node->max_obj_capacity != newcontinuous_max)
        {
            node->max_obj_capacity = newcontinuous_max;
        }
        else
        {
            slotstate_changed = false;
        }
    }

    if (slot_max_obj_capacity_changed)
    {
        obj_usage newmax = {.level = 0, .multiple = 0};
        box_head_t *child = NULL;
        for (int i = 0; i < node->avliable_slot; i++)
        {
            if (node->used_slots[i].state == BOX_FORMATTED)
            {
                child = SC(meta, node, node->childs_blockid[i]);
                if (!child)
                {
                    LOG("[ERROR] child node should not be NULL");
                    return;
                }
                obj_usage childmax = box_and_child_max_obj_capacity(child);
                if (compare_obj_usage(childmax, newmax) > 0)
                    newmax = childmax;
            }
        }
        int8_t changed = compare_obj_usage(newmax, node->child_max_obj_capacity);
        if (changed != 0)
        {
            node->child_max_obj_capacity = newmax;
        }

        if (node->max_obj_capacity > 0)
            slot_max_obj_capacity_changed = false;
        else if (changed == 0)
            slot_max_obj_capacity_changed = false;
    }
    if (slotstate_changed || slot_max_obj_capacity_changed)
    {
        if (node->parent > 0)
        {
            box_head_t *parent = SC(meta, node, node->parent);
            update_parent(meta, parent, slotstate_changed, slot_max_obj_capacity_changed);
        }
    }
}

static uint8_t put_slots(box_meta_t *meta, box_head_t *node, obj_usage objsize)
{
    uint8_t target_slot = 0;
    uint8_t continuous_count = 0;
    bool found = false;

    for (int i = 0; i < node->avliable_slot && !found; i++)
    {
        if (node->used_slots[i].state == BOX_UNUSED)
        {
            if (continuous_count == 0)
            {
                target_slot = i;
            }
            continuous_count++;

            if (continuous_count >= objsize.multiple)
            {
                found = true;
                break;
            }
        }
        else
        {
            continuous_count = 0;
        }
    }

    if (!found)
    {
        LOG("[ERROR] not enough continuous free slots");
        return 0xFF;
    }

    for (int i = 0; i < objsize.multiple; i++)
    {
        if (i == 0)
        {
            node->used_slots[target_slot + i].state = OBJ_START;
        }
        else
        {
            node->used_slots[target_slot + i].state = OBJ_CONTINUED;
        }
        node->used_slots[target_slot + i].continue_max = 0;
    }

    uint8_t continuous_max = box_continuous_max(node);

    if (node->max_obj_capacity != continuous_max)
    {
        node->max_obj_capacity = continuous_max;
        box_head_t *parent = SC(meta, node, node->parent);
        update_parent(meta, parent, false, true);
    }
    return target_slot;
}

#define BOX_FAILED (uint64_t)-1

static uint64_t box_find_alloc(box_meta_t *meta, box_head_t *node, box_head_t *parent, obj_usage objsize)
{
    if (!node)
    {
        LOG("[ERROR] node is NULL");
        return BOX_FAILED;
    }
    if (node->state == BOX_FORMATTED)
    {
        if (objsize.level == node->objlevel)
        {
            uint8_t target_slot = put_slots(meta, node, objsize);
            if (target_slot == 0xFF) {
                LOG("[ERROR] put_slots failed, capacity hint was stale");
                return BOX_FAILED;
            }
            uint64_t offset = obj_offset_raw(node->objlevel, target_slot);
            LOG("[INFO] allocated at level %d, slot [%d,%d],size %lu",node->objlevel, target_slot, target_slot+objsize.multiple - 1, obj_offset(objsize));

            return offset;
        }
        else if (objsize.level < node->objlevel)
        {
            box_head_t *child = NULL;
            for (int i = 0; i < node->avliable_slot; i++)
            {
                if (node->childs_blockid[i] >= 0)
                {
                    child = SC(meta, node, node->childs_blockid[i]);
                    obj_usage child_max = box_and_child_max_obj_capacity(child);
                    if (compare_obj_usage(child_max, objsize) >= 0)
                    {
                        uint64_t offset = obj_offset_raw(node->objlevel, i);
                        uint64_t target_box= box_find_alloc(meta, child, node, objsize);
                        if (target_box == BOX_FAILED)
                        {
                            LOG("[ERROR] box_find_alloc failed");
                            return BOX_FAILED;
                        }
                        return offset + target_box;
                    }
                }
                else if (node->used_slots[i].state == BOX_UNUSED)
                {
                    int64_t child_block_id = blocks_alloc(&meta->slot_block[node->slot_id], SP(meta, node));
                    if (child_block_id < 0)
                    {
                        LOG("[ERROR] failed to create box_head for child");
                        return BOX_FAILED;
                    }
                    node->childs_blockid[i] = child_block_id;

                    child = SC(meta, node, node->childs_blockid[i]);

                    int64_t cur_block_id = SN(meta, node);
                    box_format(meta, child, node->objlevel - 1, 16, cur_block_id);
                    child->slot_id = node->slot_id;

                    node->used_slots[i].state = BOX_FORMATTED;
                    node->used_slots[i].continue_max = 0;

                    uint8_t new_max = box_continuous_max(node);
                    if (node->max_obj_capacity != new_max)
                    {
                        node->max_obj_capacity = new_max;
                        if (parent) {
                            update_parent(meta, parent, true, false);
                        }
                    }

                    obj_usage child_max = (obj_usage){
                        .level = child->objlevel + 1,
                        .multiple = 1,
                    };
                    if (compare_obj_usage(child_max, objsize) >= 0)
                    {
                        uint64_t offset = obj_offset_raw(node->objlevel, i);
                        uint64_t target_box= box_find_alloc(meta, child, node, objsize);
                        if (target_box == BOX_FAILED)
                        {
                            LOG("[ERROR]:box_find_alloc failed");
                            return BOX_FAILED;
                        }
                        return offset + target_box;
                    }
                }
            }

            LOG("[ERROR] bug happen,but should not happen");
            return BOX_FAILED;
        }
    }

    LOG("[ERROR] bug happen,but should not happen");
    return BOX_FAILED;
}

static inline void* slot_pool(box_meta_t *meta, int si) {
    return (void*)meta + sizeof(box_meta_t) + si * meta->per_slot_meta;
}

uint64_t box_alloc(void *metaptr, const size_t size)
{
    if (!metaptr) { LOG("[ERROR] root null"); return BOX_FAILED; }

    obj_usage aligned_objsize = align_to((size + 8 - 1) / 8);
    box_meta_t *meta = metaptr;
    uint64_t alloc_bytes = obj_offset(aligned_objsize);

    int start = (int)((size ^ ((uint64_t)&size >> 4)) % meta->root_slots);
    for (int t = 0; t < meta->root_slots; t++) {
        int si = (start + t) % meta->root_slots;
        if (!box_trylock(&meta->slot_locks[si].lock)) continue;

        void *pool = slot_pool(meta, si);
        box_head_t *child = (box_head_t*)((uint8_t*)pool + meta->sizeof_block_head);

        if (compare_obj_usage(box_and_child_max_obj_capacity(child), aligned_objsize) >= 0) {
            uint64_t off = box_find_alloc(meta, child, NULL, aligned_objsize);
            if (off != BOX_FAILED) {
                uint64_t total = (uint64_t)si * meta->slot_bytes + off;
                if (total + alloc_bytes <= meta->box_bytessize) {
                    box_unlock(&meta->slot_locks[si].lock);
                    return total;
                }
                // oversize: 回滚本次 tree alloc
                uint8_t usi = 0;
                box_head_t *n = find_obj_node(meta, total, &usi);
                if (n) { n->used_slots[usi].state = BOX_UNUSED; }
            }
        }
        box_unlock(&meta->slot_locks[si].lock);
    }
    return BOX_FAILED;
}

static box_head_t *find_obj_node(box_meta_t *meta, const uint64_t obj_offset, uint8_t *out_slot_index)
{
    uint64_t unit_offset = obj_offset / 8;

    uint8_t si = (uint8_t)(obj_offset / meta->slot_bytes);
    if (si >= meta->root_slots) return NULL;

    void *pool = slot_pool(meta, si);
    box_head_t *node = (box_head_t*)((uint8_t*)pool + meta->sizeof_block_head);
    if (!node) return NULL;

    uint8_t current_level = node->objlevel;

    while (node && node->state == BOX_FORMATTED) {
        // 16^L = 2^(4L) → 位运算替代除法和取模
        uint8_t slot_index = (unit_offset >> (current_level << 2)) & 0xF;

        if (node->used_slots[slot_index].state == OBJ_START) {
            *out_slot_index = slot_index;
            return node;
        } else if (node->used_slots[slot_index].state == BOX_FORMATTED) {
            node = (box_head_t*)((uint8_t*)pool +
                node->childs_blockid[slot_index] * meta->block_stride + meta->sizeof_block_head);
            current_level--;
        } else {
            LOG("[ERROR] bug happen,invalid state %d at slot %d, level %d",
                node->used_slots[slot_index].state, slot_index, current_level);
            return NULL;
        }
    }
    LOG("[ERROR] object+%lu not found", obj_offset);
    return NULL;
}

void box_free(void *metaptr, const uint64_t obj_offset)
{
    box_meta_t *meta = metaptr;
    uint8_t root_si = (uint8_t)(obj_offset / meta->slot_bytes);
    if (root_si >= meta->root_slots) return;
    box_lock(&meta->slot_locks[root_si].lock);

    uint8_t slot_index = 0;
    box_head_t *node = find_obj_node(meta, obj_offset, &slot_index);
    if (!node) {
        box_unlock(&meta->slot_locks[root_si].lock);
        return;
    }

    // 释放 OBJ_START + 后续 OBJ_CONTINUED
    node->used_slots[slot_index].state = BOX_UNUSED;
    node->used_slots[slot_index].continue_max = 16;
    for (int i = slot_index + 1; i < node->avliable_slot; i++) {
        if (node->used_slots[i].state == OBJ_CONTINUED) {
            node->used_slots[i].state = BOX_UNUSED;
            node->used_slots[i].continue_max = 16;
        } else break;
    }

    uint8_t new_max = box_continuous_max(node);
    if (node->max_obj_capacity != new_max) {
        node->max_obj_capacity = new_max;
        if (node->parent > 0) {
            box_head_t *parent = SC(meta, node, node->parent);
            update_parent(meta, parent, false, true);
        }
    }

    box_unlock(&meta->slot_locks[root_si].lock);
    LOG("[INFO] object+%lu freed", obj_offset);
}

uint64_t box_allocated_size(void *metaptr, const uint64_t obj_off)
{
    if (!metaptr)
        return 0;

    box_meta_t *meta = metaptr;
    uint8_t root_si = (uint8_t)(obj_off / meta->slot_bytes);
    if (root_si >= meta->root_slots) return 0;
    box_lock(&meta->slot_locks[root_si].lock);

    uint8_t slot_index = 0;
    box_head_t *node = find_obj_node(meta, obj_off, &slot_index);
    if (!node) {
        box_unlock(&meta->slot_locks[root_si].lock);
        return 0;
    }

    uint8_t count = 1;
    for (int i = slot_index + 1; i < node->avliable_slot; i++)
    {
        if (node->used_slots[i].state == OBJ_CONTINUED)
            count++;
        else
            break;
    }

    obj_usage usage;
    if (count == 16) {
        usage.level = node->objlevel + 1;
        usage.multiple = 1;
    } else {
        usage.level = node->objlevel;
        usage.multiple = count;
    }

    uint64_t result = obj_offset(usage);
    box_unlock(&meta->slot_locks[root_si].lock);
    return result;
}
