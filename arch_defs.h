#ifndef ARCH_DEFS_H
#define ARCH_DEFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sched.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <stdarg.h>

/* ============================================================
 * 1. Cache line size — 平台自适应
 *    x86_64 / AArch64 / LoongArch64: 64 字节
 *    x86_32 / ARMv7:                 32 字节
 *    其他:                           默认 64 字节
 * ============================================================ */
#if defined(__x86_64__) || defined(__aarch64__) || defined(__loongarch__)
    #define CACHE_LINE_SIZE 64
#elif defined(__i386__) || defined(__arm__)
    #define CACHE_LINE_SIZE 32
#else
    #define CACHE_LINE_SIZE 64
#endif

#define CACHE_ALIGN __attribute__((aligned(CACHE_LINE_SIZE)))
#define ALIGN_8 __attribute__((aligned(8)))
#define ALIGN_4 __attribute__((aligned(4)))

/* ============================================================
 * 2. 指针位宽判断 (决定 Region Slot 的内存布局)
 * ============================================================ */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
    #define ARCH_64BIT 1
#else
    #define ARCH_64BIT 0
#endif

/* ============================================================
 * 3. Atomic wrappers (GCC __atomic_*)
 *    统一使用 __atomic_* 内建, 跨平台、跨位宽
 * ============================================================ */

/* 32-bit load/store/add/cas */
static inline uint32_t a_load32(uint32_t *p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static inline void a_store32(uint32_t *p, uint32_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
static inline uint32_t a_fadd32(uint32_t *p, uint32_t v) {
    return __atomic_fetch_add(p, v, __ATOMIC_ACQ_REL);
}
static inline int a_cas32(uint32_t *p, uint32_t *exp, uint32_t new) {
    return __atomic_compare_exchange_n(p, exp, new, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* 64-bit load/store/add (用于 bitmap 字 + 统计计数器) */
static inline uint64_t a_load64(uint64_t *p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static inline void a_store64(uint64_t *p, uint64_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
static inline void a_for64(uint64_t *p, uint64_t v) {
    __atomic_fetch_or(p, v, __ATOMIC_ACQ_REL);
}
static inline void a_fand64(uint64_t *p, uint64_t v) {
    __atomic_fetch_and(p, v, __ATOMIC_ACQ_REL);
}
static inline uint64_t a_fadd64(uint64_t *p, uint64_t v) {
    return __atomic_fetch_add(p, v, __ATOMIC_ACQ_REL);
}

/* ============================================================
 * 4. Region Slot (platform-adaptive)
 *
 * 64-bit: packed = [version:32][idx:32]  一次原子读
 * 32-bit:  idx + version 分两个 cache-line 隔离的 32-bit 变量
 * ============================================================ */
#if ARCH_64BIT
typedef struct CACHE_ALIGN {
    uint64_t packed;   /* 8-byte aligned, single atomic read */
} region_slot;

static inline void slot_publish(region_slot *s, uint32_t idx, uint32_t ver) {
    uint64_t p = ((uint64_t)ver << 32) | (uint64_t)idx;
    a_store64(&s->packed, p);
}
static inline uint64_t slot_load(region_slot *s) {
    return a_load64(&s->packed);
}
static inline uint32_t slot_idx(uint64_t p) { return (uint32_t)(p & 0xFFFFFFFFULL); }
static inline uint32_t slot_ver(uint64_t p) { return (uint32_t)(p >> 32); }
#else  /* 32-bit: split to avoid 64-bit atomics */
typedef struct CACHE_ALIGN {
    volatile uint32_t idx;
    char _pad0[CACHE_LINE_SIZE - 4];
    volatile uint32_t version;
    char _pad1[CACHE_LINE_SIZE - 4];
} region_slot;

static inline void slot_publish(region_slot *s, uint32_t idx, uint32_t ver) {
    a_store32(&s->idx, idx);
    a_store32(&s->version, ver);
}
static inline void slot_load(region_slot *s, uint32_t *idx, uint32_t *ver) {
    *ver = a_load32(&s->version);
    *idx = a_load32(&s->idx);
}
#endif

/* ============================================================
 * 5. 对齐内存分配 (cache line / 8 字节)
 * ============================================================ */
static inline void *a_alloc(size_t sz) {
    void *p = NULL;
    if (posix_memalign(&p, CACHE_LINE_SIZE, sz) != 0) abort();
    memset(p, 0, sz);
    return p;
}

/* 8-byte aligned alloc for uint64_t arrays (ARMv7 LDREXD requirement) */
static inline void *a_alloc8(size_t sz) {
    void *p = NULL;
    if (posix_memalign(&p, 8, sz) != 0) abort();
    memset(p, 0, sz);
    return p;
}

/* ============================================================
 * 6. getcpu 抽象
 * ============================================================ */
static inline int get_cpu(void) {
    int cpu = sched_getcpu();
    return cpu < 0 ? 0 : cpu;
}

/* ============================================================
 * 7. 运行时 NUMA 节点数获取
 * ============================================================ */
static inline int detect_numa_nodes(void) {
#if defined(__linux__)
    /* 读 /sys/devices/system/node/ 下的 online 文件 */
    FILE *f = fopen("/sys/devices/system/node/online", "r");
    if (f) {
        char buf[256] = {0};
        if (fgets(buf, sizeof(buf), f)) {
            /* 解析 "0-3" 或 "0,1,2,3" 格式 */
            int max_node = 0;
            char *p = buf;
            while (*p) {
                if (*p >= '0' && *p <= '9') {
                    int v = atoi(p);
                    if (v > max_node) max_node = v;
                    while (*p >= '0' && *p <= '9') p++;
                } else {
                    p++;
                }
            }
            fclose(f);
            return max_node + 1;
        }
        fclose(f);
    }
    /* fallback: 用 CPU 数估算 */
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0) return (int)n;
#endif
    return 1; /* 最小安全值 */
}

#endif /* ARCH_DEFS_H */
