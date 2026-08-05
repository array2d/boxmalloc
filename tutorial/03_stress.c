#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <stdint.h>

#include <slotsboxmalloc/slotsboxobj.h>

#define SMALL_OBJ_SIZE 8  // 小对象大小 <=8byte

int main() {
    // 初始化 slotsboxmalloc 的元数据和数据区，使用更大的内存池以支持高负载
    uint8_t *buddy = malloc(4 * 1024 * 1024);     // 元数据区，增大到4MB
    uint8_t *data = malloc(64 * 1024 * 1024);     // 数据区，增大到64MB
    if (!buddy || !data) {
        perror("Failed to allocate memory for slotsboxmalloc");
        return 1;
    }

    // 初始化 slotsboxmalloc
    if (sbo_init(buddy, 4 * 1024 * 1024, 64 * 1024 * 1024) != 0) {
        printf("Failed to initialize slotsboxmalloc\n");
        free(buddy);
        free(data);
        return 1;
    }

    // 第一阶段：分配小对象直到写满
    printf("Phase 1: Allocating small objects until full...\n");
    int capacity = 1000000;  // 初始容量，动态扩展
    void **ptrs = malloc(capacity * sizeof(void *));
    if (!ptrs) {
        perror("Failed to allocate ptrs array");
        free(buddy);
        free(data);
        return 1;
    }

    int alloc_count = 0;
    while (1) {
        uint64_t obj_offset = sbo_alloc(buddy,  SMALL_OBJ_SIZE);
        if (obj_offset == (uint64_t)-1) {
            break;  // 分配失败，box已满
        }
        if (alloc_count >= capacity) {
            capacity *= 2;
            ptrs = realloc(ptrs, capacity * sizeof(void *));
            if (!ptrs) {
                perror("Failed to realloc ptrs array");
                free(buddy);
                free(data);
                return 1;
            }
        }
        ptrs[alloc_count] = data + obj_offset;
        *(uint64_t *)ptrs[alloc_count] = alloc_count;  // 写入数据
        alloc_count++;
    }
    printf("Allocated %d small objects\n", alloc_count);

    // 第二阶段：无限循环随机free和malloc
    printf("Phase 2: Starting random free/malloc loop (2s limit)...\n");
    srand(time(NULL));  // 初始化随机种子
    long long loop_count = 0;
    clock_t start = clock();

    while (1) {
        if (loop_count > 0 && (loop_count % 5000 == 0) && (double)(clock() - start) / CLOCKS_PER_SEC > 2.0) break;
        // 随机选择一个已分配的对象
        int random_index = rand() % alloc_count;
        void *ptr_to_free = ptrs[random_index];

        // Free 随机对象
        uint64_t offset = (uint8_t *)ptr_to_free - data;
        sbo_free(buddy, offset);

        // 立即 Malloc 一个新的小对象
        uint64_t new_offset = sbo_alloc(buddy, SMALL_OBJ_SIZE);
        if (new_offset == (uint64_t)-1) {
            printf("Re-allocation failed at loop %lld\n", loop_count);
            break;  // 如果分配失败，退出循环
        }

        // 更新指针数组
        ptrs[random_index] =  data + new_offset;
        *(uint64_t *)ptrs[random_index] = random_index;

        loop_count++;

        // 每10000次循环输出一次统计信息
        if (loop_count % 10000 == 0) {
            clock_t current = clock();
            double elapsed = (double)(current - start) / CLOCKS_PER_SEC;
            printf("Loop %lld: Time elapsed %.2f seconds\n", loop_count, elapsed);
        }
    }

    // 清理
    for (int i = 0; i < alloc_count; i++) {
        uint64_t offset= (uint8_t *)ptrs[i] - data;
        sbo_free(buddy, offset);
    }
    free(ptrs);
    free(buddy);
    free(data);
    printf("Stress test completed after %lld loops.\n", loop_count);
    return 0;
}