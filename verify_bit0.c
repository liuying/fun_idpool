#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "fun_idpool.h"

int main(void) {
    fprintf(stderr, "=== Single-thread Region #0 bit 0 verification ===\n\n");

    fun_idpool_t pool = fun_idpool_create_ex(1, FUN_IDPOOL_MODE_WITH_VALUE);
    if (!pool) { fprintf(stderr, "create failed\n"); return 1; }

    /* Step 1: 分配 Region #0 的所有可用 ID (应该是 1~63) */
    fprintf(stderr, "--- Step 1: Allocating 63 IDs from Region #0 ---\n");
    for (int i = 0; i < 70; i++) {
        uint32_t id = fun_idpool_gen_id(pool, (void*)(uintptr_t)(i+1));
        if (i < 5 || i == 62 || i == 63 || id > 100)
            fprintf(stderr, "  alloc[%d] = %u\n", i, id);
        if (i < 63 && id != (uint32_t)(i+1)) {
            fprintf(stderr, "  *** MISMATCH at %d: expected %d, got %u ***\n", i, i+1, id);
        }
        if (i >= 63 && id < 100) {
            fprintf(stderr, "  [info] alloc[%d] = %u (Region #1)\n", i, id);
        }
    }

    /* Step 2: 验证 get_value */
    fprintf(stderr, "\n--- Step 2: Verifying get_value ---\n");
    for (int i = 1; i <= 5; i++) {
        void *v = fun_idpool_get_value(pool, i);
        fprintf(stderr, "  get_value(%d) = %p (expected %p)\n", i, v, (void*)(uintptr_t)i);
    }
    void *v0 = fun_idpool_get_value(pool, 0);
    fprintf(stderr, "  get_value(0) = %p (expected NULL)\n", v0);

    /* Step 3: 释放并复用 */
    fprintf(stderr, "\n--- Step 3: Release and reuse ---\n");
    fun_idpool_release_id(pool, 1);
    fun_idpool_release_id(pool, 32);
    fun_idpool_release_id(pool, 63);

    for (int i = 0; i < 5; i++) {
        uint32_t id = fun_idpool_gen_id(pool, (void*)(uintptr_t)(0xDEAD0000 + i));
        fprintf(stderr, "  reuse[%d] = %u\n", i, id);
    }

    /* Step 4: 统计 */
    fun_idpool_stats_s stats;
    fun_idpool_get_stats(pool, &stats);
    fprintf(stderr, "\n--- Stats ---\n");
    fprintf(stderr, "  total_alloc: %llu\n", (unsigned long long)stats.total_alloc);
    fprintf(stderr, "  total_freed: %llu\n", (unsigned long long)stats.total_freed);
    fprintf(stderr, "  reuse_count: %llu\n", (unsigned long long)stats.reuse_count);
    fprintf(stderr, "  regions:     %u\n", stats.total_regions);
    fprintf(stderr, "  bitmap_mem:  %llu B\n", (unsigned long long)stats.bitmap_memory);
    fprintf(stderr, "  values_mem:  %llu B\n", (unsigned long long)stats.values_memory);

    fun_idpool_destroy(pool);
    fprintf(stderr, "\n=== DONE ===\n");
    return 0;
}
