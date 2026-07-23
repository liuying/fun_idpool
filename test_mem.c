/*
 * test_mem.c — fun_idpool 精确内存使用分析
 *
 * 方法: mallinfo2() 在关键节点采集 glibc 堆内存快照，
 *       配合 fun_idpool_get_stats() 做交叉验证，
 *       并用公式化模型预测每类内存的理论值。
 *
 * 编译: gcc -O3 -std=c99 -Wall -Wextra test_mem.c fun_idpool.c -o test_mem -lpthread
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>

#include "fun_idpool.h"

/* ---------- mallinfo2 堆快照 ---------- */
static long long heap_used(void) {
    struct mallinfo2 mi = mallinfo2();
    return (long long)mi.uordblks;
}

/* 打印完整 mallinfo2 breakdown */
static void print_mallinfo(const char *when) {
    struct mallinfo2 mi = mallinfo2();
    fprintf(stderr,
        "[MEM]   mallinfo2: arena=%lld hblkhd=%lld ordblks=%lld smblks=%lld "
        "uordblks=%lld fordblks=%lld keepcost=%lld\n",
        (long long)mi.arena, (long long)mi.hblkhd,
        (long long)mi.ordblks, (long long)mi.smblks,
        (long long)mi.uordblks, (long long)mi.fordblks,
        (long long)mi.keepcost);
    (void)when;
}

/* ---------- 工具 ---------- */
static uint64_t max_u64(uint64_t a, uint64_t b) { return a > b ? a : b; }

/* ---------- 公式化预测 ---------- */
static uint32_t cap_of(uint32_t k) {
    if (k == 0) return 64;
    uint32_t c = 64;
    for (uint32_t i = 0; i < k && c < (1u << 20); i++) {
        uint32_t n = c << 1;
        if (n < c) { c = (1u << 20); break; }
        c = n;
    }
    if (c > (1u << 20)) c = (1u << 20);
    return c;
}

/* 给定总分配数 n，估算需要多少 Region（最坏情况：全在新 Region） */
static void estimate(const char *label, uint64_t n_alloc, int zones) {
    fprintf(stderr, "\n[MEM] --- 公式估算 [%s] (target_alloc=%llu, zones=%d) ---\n",
            label, (unsigned long long)n_alloc, zones);

    /* 找到覆盖 n_alloc 所需 Region 数 K */
    uint64_t sum = 0;
    int K = 0;
    for (; K < 30; K++) {
        sum += cap_of(K);
        if (sum >= n_alloc) break;
    }

    uint64_t total_struct = 0, total_bm = 0, total_su = 0, total_val = 0;
    fprintf(stderr, "[MEM]   Regions 0..%d needed (sum_cap=%llu):\n",
            K, (unsigned long long)sum);
    fprintf(stderr, "[MEM]   %4s %7s %8s %8s %8s %10s\n",
            "R#", "cap", "struct", "bitmap", "summary", "values");

    for (int i = 0; i <= K; i++) {
        uint32_t cap = cap_of(i);
        uint32_t nw = (cap + 63) >> 6;
        uint32_t sw = (nw + 63) >> 6;
        uint64_t st = 64;             /* idpool_region 结构 (对齐后) */
        uint64_t bm = (uint64_t)nw * 8;
        uint64_t su = (uint64_t)sw * 8;
        uint64_t va = (uint64_t)cap * 8;  /* 64-bit 指针 */
        total_struct += st;
        total_bm += bm;
        total_su += su;
        total_val += va;
        fprintf(stderr, "[MEM]   %4d %7u %8llu %8llu %8llu %10llu  (total=%lluB)\n",
                i, cap, (unsigned long long)st, (unsigned long long)bm,
                (unsigned long long)su, (unsigned long long)va,
                (unsigned long long)(st+bm+su+va));
    }

    /* × zones */
    uint64_t all_struct = total_struct * zones;
    uint64_t all_bm     = total_bm * zones;
    uint64_t all_su     = total_su * zones;
    uint64_t all_val    = total_val * zones;
    uint64_t grand      = all_struct + all_bm + all_su + all_val;

    /* 固定开销 */
    uint64_t fixed_pool   = 64 * zones;       /* fun_idpool_s × zones (padded) */
    uint64_t fixed_zone   = 512 * zones;      /* idpool_zone (padded) */
    uint64_t fixed_registry = 8 * 256 * zones; /* zone_registry global */
    uint64_t fixed = fixed_pool + fixed_zone + fixed_registry;

    fprintf(stderr, "[MEM]   --- ×%d zones ---\n", zones);
    fprintf(stderr, "[MEM]   region_struct: %10llu B\n", (unsigned long long)all_struct);
    fprintf(stderr, "[MEM]   bitmap:        %10llu B\n", (unsigned long long)all_bm);
    fprintf(stderr, "[MEM]   summary:       %10llu B\n", (unsigned long long)all_su);
    fprintf(stderr, "[MEM]   values:        %10llu B  <-- 占内存大头\n", (unsigned long long)all_val);
    fprintf(stderr, "[MEM]   dynamic total: %10llu B (%.2f KB)\n",
            (unsigned long long)grand, (double)grand / 1024);
    fprintf(stderr, "[MEM]   fixed overhead: %10llu B (%.2f KB)\n",
            (unsigned long long)fixed, (double)fixed / 1024);
    fprintf(stderr, "[MEM]   GRAND TOTAL:   %10llu B (%.2f KB)\n",
            (unsigned long long)(grand + fixed), (double)(grand + fixed) / 1024);

    /* 效率指标 */
    double bpa = (double)(grand + fixed) / (double)max_u64(n_alloc, 1);
    fprintf(stderr, "[MEM]   efficiency:    %.2f B per allocation\n", bpa);
}

/* ---------- 快照 + 打印 ---------- */
static fun_idpool_t g_pool = NULL;

static void snap(const char *label) {
    long long h = heap_used();

    fun_idpool_stats_s s;
    if (g_pool) fun_idpool_get_stats(g_pool, &s);
    else memset(&s, 0, sizeof(s));

    fprintf(stderr,
        "\n========== [SNAPSHOT: %s] ==========\n", label);
    fprintf(stderr, "[MEM] heap (mallinfo2.uordblks): %lld B (%.2f KB)\n",
            h, (double)h / 1024);
    fprintf(stderr, "[MEM] pool_stats: alloc=%llu freed=%llu reuse=%llu "
            "regions=%u zones=%u\n",
            (unsigned long long)s.total_alloc,
            (unsigned long long)s.total_freed,
            (unsigned long long)s.reuse_count,
            s.total_regions, s.numa_nodes);
    print_mallinfo(label);
}

/* ---------- 主测试 ---------- */
int main(void) {
    fprintf(stderr, "=== fun_idpool 内存使用精确分析 ===\n\n");

    /* 0. 基线 (无任何 pool) */
    snap("0_baseline_empty");
    estimate("baseline", 0, 4);

    /* 1. 创建 4-Zone Pool (Region #0 急切创建) */
    g_pool = fun_idpool_create(4);
    if (!g_pool) { fprintf(stderr, "FATAL: create failed\n"); return 1; }
    snap("1_after_create_4zones");
    estimate("after_create", 0, 4);

    /* 2. 分配 10 个 ID */
    fprintf(stderr, "\n>>> 分配 10 个 ID...\n");
    for (int i = 0; i < 10; i++)
        (void)fun_idpool_gen_id(g_pool, (void *)(uintptr_t)i);
    snap("2_after_10_allocs");
    estimate("10_allocs", 10, 4);

    /* 3. 分配 100 个 ID */
    fprintf(stderr, "\n>>> 分配 100 个 ID...\n");
    for (int i = 0; i < 100; i++)
        (void)fun_idpool_gen_id(g_pool, (void *)(uintptr_t)(100 + i));
    snap("3_after_100_allocs");
    estimate("100_allocs", 100, 4);

    /* 4. 分配 1000 个 ID */
    fprintf(stderr, "\n>>> 分配 1000 个 ID...\n");
    for (int i = 0; i < 1000; i++)
        (void)fun_idpool_gen_id(g_pool, (void *)(uintptr_t)(1000 + i));
    snap("4_after_1000_allocs");
    estimate("1000_allocs", 1000, 4);

    /* 5. 分配 10000 个 ID */
    fprintf(stderr, "\n>>> 分配 10000 个 ID...\n");
    for (int i = 0; i < 10000; i++)
        (void)fun_idpool_gen_id(g_pool, (void *)(uintptr_t)(10000 + i));
    snap("5_after_10000_allocs");
    estimate("10000_allocs", 10000, 4);

    /* 6. 分配 100000 个 ID */
    fprintf(stderr, "\n>>> 分配 100000 个 ID...\n");
    for (int i = 0; i < 100000; i++)
        (void)fun_idpool_gen_id(g_pool, (void *)(uintptr_t)(100000 + i));
    snap("6_after_100000_allocs");
    estimate("100000_allocs", 100000, 4);

    /* 7. 分配 1000000 个 ID */
    fprintf(stderr, "\n>>> 分配 1000000 个 ID...\n");
    for (int i = 0; i < 1000000; i++)
        (void)fun_idpool_gen_id(g_pool, (void *)(uintptr_t)(1000000 + i));
    snap("7_after_1000000_allocs");
    estimate("1M_allocs", 1000000, 4);

    /* 8. 模拟"长时间 ID 占比 10%"场景 */
    fprintf(stderr, "\n\n========== 场景: 长时间 ID 占比 10%% ==========\n");
    fprintf(stderr, ">>> 已分配 1M ID，现在释放 90%%，保留 100K...\n");

    /*
     * 问题: 我们没有保存之前的 ID，无法精确释放特定 90%。
     * 解决: 用 get_value + release 扫描前 1M 个可能的 ID 范围太慢。
     * 简化: 直接释放一批已知 tag 范围的 ID。
     * 但我们的 tag 是 1000000+i，get_value 需要 ID 本身。
     *
     * 实际上这个验证需要保存 ID 数组。改为: 重新分配一批，保存，再释放。
     * 当前堆已经很大(1M ID)，先做 destroy + recreate 干净场景。
     */
    fun_idpool_destroy(g_pool);
    g_pool = NULL;
    snap("8a_after_destroy_1M");

    /* 干净场景: 分配 1M，保存 ID，释放 90% */
    g_pool = fun_idpool_create(4);
    uint32_t *ids = (uint32_t *)malloc(1000000 * sizeof(uint32_t));
    fprintf(stderr, "\n>>> 重新分配 1M ID (保存指针)...\n");
    for (int i = 0; i < 1000000; i++)
        ids[i] = fun_idpool_gen_id(g_pool, (void *)(uintptr_t)(2000000 + i));
    snap("8b_1M_allocated");
    estimate("1M_allocated", 1000000, 4);

    /* 释放 90% (保留 i%10==0 的 10%) */
    fprintf(stderr, "\n>>> 释放 900K ID (保留 100K = 10%%)...\n");
    uint32_t kept = 0;
    for (int i = 0; i < 1000000; i++) {
        if (i % 10 != 0)
            fun_idpool_release_id(g_pool, ids[i]);
        else
            kept++;
    }
    snap("8c_after_release_90pct");
    fprintf(stderr, "[MEM] kept %u IDs (10%%)\n", kept);
    /*
     * 关键观察: 释放后堆内存不会归还 OS，但 Region 内部 bit 变空闲。
     * 后续分配应该优先复用这些空闲 bit，不创建新 Region。
     */

    /* 再分配 500K 新 ID */
    fprintf(stderr, "\n>>> 再分配 500K 新 ID (期望复用空闲 bit)...\n");
    for (int i = 0; i < 500000; i++)
        (void)fun_idpool_gen_id(g_pool, (void *)(uintptr_t)(3000000 + i));
    snap("8d_after_500K_more");
    estimate("1.5M_total", 1500000, 4);

    /* 再释放 400K，保留 200K (20% of peak) */
    fprintf(stderr, "\n>>> 释放 400K，最终保留 ~200K (20%% of peak)...\n");
    for (int i = 0; i < 500000; i++) {
        if (i % 5 != 0) {
            uint32_t id = fun_idpool_gen_id(g_pool, (void *)(uintptr_t)(4000000 + i));
            if (id != 0) fun_idpool_release_id(g_pool, id);
        }
    }
    snap("8e_final_200K_keep");

    free(ids);

    /* 9. 清理 */
    fun_idpool_destroy(g_pool);
    g_pool = NULL;
    snap("9_after_destroy_final");
    estimate("destroyed", 0, 4);

    fprintf(stderr, "\n=== 分析完成 ===\n");
    return 0;
}
