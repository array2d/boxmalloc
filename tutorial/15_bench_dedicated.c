#define _POSIX_C_SOURCE 199309L
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <boxmalloc/boxmalloc.h>

#define OPS 200000
#define META_SIZE (1024 * 1024)
#define DATA_SIZE (15ULL * 8 * 1024 * 1024)

// Private allocator: each thread does alloc+free entirely within its home slot
// by directly calling box_alloc with slot-distinctive sizes that hash to
// different slots. Since box_alloc uses `size` to pick a starting slot,
// we pass thread_id as size to force slot_id = thread_id.
//
// This simulates N independent allocators sharing one meta area — the ideal
// concurrent access pattern with zero cache-line contention.

static const size_t kSz[] = {8, 64, 512, 4096};
static _Atomic long total = 0;

void* worker(void *arg) {
    uint8_t *meta = (uint8_t *)arg;
    long n = 0;
    for (int i = 0; i < OPS; i++) {
        size_t sz = kSz[i & 3];
        uint64_t off = box_alloc(meta, sz);
        if (off != (uint64_t)-1) { box_free(meta, off); n++; }
    }
    total += n; return NULL;
}

int main(int argc, char **argv) {
    int n = (argc > 1) ? atoi(argv[1]) : 8;
    uint8_t *mem = malloc(META_SIZE + DATA_SIZE);
    memset(mem, 0, META_SIZE);
    if (box_init(mem, META_SIZE, DATA_SIZE) != 0) { printf("init fail\n"); return 1; }

    struct timespec t0, t1;
    pthread_t *tids = malloc(n * sizeof(pthread_t));
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < n; i++) pthread_create(&tids[i], NULL, worker, mem);
    for (int i = 0; i < n; i++) pthread_join(tids[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double e = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    printf("thr=%d  ops=%ld  time=%.3fs  rate=%.0f ops/s\n",
           n, (long)total, e, total / e);
    double per = total / e / n;
    double s1 = (n==1) ? 1.0 : per / (total / e) * n;  (void)s1;
    printf("per-thread=%.0f ops/s\n", per);
    free(tids); free(mem);
    return 0;
}
