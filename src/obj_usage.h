#ifndef OBJ_USAGE_H
#define OBJ_USAGE_H

#include <stdint.h>

/*
obj_usage以16为基数的幂和倍数。16 = 2^4，所有运算用位操作。
*/
typedef struct
{
    uint8_t level : 4;    // obj最大的level
    uint8_t multiple : 4; // obj最长连续可用的slots [1,15],如果==0,说明无可用
} __attribute__((packed)) obj_usage;

// 16^level = 1 << (4*level)
static inline uint64_t pow16(uint32_t level) {
    return (uint64_t)1 << (level << 2);
}

// floor(log16(n)), n >= 1（调用者保证 n >= 16）
static inline uint32_t log16(uint64_t n) {
    return (uint32_t)((63 - __builtin_clzll(n)) >> 2);
}

// 8 * 16^level * multiple = multiple << (4*level + 3)
static inline uint64_t obj_offset(obj_usage a) {
    return (uint64_t)a.multiple << (((uint32_t)a.level << 2) + 3);
}

// obj_offset variant: 直接传 level/multiple，避免构造 struct
static inline uint64_t obj_offset_raw(uint8_t level, uint8_t multiple) {
    return (uint64_t)multiple << (((uint32_t)level << 2) + 3);
}

static inline obj_usage align_to(uint64_t n)
{
    obj_usage result = {0, 0};
    if (n < 16) {
        result.multiple = (uint8_t)n;
        return result;
    }
    uint32_t lvl = log16(n);
    uint64_t base = pow16(lvl);
    uint8_t mult = (uint8_t)((n + base - 1) >> (lvl << 2));
    if (mult >= 16) {
        lvl++;
        base = pow16(lvl);
        mult = (uint8_t)((n + base - 1) >> (lvl << 2));
    }
    result.level = (uint8_t)lvl;
    result.multiple = mult;
    return result;
}

static inline int8_t compare_obj_usage(obj_usage a, obj_usage b)
{
    uint8_t pa = (uint8_t)((a.level << 4) | a.multiple);
    uint8_t pb = (uint8_t)((b.level << 4) | b.multiple);
    return (int8_t)(pa - pb);
}

#endif // OBJ_USAGE_H
