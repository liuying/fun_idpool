#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>

#include "fun_idpool.h"
#include "arch_defs.h"

/* ============================================================
 * 编译期可调参数
 * ============================================================ */
#ifndef INIT_CAP
    #define INIT_CAP 64   /* 初始 Region 容量 (bit 数) — 小以节省内存 */
#endif
#ifndef MAX_CAP
    #define MAX_CAP (1u << 20)  /* 单个 Region 最大容量 */
#endif
#ifndef MAX_REGIONS
    #define MAX_REGIONS 256
#endif
#ifndef GLOBAL_BMW
    #define GLOBAL_BMW 4   /* 256 bit 全局位图 */
#endif

#define MAX_RETRY 3

/* ============================================================
 * 拆分结构体设计（核心优化）
 *
 * idpool_region_base: 基础 Region，无 values，极致紧凑
 * idpool_region:      继承 base + 追加 values 数组指针
 *
 * NO_VALUE 模式只分配 base，WITH_VALUE 分配完整 region
 * ============================================================ */

/* ---- 基础 Region（所有模式共享） ---- */
typedef struct CACHE_ALIGN {
    /* Cache Line 0: 分配热数据 */
    uint32_t base;           /* Region 起始 ID（只读） */
    uint32_t cap;             /* Region 容量（只读） */
    uint32_t used;            /* 已使用数（原子） */
    uint32_t cursor;          /* 分配游标（原子） */

    /* Cache Line 1: 控制字段 */
    uint32_t alloced;         /* 内存是否已分配完成 */
    uint32_t state;           /* 0=ACTIVE, 1=FULL, 2=RECYCLE */
    uint32_t zone_id;         /* 所属 zone */
    uint32_t region_idx;      /* 在 zone 中的索引 */

    /* Cache Line 2+: 位图（懒加载） */
    uint64_t *bitmap  ALIGN_8;
    uint64_t *summary ALIGN_8;
    uint32_t summary_words;
    /* 注意：此处无 values 字段！ */
} idpool_region_base;

/* ---- 带 values 的 Region（继承 base） ---- */
typedef struct CACHE_ALIGN {
    idpool_region_base base;  /* 基础字段（必须与上面完全一致） */
    void **values;             /* 仅 WITH_VALUE 模式存在 */
} idpool_region;

/* 编译期断言：验证 idpool_region 的 base 部分与 idpool_region_base 布局一致 */
typedef char static_assert_base_layout[
    sizeof(((idpool_region *)0)->base) == sizeof(idpool_region_base) ? 1 : -1
];

/* ============================================================
 * Zone (NUMA 节点)
 * ============================================================ */
typedef struct CACHE_ALIGN {
    /* ---- 热数据 ---- */
    uint32_t zone_id;
    uint32_t zone_shift;
    uint32_t zone_mask;
    uint32_t region_count;     /* 已发布 Region 数 (原子) */

    /* ---- 统计 (per-zone, 减少跨核竞争) ---- */
    uint64_t total_alloc   CACHE_ALIGN;
    uint64_t total_freed   CACHE_ALIGN;
    uint64_t scan_retries  CACHE_ALIGN;
    uint64_t reuse_count   CACHE_ALIGN;
    uint64_t bitmap_bytes  CACHE_ALIGN;   /* bitmap + summary 累计字节 */
    uint64_t values_bytes  CACHE_ALIGN;   /* values 数组累计字节 */

    /* ---- 全局位图 ---- */
    uint64_t global_bm[GLOBAL_BMW] CACHE_ALIGN;

    /* ---- Region 槽数组 ---- */
    region_slot regions[MAX_REGIONS] CACHE_ALIGN;
} idpool_zone;

/* ============================================================
 * Pool
 * ============================================================ */
struct fun_idpool_s {
    uint32_t numa_nodes;         /* 实际节点数 (用于内存分配) */
    uint32_t aligned_nodes;      /* 2 的幂 (用于位运算) */
    uint32_t zone_shift;         /* log2(对齐后的节点数) */
    uint32_t zone_mask;          /* 对齐后 - 1 */
    fun_idpool_mode_t mode;      /* 创建时确定,不可变 */
    idpool_zone *zones[16] CACHE_ALIGN;
};

/* ============================================================
 * Region 注册表 (替代全局 g_regions)
 * 每个 zone 自带 region 指针数组
 * ============================================================ */
static idpool_region_base *zone_registry[16][MAX_REGIONS] CACHE_ALIGN;

static inline idpool_region_base *registry_load(int zone, uint32_t idx) {
    if (idx >= MAX_REGIONS) return NULL;
    return (idpool_region_base *)a_load64((uint64_t *)&zone_registry[zone][idx]);
}

static inline void registry_publish(int zone, uint32_t idx, idpool_region_base *r) {
    a_store64((uint64_t *)&zone_registry[zone][idx], (uint64_t)r);
}

/* ============================================================
 * 容量 / Base 计算 (公式化, 无运行时依赖)
 *   cap(k)   = INIT_CAP * 2^k  (钳制到 MAX_CAP)
 *   base(k)  = INIT_CAP * (2^k - 1)
 * ============================================================ */
static inline uint32_t cap_of(uint32_t k) {
    if (k == 0) return INIT_CAP;
    uint32_t c = INIT_CAP;
    for (uint32_t i = 0; i < k && c < MAX_CAP; i++) {
        uint32_t next = c << 1;
        if (next < c) { c = MAX_CAP; break; }
        c = next;
    }
    if (c > MAX_CAP) c = MAX_CAP;
    return c;
}

static inline uint32_t base_of(uint32_t k) {
    if (k == 0) return 0;
    uint32_t total = 0;
    for (uint32_t i = 0; i < k; i++) {
        uint32_t c = cap_of(i);
        if (total + c < total) return 0xFFFFFFFFu;
        total += c;
    }
    return total;
}

/* ============================================================
 * 位图操作
 * ============================================================ */
static inline int bit_tas(uint64_t *bm, uint32_t idx) {
    uint32_t w = idx >> 6;
    uint64_t m = 1ULL << (idx & 63);
    return (__atomic_fetch_or(&bm[w], m, __ATOMIC_ACQ_REL) & m) != 0;
}

static inline void bit_clear(uint64_t *bm, uint32_t idx) {
    uint32_t w = idx >> 6;
    uint64_t m = ~(1ULL << (idx & 63));
    __atomic_fetch_and(&bm[w], m, __ATOMIC_ACQ_REL);
}

static inline int bit_test(uint64_t *bm, uint32_t idx) {
    return (a_load64(&bm[idx >> 6]) & (1ULL << (idx & 63))) != 0;
}

/* 从 start 开始找空闲 bit (单调递增核心)
 * 返回值 >= cap 表示没找到
 */
static inline uint32_t find_zero_from(uint64_t *bm, uint32_t nw,
                                      uint32_t start, uint32_t cap) {
    if (start >= cap) return cap;  /* 越界保护 */
    uint32_t wi = start >> 6;
    uint32_t bi = start & 63;

    /* 处理起始字内剩余位 */
    if (bi > 0 && wi < nw) {
        uint64_t w = a_load64(&bm[wi]);
        if (~w) {
            uint64_t mask = ~((1ULL << bi) - 1);
            uint64_t rem = w & mask;
            if (rem) {
                uint32_t bit = wi * 64 + __builtin_ctzll(rem);
                if (bit < cap) return bit;
            }
        }
        wi++;
    }

    /* 扫描后续完整字 */
    for (uint32_t i = wi; i < nw; i++) {
        uint64_t w = a_load64(&bm[i]);
        if (~w) {
            uint32_t bit = i * 64 + __builtin_ctzll(~w);
            if (bit < cap) return bit;
        }
    }
    return cap;  /* 没找到, 返回 cap 表示失败 */
}

/* 从 0 开始扫描所有空闲位 (复用场景) */
static inline uint32_t find_zero_wrap(uint64_t *bm, uint32_t nw, uint32_t cap) {
    for (uint32_t i = 0; i < nw; i++) {
        uint64_t w = a_load64(&bm[i]);
        if (~w) {
            uint32_t bit = i * 64 + __builtin_ctzll(~w);
            if (bit < cap) return bit;
        }
    }
    return cap;  /* 没找到 */
}

/* 更新 summary bitmap */
static inline void upd_summary(uint64_t *sum, uint64_t *main_bm,
                                uint32_t mwi, uint32_t nw) {
    if (mwi >= nw) return;
    uint32_t sw = mwi >> 6, sm = 1ULL << (mwi & 63);
    if (a_load64(&main_bm[mwi]) == ~0ULL)
        a_fand64(&sum[sw], ~sm);
    else
        a_for64(&sum[sw], sm);
}

/* ============================================================
 * Region 懒加载内存分配
 *
 * 关键改动:
 *   NO_VALUE 模式: 只分配 bitmap + summary, 不分配 values
 *                  且 r 实际是 idpool_region_base 大小,无 values 字段
 *   WITH_VALUE 模式: 额外分配 values 数组
 *                  且 r 是 idpool_region 大小,含 values 字段
 *
 * 内存收益 (NO_VALUE, 1M ID):
 *   bitmap:  ~128 KB  (1M bits / 8)
 *   summary: ~2 KB
 *   values:  0
 *   region:  ~56 B/region × N regions (vs 64 B + 8 B padding)
 * ============================================================ */
static void ensure_region(idpool_region_base *r, fun_idpool_mode_t mode,
                          idpool_zone *z) {
    if (a_load32(&r->alloced)) return;

    uint32_t cap = r->cap;
    uint32_t nw = (cap + 63) >> 6;
    uint32_t sw = (nw + 63) >> 6;

    /* bitmap 必须分配 (两种模式都需要) */
    uint64_t *bm = (uint64_t *)a_alloc8(nw * sizeof(uint64_t));
    if (!bm) abort();

    /* summary 必须分配 */
    uint64_t *sum = (uint64_t *)a_alloc8(sw * sizeof(uint64_t));
    if (!sum) { free(bm); abort(); }
    /* summary 初始全 1 = 每个字都有空闲 */
    memset(sum, 0xFF, sw * sizeof(uint64_t));

    /* values: 仅 WITH_VALUE 模式分配 */
    void **vals = NULL;
    if (mode == FUN_IDPOOL_MODE_WITH_VALUE) {
        vals = (void **)a_alloc(cap * sizeof(void *));
        if (!vals) { free(bm); free(sum); abort(); }
    }

    /* CAS 发布 (只发布一次) */
    uint64_t *exp = NULL;
    if (__atomic_compare_exchange_n(&r->bitmap, &exp, bm, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        r->summary = sum;
        r->summary_words = sw;
        if (mode == FUN_IDPOOL_MODE_WITH_VALUE) {
            ((idpool_region *)r)->values = vals;
        }
        a_store32(&r->alloced, 1);

        /* 更新统计 */
        a_fadd64(&z->bitmap_bytes, nw * sizeof(uint64_t) + sw * sizeof(uint64_t));
        if (vals) a_fadd64(&z->values_bytes, cap * sizeof(void *));
    } else {
        /* 竞争失败: 别人抢先发布了 */
        free(bm); free(sum);
        if (vals) free(vals);
    }
}

/* ============================================================
 * 创建新 Region (串行化 + 懒加载, 按 mode 分配不同结构体大小)
 * ============================================================ */
static idpool_region_base *create_region(idpool_zone *z, fun_idpool_mode_t mode) {
    uint32_t rc = a_load32(&z->region_count);
    for (;;) {
        if (rc >= MAX_REGIONS) {
            fprintf(stderr, "FATAL: MAX_REGIONS (%d) reached\n", MAX_REGIONS);
            abort();
        }
        uint32_t exp = rc;
        if (a_cas32(&z->region_count, &exp, rc + 1)) {
            uint32_t k = rc;
            uint32_t base = base_of(k);
            uint32_t cap  = cap_of(k);

            /* 按 mode 选择不同大小的结构体 */
            idpool_region_base *r;
            size_t alloc_sz;
            if (mode == FUN_IDPOOL_MODE_NO_VALUE) {
                alloc_sz = sizeof(idpool_region_base);
            } else {
                alloc_sz = sizeof(idpool_region);
            }
            r = (idpool_region_base *)a_alloc(alloc_sz);
            if (!r) abort();
            memset(r, 0, alloc_sz);
            r->base = base;
            r->cap = cap;
            r->cursor = 0;
            r->alloced = 0;
            r->state = 0;  /* ACTIVE */
            r->zone_id = z->zone_id;
            r->region_idx = k;
            /* bitmap/summary/values 留 NULL, 懒加载 */

            /* 发布到注册表 */
            registry_publish(z->zone_id, k, r);

            /* 发布 slot (原子可见) */
            slot_publish(&z->regions[k], k, k + 1);

            /* 标记 global bitmap */
            a_for64(&z->global_bm[k >> 6], 1ULL << (k & 63));

            fprintf(stderr,
                    "[fun_idpool] zone%d: new region #%u base=%u cap=%u mode=%s\n",
                    z->zone_id, k, base, cap,
                    mode == FUN_IDPOOL_MODE_NO_VALUE ? "NO_VALUE" : "WITH_VALUE");
            return r;
        }
        rc = a_load32(&z->region_count);
    }
}

/* ============================================================
 * 加载 Region (通过 slot)
 * ============================================================ */
static idpool_region_base *load_region(idpool_zone *z, uint32_t idx, uint64_t *ver_out) {
    if (idx >= MAX_REGIONS) { *ver_out = 0; return NULL; }

#if ARCH_64BIT
    region_slot *s = &z->regions[idx];
    uint64_t packed = slot_load(s);
    if (packed == 0) { *ver_out = 0; return NULL; }
    *ver_out = slot_ver(packed);
    return registry_load(z->zone_id, slot_idx(packed));
#else
    uint32_t s_idx = 0, s_ver = 0;
    slot_load(&z->regions[idx], &s_idx, &s_ver);
    *ver_out = s_ver;
    return registry_load(z->zone_id, s_idx);
#endif
}

/* ============================================================
 * 核心: Region 内分配
 *
 * 策略:
 *   Phase 0: 从 cursor 往后找空闲位 (单调递增, 不复用)
 *            成功后将 cursor 设为 bit+1
 *   Phase 1: cursor 到末尾后, 回绕扫描 (复用已释放的 ID)
 *
 * 关键改进:
 *   - TAS 失败时不回退 cursor, 而是跳到下一个 word
 *   - cursor 只向前推进, 保证单调性
 *   - 使用 find_zero_from 的返回值 (保证 < cap)
 *
 * 两种模式共用同一路径:
 *   NO_VALUE:  通过 (idpool_region*) 强转访问 values 字段 — 但因 r
 *              实际只有 sizeof(idpool_region_base),无 values 字段,
 *              永不执行此分支 (mode 检查在前)
 *   WITH_VALUE: r 是 sizeof(idpool_region),values 字段有效
 * ============================================================ */
static uint32_t region_alloc(idpool_zone *z, idpool_region_base *r,
                              uint32_t node_id, void *v,
                              fun_idpool_mode_t mode) {
    ensure_region(r, mode, z);
    if (!a_load32(&r->alloced)) return FUN_IDPOOL_INVALID_ID;

    uint32_t cap = r->cap;
    uint32_t nw  = (cap + 63) >> 6;
    uint64_t *bm = r->bitmap;
    if (!bm) return FUN_IDPOOL_INVALID_ID;

    /* ---- Phase 0: 单调递增 ---- */
    for (;;) {
        uint32_t cur = a_load32(&r->cursor);
        if (cur >= cap) break;  /* 进入 Phase 1 */

        uint32_t bit = find_zero_from(bm, nw, cur, cap);
        if (bit >= cap) {
            /* 从 cur 到末尾没有空闲位了 */
            /* 尝试把 cursor 推到 cap (标记到末尾) */
            uint32_t exp_cur = cur;
            a_cas32(&r->cursor, &exp_cur, cap);
            break;  /* 去 Phase 1 */
        }

        if (!bit_tas(bm, bit)) {
            /* ✅ 成功占用! */

            /* 推进 cursor 到 bit+1 (CAS, 允许失败) */
            uint32_t exp_cur = cur;
            uint32_t new_cur = bit + 1;
            if (new_cur > cap) new_cur = cap;
            a_cas32(&r->cursor, &exp_cur, new_cur);

            /* 更新 summary */
            upd_summary(r->summary, bm, bit >> 6, nw);

            /* 仅 WITH_VALUE 模式存储指针 */
            if (mode == FUN_IDPOOL_MODE_WITH_VALUE) {
                ((idpool_region *)r)->values[bit] = v;
            }

            a_fadd32(&r->used, 1);
            a_fadd64(&z->total_alloc, 1);

            /* 检查是否满了 */
            if (new_cur >= cap) {
                a_store32(&r->state, 1);  /* FULL */
            }

            /* 编码 ID: (base + bit) << shift | node */
            return (r->base + bit) << z->zone_shift | node_id;
        }

        /* TAS 失败: 别人抢了 bit
         * 关键: 把 cursor 推过这个被抢的 bit, 不回退
         */
        uint32_t exp_cur = cur;
        a_cas32(&r->cursor, &exp_cur, bit + 1);
        a_fadd64(&z->scan_retries, 1);
    }

    /* ---- Phase 1: 回绕扫描 (复用已释放的 ID) ---- */
    /*
     * 重要: 只有当 Region 确实满了才会到这里
     * 此时 cursor >= cap, 所有 bit 都被占过
     * 如果有释放的 bit, find_zero_wrap 能找到
     */
    for (int retry = 0; retry < MAX_RETRY; retry++) {
        uint32_t bit = find_zero_wrap(bm, nw, cap);
        if (bit >= cap) break;  /* 真的全满了 */

        if (!bit_tas(bm, bit)) {
            /* 更新 summary */
            upd_summary(r->summary, bm, bit >> 6, nw);

            /* 仅 WITH_VALUE 模式存储指针 */
            if (mode == FUN_IDPOOL_MODE_WITH_VALUE) {
                ((idpool_region *)r)->values[bit] = v;
            }

            a_fadd32(&r->used, 1);
            a_fadd64(&z->total_alloc, 1);
            a_fadd64(&z->reuse_count, 1);

            /* 不再 FULL */
            a_store32(&r->state, 0);  /* ACTIVE */

            return (r->base + bit) << z->zone_shift | node_id;
        }
        a_fadd64(&z->scan_retries, 1);
    }

    return FUN_IDPOOL_INVALID_ID;
}

/* ============================================================
 * Zone 级分配
 *
 * 策略:
 *   1. 遍历所有已有 Region (从 0 开始, 优先用旧的)
 *   2. 利用 global_bm 快速跳过 FULL 的
 *   3. 全部满了才创建新 Region
 * ============================================================ */
static uint32_t zone_alloc(idpool_zone *z, uint32_t node_id, void *v,
                           fun_idpool_mode_t mode) {
    uint32_t rc = a_load32(&z->region_count);

    /* 遍历已有 Region (从 0 开始, 优先复用旧 Region 的空闲位) */
    for (uint32_t pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < rc; i++) {
            /* 快速检查: global_bm 是否已标记该 Region 为不可用 */
            uint32_t gi = i >> 6, gb = i & 63;
            if (gi < GLOBAL_BMW) {
                uint64_t gbv = a_load64(&z->global_bm[gi]);
                if ((gbv & (1ULL << gb)) == 0) {
                    /* global_bm 说这个 Region 不可用, 跳过 */
                    continue;
                }
            }

            uint64_t ver;
            idpool_region_base *r = load_region(z, i, &ver);
            if (!r) continue;

            /* 快速检查: 确实满了? */
            if (a_load32(&r->state) == 1 && a_load32(&r->used) >= r->cap) {
                /* 确实满了, 清除 global_bm */
                if (gi < GLOBAL_BMW)
                    a_fand64(&z->global_bm[gi], ~(1ULL << gb));
                continue;
            }

            uint32_t id = region_alloc(z, r, node_id, v, mode);
            if (id != FUN_IDPOOL_INVALID_ID) return id;

            /* region_alloc 失败, 可能刚满, 再检查一次 */
            if (a_load32(&r->state) == 1) {
                if (gi < GLOBAL_BMW)
                    a_fand64(&z->global_bm[gi], ~(1ULL << gb));
            }
        }
    }

    /* 所有 Region 都满了, 创建新的 */
    idpool_region_base *nr = create_region(z, mode);
    if (nr) {
        uint32_t id = region_alloc(z, nr, node_id, v, mode);
        if (id != FUN_IDPOOL_INVALID_ID) return id;
    }

    /*
     * 极端情况: 所有 Region 都满了, 包括刚新建的
     * 不 abort — 而是忙等重试, 等待其他线程释放
     * 这保证了"永不返回失败"的承诺
     */
    for (;;) {
        /* 再扫一遍所有 Region (包括新建的) */
        rc = a_load32(&z->region_count);
        for (uint32_t i = 0; i < rc; i++) {
            uint64_t ver;
            idpool_region_base *r = load_region(z, i, &ver);
            if (!r) continue;
            uint32_t id = region_alloc(z, r, node_id, v, mode);
            if (id != FUN_IDPOOL_INVALID_ID) return id;
        }
        /* 让出 CPU, 等待其他线程释放 */
        sched_yield();
    }
}

/* ============================================================
 * 公开 API 实现
 * ============================================================ */

fun_idpool_t fun_idpool_create_ex(int numa_nodes, fun_idpool_mode_t mode) {
    /* 检测实际 NUMA 节点数 */
    int detected = detect_numa_nodes();
    if (numa_nodes <= 0) numa_nodes = detected;
    if (numa_nodes > 16) numa_nodes = 16;
    if (numa_nodes < 1) numa_nodes = 1;

    /* 对齐到 2 的幂 (用于位运算) */
    int aligned = 1;
    while (aligned < numa_nodes) aligned <<= 1;

    /* 内存分配用实际数量, 运算用对齐数量 */
    int actual = numa_nodes;

    fun_idpool_t pool = (fun_idpool_t)a_alloc(sizeof(fun_idpool_s));
    if (!pool) abort();
    pool->numa_nodes   = actual;
    pool->aligned_nodes = aligned;
    pool->zone_shift   = __builtin_ctz(aligned);
    pool->zone_mask    = aligned - 1;
    pool->mode         = mode;

    for (int n = 0; n < actual; n++) {
        idpool_zone *z = (idpool_zone *)a_alloc(sizeof(idpool_zone));
        if (!z) abort();
        memset(z, 0, sizeof(*z));
        z->zone_id    = n;
        z->zone_shift = pool->zone_shift;
        z->zone_mask  = pool->zone_mask;

        /* Region #0: 立即创建并发布 (懒加载内存) */
        uint32_t cap0 = cap_of(0);
        /* 按 mode 选择不同大小的结构体 */
        size_t r0_sz = (mode == FUN_IDPOOL_MODE_NO_VALUE)
                       ? sizeof(idpool_region_base)
                       : sizeof(idpool_region);
        idpool_region_base *r0 = (idpool_region_base *)a_alloc(r0_sz);
        if (!r0) abort();
        memset(r0, 0, r0_sz);
        r0->base = 0;
        r0->cap  = cap0;
        r0->zone_id = n;
        r0->region_idx = 0;
        /* bitmap/summary/values = NULL, 首次使用时分配 */

        registry_publish(n, 0, r0);
        slot_publish(&z->regions[0], 0, 1);
        z->region_count = 1;
        /* global_bm[0] 的 bit 0 标记 Region #0 可用 */
        a_for64(&z->global_bm[0], 1ULL);

        pool->zones[n] = z;
    }

    fprintf(stderr,
            "[fun_idpool] created: %d zones (detected=%d, aligned=%d) mode=%s\n",
            actual, detected, aligned,
            mode == FUN_IDPOOL_MODE_NO_VALUE ? "NO_VALUE" : "WITH_VALUE");
    return pool;
}

void fun_idpool_destroy(fun_idpool_t pool) {
    if (!pool) return;
    for (uint32_t n = 0; n < pool->numa_nodes; n++) {
        idpool_zone *z = pool->zones[n];
        if (!z) continue;
        uint32_t rc = a_load32(&z->region_count);
        for (uint32_t i = 0; i < rc; i++) {
            idpool_region_base *r = registry_load(n, i);
            if (!r) continue;
            if (r->bitmap)  free(r->bitmap);
            if (r->summary) free(r->summary);
            if (pool->mode == FUN_IDPOOL_MODE_WITH_VALUE) {
                void **vals = ((idpool_region *)r)->values;
                if (vals) free(vals);
            }
            free(r);
        }
        free(z);
    }
    free(pool);
}

uint32_t fun_idpool_gen_id(fun_idpool_t pool, void *ptr) {
    int cpu = get_cpu();
    uint32_t n = cpu & pool->zone_mask;
    /* zone_mask 是对齐后的值, 需要再检查实际范围 */
    if (n >= pool->numa_nodes) n = cpu % pool->numa_nodes;
    idpool_zone *z = pool->zones[n];
    return zone_alloc(z, n, ptr, pool->mode);
}

void *fun_idpool_get_value(fun_idpool_t pool, uint32_t id) {
    if (id == FUN_IDPOOL_INVALID_ID) return NULL;

    uint32_t n   = id & pool->zone_mask;
    if (n >= pool->numa_nodes) return NULL;
    uint32_t idx = id >> pool->zone_shift;

    /* NO_VALUE 模式：返回存在性哨兵 */
    if (pool->mode == FUN_IDPOOL_MODE_NO_VALUE) {
        idpool_zone *z = pool->zones[n];
        uint32_t rc = a_load32(&z->region_count);
        for (uint32_t i = 0; i < rc; i++) {
            uint64_t ver;
            idpool_region_base *r = load_region(z, i, &ver);
            if (!r) continue;
            if (idx >= r->base && idx < r->base + r->cap) {
                uint32_t off = idx - r->base;
                if (off < r->cap && bit_test(r->bitmap, off)) {
                    return FUN_IDPOOL_EXISTS;
                }
                break;
            }
        }
        return NULL;
    }

    /* WITH_VALUE 模式：返回绑定的指针 */
    idpool_zone *z = pool->zones[n];
    uint32_t rc = a_load32(&z->region_count);
    for (uint32_t i = 0; i < rc; i++) {
        uint64_t ver;
        idpool_region_base *base = load_region(z, i, &ver);
        if (!base) continue;
        if (idx >= base->base && idx < base->base + base->cap) {
            idpool_region *r = (idpool_region *)base;
            uint32_t off = idx - base->base;
            if (off < base->cap && bit_test(base->bitmap, off)) {
                return r->values[off];
            }
            break;
        }
    }
    return NULL;
}

void *fun_idpool_release_id(fun_idpool_t pool, uint32_t id) {
    if (id == FUN_IDPOOL_INVALID_ID) return NULL;

    uint32_t n   = id & pool->zone_mask;
    if (n >= pool->numa_nodes) return NULL;
    uint32_t idx = id >> pool->zone_shift;

    idpool_zone *z = pool->zones[n];
    uint32_t rc = a_load32(&z->region_count);
    void *ret = NULL;

    for (uint32_t i = 0; i < rc; i++) {
        uint64_t ver;
        idpool_region_base *r = load_region(z, i, &ver);
        if (!r) continue;
        if (idx >= r->base && idx < r->base + r->cap) {
            uint32_t off = idx - r->base;
            if (off < r->cap && bit_test(r->bitmap, off)) {
                if (pool->mode == FUN_IDPOOL_MODE_WITH_VALUE) {
                    ret = ((idpool_region *)r)->values[off];
                    ((idpool_region *)r)->values[off] = NULL;
                } else {
                    ret = FUN_IDPOOL_EXISTS;
                }
                bit_clear(r->bitmap, off);
                a_fadd32(&r->used, -1);
                a_fadd64(&z->total_freed, 1);
                upd_summary(r->summary, r->bitmap, off >> 6, (r->cap + 63) >> 6);

                if (a_load32(&r->state) == 1) {
                    a_store32(&r->state, 0);
                    uint32_t gi = i >> 6, gb = i & 63;
                    if (gi < GLOBAL_BMW)
                        a_for64(&z->global_bm[gi], 1ULL << gb);
                }
            }
            return ret;
        }
    }
    return NULL;
}

void fun_idpool_get_stats(fun_idpool_t pool, fun_idpool_stats_t stats) {
    memset(stats, 0, sizeof(*stats));
    stats->mode = pool->mode;
    stats->numa_nodes = pool->numa_nodes;
    for (uint32_t n = 0; n < pool->numa_nodes; n++) {
        idpool_zone *z = pool->zones[n];
        stats->total_alloc   += a_load64(&z->total_alloc);
        stats->total_freed   += a_load64(&z->total_freed);
        stats->scan_retries  += a_load64(&z->scan_retries);
        stats->reuse_count   += a_load64(&z->reuse_count);
        stats->bitmap_memory += a_load64(&z->bitmap_bytes);
        stats->values_memory += a_load64(&z->values_bytes);
        stats->total_regions += a_load32(&z->region_count);
        stats->region_memory += a_load32(&z->region_count) * sizeof(idpool_region_base);
    }
    /* NO_VALUE 模式下 region_memory 应反映实际分配大小 */
    if (pool->mode == FUN_IDPOOL_MODE_NO_VALUE) {
        /* NO_VALUE: 每个 region 是 idpool_region_base 大小 */
        stats->region_memory = 0;
        for (uint32_t n = 0; n < pool->numa_nodes; n++) {
            idpool_zone *z = pool->zones[n];
            stats->region_memory += a_load32(&z->region_count) * sizeof(idpool_region_base);
        }
    } else {
        stats->region_memory = 0;
        for (uint32_t n = 0; n < pool->numa_nodes; n++) {
            idpool_zone *z = pool->zones[n];
            stats->region_memory += a_load32(&z->region_count) * sizeof(idpool_region);
        }
    }
}

fun_idpool_mode_t fun_idpool_get_mode(fun_idpool_t pool) {
    return pool->mode;
}
