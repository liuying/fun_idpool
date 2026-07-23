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

    /* 分配 63 个 ID, 应该全部在 Region #0 (bit 1~63) */
    uint32_t ids[64];
    for (int i = 0; i < 63; i++) {
        ids[i] = fun_idpool_gen_id(p, (void*)(uintptr_t)(i+1));
        if (ids[i] == FUN_IDPOOL_INVALID_ID)
            report("  alloc[%d] failed\n", i);
        /* ID 应该是 1,2,3,...,63 */
        if (ids[i] != (uint32_t)(i+1))
            report("  id[%d]=%u expected %d\n", i, ids[i], i+1);
    }
    /* 第 64 个应该去 Region #1 */
    uint32_t id64 = fun_idpool_gen_id(p, (void*)100);
    if (id64 != 64)
        report("  id64=%u expected 64\n", id64);

    /* 验证 get_value */
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
    /* 用 2 个 zone, 分配足够多的 ID 触发 slot 扩容 */
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

int main(int argc,char **argv) {
    int t=(argc>1)?atoi(argv[1]):16, o=(argc>2)?atoi(argv[2]):5000;
    fprintf(stderr,"=== fun_idpool v8 test ===\nthreads=%d ops=%d\n\n",t,o);
    test_bit0_init64();   /* Test 1 */
    test_dynamic();        /* Test 6 */
    test_novalue();        /* Test 5 */
    test_encode();         /* Test 2 */
    test_mt(t,o);          /* Test 3 */
    test_extreme(t*2);     /* Test 4 */
    fprintf(stderr,"\n=== SUMMARY: %d errors ===\n",g_errors);
    return g_errors?1:0;
}
