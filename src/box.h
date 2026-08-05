#ifndef BOX_H
#define BOX_H

#include <stdatomic.h>
#include <blockmalloc/blockmalloc.h>
#include "obj_usage.h"

#define MAX_ROOT_SLOTS 16

typedef struct {
    atomic_int lock;
    uint8_t _pad[60];
} padded_lock_t;

typedef struct
{
    #define BOX_MAGIC "slotsboxmalloc"
    uint8_t magic[16];
    uint64_t boxhead_bytessize;
    uint64_t box_bytessize;
    blocks_meta_t slot_block[MAX_ROOT_SLOTS];
    padded_lock_t slot_locks[MAX_ROOT_SLOTS];
    uint64_t slot_bytes;
    uint64_t per_slot_meta;
    uint8_t root_slots;
    uint16_t block_stride;
    uint8_t  sizeof_block_head;
} box_meta_t;

typedef enum
{
    BOX_UNUSED = 0,
    BOX_FORMATTED = 1,
    OBJ_START = 2,
    OBJ_CONTINUED = 3
} BoxState;

typedef struct
{
    uint8_t state : 2;
    int8_t continue_max : 6;
} __attribute__((packed)) box_child_t;

typedef struct
{
    uint8_t state : 2;
    int8_t max_obj_capacity : 6;

    uint8_t slot_id;

    int32_t parent;

    uint8_t objlevel;
    uint8_t avliable_slot;
    obj_usage child_max_obj_capacity;
    box_child_t used_slots[16];
    int32_t childs_blockid[16];

    uint8_t _pad[39];
} __attribute__((packed)) box_head_t;

#define SP(meta, node) ((void*)(meta) + sizeof(box_meta_t) + (node)->slot_id * (meta)->per_slot_meta)

#define SC(meta, node, bid) \
    ((box_head_t*)((uint8_t*)SP(meta, node) + (bid) * (meta)->block_stride + (meta)->sizeof_block_head))

#define SN(meta, node) blockid_bydataoffset(&(meta)->slot_block[(node)->slot_id], \
    (uint8_t*)(node) - (uint8_t*)SP(meta, node))

#endif // BOX_H
