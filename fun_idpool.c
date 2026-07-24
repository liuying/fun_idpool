#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>

#include "fun_idpool.h"
#include "arch_defs.h"

/* ============================================================
 * 编译期可调参数
 * ============================================================ */
#ifndef INIT_CAP
    #define INIT_CAP 64   /* 恢复为 64：Region #0 可用 63 个 ID (bit 0 保留) */
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
#ifndef SLOT_INIT_CAP
    #define SLOT_INIT_CAP 4  /* slot 动态数组初始容量 */
#endif

#define MAX_RETRY 3

/* ============================================================
 * Region Base (无 values，热数据紧凑布局)
 * ============================================================ */
typedef struct idpool_region_base {
    /* ---- Cache Line 0: 分配热数据 ---- */
    uint32_t base;           /* Region 起始 ID (publish 后只读) */
    uint32_t cap;             /* Region 容量 (publish 后只读) */
    uint32_t used;            /* 已使用数 (原子) */
    uint32_t cursor;          /* 分配游标 (原子) */

    /* ---- Cache Line 1: 控制 ---- */
    uint32_t alloced;         /* 内存是否已分配完成 (原子发布标志) */
    uint32_t state;           /* 0=ACTIVE, 1=FULL, 2=RECYCLE */
    uint32_t zone_id;         /* 所属 zone */
    uint32_t region_idx;      /* 在 zone 中的索引 */

    /* ---- Cache Line 2+: 大块内存 (懒加载) ---- */
    uint64_t *bitmap  ALIGN_8;
    uint64_t *summary ALIGN_8;
    uint32_t summary_words;
} idpool_region_base;

/* ============================================================
 * Region With Values (继承 Base + values 指针数组)
 * ============================================================ */
typedef struct idpool_region {
    idpool_region_base base;
    void **values;
} idpool_region;

/* ============================================================
 * Slot 槽 (Region 发布入口，平台自适应)
 * ============================================================ */
typedef struct CACHE_ALIGN {
#if ARCH_64BIT
    uint64_t packed;
#else
    volatile uint32_t idx;
    char _pad0[CACHE_LINE_SIZE - 4];
    volatile uint32_t version;
    char _pad1[CACHE_LINE_SIZE - 4];
#endif
} region_slot;

/* ============================================================
 * Zone (NUMA 节点，属性重排以节省空间)
 *
 * 布局分组:
 *   [热数据] zone_id / shift / mask / count / cap / mode
 *   [统计]   total_alloc / total_freed / scan_retries / reuse_count
 *   [全局位图] global_bm (GLOBAL_BMW * 8B)
 *   [动态 slot 数组] slots (指针 + capacity)
 *   [动态 registry] regions_ptr (指针 + capacity + old list]
 * ============================================================ */
typedef struct CACHE_ALIGN {
    /* ---- 热数据组 (Cache Line 0) ---- */
    uint32_t zone_id;
    uint32_t zone_shift;
    uint32_t zone_mask;
    uint32_t region_count;     /* 原子 */
    uint32_t region_cap;       /* slot 容量 */
    uint32_t mode;             /* WITH_VALUE / NO_VALUE */

    /* ---- 统计组 (Cache Line 1, 独立 cache line 减少 false sharing) ---- */
    uint64_t total_alloc   CACHE_ALIGN;
    uint64_t total_freed   CACHE_ALIGN;
    uint64_t scan_retries  CACHE_ALIGN;
    uint64_t reuse_count   CACHE_ALIGN;

    /* ---- 全局位图 (Cache Line 2) ---- */
    uint64_t global_bm[GLOBAL_BMW] CACHE_ALIGN;

    /* ---- 动态 slot 数组 (懒加载，初始 SLOT_INIT_CAP) ---- */
    region_slot *slots;
    uint32_t     slots_cap;

    /* ---- 动态 registry (懒加载 Region 指针数组) ---- */
    idpool_region_base **regions_ptr;
    uint32_t regions_cap;

    /* ---- 扩容旧数组延迟释放列表 ---- */
    void **old_arrays;
    uint32_t old_count;
    uint32_t old_cap;
} idpool_zone;

/* ============================================================
 * Pool (动态 zones 柔性数组)
 * ============================================================ */
struct fun_idpool_s {
    uint32_t numa_nodes;
    uint32_t aligned_nodes;
    uint32_t zone_shift;
    uint32_t zone_mask;
    uint32_t mode;
    /* 柔性数组: zones[0..numa_nodes-1] */
    idpool_zone *zones[];
};

/* ============================================================
 * Slot 操作 (平台自适应)
 * ============================================================ */
#if ARCH_64BIT
static inline void slot_publish(region_slot *s, uint32_t idx, uint32_t ver) {
    uint64_t p = ((uint64_t)ver << 32) | (uint64_t)idx;
    a_store64(&s->packed, p);
}
static inline uint64_t slot_load(region_slot *s) {
    return a_load64(&s->packed);
}
static inline uint32_t slot_idx(uint64_t p) { return (uint32_t)(p & 0xFFFFFFFFULL); }
static inline uint32_t slot_ver(uint64_t p) { return (uint32_t)(p >> 32); }
#else
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
 * 容量 / Base 公式 (不变)
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
        uint64_t inv = ~a_load64(&bm[wi]);  /* 空闲位 = 1 */
        if (inv) {
            uint64_t mask = ~((1ULL << bi) - 1);
            uint64_t rem = inv & mask;
            if (rem) {
                uint32_t bit = wi * 64 + __builtin_ctzll(rem);
                if (bit < cap) return bit;
            }
        }
        wi++;
    }

    /* 扫描后续完整字 */
    for (uint32_t i = wi; i < nw; i++) {
        uint64_t inv = ~a_load64(&bm[i]);
        if (inv) {
            uint32_t bit = i * 64 + __builtin_ctzll(inv);
            if (bit < cap) return bit;
        }
    }
    return cap;  /* 没找到, 返回 cap 表示失败 */
}

/* 从 0 开始扫描所有空闲位 (复用场景) */
static inline uint32_t find_zero_wrap(uint64_t *bm, uint32_t nw, uint32_t cap) {
    for (uint32_t i = 0; i < nw; i++) {
        uint64_t inv = ~a_load64(&bm[i]);
        if (inv) {
            uint32_t bit = i * 64 + __builtin_ctzll(inv);
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
 * Zone 动态数组: slot / registry 懒加载 + 扩容
 *
 * 设计要点:
 *   - slots:   region_slot 动态数组, 初始 SLOT_INIT_CAP = 4
 *   - regions: region 指针动态数组, 初始 SLOT_INIT_CAP = 4
 *   - 扩容策略: 2 倍增长, 上限 MAX_REGIONS
 *   - 旧数组进入 old_arrays 列表延迟释放, 避免正在读的线程踩空
 *
 * 内存开销 (小池):
 *   单 zone 仅 Region #0 时, slots + regions 只占 32 字节 (4+4 个指针)
 *   比之前固定数组 (128 + 1024 B) 省 96%
 * ============================================================ */

/* 确保 slots 数组足够大 */
/* 确保 slots 数组足够大
 *
 * Race 修复: 用 atomic store 逐个槽位复制, 替代 memcpy
 */
static void zone_slots_ensure(idpool_zone *z, uint32_t idx) {
    if (idx < z->slots_cap) return;

    uint32_t new_cap = z->slots_cap ? z->slots_cap : SLOT_INIT_CAP;
    while (new_cap <= idx) new_cap <<= 1;
    if (new_cap > MAX_REGIONS) new_cap = MAX_REGIONS;
    if (idx >= new_cap) abort();

    region_slot *old = z->slots;
    region_slot *new_slots = (region_slot *)a_alloc(new_cap * sizeof(region_slot));
    if (!new_slots) abort();

    if (old) {
        /* 用 atomic store 逐个槽位复制 (8 字节 packed) */
        for (uint32_t i = 0; i < z->slots_cap; i++) {
            a_store64((uint64_t *)&new_slots[i].packed,
                      a_load64((uint64_t *)&old[i].packed));
        }
        if (z->old_count >= z->old_cap) {
            uint32_t nc = z->old_cap ? z->old_cap * 2 : 4;
            void **nl = (void **)a_alloc(nc * sizeof(void *));
            if (nl) {
                for (uint32_t i = 0; i < z->old_count; i++) {
                    a_store64((uint64_t *)&nl[i], (uint64_t)z->old_arrays[i]);
                }
                free(z->old_arrays);
                z->old_arrays = nl;
                z->old_cap = nc;
            }
        }
        if (z->old_arrays && z->old_count < z->old_cap)
            z->old_arrays[z->old_count++] = old;
    }

    __atomic_thread_fence(__ATOMIC_RELEASE);
    a_store64((uint64_t *)&z->slots, (uint64_t)new_slots);
    a_store32(&z->slots_cap, new_cap);
}

/* 确保 registry 数组足够大
 *
 * Race 修复: 用 atomic store 逐个槽位复制, 替代 memcpy
 * - memcpy 不是 atomic, 期间其他 thread 可能读到部分写入的 garbage
 * - atomic_store64 逐个槽位发布, 任何 thread 读槽位 i 要么读到旧值要么新值
 */
static void zone_registry_ensure(idpool_zone *z, uint32_t idx) {
    if (idx < z->regions_cap) return;

    uint32_t new_cap = z->regions_cap ? z->regions_cap : SLOT_INIT_CAP;
    while (new_cap <= idx) new_cap <<= 1;
    if (new_cap > MAX_REGIONS) new_cap = MAX_REGIONS;
    if (idx >= new_cap) abort();

    idpool_region_base **old = z->regions_ptr;
    idpool_region_base **new_reg = (idpool_region_base **)a_alloc8(
        new_cap * sizeof(idpool_region_base *));
    if (!new_reg) abort();

    if (old) {
        /* 用 atomic store 逐个槽位复制, 避免 memcpy 撕裂 */
        for (uint32_t i = 0; i < z->regions_cap; i++) {
            a_store64((uint64_t *)&new_reg[i], (uint64_t)old[i]);
        }
        if (z->old_count >= z->old_cap) {
            uint32_t nc = z->old_cap ? z->old_cap * 2 : 4;
            void **nl = (void **)a_alloc(nc * sizeof(void *));
            if (nl) {
                for (uint32_t i = 0; i < z->old_count; i++) {
                    a_store64((uint64_t *)&nl[i], (uint64_t)z->old_arrays[i]);
                }
                free(z->old_arrays);
                z->old_arrays = nl;
                z->old_cap = nc;
            }
        }
        if (z->old_arrays && z->old_count < z->old_cap)
            z->old_arrays[z->old_count++] = old;
    }

    /* Release fence + atomic store: 确保所有槽位发布完, 再发布新数组指针 */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    a_store64((uint64_t *)&z->regions_ptr, (uint64_t)new_reg);
    a_store32(&z->regions_cap, new_cap);
}

/* 发布 Region 到动态 registry */
static void zone_publish_region(idpool_zone *z, uint32_t idx, idpool_region_base *r) {
    zone_registry_ensure(z, idx);
    a_store64((uint64_t *)&z->regions_ptr[idx], (uint64_t)r);
}

/* 加载 Region (处理扩容瞬态)
 *
 * 安全保证:
 *   - 每次循环都重读 arr 和 cap, 保证读到最新值
 *   - 重试上限 ZONE_LOAD_MAX_RETRY (避免 race 下无限循环)
 *   - 读到 NULL 或越界返回 NULL (调用方需检查)
 *
 * 性能调优: 重试上限 1000 次 × sched_yield ≈ 1 ms, 足够让快速
 *   race (zones_ptr 扩容) 完成, 不会显著影响正常路径
 */
#define ZONE_LOAD_MAX_RETRY 1000

static idpool_region_base *zone_load_region(idpool_zone *z, uint32_t idx) {
    for (int retry = 0; retry < ZONE_LOAD_MAX_RETRY; retry++) {
        idpool_region_base **arr = (idpool_region_base **)a_load64(
            (uint64_t *)&z->regions_ptr);
        if (!arr) return NULL;
        uint32_t cap = a_load32(&z->regions_cap);
        if (idx < cap) {
            idpool_region_base *r = (idpool_region_base *)a_load64(
                (uint64_t *)&arr[idx]);
            /* 防止 race 下读到陈旧值 (扩容时 memcpy 后旧数组元素可能被覆盖) */
            if (r == NULL) {
                sched_yield();
                continue;
            }
            return r;
        }
        sched_yield();
    }
    return NULL;  /* 重试超限, 让调用方安全跳过 */
}

/* 加载 slot */
static idpool_region_base *load_region(idpool_zone *z, uint32_t idx, uint64_t *ver_out) {
    if (idx >= MAX_REGIONS) { *ver_out = 0; return NULL; }

#if ARCH_64BIT
    if (idx >= z->slots_cap) { *ver_out = 0; return NULL; }
    region_slot *s = &z->slots[idx];
    uint64_t packed = slot_load(s);
    if (packed == 0) { *ver_out = 0; return NULL; }
    uint32_t s_idx = slot_idx(packed);
    *ver_out = slot_ver(packed);
    return zone_load_region(z, s_idx);
#else
    if (idx >= z->slots_cap) { *ver_out = 0; return NULL; }
    uint32_t s_idx = 0, s_ver = 0;
    slot_load(&z->slots[idx], &s_idx, &s_ver);
    *ver_out = s_ver;
    return zone_load_region(z, s_idx);
#endif
}

/* ============================================================
 * Region 懒加载内存分配
 *
 * 关键改动:
 *   NO_VALUE 模式: 只分配 bitmap + summary, 不分配 values
 *   WITH_VALUE 模式: 额外分配 values 数组
 *
 * Region #0: 分配后预设 bit 0 = 1 (永久保留, ID 0 永不分配)
 *   - cursor 已设置为 1, 但 bit 0 也显式标 1 防止 future code 误用
 * ============================================================ */
static void ensure_region(idpool_region_base *r, uint32_t mode) {
    if (a_load32(&r->alloced)) return;

    uint32_t cap = r->cap;
    uint32_t nw = (cap + 63) >> 6;     /* bitmap 字数 (64 bit/字) */
    uint32_t sw = (nw + 63) >> 6;     /* summary 字数 (summary 索引 bitmap) */

    /* bitmap 必须分配 (两种模式都需要) */
    uint64_t *bm = (uint64_t *)a_alloc8(nw * sizeof(uint64_t));
    if (!bm) abort();

    /* summary 必须分配 (记录每个 bitmap 字是否还有空闲位) */
    uint64_t *sum = (uint64_t *)a_alloc8(sw * sizeof(uint64_t));
    if (!sum) { free(bm); abort(); }
    /* summary 初始全 1 = 每个 bitmap 字都有空闲位 */
    memset(sum, 0xFF, sw * sizeof(uint64_t));

    /* values: 仅 WITH_VALUE 模式分配 (NO_VALUE 不存指针, 省内存) */
    void **vals = NULL;
    if (mode == FUN_IDPOOL_MODE_WITH_VALUE) {
        vals = (void **)a_alloc(cap * sizeof(void *));
        if (!vals) { free(bm); free(sum); abort(); }
    }

    /* CAS 发布 (只发布一次, 失败则释放已分配内存) */
    uint64_t *exp = NULL;
    if (__atomic_compare_exchange_n(&r->bitmap, &exp, bm, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        r->summary = sum;
        r->summary_words = sw;
        if (mode == FUN_IDPOOL_MODE_WITH_VALUE)
            ((idpool_region *)r)->values = vals;
        a_store32(&r->alloced, 1);

        /* Region #0: 保留 bit 0 (标记已占用, ID 0 永远不可分配) */
        if (r->region_idx == 0 && r->base == 0) {
            bit_tas(bm, 0);  /* 双保险: cursor 已 = 1, bit 0 也显式占 */
        }
    } else {
        /* 竞争失败: 其他线程抢先发布了 */
        free(bm); free(sum);
        if (vals) free(vals);
    }
}

/* ============================================================
 * 创建新 Region (串行化 + 懒加载)
 *
 * Region #0: cursor = 1 (跳过 bit 0)
 * 其他 Region: cursor = 0
 * ============================================================ */
static idpool_region_base *create_region(idpool_zone *z) {
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

            /* 按 mode 选择不同大小的结构体:
             *   NO_VALUE   → idpool_region_base (无 values 字段, 紧凑)
             *   WITH_VALUE → idpool_region       (含 values 字段) */
            idpool_region_base *r;
            if (z->mode == FUN_IDPOOL_MODE_NO_VALUE) {
                r = (idpool_region_base *)a_alloc(sizeof(idpool_region_base));
            } else {
                r = (idpool_region_base *)a_alloc(sizeof(idpool_region));
                ((idpool_region *)r)->values = NULL;
            }
            if (!r) abort();
            memset(r, 0, sizeof(idpool_region_base));  /* base 部分清零 */
            if (z->mode == FUN_IDPOOL_MODE_WITH_VALUE)
                ((idpool_region *)r)->values = NULL;

            r->base = base;
            r->cap = cap;
            r->cursor = (k == 0) ? 1 : 0;  /* Region #0 从 bit 1 开始 (跳 bit 0) */
            r->alloced = 0;
            r->state = 0;
            r->zone_id = z->zone_id;
            r->region_idx = k;

            /* 3 步发布: registry → slots → global_bm
             * 顺序很重要: 先 registry 才能 load_region, 再 slots 用于验证版本 */
            zone_publish_region(z, k, r);

            zone_slots_ensure(z, k);
            uint32_t ver = k + 1;  /* 版本号: 每次发布 +1, 用于 ABA 检测 */
            slot_publish(&z->slots[k], k, ver);

            uint32_t gi = k >> 6, gb = k & 63;
            if (gi < GLOBAL_BMW)
                a_for64(&z->global_bm[gi], 1ULL << gb);  /* 标记 Region 可用 */

            fprintf(stderr,
                    "[fun_idpool] zone%d: new region #%u base=%u cap=%u\n",
                    z->zone_id, k, base, cap);
            return r;
        }
        rc = a_load32(&z->region_count);
    }
}

/* ============================================================
 * 核心: Region 内分配 (Phase 0 单调 + Phase 1 复用)
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
 * Region #0 特殊处理:
 *   - cursor 初始为 1, 自然跳过 bit 0
 *   - bit 0 在 ensure_region 中预设为 1, 双保险
 * ============================================================ */
static uint32_t region_alloc(idpool_zone *z, idpool_region_base *r,
                             uint32_t node_id, void *v) {
    ensure_region(r, z->mode);
    if (!a_load32(&r->alloced)) return FUN_IDPOOL_INVALID_ID;

    uint32_t cap = r->cap;
    uint32_t nw  = (cap + 63) >> 6;
    uint64_t *bm = r->bitmap;
    if (!bm) return FUN_IDPOOL_INVALID_ID;

    /* ---- Phase 0: 单调递增 ---- */
    for (;;) {
        uint32_t cur = a_load32(&r->cursor);
        if (cur >= cap) break;  /* 已到末尾, 进入 Phase 1 */

        uint32_t bit = find_zero_from(bm, nw, cur, cap);
        if (bit >= cap) {
            /* cur 到 cap 之间无空闲位, 推 cursor 到 cap */
            uint32_t exp_cur = cur;
            a_cas32(&r->cursor, &exp_cur, cap);
            break;
        }

        if (!bit_tas(bm, bit)) {
            /* ✅ 成功占用! */
            /* 推进 cursor 到 bit+1 (CAS, 允许失败 — 其他线程可能也在推进) */
            uint32_t exp_cur = cur;
            uint32_t new_cur = bit + 1;
            if (new_cur > cap) new_cur = cap;
            a_cas32(&r->cursor, &exp_cur, new_cur);

            /* 更新 summary */
            upd_summary(r->summary, bm, bit >> 6, nw);

            /* 存储值 (仅 WITH_VALUE 模式)
             * 用 atomic store (RELEASE) 确保与 bit_tas 之间的 ordering,
             * 避免 worker 立即 get_value 读到陈旧值 (race) */
            if (z->mode == FUN_IDPOOL_MODE_WITH_VALUE) {
                idpool_region *rw = (idpool_region *)r;
                if (rw->values) {
                    __atomic_store_n(&rw->values[bit], (uintptr_t)v,
                                     __ATOMIC_RELEASE);
                }
            }
            a_fadd32(&r->used, 1);
            a_fadd64(&z->total_alloc, 1);

            /* 检查是否满了 */
            if (new_cur >= cap)
                a_store32(&r->state, 1);

            /* 编码 ID: (base + bit) << shift | node */
            return (r->base + bit) << z->zone_shift | node_id;
        }

        /* TAS 失败: 别人抢了 bit
         * 关键: 把 cursor 推过这个被抢的 bit, 不回退 */
        uint32_t exp_cur = cur;
        a_cas32(&r->cursor, &exp_cur, bit + 1);
        a_fadd64(&z->scan_retries, 1);
    }

    /* ---- Phase 1: 回绕扫描 (复用已释放的 ID) ----
     * 重要: 只有当 Region 真的满了才会到这里
     * 此时 cursor >= cap, 所有 bit 都被占过
     * 如果有释放的 bit, find_zero_wrap 能找到 */
    for (int retry = 0; retry < MAX_RETRY; retry++) {
        uint32_t bit = find_zero_wrap(bm, nw, cap);
        if (bit >= cap) break;  /* 真的全满了 */

        if (!bit_tas(bm, bit)) {
            /* 更新 summary */
            upd_summary(r->summary, bm, bit >> 6, nw);

            /* 存储值 (仅 WITH_VALUE 模式)
             * 用 atomic store (RELEASE) 确保与 bit_tas 之间的 ordering,
             * 避免 worker 立即 get_value 读到陈旧值 (race) */
            if (z->mode == FUN_IDPOOL_MODE_WITH_VALUE) {
                idpool_region *rw = (idpool_region *)r;
                if (rw->values) {
                    __atomic_store_n(&rw->values[bit], (uintptr_t)v,
                                     __ATOMIC_RELEASE);
                }
            }
            a_fadd32(&r->used, 1);
            a_fadd64(&z->total_alloc, 1);
            a_fadd64(&z->reuse_count, 1);

            /* Region 重新可用 */
            a_store32(&r->state, 0);
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
static uint32_t zone_alloc(idpool_zone *z, uint32_t node_id, void *v) {
    uint32_t rc = a_load32(&z->region_count);

    for (uint32_t pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < rc; i++) {
            uint32_t gi = i >> 6, gb = i & 63;
            if (gi < GLOBAL_BMW) {
                uint64_t gbv = a_load64(&z->global_bm[gi]);
                if ((gbv & (1ULL << gb)) == 0) continue;
            }

            uint64_t ver;
            idpool_region_base *r = load_region(z, i, &ver);
            if (!r) continue;

            if (a_load32(&r->state) == 1 && a_load32(&r->used) >= r->cap) {
                if (gi < GLOBAL_BMW)
                    a_fand64(&z->global_bm[gi], ~(1ULL << gb));
                continue;
            }

            uint32_t id = region_alloc(z, r, node_id, v);
            if (id != FUN_IDPOOL_INVALID_ID) return id;

            if (a_load32(&r->state) == 1) {
                if (gi < GLOBAL_BMW)
                    a_fand64(&z->global_bm[gi], ~(1ULL << gb));
            }
        }
    }

    /* 全满 → 创建新 Region */
    idpool_region_base *nr = create_region(z);
    if (nr) {
        uint32_t id = region_alloc(z, nr, node_id, v);
        if (id != FUN_IDPOOL_INVALID_ID) return id;
    }

    /* 极端竞争: 忙等 (加重试上限避免 race 下无限 spin)
     *
     * 正常情况下: 等待其他线程 release 释放 bit
     * 异常情况: 全 pool 已分配且不 release, 超限后返回 INVALID
     */
    for (int retry = 0; retry < 100; retry++) {
        rc = a_load32(&z->region_count);
        for (uint32_t i = 0; i < rc; i++) {
            uint64_t ver;
            idpool_region_base *r = load_region(z, i, &ver);
            if (!r) continue;
            uint32_t id = region_alloc(z, r, node_id, v);
            if (id != FUN_IDPOOL_INVALID_ID) return id;
        }
        sched_yield();
    }
    return FUN_IDPOOL_INVALID_ID;  /* 超限返回, 避免死循环 */
}

/* ============================================================
 * 公开 API
 * ============================================================ */

fun_idpool_t fun_idpool_create_ex(int numa_nodes, fun_idpool_mode_t mode) {
    int detected = detect_numa_nodes();
    if (numa_nodes <= 0) numa_nodes = detected;
    /* 安全上限: 1024 个 Zone (足够任何实际系统, 同时防止异常值) */
    if (numa_nodes > 1024) numa_nodes = 1024;
    if (numa_nodes < 1) numa_nodes = 1;

    int aligned = 1;
    while (aligned < numa_nodes) aligned <<= 1;

    size_t pool_sz = sizeof(fun_idpool_s) + numa_nodes * sizeof(idpool_zone *);
    fun_idpool_t pool = (fun_idpool_t)a_alloc(pool_sz);
    if (!pool) abort();
    pool->numa_nodes   = numa_nodes;
    pool->aligned_nodes = aligned;
    pool->zone_shift   = __builtin_ctz(aligned);
    pool->zone_mask    = aligned - 1;
    pool->mode         = (uint32_t)mode;

    for (int n = 0; n < numa_nodes; n++) {
        idpool_zone *z = (idpool_zone *)a_alloc(sizeof(idpool_zone));
        if (!z) abort();
        memset(z, 0, sizeof(*z));
        z->zone_id    = n;
        z->zone_shift = pool->zone_shift;
        z->zone_mask  = pool->zone_mask;
        z->mode       = (uint32_t)mode;

        /* Region #0: 立即创建并发布 */
        uint32_t cap0 = cap_of(0);
        idpool_region_base *r0;
        if (mode == FUN_IDPOOL_MODE_NO_VALUE) {
            r0 = (idpool_region_base *)a_alloc(sizeof(idpool_region_base));
        } else {
            r0 = (idpool_region_base *)a_alloc(sizeof(idpool_region));
            ((idpool_region *)r0)->values = NULL;
        }
        if (!r0) abort();
        memset(r0, 0, sizeof(idpool_region_base));
        r0->base = 0;
        r0->cap  = cap0;
        r0->cursor = 1;  /* Region #0 从 bit 1 开始 */
        r0->zone_id = n;
        r0->region_idx = 0;

        /* 初始化 slot 数组 */
        zone_slots_ensure(z, 0);
        zone_publish_region(z, 0, r0);
        slot_publish(&z->slots[0], 0, 1);

        z->region_count = 1;
        a_for64(&z->global_bm[0], 1ULL);

        pool->zones[n] = z;
    }

    fprintf(stderr,
            "[fun_idpool] created: %d zones (detected=%d aligned=%d) mode=%s INIT_CAP=%d\n",
            numa_nodes, detected, aligned,
            (mode == FUN_IDPOOL_MODE_NO_VALUE) ? "NO_VALUE" : "WITH_VALUE",
            INIT_CAP);
    return pool;
}

fun_idpool_t fun_idpool_create(int numa_nodes) {
    return fun_idpool_create_ex(numa_nodes, FUN_IDPOOL_MODE_WITH_VALUE);
}

void fun_idpool_destroy(fun_idpool_t pool) {
    if (!pool) return;
    for (uint32_t n = 0; n < pool->numa_nodes; n++) {
        idpool_zone *z = pool->zones[n];
        if (!z) continue;
        uint32_t rc = a_load32(&z->region_count);
        for (uint32_t i = 0; i < rc; i++) {
            idpool_region_base *r = zone_load_region(z, i);
            if (!r) continue;
            if (r->bitmap)  free(r->bitmap);
            if (r->summary) free(r->summary);
            if (pool->mode == FUN_IDPOOL_MODE_WITH_VALUE) {
                void **vals = ((idpool_region *)r)->values;
                if (vals) free(vals);
            }
            free(r);
        }
        if (z->slots)        free(z->slots);
        if (z->regions_ptr)  free(z->regions_ptr);

        /* 去重 old_arrays 中的指针, 防止 race 导致同一指针被多次加入
         *
         * Race 场景: 多线程同时扩容 old_arrays 时, zone_slots_ensure 或
         * zone_registry_ensure 的 realloc 路径不是 atomic:
         *   T1: memcpy(nl1, z->old_arrays) → free(z->old_arrays) → z->old_arrays = nl1
         *   T2: memcpy(nl2, z->old_arrays) → free(z->old_arrays)  // 同一指针 double free!
         *
         * 修复: destroy 时排序 + 去重, 确保每个指针只 free 一次
         */
        if (z->old_arrays && z->old_count > 0) {
            /* 简单 O(n²) 去重 (n 通常很小, < 100) */
            for (uint32_t i = 0; i < z->old_count; i++) {
                for (uint32_t j = i + 1; j < z->old_count; j++) {
                    if (z->old_arrays[i] == z->old_arrays[j]) {
                        z->old_arrays[j] = NULL;  /* 标记重复, 跳过 free */
                    }
                }
            }
            for (uint32_t i = 0; i < z->old_count; i++)
                if (z->old_arrays[i]) free(z->old_arrays[i]);
        }
        if (z->old_arrays)   free(z->old_arrays);
        free(z);
    }
    free(pool);
}

uint32_t fun_idpool_gen_id(fun_idpool_t pool, void *ptr) {
    int cpu = get_cpu();
    uint32_t n = cpu & pool->zone_mask;
    if (n >= pool->numa_nodes) n = cpu % pool->numa_nodes;

    return zone_alloc(pool->zones[n], n, ptr);
}

void *fun_idpool_get_value(fun_idpool_t pool, uint32_t id) {
    if (id == FUN_IDPOOL_INVALID_ID) return NULL;
    if (pool->mode == FUN_IDPOOL_MODE_NO_VALUE) {
        /* NO_VALUE: 仅校验存在性 */
        uint32_t n = id & pool->zone_mask;
        if (n >= pool->numa_nodes) return NULL;
        uint32_t idx = id >> pool->zone_shift;
        idpool_zone *z = pool->zones[n];
        uint32_t rc = a_load32(&z->region_count);
        for (uint32_t i = 0; i < rc; i++) {
            uint64_t ver;
            idpool_region_base *r = load_region(z, i, &ver);
            if (!r) continue;
            if (idx >= r->base && idx < r->base + r->cap) {
                uint32_t off = idx - r->base;
                if (off < r->cap && bit_test(r->bitmap, off))
                    return FUN_IDPOOL_EXISTS;
                break;
            }
        }
        return NULL;
    }

    /* WITH_VALUE: 返回绑定指针 */
    uint32_t n = id & pool->zone_mask;
    if (n >= pool->numa_nodes) return NULL;
    uint32_t idx = id >> pool->zone_shift;
    idpool_zone *z = pool->zones[n];
    uint32_t rc = a_load32(&z->region_count);
    for (uint32_t i = 0; i < rc; i++) {
        uint64_t ver;
        idpool_region_base *base = load_region(z, i, &ver);
        if (!base) continue;
        if (idx >= base->base && idx < base->base + base->cap) {
            uint32_t off = idx - base->base;
            if (off < base->cap && bit_test(base->bitmap, off)) {
                return ((idpool_region *)base)->values[off];
            }
            break;
        }
    }
    return NULL;
}

void *fun_idpool_release_id(fun_idpool_t pool, uint32_t id) {
    if (id == FUN_IDPOOL_INVALID_ID) return NULL;

    uint32_t n = id & pool->zone_mask;
    if (n >= pool->numa_nodes) return NULL;
    uint32_t idx = id >> pool->zone_shift;
    idpool_zone *z = pool->zones[n];
    uint32_t rc = a_load32(&z->region_count);

    for (uint32_t i = 0; i < rc; i++) {
        uint64_t ver;
        idpool_region_base *r = load_region(z, i, &ver);
        if (!r) continue;
        if (idx >= r->base && idx < r->base + r->cap) {
            uint32_t off = idx - r->base;
            if (off < r->cap && bit_test(r->bitmap, off)) {
                void *v = NULL;
                if (pool->mode == FUN_IDPOOL_MODE_WITH_VALUE) {
                    /* ACQUIRE load 配对 alloc 时的 RELEASE store,
                     * 保证看到 alloc 线程的完整写入 (race 防护) */
                    v = (void *)__atomic_load_n(
                        &((idpool_region *)r)->values[off],
                        __ATOMIC_ACQUIRE);
                    __atomic_store_n(
                        &((idpool_region *)r)->values[off], (uintptr_t)NULL,
                        __ATOMIC_RELAXED);
                }
                bit_clear(r->bitmap, off);
                a_fadd32(&r->used, -1);
                a_fadd64(&z->total_freed, 1);
                upd_summary(r->summary, r->bitmap, off >> 6,
                            (r->cap + 63) >> 6);
                if (a_load32(&r->state) == 1) {
                    a_store32(&r->state, 0);
                    uint32_t gi = i >> 6, gb = i & 63;
                    if (gi < GLOBAL_BMW)
                        a_for64(&z->global_bm[gi], 1ULL << gb);
                }
                return v;
            }
            return NULL;
        }
    }
    return NULL;
}

void fun_idpool_get_stats(fun_idpool_t pool, fun_idpool_stats_t stats) {
    memset(stats, 0, sizeof(*stats));
    stats->numa_nodes = pool->numa_nodes;
    stats->mode = pool->mode;
    for (uint32_t n = 0; n < pool->numa_nodes; n++) {
        idpool_zone *z = pool->zones[n];
        stats->total_alloc   += a_load64(&z->total_alloc);
        stats->total_freed   += a_load64(&z->total_freed);
        stats->scan_retries  += a_load64(&z->scan_retries);
        stats->reuse_count   += a_load64(&z->reuse_count);
        uint32_t rc = a_load32(&z->region_count);
        stats->total_regions += rc;

        /* 估算内存 */
        for (uint32_t i = 0; i < rc; i++) {
            idpool_region_base *r = zone_load_region(z, i);
            /* 双重 NULL 检查: zone_load_region 失败可能因 race 读 NULL */
            if (!r) continue;
            if (!a_load32(&r->alloced)) continue;  /* Region 未完成初始化 */
            /* 验证 cap 字段合法 (防止 race 读到陈旧指针) */
            uint32_t cap = r->cap;
            if (cap == 0 || cap > MAX_CAP) continue;
            uint32_t nw = (cap + 63) >> 6;
            uint32_t sw = (nw + 63) >> 6;
            stats->bitmap_memory += (nw + sw) * sizeof(uint64_t);
            if (pool->mode == FUN_IDPOOL_MODE_WITH_VALUE)
                stats->values_memory += cap * sizeof(void *);
        }
    }
}
