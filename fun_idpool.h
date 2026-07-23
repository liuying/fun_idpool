#ifndef FUN_IDPOOL_H
#define FUN_IDPOOL_H

#include <stdint.h>

/* ============================================================
 * fun_idpool — 无锁 ID 池 (Region #0 bit 0 保留版)
 *
 * 特性：
 *   - 严格 C99, 零依赖 (仅 POSIX + GCC 内建原子)
 *   - 零锁 (mutex/spinlock free)
 *   - 零 TLS (__thread / pthread_key free)
 *   - 零引用计数
 *   - Region 内 ID 单调递增
 *   - Region #0 的 bit 0 永远保留 → ID 0 = INVALID_ID (永不分配)
 *   - 懒加载 (Region 内存按需分配)
 *   - 双模式: WITH_VALUE / NO_VALUE
 *   - 跨平台 (x86_64 / x86_32 / AArch64 / ARMv7 / LoongArch64)
 *   - 永不返回失败 (OOM 则 abort)
 * ============================================================ */

/* 无效 ID 哨兵值 (gen_id 永不返回 0, 即 INVALID_ID) */
#define FUN_IDPOOL_INVALID_ID 0

/* 哨兵值: NO_VALUE 模式下, ID 存在但无绑定值
 *   - get_value 返回此值表示 ID 存在
 *   - release_id 返回此值表示\"成功释放\"(原 ptr 不可知)
 *   - 区分于 NULL (NULL = 不存在 / 无效 ID / 已释放)
 */
#define FUN_IDPOOL_EXISTS ((void *)1)

/* ---------- 模式枚举 ---------- */
typedef enum {
    FUN_IDPOOL_MODE_WITH_VALUE = 0,  /* 保存用户指针, get_value 返回实际值 */
    FUN_IDPOOL_MODE_NO_VALUE   = 1   /* 不保存指针, get_value 返回 FUN_IDPOOL_EXISTS */
} fun_idpool_mode_t;

/* ---------- 类型前向声明 ---------- */
typedef struct fun_idpool_s       fun_idpool_s,       *fun_idpool_t;
typedef struct fun_idpool_stats_s fun_idpool_stats_s, *fun_idpool_stats_t;

/* ---------- API ---------- */

/* 创建 ID 池 (默认 WITH_VALUE 模式)
 *   numa_nodes: NUMA zone 数量 (0 = 自动检测, 上限 16)
 *   返回: 池指针, 内存不足时 abort
 */
fun_idpool_t fun_idpool_create(int numa_nodes);

/* 创建 ID 池 (指定模式)
 *   numa_nodes: NUMA zone 数量 (0 = 自动检测, 上限 16)
 *   mode:       FUN_IDPOOL_MODE_WITH_VALUE 或 FUN_IDPOOL_MODE_NO_VALUE
 *   返回: 池指针, 内存不足时 abort
 *   注意: mode 在创建时确定, 创建后不可更改
 */
fun_idpool_t fun_idpool_create_ex(int numa_nodes, fun_idpool_mode_t mode);

/* 销毁 ID 池 (不可在线程运行时调用)
 *   释放所有 Region 的 bitmap / summary / values
 *   释放 Zone Registry 当前数组 + 所有延迟释放的旧数组
 */
void fun_idpool_destroy(fun_idpool_t pool);

/* 生成 ID 并绑定指针 (永不返回 0)
 *   WITH_VALUE: ptr 存入 values 数组, get_value 可取回
 *   NO_VALUE:   ptr 被忽略, ID 仅记录存在性
 *   返回: ID (1..), 永不返回 0 (FUN_IDPOOL_INVALID_ID)
 *   内存不足时 abort
 */
uint32_t fun_idpool_gen_id(fun_idpool_t pool, void *ptr);

/* 通过 ID 查询状态
 *   WITH_VALUE: 返回绑定的指针 (可能为 NULL;不存在/无效 ID 也返回 NULL)
 *   NO_VALUE:   存在返回 FUN_IDPOOL_EXISTS, 不存在返回 NULL
 */
void *fun_idpool_get_value(fun_idpool_t pool, uint32_t id);

/* 释放 ID
 *   WITH_VALUE: 返回之前绑定的指针, ID 无效时返回 NULL
 *   NO_VALUE:   成功释放返回 FUN_IDPOOL_EXISTS, ID 无效返回 NULL
 */
void *fun_idpool_release_id(fun_idpool_t pool, uint32_t id);

/* 获取统计信息 (累加所有 zone 的数据)
 *   stats: 调用方提供的 stats 结构体指针
 */
void fun_idpool_get_stats(fun_idpool_t pool, fun_idpool_stats_t stats);

/* ---------- 统计结构体 ---------- */
struct fun_idpool_stats_s {
    uint64_t total_alloc;           /* 总分配次数 (跨 zone 累计) */
    uint64_t total_freed;           /* 总释放次数 */
    uint64_t scan_retries;          /* TAS 失败回退次数 (cursor 推进计数) */
    uint64_t reuse_count;           /* 复用已释放 ID 的次数 */
    uint64_t abort_count;           /* 内部错误中止次数 (预留, 当前未使用) */
    uint32_t numa_nodes;            /* 实际 zone 数 */
    uint32_t total_regions;         /* 当前已发布 Region 总数 (跨 zone) */
    fun_idpool_mode_t mode;         /* 当前模式 (创建时确定, 不可变) */

    /* 内存统计 (按 mode 区分大小, 可与 mallinfo2 交叉验证) */
    uint64_t bitmap_memory;         /* bitmap + summary 占用字节 (跨 zone 累计) */
    uint64_t values_memory;         /* values[] 占用字节 (NO_VALUE 模式恒为 0) */
    uint64_t region_struct_memory;  /* Region 结构体占用字节 (按 mode 区分 sizeof) */
};

#endif /* FUN_IDPOOL_H */
