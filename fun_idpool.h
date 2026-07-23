#ifndef FUN_IDPOOL_H
#define FUN_IDPOOL_H

#include <stdint.h>

/* ============================================================
 * fun_idpool — 无锁 ID 池
 *
 * 特性：
 *   - 严格 C99
 *   - 零锁 (mutex/spinlock free)
 *   - 零 TLS (__thread / pthread_key free)
 *   - 零引用计数
 *   - Region 内 ID 单调递增
 *   - 耗尽后才复用已释放 ID
 *   - 永不返回失败 (内存不足直接 abort)
 *   - 懒加载 (Region 内存按需分配)
 *   - 跨平台 (x86_64 / x86_32 / AArch64 / ARMv7 / LoongArch64)
 * ============================================================ */

/* 无效 ID */
#define FUN_IDPOOL_INVALID_ID 0

/* 前向声明
 *   fun_idpool_s  : 结构体值类型
 *   fun_idpool_t  : 指针类型
 */
typedef struct fun_idpool_s fun_idpool_s, *fun_idpool_t;
typedef struct fun_idpool_stats_s fun_idpool_stats_s, *fun_idpool_stats_t;

/* ---------- API ---------- */

/* 创建 ID 池
 *   numa_nodes: 指定 NUMA zone 数量 (0 = 自动检测)
 *   返回池指针，内存不足时 abort
 */
fun_idpool_t fun_idpool_create(int numa_nodes);

/* 销毁 ID 池 (不可在线程运行时调用) */
void fun_idpool_destroy(fun_idpool_t pool);

/* 生成 ID 并绑定指针
 *   pool: 池
 *   ptr:  要绑定的指针 (可为 NULL)
 *   返回: ID (永不返回 0)
 *   内存不足时 abort
 */
uint32_t fun_idpool_gen_id(fun_idpool_t pool, void *ptr);

/* 通过 ID 取回指针
 *   返回绑定的指针，ID 无效或已释放时返回 NULL
 */
void *fun_idpool_get_value(fun_idpool_t pool, uint32_t id);

/* 释放 ID
 *   返回之前绑定的指针 (便于清理)
 *   ID 无效或已释放时返回 NULL
 */
void *fun_idpool_release_id(fun_idpool_t pool, uint32_t id);

/* 获取统计信息 */
void fun_idpool_get_stats(fun_idpool_t pool, fun_idpool_stats_t stats);

/* ---------- 统计结构体 ---------- */
struct fun_idpool_stats_s {
    uint64_t total_alloc;
    uint64_t total_freed;
    uint64_t scan_retries;
    uint64_t reuse_count;
    uint64_t abort_count;
    uint32_t numa_nodes;
    uint32_t total_regions;
};

#endif /* FUN_IDPOOL_H */