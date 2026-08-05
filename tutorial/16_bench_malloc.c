#define _POSIX_C_SOURCE 199309L
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <slotsboxmalloc/slotsboxmalloc.h>

#define OPS 200000
#define META_SIZE (1024 * 1024)
#define DATA_SIZE (15ULL * 8 * 1024 * 1024)
static const size_t kSz[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};

static _Atomic long total = 0;

// ─── malloc/free ───
void* bench_malloc(void *arg) {
    long n = 0;
    for (int i = 0; i < OPS; i++) {
        size_t sz = kSz[i % 10];
        void *p = malloc(sz);
        if (p) { memset(p, 0, sz > 64 ? 64 : sz); free(p); n++; }
    }
    total += n; return NULL;
}

// ─── box_alloc/box_free ───
void* bench_box(void *arg) {
    uint8_t *meta = (uint8_t *)arg;
    long n = 0;
    for (int i = 0; i < OPS; i++) {
        size_t sz = kSz[i % 10];
        uint64_t off = box_alloc(meta, sz);
        if (off != (uint64_t)-1) { box_free(meta, off); n++; }
    }
    total += n; return NULL;
}

static void run(const char *label, void*(*fn)(void*), void *arg, int nthr) {
    total = 0;
    struct timespec t0, t1;
    pthread_t *tids = malloc(nthr * sizeof(pthread_t));
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < nthr; i++) pthread_create(&tids[i], NULL, fn, arg);
    for (int i = 0; i < nthr; i++) pthread_join(tids[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double e = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("%-14s thr=%-2d  ops=%ld  time=%.3fs  rate=%9.0f ops/s\n",
           label, nthr, (long)total, e, total / e);
    free(tids);
}

int main() {
    // box init
    uint8_t *mem = malloc(META_SIZE + DATA_SIZE);
    memset(mem, 0, META_SIZE);
    box_init(mem, META_SIZE, DATA_SIZE);

    printf("%-14s %6s %8s %8s %12s\n", "allocator", "thr", "ops", "time", "rate");
    printf("-------------- ------ -------- -------- ------------\n");
    for (int n = 1; n <= 8; n++) {
        run("glibc-malloc", bench_malloc, NULL, n);
        run("slotsboxmalloc",    bench_box,    mem,  n);
    }
    free(mem);
    return 0;
}
