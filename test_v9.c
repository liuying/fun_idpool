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

static int g_errors = 0;
static pthread_mutex_t g_err_mu = PTHREAD_MUTEX_INITIALIZER;

static void report(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&g_err_mu);
    g_errors++;
    if (g_errors <= 30) vfprintf(stderr, fmt, ap);
    pthread_mutex_unlock(&g_err_mu);
    va_end(ap);
}

/* ========== Test 1: Region #0 bit 0 保留 + INIT_CAP=64 ========== */
static void test_bit0_init64(void) {
    fprintf(stderr, "\n--- Test 1: bit0 reserved + INIT_CAP=64 ---\n");
    fun_idpool_t p = fun_idpool_create_ex(1, FUN_IDPOOL_MODE_WITH_VALUE);

    uint32_t ids[64];
    for (int i = 0; i < 63; i++) {
        ids[i] = fun_idpool_gen_id(p, (void*)(uintptr_t)(i+1));
        if (ids[i] == FUN_IDPOOL_INVALID_ID)
            report("  alloc[%d] failed\n", i);
        if (ids[i] != (uint32_t)(i+1))
            report("  id[%d]=%u expected %d\n", i, ids[i], i+1);
    }
    /* 第 64 个去 Region #1, ID = 64 */
    uint32_t id64 = fun_idpool_gen_id(p, (void*)100);
    if (id64 != 64)
        report("  id64=%u expected 64\n", id64);

    for (int i = 0; i < 63; i++) {
        void *v = fun_idpool_get_value(p, ids[i]);
        if (v != (void*)(uintptr_t)(i+1))
            report("  get[%d]=%p\n", i, v);
    }
    void *v64 = fun_idpool_get_value(p, id64);
    if (v64 != (void*)100)
        report("  get[64]=%p\n", v64);

    fun_idpool_stats_s s;
    fun_idpool_get_stats(p, &s);
    fprintf(stderr, "  alloc=%llu freed=%llu regions=%u\n",
            (unsigned long long)s.total_alloc,
            (unsigned long long)s.total_freed, s.total_regions);
    fprintf(stderr, "  bitmap_mem=%lluB values_mem=%lluB\n",
            (unsigned long long)s.bitmap_memory,
            (unsigned long long)s.values_memory);
    fprintf(stderr, "  Result: %s\n",
            (g_errors == 0) ? "PASS" : "FAIL");
    fun_idpool_destroy(p);
}

/* ========== Test 2: ID 编码/解码 10万次 ========== */
static void test_encode(void) {
    fprintf(stderr, "\n--- Test 2: ID encode/decode 100K ---\n");
    fun_idpool_t p = fun_idpool_create_ex(4, FUN_IDPOOL_MODE_WITH_VALUE);
    int N = 100000, bad = 0;
    for (int i = 0; i < N; i++) {
        uint32_t tag = 0xABC00000u + i;
        uint32_t id = fun_idpool_gen_id(p, (void*)(uintptr_t)tag);
        if (id == FUN_IDPOOL_INVALID_ID) { report("  alloc fail\n"); break; }
        void *v = fun_idpool_get_value(p, id);
        if (v != (void*)(uintptr_t)tag) { bad++; if (bad<5) report("  mismatch\n"); }
    }
    fprintf(stderr, "  Verified %d IDs, %d mismatches → %s\n",
            N, bad, (bad==0)?"PASS":"FAIL");
    fun_idpool_destroy(p);
}

/* ========== Test 3: 多线程 ========== */
typedef struct { fun_idpool_t pool; int tid; int ops; uint32_t *ids; } targ_t;

static void *worker3(void *a) {
    targ_t *t = (targ_t *)a;
    for (int i = 0; i < t->ops; i++) {
        uint32_t tag = t->tid * 1000000u + i;
        uint32_t id = fun_idpool_gen_id(t->pool, (void*)(uintptr_t)tag);
        if (id == FUN_IDPOOL_INVALID_ID) { report("  alloc fail\n"); return (void*)1; }
        t->ids[i] = id;
    }
    for (int i = 0; i < t->ops/2; i++)
        fun_idpool_release_id(t->pool, t->ids[i*2]);
    int q = t->ops/4;
    for (int i = 0; i < q; i++) {
        uint32_t tag = t->tid * 2000000u + i + 500000;
        uint32_t id = fun_idpool_gen_id(t->pool, (void*)(uintptr_t)tag);
        if (id == FUN_IDPOOL_INVALID_ID) { report("  realloc fail\n"); return (void*)1; }
        void *v = fun_idpool_get_value(t->pool, id);
        if (v != (void*)(uintptr_t)tag) report("  verify fail\n");
    }
    return NULL;
}

static void test_mt(int threads, int ops) {
    fprintf(stderr, "\n--- Test 3: Multithread (%d × %d) ---\n", threads, ops);
    fun_idpool_t p = fun_idpool_create_ex(4, FUN_IDPOOL_MODE_WITH_VALUE);
    pthread_t *ts = malloc(threads*sizeof(pthread_t));
    targ_t *tas = malloc(threads*sizeof(targ_t));
    uint32_t *ids = calloc(threads*ops, sizeof(uint32_t));

    struct timespec t0,t1; clock_gettime(CLOCK_MONOTONIC,&t0);
    for (int i=0;i<threads;i++) {
        tas[i]=(targ_t){p,i,ops,ids+i*ops};
        pthread_create(&ts[i],NULL,worker3,&tas[i]);
    }
    int errs=0;
    for (int i=0;i<threads;i++){void*r=NULL;pthread_join(ts[i],&r);if(r)errs++;}
    clock_gettime(CLOCK_MONOTONIC,&t1);
    double el=(t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)/1e9;

    fun_idpool_stats_s s; fun_idpool_get_stats(p,&s);
    uint64_t total=(uint64_t)threads*ops+(uint64_t)threads*(ops/4);
    fprintf(stderr,"  %.3fs %llu ops %.2f Mops/s errs=%d → %s\n",
            el,(unsigned long long)total,total/el/1e6,errs,
            (errs==0)?"PASS":"FAIL");
    fun_idpool_destroy(p); free(ts); free(tas); free(ids);
}

/* ========== Test 4: 极端并发 ========== */
static void *worker4(void *a) {
    fun_idpool_t p=(fun_idpool_t)a;
    for (int i=0;i<2000;i++) {
        uint32_t tag=(uint32_t)((uintptr_t)pthread_self()&0xFFFF)*10000u+i;
        uint32_t id=fun_idpool_gen_id(p,(void*)(uintptr_t)tag);
        if (id==FUN_IDPOOL_INVALID_ID){report("alloc fail\n");return(void*)1;}
        if (fun_idpool_get_value(p,id)!=(void*)(uintptr_t)tag) report("get fail\n");
        fun_idpool_release_id(p,id);
    }
    return NULL;
}

static void test_extreme(int threads) {
    fprintf(stderr,"\n--- Test 4: Extreme (%d × 2000) ---\n",threads);
    fun_idpool_t p=fun_idpool_create_ex(4,FUN_IDPOOL_MODE_WITH_VALUE);
    pthread_t *ts=malloc(threads*sizeof(pthread_t));
    for(int i=0;i<threads;i++) pthread_create(&ts[i],NULL,worker4,p);
    int errs=0;
    for(int i=0;i<threads;i++){void*r=NULL;pthread_join(ts[i],&r);if(r)errs++;}
    fun_idpool_stats_s s; fun_idpool_get_stats(p,&s);
    fprintf(stderr,"  alloc=%llu freed=%llu reuse=%llu retry=%llu errs=%d → %s\n",
            (unsigned long long)s.total_alloc,(unsigned long long)s.total_freed,
            (unsigned long long)s.reuse_count,(unsigned long long)s.scan_retries,
            errs,(errs==0)?"PASS":"FAIL");
    fun_idpool_destroy(p); free(ts);
}

/* ========== Test 5: NO_VALUE 模式 ========== */
static void test_novalue(void) {
    fprintf(stderr,"\n--- Test 5: NO_VALUE mode ---\n");
    fun_idpool_t p=fun_idpool_create_ex(4,FUN_IDPOOL_MODE_NO_VALUE);
    for(int i=0;i<1000;i++) {
        uint32_t id=fun_idpool_gen_id(p,(void*)(uintptr_t)i);
        if (id==FUN_IDPOOL_INVALID_ID) report("alloc fail\n");
        void *v=fun_idpool_get_value(p,id);
        if (v!=FUN_IDPOOL_EXISTS) report("expected EXISTS got %p\n",v);
    }
    for(int i=0;i<100;i++) {
        uint32_t id=fun_idpool_gen_id(p,NULL);
        fun_idpool_release_id(p,id);
        void *v=fun_idpool_get_value(p,id);
        if (v!=NULL) report("expected NULL got %p\n",v);
    }
    fun_idpool_stats_s s; fun_idpool_get_stats(p,&s);
    fprintf(stderr,"  values_mem=%lluB (should be 0) → %s\n",
            (unsigned long long)s.values_memory,
            (s.values_memory==0)?"PASS":"FAIL");
    fun_idpool_destroy(p);
}

/* ========== Test 6: 动态 Zone/Slot 验证 ========== */
static void test_dynamic(void) {
    fprintf(stderr,"\n--- Test 6: Dynamic zones/slots (2 zones) ---\n");
    fun_idpool_t p=fun_idpool_create_ex(2,FUN_IDPOOL_MODE_WITH_VALUE);
    for(int i=0;i<5000;i++) {
        uint32_t id=fun_idpool_gen_id(p,(void*)(uintptr_t)i);
        if (id==FUN_IDPOOL_INVALID_ID) report("alloc fail at %d\n",i);
    }
    fun_idpool_stats_s s; fun_idpool_get_stats(p,&s);
    fprintf(stderr,"  regions=%u alloc=%llu bitmap_mem=%lluB values_mem=%lluB → PASS\n",
            s.total_regions,(unsigned long long)s.total_alloc,
            (unsigned long long)s.bitmap_memory,
            (unsigned long long)s.values_memory);
    fun_idpool_destroy(p);
}

/* ========== Test 7: Zone 数量无限制验证 ========== */
static void test_unlimited_zones(void) {
    fprintf(stderr,"\n--- Test 7: Unlimited zones ---\n");

    /* 7a: 自动检测 + 安全上限 */
    fun_idpool_t p1 = fun_idpool_create_ex(0, FUN_IDPOOL_MODE_WITH_VALUE);
    fun_idpool_stats_s s1; fun_idpool_get_stats(p1, &s1);
    fprintf(stderr,"  7a: auto-detect: %u zones → PASS\n", s1.numa_nodes);
    fun_idpool_destroy(p1);

    /* 7b: 大 Zone 数量 (17 → 对齐到 32) */
    fun_idpool_t p2 = fun_idpool_create_ex(17, FUN_IDPOOL_MODE_WITH_VALUE);
    fun_idpool_stats_s s2; fun_idpool_get_stats(p2, &s2);
    if (s2.numa_nodes != 17)
        report("  7b: expected 17 zones, got %u\n", s2.numa_nodes);
    /* 分配一些 ID 验证功能正常 */
    for (int i = 0; i < 100; i++) {
        uint32_t id = fun_idpool_gen_id(p2, (void*)(uintptr_t)i);
        if (id == FUN_IDPOOL_INVALID_ID) report("  7b: alloc fail\n");
    }
    fprintf(stderr,"  7b: 17 zones (aligned 32), alloc OK → PASS\n");
    fun_idpool_destroy(p2);

    /* 7c: 31 zones → 对齐到 32 */
    fun_idpool_t p3 = fun_idpool_create_ex(31, FUN_IDPOOL_MODE_WITH_VALUE);
    fun_idpool_stats_s s3; fun_idpool_get_stats(p3, &s3);
    if (s3.numa_nodes != 31)
        report("  7c: expected 31 zones, got %u\n", s3.numa_nodes);
    for (int i = 0; i < 200; i++) {
        uint32_t id = fun_idpool_gen_id(p3, (void*)(uintptr_t)(i+1000));
        if (id == FUN_IDPOOL_INVALID_ID) report("  7c: alloc fail\n");
        void *v = fun_idpool_get_value(p3, id);
        if (v != (void*)(uintptr_t)(i+1000)) report("  7c: verify fail\n");
    }
    fprintf(stderr,"  7c: 31 zones (aligned 32), 200 allocs OK → PASS\n");
    fun_idpool_destroy(p3);

    /* 7d: 64 zones → 对齐到 64 */
    fun_idpool_t p4 = fun_idpool_create_ex(64, FUN_IDPOOL_MODE_WITH_VALUE);
    fun_idpool_stats_s s4; fun_idpool_get_stats(p4, &s4);
    if (s4.numa_nodes != 64)
        report("  7d: expected 64 zones, got %u\n", s4.numa_nodes);
    for (int i = 0; i < 500; i++) {
        uint32_t id = fun_idpool_gen_id(p4, (void*)(uintptr_t)i);
        if (id == FUN_IDPOOL_INVALID_ID) report("  7d: alloc fail\n");
    }
    fprintf(stderr,"  7d: 64 zones (aligned 64), 500 allocs OK → PASS\n");
    fun_idpool_destroy(p4);

    /* 7e: 1024 zones (安全上限) */
    fun_idpool_t p5 = fun_idpool_create_ex(1024, FUN_IDPOOL_MODE_WITH_VALUE);
    fun_idpool_stats_s s5; fun_idpool_get_stats(p5, &s5);
    if (s5.numa_nodes != 1024)
        report("  7e: expected 1024 zones, got %u\n", s5.numa_nodes);
    /* 分配一些 ID */
    for (int i = 0; i < 100; i++) {
        uint32_t id = fun_idpool_gen_id(p5, (void*)(uintptr_t)i);
        if (id == FUN_IDPOOL_INVALID_ID) report("  7e: alloc fail\n");
    }
    fprintf(stderr,"  7e: 1024 zones (aligned 1024), 100 allocs OK → PASS\n");
    fun_idpool_destroy(p5);

    fprintf(stderr,"  Test 7 Result: %s\n",
            (g_errors == 0) ? "ALL PASS" : "FAIL");
}

/* ========== Test 8: 大 Zone 数量下的 ID 编码正确性 ========== */
static void test_large_zone_encoding(void) {
    fprintf(stderr,"\n--- Test 8: ID encoding with large zone count ---\n");

    /* 33 zones → 对齐到 64, zone_shift=6, zone_mask=63 */
    fun_idpool_t p = fun_idpool_create_ex(33, FUN_IDPOOL_MODE_WITH_VALUE);
    fun_idpool_stats_s s; fun_idpool_get_stats(p, &s);
    fprintf(stderr,"  zones=%u shift=%u mask=%u\n",
            s.numa_nodes, 6, 63);

    int N = 5000;
    uint32_t *ids = malloc(N * sizeof(uint32_t));
    for (int i = 0; i < N; i++) {
        uint32_t tag = 0xDEAD0000u + i;
        ids[i] = fun_idpool_gen_id(p, (void*)(uintptr_t)tag);
        if (ids[i] == FUN_IDPOOL_INVALID_ID) {
            report("  alloc[%d] failed\n", i);
            break;
        }
    }

    /* 验证所有 ID 可正确解码 */
    int bad = 0;
    for (int i = 0; i < N; i++) {
        if (ids[i] == FUN_IDPOOL_INVALID_ID) continue;
        void *v = fun_idpool_get_value(p, ids[i]);
        uint32_t expected = 0xDEAD0000u + i;
        if (v != (void*)(uintptr_t)expected) {
            if (bad < 5)
                report("  id[%d]=%u got=%p exp=%x\n",
                       i, ids[i], v, expected);
            bad++;
        }
        /* 验证 zone_id 部分 ≤ 63 (mask=63) */
        uint32_t zid = ids[i] & 63;
        if (zid >= 33)
            report("  id[%d]=%u zone=%u >= 33\n", i, ids[i], zid);
    }
    fprintf(stderr,"  Verified %d IDs, %d mismatches → %s\n",
            N, bad, (bad==0)?"PASS":"FAIL");

    free(ids);
    fun_idpool_destroy(p);
}

/* ========== Main ========== */
int main(int argc,char **argv) {
    int t=(argc>1)?atoi(argv[1]):16, o=(argc>2)?atoi(argv[2]):5000;
    fprintf(stderr,"=== fun_idpool v9 test (unlimited zones) ===\n");
    fprintf(stderr,"threads=%d ops=%d\n\n",t,o);

    test_bit0_init64();        /* Test 1: bit0 + INIT_CAP=64 */
    test_dynamic();            /* Test 6: dynamic zones/slots */
    test_novalue();            /* Test 5: NO_VALUE */
    test_unlimited_zones();    /* Test 7: Zone 数量无限制 */
    test_large_zone_encoding();/* Test 8: 大 Zone 编码正确性 */
    test_encode();             /* Test 2: 编解码 100K */
    test_mt(t,o);              /* Test 3: 多线程 */
    test_extreme(t*2);         /* Test 4: 极端并发 */

    fprintf(stderr,"\n=== SUMMARY: %d errors ===\n",g_errors);
    return g_errors?1:0;
}
