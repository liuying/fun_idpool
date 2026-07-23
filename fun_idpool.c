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
 * 编译期参数
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
 * Region #0 bit 0 保留策略
 *
 * Region #0: bit 0 永远不分配 → ID 0 = FUN_IDPOOL_INVALID_ID
 *   - bitmap[0] 的 bit 0 在初始化时预设为 1
 *   - cursor 初始值为 1 (不从 0 开始)
 *   - find_zero_from 从 bit 1 开始扫描
 *
 * 其他 Region: 正常使用 bit 0~63
 *
 * ID 编码: ID = (base + bit) << zone_shift | zone_id
 *   Region #0: base=0, bit 1~63 → ID 1~63
 *   Region #1: base=64, bit 0~63 → ID 64~127
 *
 * 为什么需要保留 bit 0?
 *   - ID 0 作为 FUN_IDPOOL_INVALID_ID 永不分配, 避免与"已分配 ID = 0"歧义
 *   - 调用方可以用 id == 0 作为"无效/未分配"判断, 无需额外标志
 * ============================================================ */

/* ============================================================
 * 数据结构
 * ============================================================ */

/* 基础 Region (所有模式共享) — 无 values 字段 */
typedef struct CACHE_ALIGN {
    /* ---- Cache Line 0: 分配热数据 ---- */
    uint32_t base;           /* Region 起始 ID (publish 后只读) */
    uint32_t cap;            /* Region 容量 (publish 后只读) */
    uint32_t used;           /* 已使用数 (原子) */
    uint32_t cursor;         /* 分配游标 (原子) */

    /* ---- Cache Line 1: 控制 ---- */
    uint32_t alloced;        /* 内存是否已分配完成 (原子发布标志) */
    uint32_t state;          /* 0=ACTIVE, 1=FULL, 2=RECYCLE */
    uint32_t zone_id;        /* 所属 zone */
    uint32_t region_idx;     /* 在 zone 中的索引 */

    /* ---- Cache Line 2+: 大块内存 (懒加载) ---- */
    uint64_t *bitmap  ALIGN_8;
    uint64_t *summary ALIGN_8;
    uint32_t summary_words;
    char _pad[4];            /* 对齐到 8 字节边界 */
} idpool_region_base;

/* 带 values 的 Region (WITH_VALUE 模式) — 嵌入 base + 追加 values */
typedef struct CACHE_ALIGN {
    idpool_region_base base; /* 基础字段 (必须与上面完全一致) */
    void **values;           /* 仅 WITH_VALUE 模式访问 */
} idpool_region;

/* 编译期断言: 验证 base 字段布局一致 */
typedef char static_assert_base_layout[
    sizeof(((idpool_region *)0)->base) == sizeof(idpool_region_base) ? 1 : -1
];

/* Zone (NUMA 节点) — registry 懒加载动态数组 */
typedef struct CACHE_ALIGN {
    /* ---- 热数据 ---- */
    uint32_t zone_id;
    uint32_t zone_shift;
    uint32_t zone_mask;
    uint32_t region_count;     /* 已发布 Region 数 (原子) */
    uint32_t region_cap;       /* regions_ptr 数组当前容量 (原子) */

    /* ---- 懒加载 registry: 按需扩容, 旧数组延迟释放 ---- */
    idpool_region_base **regions_ptr ALIGN_8;  /* 当前 region 指针数组 */
    void **old_arrays         ALIGN_8;         /* 已退役的 region 数组, 延迟 free */
    uint32_t old_array_count;                  /* 旧数组数量 */
    uint32_t old_array_cap;                    /* old_arrays 数组容量 */

    /* ---- 统计 (per-zone, 减少跨核竞争) ---- */
    uint64_t total_alloc   CACHE_ALIGN;
    uint64_t total_freed   CACHE_ALIGN;
    uint64_t scan_retries  CACHE_ALIGN;
    uint64_t reuse_count   CACHE_ALIGN;

    /* ---- 全局位图 ---- */
    uint64_t global_bm[GLOBAL_BMW] CACHE_ALIGN;

    /* ---- Region 槽数组 ---- */
    region_slot slots[MAX_REGIONS] CACHE_ALIGN;
} idpool_zone;

/* Pool */
struct fun_idpool_s {
    uint32_t numa_nodes;       /* 实际节点数 (用于内存分配) */
    uint32_t aligned_nodes;    /* 2 的幂 (用于位运算) */
    uint32_t zone_shift;       /* log2(对齐后的节点数) */
    uint32_t zone_mask;        /* 对齐后 - 1 */
    fun_idpool_mode_t mode;    /* 创建时确定, 不可变 */
    idpool_zone *zones[16] CACHE_ALIGN;
};

/* ============================================================
 * 前向声明
 * ============================================================ */
static void ensure_region_full(idpool_region_base *r, fun_idpool_mode_t mode);
static idpool_region_base *zone_registry_load(idpool_zone *z, uint32_t idx);

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
        uint64_t inv = ~w;  /* 空闲位 = 1 */
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
        uint64_t w = a_load64(&bm[i]);
        uint64_t inv = ~w;
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
        uint64_t w = a_load64(&bm[i]);
        uint64_t inv = ~w;
        if (inv) {
            uint32_t bit = i * 64 + __builtin_ctzll(inv);
            if (bit < cap) return bit;
        }
    }
    return cap;  /* 没找到 */
}

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
 * Zone Registry — 懒加载动态数组
 *
 * 设计要点:
 *   - 初始 cap = 2, 按 2 倍扩容 (2 → 4 → 8 → ... → MAX_REGIONS)
 *   - 旧数组进入 old_arrays 列表延迟释放, 避免正在读的线程踩空
 *   - publish 不需要扩容检测, ensure 自动处理
 *
 * 内存开销 (小池):
 *   单 zone 只用 Region #0 时, 数组只占 16 字节 (2 个指针)
 *   比 MAX_REGIONS (256 个指针 = 2KB) 省 99%
 * ============================================================ */
static void zone_registry_ensure(idpool_zone *z, uint32_t idx) {
    /* 首次分配: 初始 cap = 2 */
    if (a_load64((uint64_t *)&z->regions_ptr) == 0) {
        idpool_region_base **new_arr = (idpool_region_base **)a_alloc8(2 * sizeof(void *));
        if (!new_arr) abort();
        memset(new_arr, 0, 2 * sizeof(void *));
        uint64_t *exp = NULL;
        if (__atomic_compare_exchange_n((uint64_t *)&z->regions_ptr, &exp,
                                        (uint64_t)new_arr, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            a_store32(&z->region_cap, 2);
        } else {
            /* 竞争失败: 别人抢先发布了 */
            free(new_arr);
        }
    }

    /* 按需扩容: 2 倍增长, 上限 MAX_REGIONS */
    uint32_t cur_cap = a_load32(&z->region_cap);
    if (idx >= cur_cap) {
        uint32_t new_cap = cur_cap;
        while (new_cap <= idx) new_cap <<= 1;
        if (new_cap > MAX_REGIONS) new_cap = MAX_REGIONS;

        idpool_region_base **old_arr = z->regions_ptr;
        idpool_region_base **new_arr = (idpool_region_base **)a_alloc8(new_cap * sizeof(void *));
        if (!new_arr) abort();
        memcpy(new_arr, old_arr, cur_cap * sizeof(void *));
        memset(new_arr + cur_cap, 0, (new_cap - cur_cap) * sizeof(void *));

        a_store64((uint64_t *)&z->regions_ptr, (uint64_t)new_arr);
        a_store32(&z->region_cap, new_cap);

        /* 旧数组进入延迟释放列表, 等待所有线程读完 */
        if (z->old_array_count >= z->old_array_cap) {
            uint32_t nc = z->old_array_cap + 4;
            void **nl = (void **)a_alloc(nc * sizeof(void *));
            if (nl) {
                memcpy(nl, z->old_arrays, z->old_array_count * sizeof(void *));
                free(z->old_arrays);
                z->old_arrays = nl;
                z->old_array_cap = nc;
            }
        }
        if (z->old_array_count < z->old_array_cap)
            z->old_arrays[z->old_array_count++] = old_arr;
    }
}

/* 发布: 先确保容量足够, 再原子写入 */
static void zone_registry_publish(idpool_zone *z, uint32_t idx, idpool_region_base *r) {
    zone_registry_ensure(z, idx);
    a_store64((uint64_t *)&z->regions_ptr[idx], (uint64_t)r);
}

/* 加载: 循环等待扩容完成, 然后原子读取 */
static idpool_region_base *zone_registry_load(idpool_zone *z, uint32_t idx) {
    if (idx >= MAX_REGIONS) return NULL;
    for (;;) {
        idpool_region_base **arr = (idpool_region_base **)a_load64((uint64_t *)&z->regions_ptr);
        if (!arr) return NULL;
        uint32_t cur_cap = a_load32(&z->region_cap);
        if (idx < cur_cap) {
            return (idpool_region_base *)a_load64((uint64_t *)&arr[idx]);
        }
        /* idx 超出当前容量, 等待 ensure 扩容完成 */
        sched_yield();
    }
}

/* ============================================================
 * Region 内存分配 (懒加载)
 *
 * 关键改动:
 *   NO_VALUE 模式: 只分配 bitmap + summary, 不分配 values
 *                  且 r 实际是 idpool_region_base 大小, 无 values 字段
 *   WITH_VALUE 模式: 额外分配 values 数组
 *                  且 r 是 idpool_region 大小, 含 values 字段
 *
 * Region #0: 分配后预设 bit 0 = 1 (永久保留)
 *   - cursor 已设置为 1, 但 bit 0 也显式标 1 防止 future code 误用
 * ============================================================ */
static void ensure_region_full(idpool_region_base *r, fun_idpool_mode_t mode) {
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
            vals = NULL;  /* ownership transferred */
        }

        /* ★ Region #0: bit 0 永远保留 (双保险, 即使 cursor 起点正确) */
        if (r->region_idx == 0) {
            __atomic_fetch_or(&bm[0], 1ULL, __ATOMIC_RELEASE);
        }

        a_store32(&r->alloced, 1);
    } else {
        /* 竞争失败: 别人抢先发布了 */
        free(bm);
        free(sum);
        if (vals) free(vals);
    }
}

/* ============================================================
 * 创建新 Region (串行化 + 按 mode 分配不同结构体大小)
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

            idpool_region_base *r;
            if (mode == FUN_IDPOOL_MODE_NO_VALUE) {
                r = (idpool_region_base *)a_alloc(sizeof(idpool_region_base));
            } else {
                idpool_region *rw = (idpool_region *)a_alloc(sizeof(idpool_region));
                rw->values = NULL;
                r = (idpool_region_base *)rw;
            }
            if (!r) abort();
            memset(r, 0, sizeof(*r));
            r->base = base;
            r->cap = cap;
            r->zone_id = z->zone_id;
            r->region_idx = k;

            /*
             * ★ Region #0: cursor 从 1 开始 (bit 0 保留)
             *   其他 Region: cursor 从 0 开始
             */
            r->cursor = (k == 0) ? 1 : 0;

            /* 立即分配内存并发布 */
            ensure_region_full(r, mode);

            zone_registry_publish(z, k, r);
            slot_publish(&z->slots[k], k, k + 1);

            uint32_t gi = k >> 6, gb = k & 63;
            if (gi < GLOBAL_BMW)
                a_for64(&z->global_bm[gi], 1ULL << gb);

            fprintf(stderr,
                    "[fun_idpool] zone%d: region #%u base=%u cap=%u%s\n",
                    z->zone_id, k, base, cap,
                    (k == 0) ? " (bit0 reserved)" : "");
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
    uint64_t packed = slot_load(&z->slots[idx]);
    if (packed == 0) { *ver_out = 0; return NULL; }
    *ver_out = slot_ver(packed);
    return zone_registry_load(z, slot_idx(packed));
#else
    uint32_t s_idx = 0, s_ver = 0;
    slot_load(&z->slots[idx], &s_idx, &s_ver);
    *ver_out = s_ver;
    return zone_registry_load(z, s_idx);
#endif
}

/* ============================================================
 * Region 内分配 (核心)
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
 *   - bit 0 在 ensure_region_full 中预设为 1, 双保险
 * ============================================================ */
static uint32_t region_alloc(idpool_zone *z, idpool_region_base *r,
                              uint32_t node_id, void *v, fun_idpool_mode_t mode) {
    /* 确保内存已分配 */
    if (!a_load32(&r->alloced)) {
        ensure_region_full(r, mode);
        if (!a_load32(&r->alloced)) return FUN_IDPOOL_INVALID_ID;
    }

    uint32_t cap = r->cap;
    uint32_t nw  = (cap + 63) >> 6;
    uint64_t *bm  = r->bitmap;
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

            /* 存储值 (仅 WITH_VALUE 模式) */
            if (mode == FUN_IDPOOL_MODE_WITH_VALUE) {
                idpool_region *rw = (idpool_region *)r;
                if (rw->values) rw->values[bit] = v;
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

            /* 存储值 (仅 WITH_VALUE 模式) */
            if (mode == FUN_IDPOOL_MODE_WITH_VALUE) {
                idpool_region *rw = (idpool_region *)r;
                if (rw->values) rw->values[bit] = v;
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
 * 公开 API
 * ============================================================ */

fun_idpool_t fun_idpool_create(int numa_nodes) {
    return fun_idpool_create_ex(numa_nodes, FUN_IDPOOL_MODE_WITH_VALUE);
}

fun_idpool_t fun_idpool_create_ex(int numa_nodes, fun_idpool_mode_t mode) {
    int detected = detect_numa_nodes();
    if (numa_nodes <= 0) numa_nodes = detected;
    if (numa_nodes > 16) numa_nodes = 16;
    if (numa_nodes < 1) numa_nodes = 1;

    int aligned = 1;
    while (aligned < numa_nodes) aligned <<= 1;

    fun_idpool_t pool = (fun_idpool_t)a_alloc(sizeof(fun_idpool_s));
    if (!pool) abort();
    pool->numa_nodes   = numa_nodes;
    pool->aligned_nodes = aligned;
    pool->zone_shift   = __builtin_ctz(aligned);
    pool->zone_mask    = aligned - 1;
    pool->mode         = mode;

    for (int n = 0; n < numa_nodes; n++) {
        idpool_zone *z = (idpool_zone *)a_alloc(sizeof(idpool_zone));
        if (!z) abort();
        memset(z, 0, sizeof(*z));
        z->zone_id    = n;
        z->zone_shift = pool->zone_shift;
        z->zone_mask  = pool->zone_mask;

        /* Region #0: 立即创建并初始化 */
        uint32_t cap0 = cap_of(0);
        idpool_region_base *r0;
        if (mode == FUN_IDPOOL_MODE_NO_VALUE) {
            r0 = (idpool_region_base *)a_alloc(sizeof(idpool_region_base));
        } else {
            idpool_region *rw = (idpool_region *)a_alloc(sizeof(idpool_region));
            rw->values = NULL;
            r0 = (idpool_region_base *)rw;
        }
        if (!r0) abort();
        memset(r0, 0, sizeof(*r0));
        r0->base = 0;
        r0->cap  = cap0;
        r0->zone_id = n;
        r0->region_idx = 0;
        r0->cursor = 1;  /* ★ bit 0 保留, 从 bit 1 开始 */

        /* 立即分配 bitmap (含 bit 0 预设) */
        ensure_region_full(r0, mode);

        zone_registry_publish(z, 0, r0);
        slot_publish(&z->slots[0], 0, 1);

        z->region_count = 1;
        a_for64(&z->global_bm[0], 1ULL);

        pool->zones[n] = z;
    }

    fprintf(stderr,
            "[fun_idpool] created: %d zones (mode=%s, Region#0 bit0 reserved)\n",
            numa_nodes,
            (mode == FUN_IDPOOL_MODE_NO_VALUE) ? "NO_VALUE" : "WITH_VALUE");
    return pool;
}

void fun_idpool_destroy(fun_idpool_t pool) {
    if (!pool) return;
    for (uint32_t n = 0; n < pool->numa_nodes; n++) {
        idpool_zone *z = pool->zones[n];
        if (!z) continue;
        uint32_t rc = a_load32(&z->region_count);
        for (uint32_t i = 0; i < rc; i++) {
            idpool_region_base *r = zone_registry_load(z, i);
            if (!r) continue;
            if (r->bitmap)  free(r->bitmap);
            if (r->summary) free(r->summary);
            if (pool->mode == FUN_IDPOOL_MODE_WITH_VALUE) {
                idpool_region *rw = (idpool_region *)r;
                if (rw->values) free(rw->values);
            }
            free(r);
        }
        if (z->regions_ptr) free(z->regions_ptr);
        for (uint32_t i = 0; i < z->old_array_count; i++)
            free(z->old_arrays[i]);
        if (z->old_arrays) free(z->old_arrays);
        free(z);
    }
    free(pool);
}

uint32_t fun_idpool_gen_id(fun_idpool_t pool, void *ptr) {
    int cpu = get_cpu();
    uint32_t n = cpu & pool->zone_mask;
    if (n >= pool->numa_nodes) n = cpu % pool->numa_nodes;

    idpool_zone *z = pool->zones[n];
    return zone_alloc(z, n, ptr, pool->mode);
}

void *fun_idpool_get_value(fun_idpool_t pool, uint32_t id) {
    /* ID 0 永远不存在 */
    if (id == FUN_IDPOOL_INVALID_ID) return NULL;
    if (id == 0) return NULL;

    uint32_t n   = id & pool->zone_mask;
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
                if (pool->mode == FUN_IDPOOL_MODE_NO_VALUE) {
                    return FUN_IDPOOL_EXISTS;
                } else {
                    idpool_region *rw = (idpool_region *)r;
                    if (rw->values) return rw->values[off];
                }
            }
            break;
        }
    }
    return NULL;
}

void *fun_idpool_release_id(fun_idpool_t pool, uint32_t id) {
    if (id == FUN_IDPOOL_INVALID_ID) return NULL;
    if (id == 0) return NULL;  /* ID 0 永远不分配 */

    uint32_t n   = id & pool->zone_mask;
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
                    idpool_region *rw = (idpool_region *)r;
                    if (rw->values) {
                        v = rw->values[off];
                        rw->values[off] = NULL;
                    }
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
        stats->total_regions += a_load32(&z->region_count);

        uint32_t rc = a_load32(&z->region_count);
        for (uint32_t i = 0; i < rc; i++) {
            idpool_region_base *r = zone_registry_load(z, i);
            if (!r) continue;
            uint32_t nw = (r->cap + 63) >> 6;
            uint32_t sw = (nw + 63) >> 6;
            stats->bitmap_memory += nw * 8 + sw * 8;
            if (pool->mode == FUN_IDPOOL_MODE_WITH_VALUE) {
                stats->values_memory += r->cap * 8;
            }
            stats->region_struct_memory += (pool->mode == FUN_IDPOOL_MODE_NO_VALUE)
                ? sizeof(idpool_region_base) : sizeof(idpool_region);
        }
    }
}
