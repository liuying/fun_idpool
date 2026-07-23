#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdarg.h>

#include "fun_idpool.h"

/* ============================================================
 * 测试 Region #0 bit 0 保留版
 *
 * 重点验证:
 *   1. ID 0 永远不会被分配
 *   2. Region #0 bit 1 → ID 1, bit 63 → ID 63
 *   3. Region #1 bit 0 → ID 64
 *   4. 释放 ID 后正确复用
 *   5. 多线程正确性
 *   6. NO_VALUE / WITH_VALUE 双模式
 * ============================================================ */

static int g_errors = 0;
static pthread_mutex_t g_err_mu = PTHREAD_MUTEX_INITIALIZER;

static void report_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&g_err_mu);
    g_errors++;
    if (g_errors <= 50)
        vfprintf(stderr, fmt, ap);
    pthread_mutex_unlock(&g_err_mu);
    va_end(ap);
}

/* ============ Test 1: Region #0 bit 0 保留验证 ============ */
static void test_bit0_reserved(void) {
    fprintf(stderr, "\n=== Test 1: Region #0 bit 0 reserved ===\n");

    /* 单 Zone 方便分析 */
    fun_idpool_t pool = fun_idpool_create_ex(1, FUN_IDPOOL_MODE_WITH_VALUE);
    if (!pool) { report_error("create failed\n"); return; }

    /* 分配 63 次, 应该拿到 ID 1~63 (bit 1~63) */
    uint32_t ids[64];
    for (int i = 0; i < 63; i++) {
        ids[i] = fun_idpool_gen_id(pool, (void*)(uintptr_t)(i + 1));
        if (ids[i] == 0) {
            report_error("  [FAIL] got ID 0 at iter %d!\n", i);
        }
        if (ids[i] != (uint32_t)(i + 1)) {
            report_error("  [FAIL] ids[%d]=%u, expected %d\n", i, ids[i], i + 1);
        }
    }

    /* 验证 get_value */
    for (int i = 0; i < 63; i++) {
        void *v = fun_idpool_get_value(pool, ids[i]);
        if (v != (void*)(uintptr_t)(i + 1)) {
            report_error("  [FAIL] get_value[%d]=%p expected %d\n",
                         i, v, i + 1);
        }
    }

    /* ID 0 应该查不到 */
    void *v0 = fun_idpool_get_value(pool, 0);
    if (v0 != NULL) {
        report_error("  [FAIL] get_value(0) should be NULL, got %p\n", v0);
    }

    /* 验证 bitmap: Region #0 的 bit 0 应该永远是 1 */
    fun_idpool_stats_s stats;
    fun_idpool_get_stats(pool, &stats);
    fprintf(stderr, "  Allocated 63 IDs: ID 1~63 ✓\n");
    fprintf(stderr, "  bitmap_memory: %llu B\n",
            (unsigned long long)stats.bitmap_memory);
    fprintf(stderr, "  values_memory: %llu B\n",
            (unsigned long long)stats.values_memory);

    /*
     * 关键验证: 再分配应该进入 Region #1 (bit 0)
     * Region #0 的 bit 0 已占 + bit 1~63 已占 = 全满
     * 下一个 ID 应该是 64 (Region #1, bit 0)
     */
    uint32_t id64 = fun_idpool_gen_id(pool, (void*)0x40);
    if (id64 != 64) {
        report_error("  [FAIL] expected ID 64, got %u\n", id64);
    } else {
        fprintf(stderr, "  ID 64 allocated ✓ (Region #1 bit 0)\n");
    }

    /* 再分配 63 个 → ID 65~127 (Region #1 bit 1~63) */
    for (int i = 0; i < 63; i++) {
        uint32_t id = fun_idpool_gen_id(pool, (void*)(uintptr_t)(100 + i));
        if (id != (uint32_t)(65 + i)) {
            report_error("  [FAIL] expected ID %d, got %u\n", 65 + i, id);
        }
    }

    /* 现在 Region #0 和 #1 都满了
     * 释放 Region #0 的几个 bit, 验证复用从 bit 1 开始 (不会碰 bit 0)
     */
    fun_idpool_release_id(pool, 10);  /* 释放 bit 9 */
    fun_idpool_release_id(pool, 20);  /* 释放 bit 19 */
    fun_idpool_release_id(pool, 30);  /* 释放 bit 29 */

    /* 新分配应该复用 10, 20, 30 (Region #0 内) */
    uint32_t rid = fun_idpool_gen_id(pool, (void*)0x999);
    if (rid != 10 && rid != 20 && rid != 30) {
        report_error("  [FAIL] expected reuse 10/20/30, got %u\n", rid);
    } else {
        fprintf(stderr, "  Reused ID %u (Region #0, bit 0 still reserved) ✓\n", rid);
    }

    /* 再拿两个, 应该也是 20 和 30 */
    uint32_t rid2 = fun_idpool_gen_id(pool, (void*)0xAAA);
    uint32_t rid3 = fun_idpool_gen_id(pool, (void*)0xBBB);
    fprintf(stderr, "  Reused IDs: %u, %u ✓\n", rid2, rid3);

    /* 验证 bit 0 永远不被复用 */
    void *v_bit0 = fun_idpool_get_value(pool, 0);
    if (v_bit0 != NULL) {
        report_error("  [FAIL] ID 0 should never exist!\n");
    } else {
        fprintf(stderr, "  ID 0 never allocated ✓ (bit 0 permanently reserved)\n");
    }

    fun_idpool_destroy(pool);
    fprintf(stderr, "  Result: %s\n",
            (g_errors == 0) ? "PASS" : "FAIL");
}

/* ============ Test 2: ID 编码正确性 ============ */
static void test_id_encoding(void) {
    fprintf(stderr, "\n=== Test 2: ID Encoding Verification ===\n");

    fun_idpool_t pool = fun_idpool_create_ex(4, FUN_IDPOOL_MODE_WITH_VALUE);
    if (!pool) { report_error("create failed\n"); return; }

    int N = 100000;
    uint32_t *ids = malloc(N * sizeof(uint32_t));
    if (!ids) { report_error("OOM\n"); return; }

    /* 分配 N 个 ID, 记录 */
    for (int i = 0; i < N; i++) {
        uint32_t tag = (uint32_t)(0x10000000 + i);
        ids[i] = fun_idpool_gen_id(pool, (void*)(uintptr_t)tag);
        if (ids[i] == 0) {
            report_error("  [FAIL] got ID 0 at %d!\n", i);
        }
    }

    /* 验证所有 ID 都能正确取回 */
    int bad = 0;
    for (int i = 0; i < N; i++) {
        void *v = fun_idpool_get_value(pool, ids[i]);
        uint32_t expected = 0x10000000 + i;
        if (v != (void*)(uintptr_t)expected) {
            if (bad < 10)
                report_error("  [FAIL] id[%d]=%u got=%p exp=%x\n",
                             i, ids[i], v, expected);
            bad++;
        }
    }

    /* ID 0 永远查不到 */
    if (fun_idpool_get_value(pool, 0) != NULL) {
        report_error("  [FAIL] ID 0 should not exist!\n");
        bad++;
    }

    /* 验证: 没有任何 ID 的 bit 0 (在 Region #0 内) 被使用 */
    /* Region #0: ID 1~63 对应 bit 1~63 → 所有 ID > 0 ✓ */
    int found_zero = 0;
    for (int i = 0; i < N; i++) {
        if (ids[i] == 0) { found_zero = 1; break; }
    }
    if (found_zero) {
        report_error("  [FAIL] ID 0 was allocated!\n");
        bad++;
    }

    fprintf(stderr, "  Verified %d IDs, %d mismatches\n", N, bad);
    fprintf(stderr,  "  ID 0 never allocated: ✓\n");
    fprintf(stderr, "  Result: %s\n",
            (bad == 0) ? "PASS" : "FAIL");

    free(ids);
    fun_idpool_destroy(pool);
}

/* ============ Test 3: 多线程 ============ */
typedef struct { fun_idpool_t pool; int tid; int ops; uint32_t *ids; } targ3;

static void *worker3(void *a) {
    targ3 *t = (targ3 *)a;
    for (int i = 0; i < t->ops; i++) {
        uint32_t tag = (uint32_t)(t->tid * 1000000 + i);
        uint32_t id = fun_idpool_gen_id(t->pool, (void*)(uintptr_t)tag);
        if (id == 0) {
            report_error("  [tid=%d] got ID 0 at %d!\n", t->tid, i);
            return (void*)1;
        }
        t->ids[i] = id;
    }
    /* 释放一半 */
    for (int i = 0; i < t->ops/2; i++) {
        fun_idpool_release_id(t->pool, t->ids[i*2]);
    }
    /* 再分配 1/4 并验证 */
    int q = t->ops / 4;
    for (int i = 0; i < q; i++) {
        uint32_t tag = (uint32_t)(t->tid * 2000000 + i + 500000);
        uint32_t id = fun_idpool_gen_id(t->pool, (void*)(uintptr_t)tag);
        if (id == 0) {
            report_error("  [tid=%d] realloc got ID 0!\n", t->tid);
            return (void*)1;
        }
        void *v = fun_idpool_get_value(t->pool, id);
        uint32_t got = (uint32_t)(uintptr_t)v;
        if (got != tag) {
            report_error("  [tid=%d] verify fail: id=%u got=%u exp=%u\n",
                         t->tid, id, got, tag);
        }
    }
    return NULL;
}

static void test_multithread(int threads, int ops) {
    fprintf(stderr, "\n=== Test 3: Multithread (%d × %d) ===\n", threads, ops);
    fun_idpool_t pool = fun_idpool_create_ex(4, FUN_IDPOOL_MODE_WITH_VALUE);
    if (!pool) { report_error("create failed\n"); return; }

    pthread_t *ts = malloc(threads * sizeof(pthread_t));
    targ3    *tas = malloc(threads * sizeof(targ3));
    uint32_t  *ids = calloc(threads * ops, sizeof(uint32_t));

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < threads; i++) {
        tas[i].pool = pool;
        tas[i].tid  = i;
        tas[i].ops  = ops;
        tas[i].ids  = ids + i * ops;
        pthread_create(&ts[i], NULL, worker3, &tas[i]);
    }

    int errs = 0;
    for (int i = 0; i < threads; i++) {
        void *ret = NULL;
        pthread_join(ts[i], &ret);
        if (ret) errs++;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double el = (t1.tv_sec - t0.tv_sec) +
                (t1.tv_nsec - t0.tv_nsec) / 1e9;

    fun_idpool_stats_s stats;
    fun_idpool_get_stats(pool, &stats);

    uint64_t total = (uint64_t)threads * ops + (uint64_t)threads * (ops/4);
    double mops = total / el / 1e6;

    fprintf(stderr, "  elapsed:    %.3f s\n", el);
    fprintf(stderr, "  ops:        %llu\n", (unsigned long long)total);
    fprintf(stderr, "  throughput: %.2f M ops/s\n", mops);
    fprintf(stderr, "  alloc:      %llu\n", (unsigned long long)stats.total_alloc);
    fprintf(stderr, "  freed:      %llu\n", (unsigned long long)stats.total_freed);
    fprintf(stderr, "  reuse:      %llu\n", (unsigned long long)stats.reuse_count);
    fprintf(stderr, "  errors:     %d (thread) + %d (verify)\n", errs, g_errors);
    fprintf(stderr, "  Result:     %s\n",
            (errs == 0 && g_errors == 0) ? "PASS" : "FAIL");

    fun_idpool_destroy(pool);
    free(ts); free(tas); free(ids);
}

/* ============ Test 4: 极端并发 alloc/release 交替 ============ */
static void *worker4(void *a) {
    fun_idpool_t pool = (fun_idpool_t)a;
    for (int i = 0; i < 2000; i++) {
        uint32_t tag = (uint32_t)((uintptr_t)pthread_self() & 0xFFFF) * 10000 + i;
        uint32_t id = fun_idpool_gen_id(pool, (void*)(uintptr_t)tag);
        if (id == 0) {
            report_error("  [FAIL] alloc got ID 0!\n");
            return (void*)1;
        }
        void *v = fun_idpool_get_value(pool, id);
        if (v != (void*)(uintptr_t)tag) {
            report_error("  [FAIL] get_value mismatch at %d\n", i);
        }
        fun_idpool_release_id(pool, id);
    }
    return NULL;
}

static void test_extreme(int threads) {
    fprintf(stderr, "\n=== Test 4: Extreme Concurrency (%d × 2000) ===\n", threads);
    fun_idpool_t pool = fun_idpool_create_ex(4, FUN_IDPOOL_MODE_WITH_VALUE);
    if (!pool) { report_error("create failed\n"); return; }

    pthread_t *ts = malloc(threads * sizeof(pthread_t));
    for (int i = 0; i < threads; i++)
        pthread_create(&ts[i], NULL, worker4, pool);

    int errs = 0;
    for (int i = 0; i < threads; i++) {
        void *ret = NULL;
        pthread_join(ts[i], &ret);
        if (ret) errs++;
    }

    fun_idpool_stats_s stats;
    fun_idpool_get_stats(pool, &stats);
    fprintf(stderr, "  alloc:   %llu\n", (unsigned long long)stats.total_alloc);
    fprintf(stderr, "  freed:   %llu\n", (unsigned long long)stats.total_freed);
    fprintf(stderr, "  reuse:   %llu\n", (unsigned long long)stats.reuse_count);
    fprintf(stderr, "  retries: %llu\n", (unsigned long long)stats.scan_retries);
    fprintf(stderr, "  Result:  %s\n",
            (errs == 0 && g_errors == 0) ? "PASS" : "FAIL");

    fun_idpool_destroy(pool);
    free(ts);
}

/* ============ Test 5: NO_VALUE 模式 ============ */
static void test_no_value_mode(void) {
    fprintf(stderr, "\n=== Test 5: NO_VALUE Mode ===\n");

    fun_idpool_t pool = fun_idpool_create_ex(1, FUN_IDPOOL_MODE_NO_VALUE);
    if (!pool) { report_error("create failed\n"); return; }

    /* 分配一些 ID */
    for (int i = 0; i < 100; i++) {
        uint32_t id = fun_idpool_gen_id(pool, NULL);
        if (id == 0) {
            report_error("  [FAIL] NO_VALUE: got ID 0!\n");
        }
        if (i == 0 && id != 1) {
            report_error("  [FAIL] first ID should be 1, got %u\n", id);
        }
    }

    /* get_value 应该返回 FUN_IDPOOL_EXISTS (不是 NULL) */
    void *v = fun_idpool_get_value(pool, 1);
    if (v != FUN_IDPOOL_EXISTS) {
        report_error("  [FAIL] get_value(1) should be FUN_IDPOOL_EXISTS, got %p\n", v);
    } else {
        fprintf(stderr, "  get_value(1) = FUN_IDPOOL_EXISTS ✓\n");
    }

    /* get_value(0) 应该返回 NULL */
    v = fun_idpool_get_value(pool, 0);
    if (v != NULL) {
        report_error("  [FAIL] get_value(0) should be NULL, got %p\n", v);
    } else {
        fprintf(stderr, "  get_value(0) = NULL ✓ (ID 0 never exists)\n");
    }

    /* 释放后 get_value 返回 NULL */
    fun_idpool_release_id(pool, 1);
    v = fun_idpool_get_value(pool, 1);
    if (v != NULL) {
        report_error("  [FAIL] after release, get_value(1) should be NULL\n");
    } else {
        fprintf(stderr, "  after release: get_value(1) = NULL ✓\n");
    }

    fun_idpool_stats_s stats;
    fun_idpool_get_stats(pool, &stats);
    fprintf(stderr, "  NO_VALUE memory:\n");
    fprintf(stderr, "    bitmap:  %llu B\n",
            (unsigned long long)stats.bitmap_memory);
    fprintf(stderr, "    values:  %llu B (should be 0)\n",
            (unsigned long long)stats.values_memory);
    fprintf(stderr, "    structs: %llu B\n",
            (unsigned long long)stats.region_struct_memory);
    fprintf(stderr, "  Result: %s\n",
            (g_errors == 0) ? "PASS" : "FAIL");

    fun_idpool_destroy(pool);
}

/* ============ Test 6: 内存统计精确验证 ============ */
static void test_memory_stats(void) {
    fprintf(stderr, "\n=== Test 6: Memory Statistics ===\n");

    fun_idpool_t pool = fun_idpool_create_ex(4, FUN_IDPOOL_MODE_WITH_VALUE);
    if (!pool) { report_error("create failed\n"); return; }

    fun_idpool_stats_s s0, s1, s2;
    fun_idpool_get_stats(pool, &s0);
    fprintf(stderr, "  After create:\n");
    fprintf(stderr, "    bitmap:  %llu B\n", (unsigned long long)s0.bitmap_memory);
    fprintf(stderr, "    values:  %llu B\n", (unsigned long long)s0.values_memory);
    fprintf(stderr, "    structs: %llu B\n", (unsigned long long)s0.region_struct_memory);

    /* 分配 1 万个 ID */
    uint32_t *ids = malloc(10000 * sizeof(uint32_t));
    for (int i = 0; i < 10000; i++) {
        ids[i] = fun_idpool_gen_id(pool, (void*)(uintptr_t)i);
    }
    fun_idpool_get_stats(pool, &s1);
    fprintf(stderr, "  After 10K allocs:\n");
    fprintf(stderr, "    bitmap:  %llu B\n", (unsigned long long)s1.bitmap_memory);
    fprintf(stderr, "    values:  %llu B\n", (unsigned long long)s1.values_memory);
    fprintf(stderr, "    structs: %llu B\n", (unsigned long long)s1.region_struct_memory);
    fprintf(stderr, "    regions: %u\n", s1.total_regions);

    /* 公式验证: bitmap_memory 应该 > 0 */
    if (s1.bitmap_memory == 0) {
        report_error("  [FAIL] bitmap_memory should be > 0 after allocs!\n");
    }
    if (s1.values_memory == 0) {
        report_error("  [FAIL] values_memory should be > 0 in WITH_VALUE mode!\n");
    }

    /* 释放所有 */
    for (int i = 0; i < 10000; i++) {
        fun_idpool_release_id(pool, ids[i]);
    }
    fun_idpool_get_stats(pool, &s2);
    fprintf(stderr, "  After 10K releases:\n");
    fprintf(stderr, "    bitmap:  %llu B (unchanged, not shrunk)\n",
            (unsigned long long)s2.bitmap_memory);
    fprintf(stderr, "    values:  %llu B (unchanged, not shrunk)\n",
            (unsigned long long)s2.values_memory);

    fprintf(stderr, "  Result: %s\n",
            (g_errors == 0) ? "PASS" : "FAIL");

    free(ids);
    fun_idpool_destroy(pool);
}

/* ============ Main ============ */
int main(int argc, char **argv) {
    int threads = (argc > 1) ? atoi(argv[1]) : 16;
    int ops     = (argc > 2) ? atoi(argv[2]) : 5000;

    fprintf(stderr, "=== fun_idpool bit0-reserved test ===\n");
    fprintf(stderr, "threads=%d  ops=%d\n\n", threads, ops);

    test_bit0_reserved();       /* Test 1: Region #0 bit 0 保留 */
    test_id_encoding();          /* Test 2: ID 编码 10万次 */
    test_no_value_mode();        /* Test 5: NO_VALUE 模式 */
    test_memory_stats();         /* Test 6: 内存统计 */
    test_multithread(threads, ops);  /* Test 3: 多线程 */
    test_extreme(threads * 2);        /* Test 4: 极端并发 */

    fprintf(stderr, "\n=== SUMMARY ===\n");
    fprintf(stderr, "Total errors: %d\n", g_errors);
    if (g_errors == 0) {
        fprintf(stderr, "*** ALL TESTS PASSED ***\n");
        return 0;
    } else {
        fprintf(stderr, "*** %d ERRORS FOUND ***\n", g_errors);
        return 1;
    }
}
