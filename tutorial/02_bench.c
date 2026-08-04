#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <boxmalloc/boxmalloc.h>

#define LOOPS 10000

int main() {
    // 120MB = 15 * 8MB → root level=5 avliable_slot=15
    size_t meta_sz = 4 * 1024 * 1024;
    size_t data_sz = 15ULL * 8 * 1024 * 1024;
    uint8_t *buddy = malloc(meta_sz);
    uint8_t *data = malloc(data_sz);
    if (box_init(buddy, meta_sz, data_sz) != 0) { printf("init failed\n"); return 1; }

    size_t sizes[] = {8, 64, 512, 4096};
    uint64_t live[4] = {0};
    int live_n = 0;
    int n_sizes = sizeof(sizes) / sizeof(sizes[0]);

    clock_t start = clock();
    for (int i = 0; i < LOOPS; i++) {
        if (live_n >= 2) {
            int idx = i % live_n;
            box_free(buddy, live[idx]);
            live[idx] = live[--live_n];
        }
        size_t sz = sizes[i % n_sizes];
        uint64_t off = box_alloc(buddy, sz);
        if (off == (uint64_t)-1) { printf("alloc failed at %d\n", i); return 1; }
        *(uint64_t *)(data + off) = i;
        if (live_n < 4) live[live_n++] = off;
    }
    for (int i = 0; i < live_n; i++) box_free(buddy, live[i]);

    clock_t end = clock();
    printf("Time taken: %f seconds\n", (double)(end - start) / CLOCKS_PER_SEC);
    free(buddy); free(data);
    return 0;
}
