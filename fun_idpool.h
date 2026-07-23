#ifndef FUN_IDPOOL_H
#define FUN_IDPOOL_H

#include <stdint.h>

/* ============================================================
 * fun_idpool — 无锁 ID 池（拆分结构体版）
 *
 * 特性：
 *   - 严格 C99，零锁，零 TLS，零引用计数
 *   - Region 内 ID 单调递增，耗尽后才复用
 *   - 永不返回失败（OOM 则 abort）
 *   - 懒加载（Region 内存按需分配）
 *   - 双模式：WITH_VALUE（绑定指针）/ NO_VALUE（仅管理 ID 存在性）
 *   - NO_VALUE 模式极致省内存：无 values 数组，结构体更紧凑
 *   - 跨平台（x86_64/x86_32/AArch64/ARMv7/LoongArch64）
 * ============================================================ */

#define FUN_IDPOOL_INVALID_ID 0

/* ID 存在哨兵值（NO_VALUE 模式下 get_value 返回此值表示 ID 存在） */
#define FUN_IDPOOL_EXISTS ((void *)1)

/* ---------- 模式枚举 ---------- */
typedef enum {
    FUN_IDPOOL_MODE_WITH_VALUE = 0,  /* 绑定指针，支持 get_value */
    FUN_IDPOOL_MODE_NO_VALUE   = 1   /* 仅管理 ID，不存指针，极致省内存 */
} fun_idpool_mode_t;

/* ---------- 类型定义 ---------- */
typedef struct fun_idpool_s fun_idpool_s, *fun_idpool_t;
typedef struct fun_idpool_stats_s fun_idpool_stats_s, *fun_idpool_stats_t;

/* ---------- API ---------- */

/* 创建 ID 池（指定模式和 NUMA 节点数）
 *   numa_nodes: Zone 数量 (0 = 自动检测)
 *   mode:       WITH_VALUE 或 NO_VALUE
 */
fun_idpool_t fun_idpool_create_ex(int numa_nodes, fun_idpool_mode_t mode);

/* 兼容旧 API：等价于 fun_idpool_create_ex(numa_nodes, WITH_VALUE) */
static inline fun_idpool_t fun_idpool_create(int numa_nodes) {
    return fun_idpool_create_ex(numa_nodes, FUN_IDPOOL_MODE_WITH_VALUE);
}

/* 销毁 ID 池（不可在线程运行时调用） */
void fun_idpool_destroy(fun_idpool_t pool);

/* 生成 ID 并绑定指针
 *   WITH_VALUE: ptr 存入 values 数组，get_value 可取回
 *   NO_VALUE:   ptr 被忽略，ID 仅记录存在性
 *   返回: ID (永不返回 0)
 */
uint32_t fun_idpool_gen_id(fun_idpool_t pool, void *ptr);

/* 通过 ID 取回指针
 *   WITH_VALUE: 返回绑定的指针（NULL 也可能是绑定的值）
 *   NO_VALUE:   存在返回 FUN_IDPOOL_EXISTS，不存在返回 NULL
 */
void *fun_idpool_get_value(fun_idpool_t pool, uint32_t id);

/* 释放 ID，返回之前绑定的指针（NO_VALUE 模式返回 FUN_IDPOOL_EXISTS 或 NULL） */
void *fun_idpool_release_id(fun_idpool_t pool, uint32_t id);

/* 获取统计信息 */
void fun_idpool_get_stats(fun_idpool_t pool, fun_idpool_stats_t stats);

/* 查询当前模式 */
fun_idpool_mode_t fun_idpool_get_mode(fun_idpool_t pool);

/* ---------- 统计结构体 ---------- */
struct fun_idpool_stats_s {
    uint64_t total_alloc;
    uint64_t total_freed;
    uint64_t scan_retries;
    uint64_t reuse_count;
    uint64_t abort_count;
    uint32_t numa_nodes;
    uint32_t total_regions;
    uint64_t bitmap_memory;    /* 位图占用字节数 */
    uint64_t values_memory;    /* values 数组占用字节数（NO_VALUE 为 0）*/
    uint64_t region_memory;    /* Region 结构体占用字节数 */
    fun_idpool_mode_t mode;    /* 当前模式 */
};

#endif /* FUN_IDPOOL_H */
