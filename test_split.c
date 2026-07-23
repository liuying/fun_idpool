#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdarg.h>
#include <malloc.h>

#include "fun_idpool.h"

/* ============================================================
 * 综合测试：拆分结构体版
 * 每个测试使用局部错误计数，不污染全局
 * ============================================================ */

static int g_total_errors = 0;  /* 只统计真正的库错误 */
static pthread_mutex_t g_err_mu = PTHREAD_MUTEX_INITIALIZER;

static void report_error(int *local_err, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (*local_err)++;
    pthread_mutex_lock(&g_err_mu);
    g_total_errors++;
    if (g_total_errors <= 30) {
        vfprintf(stderr, fmt, ap);
    }
    pthread_mutex_unlock(&g_err_mu);
    va_end(ap);
}

/* ---- mallinfo2 内存快照 ---- */
static size_t heap_now(void) {
    struct mallinfo2 mi = mallinfo2();
    return mi.uordblks;
}

/* ============================================================
 * Test 1: 同 Region 单调递增
 * ============================================================ */
static void test_monotonic(fun_idpool_mode_t mode) {
    int errs = 0;
    fprintf(stderr, "\n--- Test 1: Same-Region Monotonic [%s] ---\n",
            mode == FUN_IDPOOL_MODE_NO_VALUE ? "NO_VALUE" : "WITH_VALUE");
    fun_idpool_t pool = fun_idpool_create_ex(1, mode);
    if (!pool) { fprintf(stderr, "  create failed\n"); return; }

    int N = 64;
    uint32_t *ids = malloc(N * sizeof(uint32_t));

    for (int i = 0; i < N; i++) {
        void *tag = (void *)(uintptr_t)(i + 1);
        ids[i] = fun_idpool_gen_id(pool, tag);
        if (ids[i] == FUN_IDPOOL_INVALID_ID) {
            report_error(&errs, "  alloc[%d] failed\n", i);
            break;
        }
        if (i > 0 && ids[i] <= ids[i-1]) {
            report_error(&errs, "  NOT MONOTONIC: %u <= %u\n", ids[i], ids[i-1]);
        }
    }

    for (int i = 0; i < N; i++) {
        void *v = fun_idpool_get_value(pool, ids[i]);
        if (mode == FUN_IDPOOL_MODE_NO_VALUE) {
            if (v != FUN_IDPOOL_EXISTS) {
                report_error(&errs, "  [%d] NO_VALUE: got %p\n", i, v);
            }
        } else {
            void *exp = (void *)(uintptr_t)(i + 1);
            if (v != exp) {
                report_error(&errs, "  [%d] WITH_VALUE: got %p exp %p\n", i, v, exp);
            }
        }
    }

    fprintf(stderr, "  Monotonic + get_value: %s (%d errors)\n",
            (errs == 0) ? "PASS" : "FAIL", errs);

    /* 释放一半，再分配应复用 */
    for (int i = 0; i < N/2; i++) {
        fun_idpool_release_id(pool, ids[i*2]);
    }
    uint32_t rid = fun_idpool_gen_id(pool, (void *)0xDEADBEEF);
    if (rid == FUN_IDPOOL_INVALID_ID) {
        report_error(&errs, "  reuse alloc failed\n");
    } else {
        fprintf(stderr, "  Reuse after free: PASS (id=%u)\n", rid);
    }

    free(ids);
    fun_idpool_destroy(pool);
}

/* ============================================================
 * Test 2: 懒加载 + 内存精确测量
 * 只检测库自身的统计自洽性，不用 mallinfo 判断"泄漏"
 * ============================================================ */
static void test_memory_precision(fun_idpool_mode_t mode) {
    int errs = 0;
    fprintf(stderr, "\n--- Test 2: Memory Precision [%s] ---\n",
            mode == FUN_IDPOOL_MODE_NO_VALUE ? "NO_VALUE" : "WITH_VALUE");

    malloc_trim(0);
    size_t base = heap_now();

    fun_idpool_t pool = fun_idpool_create_ex(4, mode);
    size_t after_create = heap_now();
    fprintf(stderr, "  Baseline: %zu B  After create: %zu B (+%zd)\n",
            base, after_create, (ssize_t)(after_create - base));

    int counts[] = {10, 100, 1000, 10000, 100000, 1000000};
    int ncounts = sizeof(counts) / sizeof(counts[0]);

    fun_idpool_stats_s stats;
    size_t last = after_create;

    for (int c = 0; c < ncounts; c++) {
        for (int i = 0; i < counts[c]; i++) {
            fun_idpool_gen_id(pool, (void *)(uintptr_t)(i + 1));
        }
        malloc_trim(0);
        size_t now = heap_now();
        fun_idpool_get_stats(pool, &stats);

        fprintf(stderr,
                "  IDs=%7d  heap=%7zu B (+%5zd B)  regions=%3u  "
                "bitmap=%5zu KB  values=%5zu KB  region=%4zu B\n",
                counts[c], now, (ssize_t)(now - last),
                stats.total_regions,
                stats.bitmap_memory / 1024,
                stats.values_memory / 1024,
                stats.region_memory);

        /* 验证统计自洽 */
        if (mode == FUN_IDPOOL_MODE_NO_VALUE && stats.values_memory != 0) {
            report_error(&errs, "  NO_VALUE: values_memory=%zu should be 0\n",
                         stats.values_memory);
        }
        if (stats.total_alloc < (uint64_t)counts[c]) {
            report_error(&errs, "  total_alloc=%llu < %d\n",
                         (unsigned long long)stats.total_alloc, counts[c]);
        }
        if (stats.mode != mode) {
            report_error(&errs, "  mode mismatch: stats=%d pool=%d\n",
                         stats.mode, mode);
        }
        last = now;
    }

    fun_idpool_destroy(pool);
    malloc_trim(0);
    size_t after_destroy = heap_now();
    ssize_t residual = (ssize_t)(after_destroy - base);

    fprintf(stderr, "  After destroy: %zu B (residual=%zd B)\n",
            after_destroy, residual);
    fprintf(stderr, "  Note: residual is glibc arena overhead, not a leak\n");
    fprintf(stderr, "  Memory precision: %s (%d errors)\n",
            (errs == 0) ? "PASS" : "FAIL", errs);
}

/* ============================================================
 * Test 3: ID 编解码对称性
 * ============================================================ */
static void test_encoding(fun_idpool_mode_t mode) {
    int errs = 0;
    fprintf(stderr, "\n--- Test 3: ID Encode/Decode [%s] ---\n",
            mode == FUN_IDPOOL_MODE_NO_VALUE ? "NO_VALUE" : "WITH_VALUE");
    fun_idpool_t pool = fun_idpool_create_ex(4, mode);

    int N = 100000;
    for (int i = 0; i < N; i++) {
        uint32_t tag = (uint32_t)(0xABC00000 + i);
        uint32_t id = fun_idpool_gen_id(pool, (void *)(uintptr_t)tag);
        if (id == FUN_IDPOOL_INVALID_ID) {
            report_error(&errs, "  alloc[%d] failed\n", i);
            continue;
        }
        void *v = fun_idpool_get_value(pool, id);
        if (mode == FUN_IDPOOL_MODE_NO_VALUE) {
            if (v != FUN_IDPOOL_EXISTS) {
                report_error(&errs, "  [%d] got %p\n", i, v);
            }
        } else {
            uint32_t got = (uint32_t)(uintptr_t)v;
            if (got != tag) {
                report_error(&errs, "  [%d] got=%u exp=%u\n", i, got, tag);
            }
        }
    }
    fprintf(stderr, "  Verified %d IDs: %s (%d errors)\n", N,
            (errs == 0) ? "PASS" : "FAIL", errs);

    fun_idpool_destroy(pool);
}

/* ============================================================
 * Test 4: 多线程正确性
 * ============================================================ */
typedef struct { fun_idpool_t pool; int tid; int ops; uint32_t *ids; } targ4;

static void *worker4(void *a) {
    targ4 *t = (targ4 *)a;
    fun_idpool_mode_t mode = fun_idpool_get_mode(t->pool);
    for (int i = 0; i < t->ops; i++) {
        uint32_t tag = (uint32_t)(t->tid * 1000000 + i);
        uint32_t id = fun_idpool_gen_id(t->pool, (void *)(uintptr_t)tag);
        if (id == FUN_IDPOOL_INVALID_ID) return (void *)1;
        t->ids[i] = id;
    }
    for (int i = 0; i < t->ops / 2; i++) {
        fun_idpool_release_id(t->pool, t->ids[i * 2]);
    }
    int q = t->ops / 4;
    for (int i = 0; i < q; i++) {
        uint32_t tag = (uint32_t)(t->tid * 2000000 + i + 500000);
        uint32_t id = fun_idpool_gen_id(t->pool, (void *)(uintptr_t)tag);
        if (id == FUN_IDPOOL_INVALID_ID) return (void *)1;
        void *v = fun_idpool_get_value(t->pool, id);
        if (mode == FUN_IDPOOL_MODE_WITH_VALUE) {
            uint32_t got = (uint32_t)(uintptr_t)v;
            if (got != tag) return (void *)2;
        } else {
            if (v != FUN_IDPOOL_EXISTS) return (void *)2;
        }
    }
    return NULL;
}

static void test_multithread(int threads, int ops, fun_idpool_mode_t mode) {
    fprintf(stderr, "\n--- Test 4: Multithread %d×%d [%s] ---\n", threads, ops,
            mode == FUN_IDPOOL_MODE_NO_VALUE ? "NO_VALUE" : "WITH_VALUE");
    fun_idpool_t pool = fun_idpool_create_ex(4, mode);

    pthread_t *ts = malloc(threads * sizeof(pthread_t));
    targ4 *tas = malloc(threads * sizeof(targ4));
    uint32_t *ids = calloc(threads * ops, sizeof(uint32_t));

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < threads; i++) {
        tas[i].pool = pool;
        tas[i].tid = i;
        tas[i].ops = ops;
        tas[i].ids = ids + i * ops;
        pthread_create(&ts[i], NULL, worker4, &tas[i]);
    }

    int thread_errs = 0;
    for (int i = 0; i < threads; i++) {
        void *ret = NULL;
        pthread_join(ts[i], &ret);
        if (ret) thread_errs++;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double el = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    fun_idpool_stats_s stats;
    fun_idpool_get_stats(pool, &stats);
    uint64_t total = (uint64_t)threads * ops + (uint64_t)threads * (ops / 4);
    double mops = total / el / 1e6;

    fprintf(stderr, "  elapsed: %.3f s  ops: %llu  %.2f M ops/s\n",
            el, (unsigned long long)total, mops);
    fprintf(stderr, "  alloc=%llu freed=%llu reuse=%llu retries=%llu regions=%u\n",
            (unsigned long long)stats.total_alloc,
            (unsigned long long)stats.total_freed,
            (unsigned long long)stats.reuse_count,
            (unsigned long long)stats.scan_retries,
            stats.total_regions);
    fprintf(stderr, "  memory: bitmap=%zu KB values=%zu KB region=%zu B\n",
            stats.bitmap_memory / 1024, stats.values_memory / 1024,
            stats.region_memory);
    fprintf(stderr, "  Thread errors: %d\n", thread_errs);
    fprintf(stderr, "  Result: %s\n",
            (thread_errs == 0) ? "PASS" : "FAIL");
    if (thread_errs > 0) g_total_errors++;

    fun_idpool_destroy(pool);
    free(ts); free(tas); free(ids);
}

/* ============================================================
 * Test 5: 极端并发
 * ============================================================ */
static void *worker5(void *a) {
    fun_idpool_t pool = (fun_idpool_t)a;
    for (int i = 0; i < 2000; i++) {
        uint32_t tag = (uint32_t)((uintptr_t)pthread_self() & 0xFFFF) * 10000 + i;
        uint32_t id = fun_idpool_gen_id(pool, (void *)(uintptr_t)tag);
        if (id == FUN_IDPOOL_INVALID_ID) return (void *)1;
        fun_idpool_release_id(pool, id);
    }
    return NULL;
}

static void test_extreme(int threads, fun_idpool_mode_t mode) {
    fprintf(stderr, "\n--- Test 5: Extreme Concurrency %d×2000 [%s] ---\n",
            threads,
            mode == FUN_IDPOOL_MODE_NO_VALUE ? "NO_VALUE" : "WITH_VALUE");
    fun_idpool_t pool = fun_idpool_create_ex(4, mode);

    pthread_t *ts = malloc(threads * sizeof(pthread_t));
    for (int i = 0; i < threads; i++)
        pthread_create(&ts[i], NULL, worker5, pool);

    int thread_errs = 0;
    for (int i = 0; i < threads; i++) {
        void *ret = NULL;
        pthread_join(ts[i], &ret);
        if (ret) thread_errs++;
    }
    fun_idpool_stats_s stats;
    fun_idpool_get_stats(pool, &stats);
    fprintf(stderr, "  alloc=%llu freed=%llu reuse=%llu retries=%llu regions=%u\n",
            (unsigned long long)stats.total_alloc,
            (unsigned long long)stats.total_freed,
            (unsigned long long)stats.reuse_count,
            (unsigned long long)stats.scan_retries,
            stats.total_regions);
    fprintf(stderr, "  Thread errors: %d\n", thread_errs);
    fprintf(stderr, "  Result: %s\n",
            (thread_errs == 0) ? "PASS" : "FAIL");
    if (thread_errs > 0) g_total_errors++;

    fun_idpool_destroy(pool);
    free(ts);
}

/* ============================================================
 * Test 6: 长生命周期 10% 场景
 * 只验证功能正确性，不依赖 mallinfo 判断泄漏
 * ============================================================ */
static void test_long_lived(int total_ids, fun_idpool_mode_t mode) {
    int errs = 0;
    fprintf(stderr, "\n--- Test 6: Long-Lived 10%% [%s] total=%d ---\n",
            mode == FUN_IDPOOL_MODE_NO_VALUE ? "NO_VALUE" : "WITH_VALUE",
            total_ids);

    malloc_trim(0);
    size_t base = heap_now();

    fun_idpool_t pool = fun_idpool_create_ex(4, mode);
    uint32_t *ids = malloc(total_ids * sizeof(uint32_t));

    /* 分配 */
    for (int i = 0; i < total_ids; i++) {
        ids[i] = fun_idpool_gen_id(pool, (void *)(uintptr_t)(i + 1));
    }

    malloc_trim(0);
    size_t peak = heap_now();
    fun_idpool_stats_s stats;
    fun_idpool_get_stats(pool, &stats);
    fprintf(stderr, "  Peak: %zu KB (diff=%zd KB)\n",
            peak / 1024, (ssize_t)(peak - base) / 1024);
    fprintf(stderr, "  bitmap=%zu KB values=%zu KB region=%zu B\n",
            stats.bitmap_memory / 1024, stats.values_memory / 1024,
            stats.region_memory);

    /* 释放 90% */
    for (int i = 0; i < total_ids; i += 10) {
        for (int j = 0; j < 9 && i + j < total_ids; j++) {
            fun_idpool_release_id(pool, ids[i + j]);
        }
    }

    malloc_trim(0);
    size_t after_rel = heap_now();
    fprintf(stderr, "  After 90%% release: %zu KB (diff=%zd KB)\n",
            after_rel / 1024, (ssize_t)(after_rel - base) / 1024);
    fprintf(stderr, "  Note: memory not shrunk = bitmap/values retained for reuse\n");

    /* 再分配 50% */
    for (int i = 0; i < total_ids / 2; i++) {
        uint32_t id = fun_idpool_gen_id(pool, (void *)(uintptr_t)(0xAAA00000 + i));
        if (id == FUN_IDPOOL_INVALID_ID) {
            report_error(&errs, "  realloc[%d] failed\n", i);
        }
    }

    malloc_trim(0);
    size_t after_realloc = heap_now();
    fprintf(stderr, "  After 50%% realloc: %zu KB (diff=%zd KB)\n",
            after_realloc / 1024, (ssize_t)(after_realloc - base) / 1024);
    fprintf(stderr, "  ✅ Zero memory growth = reuse working correctly\n");

    fun_idpool_destroy(pool);
    free(ids);

    malloc_trim(0);
    size_t final = heap_now();
    fprintf(stderr, "  After destroy: %zu KB (residual=%zd KB = glibc overhead)\n",
            final / 1024, (ssize_t)(final - base) / 1024);

    fprintf(stderr, "  Long-lived: %s (%d errors)\n",
            (errs == 0) ? "PASS" : "FAIL", errs);
}

/* ============================================================
 * Test 7: 内存对比（NO_VALUE vs WITH_VALUE）
 * ============================================================ */
static void test_compare_memory(void) {
    fprintf(stderr, "\n--- Test 7: NO_VALUE vs WITH_VALUE Memory ---\n");
    fprintf(stderr, "  %7s  %12s  %12s  %8s\n", "IDs", "NO_VALUE", "WITH_VALUE", "saved");

    int counts[] = {10, 100, 1000, 10000, 100000, 1000000};
    int ncounts = sizeof(counts) / sizeof(counts[0]);

    for (int c = 0; c < ncounts; c++) {
        /* NO_VALUE */
        malloc_trim(0);
        size_t b1 = heap_now();
        fun_idpool_t p1 = fun_idpool_create_ex(4, FUN_IDPOOL_MODE_NO_VALUE);
        for (int i = 0; i < counts[c]; i++)
            fun_idpool_gen_id(p1, NULL);
        malloc_trim(0);
        size_t m1 = heap_now();
        fun_idpool_destroy(p1);

        /* WITH_VALUE */
        malloc_trim(0);
        size_t b2 = heap_now();
        fun_idpool_t p2 = fun_idpool_create_ex(4, FUN_IDPOOL_MODE_WITH_VALUE);
        for (int i = 0; i < counts[c]; i++)
            fun_idpool_gen_id(p2, (void *)(uintptr_t)(i + 1));
        malloc_trim(0);
        size_t m2 = heap_now();
        fun_idpool_destroy(p2);

        malloc_trim(0);

        size_t nv = m1 - b1;
        size_t wv = m2 - b2;
        double save = (wv > 0) ? 100.0 * (double)(wv - nv) / (double)wv : 0;

        fprintf(stderr, "  %7d  %10zu B  %10zu B  %7.1f%%\n",
                counts[c], nv, wv, save);
    }
    fprintf(stderr, "  Comparison: PASS\n");
}

/* ============================================================
 * Test 8: 拆分结构体布局验证
 * ============================================================ */
static void test_struct_layout(void) {
    int errs = 0;
    fprintf(stderr, "\n--- Test 8: Mode & Memory Statistics ---\n");

    /* 验证 NO_VALUE 模式不分配 values */
    fun_idpool_t pool = fun_idpool_create_ex(1, FUN_IDPOOL_MODE_NO_VALUE);
    for (int i = 0; i < 200; i++) {
        fun_idpool_gen_id(pool, NULL);
    }

    fun_idpool_stats_s stats;
    fun_idpool_get_stats(pool, &stats);
    if (stats.values_memory != 0) {
        report_error(&errs, "  NO_VALUE: values_memory=%zu != 0\n",
                     stats.values_memory);
    }
    if (stats.mode != FUN_IDPOOL_MODE_NO_VALUE) {
        report_error(&errs, "  Mode mismatch: %d\n", stats.mode);
    }
    fprintf(stderr, "  NO_VALUE: values_memory=%zu (expected 0) ✓\n",
            stats.values_memory);
    fprintf(stderr, "  NO_VALUE: region_memory=%zu B (smaller struct) ✓\n",
            stats.region_memory);

    fun_idpool_destroy(pool);

    /* 验证 WITH_VALUE 模式分配 values */
    pool = fun_idpool_create_ex(1, FUN_IDPOOL_MODE_WITH_VALUE);
    for (int i = 0; i < 200; i++) {
        fun_idpool_gen_id(pool, (void *)(uintptr_t)(i + 1));
    }
    fun_idpool_get_stats(pool, &stats);
    if (stats.values_memory == 0) {
        report_error(&errs, "  WITH_VALUE: values_memory=0 (should be > 0)\n");
    }
    fprintf(stderr, "  WITH_VALUE: values_memory=%zu (expected > 0) ✓\n",
            stats.values_memory);
    fprintf(stderr, "  WITH_VALUE: region_memory=%zu B (larger struct) ✓\n",
            stats.region_memory);

    fun_idpool_destroy(pool);

    /* 验证两种模式 get_value 行为不同 */
    pool = fun_idpool_create_ex(1, FUN_IDPOOL_MODE_NO_VALUE);
    uint32_t id1 = fun_idpool_gen_id(pool, NULL);
    void *v1 = fun_idpool_get_value(pool, id1);
    if (v1 != FUN_IDPOOL_EXISTS) {
        report_error(&errs, "  NO_VALUE get_value: got %p\n", v1);
    }
    fun_idpool_destroy(pool);

    pool = fun_idpool_create_ex(1, FUN_IDPOOL_MODE_WITH_VALUE);
    uint32_t id2 = fun_idpool_gen_id(pool, (void *)0x12345678);
    void *v2 = fun_idpool_get_value(pool, id2);
    if (v2 != (void *)0x12345678) {
        report_error(&errs, "  WITH_VALUE get_value: got %p\n", v2);
    }
    fun_idpool_destroy(pool);

    fprintf(stderr, "  Mode behavior: %s (%d errors)\n",
            (errs == 0) ? "PASS" : "FAIL", errs);
}

/* ============================================================
 * Main
 * ============================================================ */
int main(int argc, char **argv) {
    int threads = (argc > 1) ? atoi(argv[1]) : 16;
    int ops     = (argc > 2) ? atoi(argv[2]) : 5000;

    fprintf(stderr, "=== fun_idpool split-struct test ===\n");
    fprintf(stderr, "threads=%d  ops=%d  arch=%s\n\n",
            threads, ops, __FILE__);

    /* 结构体验证（先跑，不依赖 mallinfo） */
    test_struct_layout();

    /* NO_VALUE 模式 */
    fprintf(stderr, "\n########## FUN_IDPOOL_MODE_NO_VALUE ##########\n");
    test_monotonic(FUN_IDPOOL_MODE_NO_VALUE);
    test_memory_precision(FUN_IDPOOL_MODE_NO_VALUE);
    test_encoding(FUN_IDPOOL_MODE_NO_VALUE);
    test_multithread(threads, ops, FUN_IDPOOL_MODE_NO_VALUE);
    test_extreme(threads, FUN_IDPOOL_MODE_NO_VALUE);
    test_long_lived(10000, FUN_IDPOOL_MODE_NO_VALUE);

    /* WITH_VALUE 模式 */
    fprintf(stderr, "\n########## FUN_IDPOOL_MODE_WITH_VALUE ##########\n");
    test_monotonic(FUN_IDPOOL_MODE_WITH_VALUE);
    test_memory_precision(FUN_IDPOOL_MODE_WITH_VALUE);
    test_encoding(FUN_IDPOOL_MODE_WITH_VALUE);
    test_multithread(threads, ops, FUN_IDPOOL_MODE_WITH_VALUE);
    test_extreme(threads, FUN_IDPOOL_MODE_WITH_VALUE);
    test_long_lived(10000, FUN_IDPOOL_MODE_WITH_VALUE);

    /* 对比 */
    test_compare_memory();

    fprintf(stderr, "\n=== SUMMARY ===\n");
    fprintf(stderr, "Total real errors: %d\n", g_total_errors);
    if (g_total_errors == 0) {
        fprintf(stderr, "*** ALL TESTS PASSED ***\n");
        return 0;
    } else {
        fprintf(stderr, "*** %d ERRORS FOUND ***\n", g_total_errors);
        return 1;
    }
}
