#ifndef ARCH_DEFS_H
#define ARCH_DEFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sched.h>
#include <unistd.h>

/* ============================================================
 * 1. Cache line size — 平台自适应
 *
 * 用于:
 *   - CACHE_ALIGN:  结构体对齐到 cache line 边界, 避免 false sharing
 *   - a_alloc:      分配 cache line 对齐的内存
 *
 * 各平台字节数:
 *   x86_64 / AArch64 / LoongArch64: 64 字节
 *   x86_32 / ARMv7:                 32 字节
 *   其他:                           默认 64 字节
 * ============================================================ */
#if defined(__x86_64__) || defined(__aarch64__) || defined(__loongarch__)
    #define CACHE_LINE_SIZE 64
#elif defined(__i386__) || defined(__arm__)
    #define CACHE_LINE_SIZE 32
#else
    #define CACHE_LINE_SIZE 64
#endif

/* CACHE_ALIGN: 结构体对齐到 cache line (用于 Zone 等大结构体, 避免 false sharing) */
#define CACHE_ALIGN __attribute__((aligned(CACHE_LINE_SIZE)))
/* ALIGN_8: 8 字节对齐 (用于 uint64_t 数组, ARMv7 LDREXD 要求) */
#define ALIGN_8 __attribute__((aligned(8)))
/* ALIGN_4: 4 字节对齐 (备用) */
#define ALIGN_4 __attribute__((aligned(4)))

/* ============================================================
 * 2. 指针位宽判断
 *
 * 用于:
 *   - 决定 region_slot 的内存布局 (64-bit: packed 8 字节, 32-bit: split idx+version)
 *   - fun_idpool.c 中根据 ARCH_64BIT 选择 slot 操作路径
 * ============================================================ */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
    #define ARCH_64BIT 1
#else
    #define ARCH_64BIT 0
#endif

/* ============================================================
 * 3. 原子操作包装
 *
 * 设计原则:
 *   - 统一使用 GCC __atomic_* 内建, 跨平台、跨位宽
 *   - ACQUIRE 语义: 读屏障, 看到其他线程 ACQUIRE/RELEASE 之前的写
 *   - RELEASE 语义: 写屏障, 本线程的写在其他线程 ACQUIRE 之前可见
 *   - ACQ_REL:    读+写屏障, 用于 fetch_add / cas 等 RMW 操作
 *
 * 内存序:
 *   - a_load32/64: ACQUIRE (用于读 stats / cursor / bitmap)
 *   - a_store32/64: RELEASE (用于写 alloced / state / version)
 *   - a_fadd32/64:  ACQ_REL (用于 total_alloc / used / cursor 推进)
 *   - a_cas32:      ACQ_REL (用于 region_count 原子推进)
 *   - a_for64:      ACQ_REL (用于 global_bm 标位)
 *   - a_fand64:     ACQ_REL (用于 global_bm 清位)
 * ============================================================ */

/* 32-bit 操作: 用于 stats / cursor / region_count (轻量计数器) */
static inline uint32_t a_load32(uint32_t *p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static inline void a_store32(uint32_t *p, uint32_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
static inline uint32_t a_fadd32(uint32_t *p, uint32_t v) {
    /* 返回旧值 (用于 total_alloc 等场景, 旧值不需要时可忽略) */
    return __atomic_fetch_add(p, v, __ATOMIC_ACQ_REL);
}
static inline int a_cas32(uint32_t *p, uint32_t *exp, uint32_t new) {
    /* 比较相等则设置为 new, *exp 被设为当前值
     * 返回 0 = 成功, 非 0 = 失败 */
    return __atomic_compare_exchange_n(p, exp, new, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* 64-bit 操作: 用于 bitmap 字 (uint64_t 数组) + 64-bit stats 计数器 */
static inline uint64_t a_load64(uint64_t *p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static inline void a_store64(uint64_t *p, uint64_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
static inline void a_for64(uint64_t *p, uint64_t v) {
    /* 原子或: 用于 global_bm 标位 (OR 1 bit) */
    __atomic_fetch_or(p, v, __ATOMIC_ACQ_REL);
}
static inline void a_fand64(uint64_t *p, uint64_t v) {
    /* 原子与: 用于 global_bm 清位 (AND 反掩码) */
    __atomic_fetch_and(p, v, __ATOMIC_ACQ_REL);
}
static inline uint64_t a_fadd64(uint64_t *p, uint64_t v) {
    /* 用于 64-bit 计数器 (total_freed / scan_retries 等) */
    return __atomic_fetch_add(p, v, __ATOMIC_ACQ_REL);
}

/* ============================================================
 * 4. 对齐内存分配
 *
 * a_alloc:   cache line 对齐分配 (用于 Zone / Region 等大结构体)
 * a_alloc8:  8 字节对齐分配 (用于 uint64_t 数组, ARMv7 LDREXD 要求)
 *
 * 失败处理: posix_memalign 失败直接 abort (永不返回 NULL)
 * 初始化:   分配后自动 memset 0
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
 * 5. getcpu 抽象
 *
 * 用于 fun_idpool_gen_id: 根据当前 CPU 选择 zone
 * 失败回退: 返回 0 (zone 0 总是存在)
 * ============================================================ */
static inline int get_cpu(void) {
    int cpu = sched_getcpu();
    return cpu < 0 ? 0 : cpu;
}

/* ============================================================
 * 6. 运行时 NUMA 节点数获取
 *
 * 三级回退:
 *   1. 读 /sys/devices/system/node/online (Linux)
 *   2. sysconf(_SC_NPROCESSORS_ONLN) 估算
 *   3. 返回 1 (最小安全值)
 *
 * 用于 fun_idpool_create(numa_nodes=0) 自动检测
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
