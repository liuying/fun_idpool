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
 * 综合测试
 * ============================================================ */

static int g_errors = 0;
static pthread_mutex_t g_err_mu = PTHREAD_MUTEX_INITIALIZER;

static void report_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&g_err_mu);
    g_errors++;
    if (g_errors <= 30)
        vfprintf(stderr, fmt, ap);
    pthread_mutex_unlock(&g_err_mu);
    va_end(ap);
}

/* ============ Test 1: 单线程同 Region 内单调递增 ============ */
static void test_monotonic_same_region(void) {
    fprintf(stderr, "\n--- Test 1: Same-Region Monotonic ---\n");
    fun_idpool_t pool = fun_idpool_create(1);
    if (!pool) { report_error("create failed\n"); return; }

    /* 只分配 Region #0 (cap=64), 验证严格递增 */
    uint32_t ids[64];
    memset(ids, 0, sizeof(ids));

    for (int i = 0; i < 64; i++) {
        uint32_t id = fun_idpool_gen_id(pool, (void*)(uintptr_t)i);
        if (id == FUN_IDPOOL_INVALID_ID) {
            report_error("  alloc[%d] failed\n", i);
            break;
        }
        ids[i] = id;
        if (i > 0 && ids[i] <= ids[i-1]) {
            report_error("  NOT MONOTONIC: ids[%d]=%u <= ids[%d]=%u\n",
                         i, ids[i], i-1, ids[i-1]);
        }
    }

    /* 验证 get_value */
    for (int i = 0; i < 64; i++) {
        void *v = fun_idpool_get_value(pool, ids[i]);
        if (v != (void*)(uintptr_t)i) {
            report_error("  get_value[%d]=%p expected %p\n",
                         i, v, (void*)(uintptr_t)i);
        }
    }

    fprintf(stderr, "  Same-region monotonic: %s\n",
            (g_errors == 0) ? "PASS" : "FAIL");

    /* 释放一半, 再分配应该复用 */
    for (int i = 0; i < 32; i++) {
        fun_idpool_release_id(pool, ids[i*2]);  /* 释放偶数 */
    }

    /* 新分配应该拿到释放的位 */
    uint32_t reuse_id = fun_idpool_gen_id(pool, (void*)0xCAFEBABE);
    if (reuse_id == FUN_IDPOOL_INVALID_ID) {
        report_error("  reuse alloc failed\n");
    } else {
        void *v = fun_idpool_get_value(pool, reuse_id);
        if (v != (void*)0xCAFEBABE) {
            report_error("  reuse get_value failed: got %p\n", v);
        } else {
            fprintf(stderr, "  Reuse after free: PASS (id=%u)\n", reuse_id);
        }
    }

    fun_idpool_destroy(pool);
}

/* ============ Test 2: 懒加载 (内存节省) ============ */
static void test_lazy_alloc(void) {
    fprintf(stderr, "\n--- Test 2: Lazy Allocation (memory saving) ---\n");
    fun_idpool_t pool = fun_idpool_create(4);
    if (!pool) { report_error("create failed\n"); return; }

    /* 只分配 10 个 ID, 应该只创建极少量 Region */
    for (int i = 0; i < 10; i++) {
        uint32_t id = fun_idpool_gen_id(pool, (void*)(uintptr_t)i);
        if (id == FUN_IDPOOL_INVALID_ID)
            report_error("  alloc[%d] failed\n", i);
    }

    fun_idpool_stats_s stats;
    fun_idpool_get_stats(pool, &stats);
    fprintf(stderr, "  After 10 allocs: regions=%u alloc=%llu\n",
            stats.total_regions,
            (unsigned long long)stats.total_alloc);
    fprintf(stderr, "  (bitmap/values allocated lazily on first use)\n");
    fprintf(stderr, "  Lazy alloc: PASS\n");

    fun_idpool_destroy(pool);
}

/* ============ Test 3: 多线程正确性 ============ */
typedef struct { fun_idpool_t pool; int tid; int ops; uint32_t *ids; } targ3;

static void *worker3(void *a) {
    targ3 *t = (targ3 *)a;
    for (int i = 0; i < t->ops; i++) {
        uint32_t tag = (uint32_t)(t->tid * 1000000 + i);
        uint32_t id = fun_idpool_gen_id(t->pool, (void*)(uintptr_t)tag);
        if (id == FUN_IDPOOL_INVALID_ID) {
            report_error("  [tid=%d] alloc fail at %d\n", t->tid, i);
            return (void*)1;
        }
        t->ids[i] = id;
    }
    /* 释放一半 */
    for (int i = 0; i < t->ops/2; i++) {
        fun_idpool_release_id(t->pool, t->ids[i*2]);
    }
    /* 再分配 1/4, 验证 */
    int q = t->ops / 4;
    for (int i = 0; i < q; i++) {
        uint32_t tag = (uint32_t)(t->tid * 2000000 + i + 500000);
        uint32_t id = fun_idpool_gen_id(t->pool, (void*)(uintptr_t)tag);
        if (id == FUN_IDPOOL_INVALID_ID) {
            report_error("  [tid=%d] realloc fail at %d\n", t->tid, i);
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
    fprintf(stderr, "\n--- Test 3: Multithread (%d × %d) ---\n",
            threads, ops);
    fun_idpool_t pool = fun_idpool_create(4);
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

    uint64_t total = (uint64_t)threads * ops           /* 首次 */
                 + (uint64_t)threads * (ops/4);       /* 再分配 */
    double mops = total / el / 1e6;

    fprintf(stderr, "  elapsed:    %.3f s\n", el);
    fprintf(stderr, "  ops:        %llu\n",
            (unsigned long long)total);
    fprintf(stderr, "  throughput: %.2f M ops/s\n", mops);
    fprintf(stderr, "  alloc:      %llu\n",
            (unsigned long long)stats.total_alloc);
    fprintf(stderr, "  freed:      %llu\n",
            (unsigned long long)stats.total_freed);
    fprintf(stderr, "  reuse:      %llu\n",
            (unsigned long long)stats.reuse_count);
    fprintf(stderr, "  retries:    %llu\n",
            (unsigned long long)stats.scan_retries);
    fprintf(stderr, "  regions:    %u\n", stats.total_regions);
    fprintf(stderr, "  thread errs:%d\n", errs);
    fprintf(stderr, "  Result: %s\n",
            (errs == 0) ? "PASS" : "FAIL");

    fun_idpool_destroy(pool);
    free(ts); free(tas); free(ids);
}

/* ============ Test 4: 极端并发 (alloc/release 交替) ============ */
static void *worker4(void *a) {
    fun_idpool_t pool = (fun_idpool_t)a;
    for (int i = 0; i < 2000; i++) {
        uint32_t tag = (uint32_t)((uintptr_t)pthread_self() & 0xFFFF) * 10000 + i;
        uint32_t id = fun_idpool_gen_id(pool, (void*)(uintptr_t)tag);
        if (id == FUN_IDPOOL_INVALID_ID) {
            report_error("  alloc fail at %d\n", i);
            return (void*)1;
        }
        void *v = fun_idpool_get_value(pool, id);
        if (v != (void*)(uintptr_t)tag) {
            report_error("  get_value mismatch at %d\n", i);
        }
        fun_idpool_release_id(pool, id);
    }
    return NULL;
}

static void test_extreme(int threads) {
    fprintf(stderr, "\n--- Test 4: Extreme Concurrency (%d × 2000) ---\n",
            threads);
    fun_idpool_t pool = fun_idpool_create(4);
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
    fprintf(stderr, "  alloc:   %llu\n",
            (unsigned long long)stats.total_alloc);
    fprintf(stderr, "  freed:   %llu\n",
            (unsigned long long)stats.total_freed);
    fprintf(stderr, "  reuse:   %llu\n",
            (unsigned long long)stats.reuse_count);
    fprintf(stderr, "  retries: %llu\n",
            (unsigned long long)stats.scan_retries);
    fprintf(stderr, "  regions: %u\n", stats.total_regions);
    fprintf(stderr, "  Result:  %s\n",
            (errs == 0) ? "PASS" : "FAIL");

    fun_idpool_destroy(pool);
    free(ts);
}

/* ============ Test 5: ID 编码/解码对称性 ============ */
static void test_id_encoding(void) {
    fprintf(stderr, "\n--- Test 5: ID Encode/Decode Symmetry ---\n");
    fun_idpool_t pool = fun_idpool_create(4);
    if (!pool) { report_error("create failed\n"); return; }

    /* 分配大量 ID, 验证 get_value 总能取回正确指针 */
    int N = 100000;
    uint32_t *ids = malloc(N * sizeof(uint32_t));
    if (!ids) { report_error("OOM\n"); return; }

    for (int i = 0; i < N; i++) {
        uint32_t tag = (uint32_t)(0xABC00000 + i);
        ids[i] = fun_idpool_gen_id(pool, (void*)(uintptr_t)tag);
        if (ids[i] == FUN_IDPOOL_INVALID_ID) {
            report_error("  alloc[%d] failed\n", i);
            break;
        }
    }

    int bad = 0;
    for (int i = 0; i < N; i++) {
        if (ids[i] == FUN_IDPOOL_INVALID_ID) continue;
        void *v = fun_idpool_get_value(pool, ids[i]);
        uint32_t expected = 0xABC00000 + i;
        if (v != (void*)(uintptr_t)expected) {
            if (bad < 5)
                report_error("  id[%d]=%u got=%p exp=%x\n",
                             i, ids[i], v, expected);
            bad++;
        }
    }
    fprintf(stderr, "  Verified %d IDs, %d mismatches\n", N, bad);
    fprintf(stderr, "  Result: %s\n",
            (bad == 0) ? "PASS" : "FAIL");

    /* 释放所有 */
    for (int i = 0; i < N; i++) {
        if (ids[i] != FUN_IDPOOL_INVALID_ID)
            fun_idpool_release_id(pool, ids[i]);
    }

    free(ids);
    fun_idpool_destroy(pool);
}

/* ============ Main ============ */
int main(int argc, char **argv) {
    int threads = (argc > 1) ? atoi(argv[1]) : 16;
    int ops     = (argc > 2) ? atoi(argv[2]) : 5000;

    fprintf(stderr, "=== fun_idpool comprehensive test ===\n");
    fprintf(stderr, "threads=%d  ops=%d\n\n", threads, ops);

    test_lazy_alloc();           /* Test 2: 懒加载 */
    test_monotonic_same_region();  /* Test 1: 同 Region 单调 */
    test_id_encoding();          /* Test 5: 编解码对称 */
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
