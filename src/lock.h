#ifndef LOCK_H
#define LOCK_H

#include <stdatomic.h>

static inline void box_lock(atomic_int *lock) {
    int expected = 0;
    while (!atomic_compare_exchange_weak(lock, &expected, 1)) {
        expected = 0;
#ifdef __x86_64__
        __asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__)
        __asm__ __volatile__("yield" ::: "memory");
#endif
    }
}

static inline bool box_trylock(atomic_int *lock) {
    int expected = 0;
    return atomic_compare_exchange_weak(lock, &expected, 1);
}

static inline void box_unlock(atomic_int *lock) {
    atomic_store(lock, 0);
}

#endif // LOCK_H
