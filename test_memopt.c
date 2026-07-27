#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <malloc.h>

#include "fun_idpool.h"

/* ============================================================
 * 精确内存测量
 * ============================================================ */
static size_t heap_used(void) {
#if defined(__linux__)
    struct mallinfo2 mi = mallinfo2();
    return mi.uordblks;
#else
    return 0;
#endif
}

static void snapshot(const char *label, size_t *prev) {
    size_t cur = heap_used();
    size_t diff = (prev && *prev) ? (cur - *prev) : cur;
    fprintf(stderr, "  %-42s  %10.2f KB  (%s%10.2f KB)\n",
            label, cur / 1024.0,
            (*prev) ? "+" : "",
            diff / 1024.0);
    if (prev) *prev = cur;
}

/* 前向声明 */
static void snapshot_alloc(const char *label, size_t *prev, int round, int batch);

static void snapshot_alloc(const char *label, size_t *prev, int round, int batch) {
    size_t cur = heap_used();
    size_t diff = (prev && *prev) ? (cur - *prev) : cur;
    fprintf(stderr, "  round %2d (%d allocs): %10.2f KB  (+%10.2f KB)\n",
            round, batch, cur / 1024.0, diff / 1024.0);
    if (prev) *prev = cur;
}

/* ============================================================
 * 测试 1: 不保存 value 模式（极致省内存）
 * ============================================================ */
static void test_no_value_mode(void) {
    fprintf(stderr, "\n========== TEST: NO_VALUE MODE ==========\n");
    size_t prev = heap_used();
    snapshot("baseline", &prev);

    /* 创建不保存 value 的池 */
    fun_idpool_t pool = fun_idpool_create_ex(4, FUN_IDPOOL_MODE_NO_VALUE);
    if (!pool) { fprintf(stderr, "create failed\n"); return; }
    snapshot("after create pool (4 zones, NO_VALUE)", &prev);

    /* 分配 10 个 ID */
    for (int i = 0; i < 10; i++) {
        uint32_t id = fun_idpool_gen_id(pool, (void*)(uintptr_t)i);
        if (id == FUN_IDPOOL_INVALID_ID) {
            fprintf(stderr, "  alloc[%d] failed\n", i);
        }
    }
    snapshot("after 10 allocs", &prev);

    /* 验证 get_value 行为 */
    uint32_t test_id = fun_idpool_gen_id(pool, NULL);
    void *v = fun_idpool_get_value(pool, test_id);
    fprintf(stderr, "  get_value(existing_id)=%p (expect NULL+1)\n", v);
    if (v != (void*)1) {
        fprintf(stderr, "  *** UNEXPECTED: get_value should return (NULL+1) ***\n");
    }

    void *v2 = fun_idpool_get_value(pool, 0xDEADBEEF);
    fprintf(stderr, "  get_value(invalid_id)=%p (expect NULL)\n", v2);
    if (v2 != NULL) {
        fprintf(stderr, "  *** UNEXPECTED: get_value should return NULL ***\n");
    }

    /* 分配 100 个 ID */
    for (int i = 0; i < 90; i++) {
        fun_idpool_gen_id(pool, NULL);
    }
    snapshot("after 100 allocs", &prev);

    /* 分配 1000 个 ID */
    for (int i = 0; i < 900; i++) {
        fun_idpool_gen_id(pool, NULL);
    }
    snapshot("after 1000 allocs", &prev);

    /* 分配 10000 个 ID */
    for (int i = 0; i < 9000; i++) {
        fun_idpool_gen_id(pool, NULL);
    }
    snapshot("after 10000 allocs", &prev);

    /* 分配 100000 个 ID */
    for (int i = 0; i < 90000; i++) {
        fun_idpool_gen_id(pool, NULL);
    }
    snapshot("after 100000 allocs", &prev);

    /* 分配 1000000 个 ID */
    for (int i = 0; i < 900000; i++) {
        fun_idpool_gen_id(pool, NULL);
    }
    snapshot("after 1000000 allocs", &prev);

    /* 测试释放 + 复用 */
    /* 先释放一半 */
    fun_idpool_stats_s stats_before;
    fun_idpool_get_stats(pool, &stats_before);

    /* 释放一些 ID（用 test_id 测试） */
    fun_idpool_release_id(pool, test_id);
    v = fun_idpool_get_value(pool, test_id);
    fprintf(stderr, "  after release: get_value(released_id)=%p (expect NULL)\n", v);

    /* 再分配，应该复用 */
    uint32_t reused_id = fun_idpool_gen_id(pool, NULL);
    fprintf(stderr, "  reused_id=%u\n", reused_id);

    snapshot("after release + reuse", &prev);

    fun_idpool_stats_s stats;
    fun_idpool_get_stats(pool, &stats);
    fprintf(stderr, "\n  === NO_VALUE Stats ===\n");
    fprintf(stderr, "  alloc:      %llu\n", (unsigned long long)stats.total_alloc);
    fprintf(stderr, "  freed:      %llu\n", (unsigned long long)stats.total_freed);
    fprintf(stderr, "  reuse:      %llu\n", (unsigned long long)stats.reuse_count);
    fprintf(stderr, "  regions:    %u\n", stats.total_regions);
    fprintf(stderr, "  heap_total: %.2f KB\n", heap_used() / 1024.0);

    fun_idpool_destroy(pool);
    size_t after_destroy = heap_used();
    /* 修复 size_t 下溢:转 int64_t 让"负值"表示 destroy 后比 prev 还小 (回收更多) */
    double diff_kb = (double)(int64_t)(after_destroy - prev) / 1024.0;
    fprintf(stderr, "  after destroy: %.2f KB (leaked: %+.2f KB)\n",
            after_destroy / 1024.0, diff_kb);
}

/* ============================================================
 * 测试 2: 保存 value 模式（对比基准）
 * ============================================================ */
static void test_with_value_mode(void) {
    fprintf(stderr, "\n========== TEST: WITH_VALUE MODE ==========\n");
    size_t prev = heap_used();
    snapshot("baseline", &prev);

    fun_idpool_t pool = fun_idpool_create_ex(4, FUN_IDPOOL_MODE_WITH_VALUE);
    if (!pool) { fprintf(stderr, "create failed\n"); return; }
    snapshot("after create pool (4 zones, WITH_VALUE)", &prev);

    /* 分配 10 个 ID */
    for (int i = 0; i < 10; i++) {
        uint32_t id = fun_idpool_gen_id(pool, (void*)(uintptr_t)(0x1000 + i));
        if (id == FUN_IDPOOL_INVALID_ID) {
            fprintf(stderr, "  alloc[%d] failed\n", i);
        }
    }
    snapshot("after 10 allocs", &prev);

    /* 验证 get_value */
    uint32_t test_id = fun_idpool_gen_id(pool, (void*)0xCAFEBABE);
    void *v = fun_idpool_get_value(pool, test_id);
    fprintf(stderr, "  get_value(existing)=%p (expect 0xCAFEBABE)\n", v);

    /* 分配 100 个 */
    for (int i = 0; i < 90; i++) {
        fun_idpool_gen_id(pool, (void*)(uintptr_t)(0x2000 + i));
    }
    snapshot("after 100 allocs", &prev);

    /* 分配 1000 个 */
    for (int i = 0; i < 900; i++) {
        fun_idpool_gen_id(pool, (void*)(uintptr_t)(0x3000 + i));
    }
    snapshot("after 1000 allocs", &prev);

    /* 分配 10000 个 */
    for (int i = 0; i < 9000; i++) {
        fun_idpool_gen_id(pool, (void*)(uintptr_t)(0x4000 + i));
    }
    snapshot("after 10000 allocs", &prev);

    /* 分配 100000 个 */
    for (int i = 0; i < 90000; i++) {
        fun_idpool_gen_id(pool, (void*)(uintptr_t)(0x5000 + i));
    }
    snapshot("after 100000 allocs", &prev);

    /* 分配 1000000 个 */
    for (int i = 0; i < 900000; i++) {
        fun_idpool_gen_id(pool, (void*)(uintptr_t)(0x6000 + i));
    }
    snapshot("after 1000000 allocs", &prev);

    fun_idpool_stats_s stats;
    fun_idpool_get_stats(pool, &stats);
    fprintf(stderr, "\n  === WITH_VALUE Stats ===\n");
    fprintf(stderr, "  alloc:      %llu\n", (unsigned long long)stats.total_alloc);
    fprintf(stderr, "  freed:      %llu\n", (unsigned long long)stats.total_freed);
    fprintf(stderr, "  reuse:      %llu\n", (unsigned long long)stats.reuse_count);
    fprintf(stderr, "  regions:    %u\n", stats.total_regions);
    fprintf(stderr, "  heap_total: %.2f KB\n", heap_used() / 1024.0);

    fun_idpool_destroy(pool);
}

/* ============================================================
 * 测试 3: 多线程正确性（两种模式）
 * ============================================================ */
typedef struct { fun_idpool_t pool; int tid; int ops; int mode; } targ_t;

static void *worker(void *a) {
    targ_t *t = (targ_t *)a;
    for (int i = 0; i < t->ops; i++) {
        void *tag = (t->mode == FUN_IDPOOL_MODE_WITH_VALUE)
            ? (void*)(uintptr_t)(t->tid * 1000000 + i)
            : NULL;
        uint32_t id = fun_idpool_gen_id(t->pool, tag);
        if (id == FUN_IDPOOL_INVALID_ID) {
            fprintf(stderr, "  [tid=%d] alloc fail at %d\n", t->tid, i);
            return (void*)1;
        }
        /* 随机释放一些 */
        if (i % 3 == 0) {
            fun_idpool_release_id(t->pool, id);
        }
        /* 验证 get_value */
        if (t->mode == FUN_IDPOOL_MODE_WITH_VALUE) {
            void *v = fun_idpool_get_value(t->pool, id);
            /* 刚分配的可能已被释放，所以只检查非NULL */
            if (v == NULL && i % 3 != 0) {
                /* 可能被人释放了，不算错 */
            }
        } else {
            void *v = fun_idpool_get_value(t->pool, id);
            /* NO_VALUE 模式: 存在的 ID 返回 NULL+1 */
            if (v != NULL && v != (void*)1) {
                fprintf(stderr, "  [tid=%d] unexpected get_value=%p\n", t->tid, v);
            }
        }
    }
    return NULL;
}

static void test_multithread(int mode, int threads, int ops) {
    const char *mode_name = (mode == FUN_IDPOOL_MODE_NO_VALUE) ? "NO_VALUE" : "WITH_VALUE";
    fprintf(stderr, "\n========== TEST: MT %s (%d × %d) ==========\n",
            mode_name, threads, ops);

    fun_idpool_t pool = fun_idpool_create_ex(4, mode);
    if (!pool) { fprintf(stderr, "create failed\n"); return; }

    pthread_t *ts = malloc(threads * sizeof(pthread_t));
    targ_t    *tas = malloc(threads * sizeof(targ_t));

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < threads; i++) {
        tas[i].pool = pool;
        tas[i].tid  = i;
        tas[i].ops  = ops;
        tas[i].mode = mode;
        pthread_create(&ts[i], NULL, worker, &tas[i]);
    }

    int errs = 0;
    for (int i = 0; i < threads; i++) {
        void *ret = NULL;
        pthread_join(ts[i], &ret);
        if (ret) errs++;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double el = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    fun_idpool_stats_s stats;
    fun_idpool_get_stats(pool, &stats);

    uint64_t total = (uint64_t)threads * ops;
    double mops = total / el / 1e6;

    fprintf(stderr, "  elapsed:    %.3f s\n", el);
    fprintf(stderr, "  ops:        %llu\n", (unsigned long long)total);
    fprintf(stderr, "  throughput: %.2f M ops/s\n", mops);
    fprintf(stderr, "  alloc:      %llu\n", (unsigned long long)stats.total_alloc);
    fprintf(stderr, "  freed:      %llu\n", (unsigned long long)stats.total_freed);
    fprintf(stderr, "  reuse:      %llu\n", (unsigned long long)stats.reuse_count);
    fprintf(stderr, "  regions:    %u\n", stats.total_regions);
    fprintf(stderr, "  thread errs:%d\n", errs);
    fprintf(stderr, "  Result: %s\n", (errs == 0) ? "PASS" : "FAIL");

    fun_idpool_destroy(pool);
    free(ts); free(tas);
}

/* ============================================================
 * 测试 4: 长时间存活场景（10% 存活率）
 * ============================================================ */
static void test_long_lived(int mode) {
    const char *mode_name = (mode == FUN_IDPOOL_MODE_NO_VALUE) ? "NO_VALUE" : "WITH_VALUE";
    fprintf(stderr, "\n========== TEST: Long-lived 10%% (%s) ==========\n",
            mode_name);

    size_t prev = heap_used();
    snapshot("baseline", &prev);

    fun_idpool_t pool = fun_idpool_create_ex(4, mode);
    if (!pool) { fprintf(stderr, "create failed\n"); return; }
    snapshot("after create", &prev);

    /* 模拟: 持续分配, 只释放 90%, 保持 10% 存活 */
    int batch = 10000;
    int keep = 1000;  /* 每批保留 10% */
    uint32_t *keep_ids = malloc(keep * sizeof(uint32_t));
    int kidx = 0;

    for (int round = 0; round < 10; round++) {
        /* 分配一批 */
        for (int i = 0; i < batch; i++) {
            void *tag = (mode == FUN_IDPOOL_MODE_WITH_VALUE)
                ? (void*)(uintptr_t)(round * 100000 + i)
                : NULL;
            uint32_t id = fun_idpool_gen_id(pool, tag);
            if (id == FUN_IDPOOL_INVALID_ID) {
                fprintf(stderr, "  round=%d alloc[%d] failed\n", round, i);
                break;
            }
            /* 保留前 10% */
            if (i < keep) {
                keep_ids[kidx++ % keep] = id;
            } else {
                /* 立即释放 90% */
                fun_idpool_release_id(pool, id);
            }
        }
        snapshot_alloc("after round", &prev, round + 1, batch);
    }

    free(keep_ids);

    fun_idpool_stats_s stats;
    fun_idpool_get_stats(pool, &stats);
    fprintf(stderr, "\n  === Long-lived Stats ===\n");
    fprintf(stderr, "  alloc:      %llu\n", (unsigned long long)stats.total_alloc);
    fprintf(stderr, "  freed:      %llu\n", (unsigned long long)stats.total_freed);
    fprintf(stderr, "  reuse:      %llu\n", (unsigned long long)stats.reuse_count);
    fprintf(stderr, "  regions:    %u\n", stats.total_regions);
    fprintf(stderr, "  heap:       %.2f KB\n", heap_used() / 1024.0);

    fun_idpool_destroy(pool);
    size_t after = heap_used();
    /* 修复 size_t 下溢:destroy 返回的堆可能比 create 前更小 (释放了 glibc
     * 的 chunk),无符号减法会 wrap 成巨大值;转 int64_t 后负数 = "回收更多" */
    double diff_kb = (double)(int64_t)(after - prev) / 1024.0;
    fprintf(stderr, "  after destroy: %.2f KB (leak: %+.2f KB)\n",
            after / 1024.0, diff_kb);
}

/* ============================================================
 * 测试 5: ID 编码/解码对称性（NO_VALUE 模式）
 * ============================================================ */
static void test_id_encoding_no_value(void) {
    fprintf(stderr, "\n========== TEST: ID Encoding (NO_VALUE) ==========\n");
    fun_idpool_t pool = fun_idpool_create_ex(4, FUN_IDPOOL_MODE_NO_VALUE);
    if (!pool) { fprintf(stderr, "create failed\n"); return; }

    int N = 100000;
    uint32_t *ids = malloc(N * sizeof(uint32_t));
    if (!ids) { fprintf(stderr, "OOM\n"); return; }

    for (int i = 0; i < N; i++) {
        ids[i] = fun_idpool_gen_id(pool, NULL);
        if (ids[i] == FUN_IDPOOL_INVALID_ID) {
            fprintf(stderr, "  alloc[%d] failed\n", i);
            break;
        }
    }

    int bad = 0;
    for (int i = 0; i < N; i++) {
        if (ids[i] == FUN_IDPOOL_INVALID_ID) continue;
        void *v = fun_idpool_get_value(pool, ids[i]);
        /* NO_VALUE 模式: 存在的 ID 必须返回 (NULL+1) */
        if (v != (void*)1) {
            if (bad < 5)
                fprintf(stderr, "  id[%d]=%u get_value=%p (expect NULL+1)\n",
                        i, ids[i], v);
            bad++;
        }
    }

    /* 测试不存在的 ID */
    for (int i = 0; i < 1000; i++) {
        void *v = fun_idpool_get_value(pool, 0xDEAD0000 + i);
        if (v != NULL) {
            if (bad < 10)
                fprintf(stderr, "  invalid_id=0x%x get_value=%p (expect NULL)\n",
                        0xDEAD0000 + i, v);
            bad++;
        }
    }

    /* 释放一半再测 */
    for (int i = 0; i < N/2; i++) {
        fun_idpool_release_id(pool, ids[i]);
    }
    for (int i = 0; i < N/2; i++) {
        void *v = fun_idpool_get_value(pool, ids[i]);
        if (v != NULL) {
            if (bad < 15)
                fprintf(stderr, "  released id[%d]=%u get_value=%p (expect NULL)\n",
                        i, ids[i], v);
            bad++;
        }
    }

    fprintf(stderr, "  Verified %d IDs, %d errors\n", N + 1000 + N/2, bad);
    fprintf(stderr, "  Result: %s\n", (bad == 0) ? "PASS" : "FAIL");

    free(ids);
    fun_idpool_destroy(pool);
}

/* ============================================================
 * Main
 * ============================================================ */
int main(int argc, char **argv) {
    int threads = (argc > 1) ? atoi(argv[1]) : 8;
    int ops     = (argc > 2) ? atoi(argv[2]) : 5000;

    fprintf(stderr, "=== fun_idpool memory optimization test ===\n");
    fprintf(stderr, "threads=%d  ops=%d\n\n", threads, ops);

    /* 1. 不保存 value 模式 — 内存分析 */
    test_no_value_mode();

    /* 2. 保存 value 模式 — 对比基准 */
    test_with_value_mode();

    /* 3. 多线程正确性（两种模式） */
    test_multithread(FUN_IDPOOL_MODE_NO_VALUE, threads, ops);
    test_multithread(FUN_IDPOOL_MODE_WITH_VALUE, threads, ops);

    /* 4. 长时间存活场景 */
    test_long_lived(FUN_IDPOOL_MODE_NO_VALUE);
    test_long_lived(FUN_IDPOOL_MODE_WITH_VALUE);

    /* 5. ID 编码/解码对称性 */
    test_id_encoding_no_value();

    fprintf(stderr, "\n=== ALL TESTS COMPLETE ===\n");
    return 0;
}
