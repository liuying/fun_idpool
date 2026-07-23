#ifndef FUN_IDPOOL_H
#define FUN_IDPOOL_H

#include <stdint.h>

/* ============================================================
 * fun_idpool — 无锁 ID 池 (v8: 柔性数组 + 动态 slots)
 *
 * 特性：
 *   - 严格 C99, 零依赖 (仅 POSIX + GCC 内建原子)
 *   - 零锁 (mutex/spinlock free)
 *   - 零 TLS (__thread / pthread_key free)
 *   - 零引用计数
 *   - Region #0 bit 0 保留 (ID 0 = FUN_IDPOOL_INVALID_ID 永不分配)
 *   - Region 内 ID 单调递增, 耗尽后才复用
 *   - 懒加载: bitmap / values / slots / registry 全部按需分配
 *   - 动态 zones (柔性数组, 精确 numa_nodes 内存)
 *   - 动态 slots (初始 SLOT_INIT_CAP = 4, 2 倍扩容)
 *   - Zone 属性重排 (热数据 / 统计 / 全局位图 / 动态数组, 独立 cache line)
 *   - 双模式: WITH_VALUE / NO_VALUE
 *   - INIT_CAP = 64 (Region #0 可用 63 个 ID, bit 0 保留)
 *   - 永不返回失败 (OOM 则 abort)
 *
 * 线程安全:
 *   - gen_id / release_id / get_value: 多线程并发安全 (lock-free)
 *   - create / destroy: 不可与 gen_id 并发 (单线程独占)
 *   - 同一进程内可同时存在多种模式的池, 互不影响
 *
 * 典型用法:
 *   fun_idpool_t pool = fun_idpool_create_ex(4, FUN_IDPOOL_MODE_WITH_VALUE);
 *   uint32_t id = fun_idpool_gen_id(pool, my_ptr);     // 永不失败
 *   void *ptr = fun_idpool_get_value(pool, id);        // 取回指针
 *   void *ptr = fun_idpool_release_id(pool, id);       // 释放
 *   fun_idpool_destroy(pool);                          // 销毁
 * ============================================================ */

/* 无效 ID 哨兵值 (gen_id 永不返回 0, 即 INVALID_ID)
 *   调用方可用 id == 0 判定"无效/未分配", 无需额外标志位 */
#define FUN_IDPOOL_INVALID_ID 0

/* 哨兵值: NO_VALUE 模式下表示 ID 存在但无绑定值
 *   - get_value 返回此值表示 ID 存在
 *   - release_id 返回此值表示"成功释放"(原 ptr 不可知)
 *   - 区分于 NULL (NULL = 不存在 / 无效 ID / 已释放)
 *   - 等价于 (void*)(NULL + 1), 即非空非 NULL 的特殊值 */
#define FUN_IDPOOL_EXISTS ((void *)1)

/* ---------- 模式枚举 ---------- */
typedef enum {
    /* WITH_VALUE: 保存用户指针
     *   - gen_id(pool, ptr) → ptr 存入 values 数组
     *   - get_value(id) → 返回原 ptr (可能为 NULL)
     *   - release_id(id) → 返回原 ptr
     *   - 内存: 1M ID 大约 8 MB (values 数组) */
    FUN_IDPOOL_MODE_WITH_VALUE = 0,

    /* NO_VALUE: 仅管理 ID 存在性
     *   - gen_id(pool, ptr) → ptr 被忽略
     *   - get_value(id) → 存在返回 FUN_IDPOOL_EXISTS, 不存在返回 NULL
     *   - release_id(id) → 成功释放返回 FUN_IDPOOL_EXISTS, 无效返回 NULL
     *   - 内存: 1M ID 大约 140 KB (省 ~98%) */
    FUN_IDPOOL_MODE_NO_VALUE   = 1
} fun_idpool_mode_t;

/* ---------- 类型前向声明 ---------- */
typedef struct fun_idpool_s       fun_idpool_s,       *fun_idpool_t;
typedef struct fun_idpool_stats_s fun_idpool_stats_s, *fun_idpool_stats_t;

/* ---------- API ---------- */

/* 创建 ID 池 (默认 WITH_VALUE 模式)
 *
 * 参数:
 *   numa_nodes: NUMA zone 数量 (0 = 自动检测, 上限 16)
 *               自动检测从 /sys/devices/system/node/online 读取
 *               或用 sysconf(_SC_NPROCESSORS_ONLN) 估算
 *
 * 返回:
 *   池指针 (柔性数组 zones[] 精确分配 numa_nodes 个 zone)
 *
 * 失败:
 *   内存不足时直接 abort()
 *
 * 等价于:
 *   fun_idpool_create_ex(numa_nodes, FUN_IDPOOL_MODE_WITH_VALUE)
 */
fun_idpool_t fun_idpool_create(int numa_nodes);

/* 创建 ID 池 (指定模式)
 *
 * 参数:
 *   numa_nodes: NUMA zone 数量 (0 = 自动检测, 上限 16)
 *   mode:       FUN_IDPOOL_MODE_WITH_VALUE 或 FUN_IDPOOL_MODE_NO_VALUE
 *
 * 返回:
 *   池指针
 *
 * 失败:
 *   内存不足时直接 abort()
 *
 * 注意:
 *   - mode 在创建时确定, 创建后不可更改
 *   - 同一进程可创建多个不同模式的池, 互不影响
 */
fun_idpool_t fun_idpool_create_ex(int numa_nodes, fun_idpool_mode_t mode);

/* 销毁 ID 池
 *
 * 前置条件:
 *   - 不可与 gen_id / release_id / get_value 并发调用
 *   - 建议在所有使用线程结束后调用
 *
 * 释放内容:
 *   - 所有 Region 的 bitmap / summary / values 数组
 *   - 所有 Zone 的 slots / regions_ptr / old_arrays 数组
 *   - 柔性 zones 数组
 *   - Pool 结构体本身
 */
void fun_idpool_destroy(fun_idpool_t pool);

/* 生成 ID 并绑定指针
 *
 * 参数:
 *   pool: 池指针
 *   ptr:  要绑定的指针 (WITH_VALUE 模式存入, NO_VALUE 模式忽略)
 *
 * 返回:
 *   新生成的 ID (1..UINT32_MAX), 永不返回 0 (FUN_IDPOOL_INVALID_ID)
 *   ID 编码: (region_base + bit_offset) << zone_shift | zone_id
 *
 * 失败:
 *   - 内存不足时 abort() (永不返回失败)
 *   - 注意: 内部有忙等路径, 即使所有 Region 满, 也会等待其他线程释放
 *
 * 线程安全: 多线程并发安全 (lock-free)
 */
uint32_t fun_idpool_gen_id(fun_idpool_t pool, void *ptr);

/* 通过 ID 查询状态
 *
 * 参数:
 *   pool: 池指针
 *   id:   要查询的 ID
 *
 * 返回:
 *   WITH_VALUE 模式:
 *     - ID 存在: 返回绑定的指针 (可能为 NULL, 因为绑了 NULL 也算有效)
 *     - ID 不存在 / 无效: 返回 NULL
 *   NO_VALUE 模式:
 *     - ID 存在: 返回 FUN_IDPOOL_EXISTS
 *     - ID 不存在 / 无效: 返回 NULL
 *
 * 性能: O(numa_nodes * MAX_REGIONS), 内部会遍历查找
 *
 * 线程安全: 多线程并发安全 (lock-free)
 */
void *fun_idpool_get_value(fun_idpool_t pool, uint32_t id);

/* 释放 ID
 *
 * 参数:
 *   pool: 池指针
 *   id:   要释放的 ID
 *
 * 返回:
 *   WITH_VALUE 模式:
 *     - 成功: 返回之前绑定的指针 (可能为 NULL)
 *     - 失败 (ID 无效 / 已释放): 返回 NULL
 *   NO_VALUE 模式:
 *     - 成功: 返回 FUN_IDPOOL_EXISTS
 *     - 失败 (ID 无效 / 已释放): 返回 NULL
 *
 * 释放后:
 *   - 该 ID 可被后续 gen_id 复用 (Phase 1 回绕扫描)
 *   - get_value 返回 NULL (或不存在标记)
 *
 * 线程安全: 多线程并发安全 (lock-free)
 */
void *fun_idpool_release_id(fun_idpool_t pool, uint32_t id);

/* 获取统计信息 (累加所有 zone 的数据)
 *
 * 参数:
 *   pool:  池指针
 *   stats: 调用方提供的 stats 结构体指针, 函数填充其字段
 *
 * 用途:
 *   - 监控池的使用情况 (分配次数 / 复用率)
 *   - 与 mallinfo2() 交叉验证内存占用
 *   - 性能分析 (scan_retries 反映竞争激烈程度)
 *
 * 线程安全: 多线程并发安全 (lock-free)
 */
void fun_idpool_get_stats(fun_idpool_t pool, fun_idpool_stats_t stats);

/* ---------- 统计结构体 ---------- */
struct fun_idpool_stats_s {
    /* ===== 累计统计 (跨 zone) ===== */
    uint64_t total_alloc;         /* 总分配次数 (含复用) */
    uint64_t total_freed;         /* 总释放次数 */
    uint64_t scan_retries;        /* TAS 失败回退次数 (cursor 推进计数) */
    uint64_t reuse_count;         /* 复用已释放 ID 的次数 */
    uint64_t abort_count;         /* 内部错误中止次数 (预留, 当前未使用) */

    /* ===== 内存统计 (可与 mallinfo2 交叉验证) ===== */
    uint64_t bitmap_memory;       /* 位图实际占用字节数 (bitmap + summary 累计) */
    uint64_t values_memory;       /* values 数组实际占用字节数 (NO_VALUE 模式恒为 0) */

    /* ===== 元数据 ===== */
    uint32_t numa_nodes;          /* 实际 zone 数 */
    uint32_t total_regions;       /* 当前已发布 Region 总数 (跨 zone) */
    uint32_t mode;                /* 当前模式 (创建时确定, 不可变) */
};

#endif /* FUN_IDPOOL_H */
