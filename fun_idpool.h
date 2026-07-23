#ifndef FUN_IDPOOL_H
#define FUN_IDPOOL_H

#include <stdint.h>

/* ============================================================
 * fun_idpool — 无锁 ID 池 (双模式)
 *
 * 两种模式:
 *   FUN_IDPOOL_MODE_WITH_VALUE  - 保存用户指针, get_value() 返回原指针
 *   FUN_IDPOOL_MODE_NO_VALUE   - 不保存指针, get_value() 返回 NULL(不存在)
 *                               或 (NULL+1)(存在), 极致省内存
 *
 * 特性:
 *   - 严格 C99, 零依赖 (仅 POSIX + GCC 内建原子)
 *   - 零锁 / 零 TLS / 零引用计数
 *   - Region 内 ID 单调递增
 *   - 耗尽后才复用已释放 ID
 *   - 永不返回失败 (OOM 直接 abort)
 *   - 懒加载 (Region 内存按需分配)
 *   - 跨平台
 * ============================================================ */

/* 无效 ID */
#define FUN_IDPOOL_INVALID_ID 0

/* 模式常量 */
#define FUN_IDPOOL_MODE_WITH_VALUE  1   /* 保存 value, get_value 返回原指针 */
#define FUN_IDPOOL_MODE_NO_VALUE    0   /* 不保存 value, get_value 返回 NULL/NULL+1 */

/* "ID 存在" 的哨兵返回值 (NO_VALUE 模式) */
#define FUN_IDPOOL_EXISTS ((void *)1)   /* NULL + 1 */

/* 前向声明 */
typedef struct fun_idpool_s fun_idpool_s, *fun_idpool_t;
typedef struct fun_idpool_stats_s fun_idpool_stats_s, *fun_idpool_stats_t;

/* ---------- API ---------- */

/* 创建 ID 池 (默认 WITH_VALUE 模式)
 *   numa_nodes: 指定 NUMA zone 数量 (0 = 自动检测)
 */
fun_idpool_t fun_idpool_create(int numa_nodes);

/* 创建 ID 池 (指定模式)
 *   numa_nodes: 指定 NUMA zone 数量 (0 = 自动检测)
 *   mode:       FUN_IDPOOL_MODE_WITH_VALUE 或 FUN_IDPOOL_MODE_NO_VALUE
 */
fun_idpool_t fun_idpool_create_ex(int numa_nodes, int mode);

/* 销毁 ID 池 (不可在线程运行时调用) */
void fun_idpool_destroy(fun_idpool_t pool);

/* 生成 ID 并绑定指针
 *   pool: 池
 *   ptr:  要绑定的指针 (NO_VALUE 模式下被忽略, 可传 NULL)
 *   返回: ID (永不返回 0)
 *   内存不足时 abort
 */
uint32_t fun_idpool_gen_id(fun_idpool_t pool, void *ptr);

/* 通过 ID 查询状态
 *   WITH_VALUE 模式:
 *     返回绑定的指针 (不存在返回 NULL)
 *   NO_VALUE 模式:
 *     存在返回 FUN_IDPOOL_EXISTS (NULL+1)
 *     不存在返回 NULL
 *     注意: 无法区分"绑定了 NULL"和"不存在"
 */
void *fun_idpool_get_value(fun_idpool_t pool, uint32_t id);

/* 释放 ID
 *   返回之前绑定的指针 (WITH_VALUE) 或 FUN_IDPOOL_EXISTS (NO_VALUE)
 *   ID 无效或已释放时返回 NULL
 */
void *fun_idpool_release_id(fun_idpool_t pool, uint32_t id);

/* 获取统计信息 */
void fun_idpool_get_stats(fun_idpool_t pool, fun_idpool_stats_t stats);

/* 查询池模式 */
int fun_idpool_get_mode(fun_idpool_t pool);

/* ---------- 统计结构体 ---------- */
struct fun_idpool_stats_s {
    uint64_t total_alloc;
    uint64_t total_freed;
    uint64_t scan_retries;
    uint64_t reuse_count;
    uint64_t abort_count;
    uint32_t numa_nodes;
    uint32_t total_regions;
    uint32_t mode;            /* FUN_IDPOOL_MODE_* */
    uint64_t bitmap_memory;   /* bitmap + summary 占用字节数 */
    uint64_t values_memory;   /* values 数组占用字节数 (NO_VALUE=0) */
};

#endif /* FUN_IDPOOL_H */
