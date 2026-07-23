# fun_idpool v9 — 无锁 ID 池 (Zone 无限制版)

## 核心特性

- **严格 C99**, 零依赖 (仅 POSIX + GCC 内建原子)
- **零锁** (mutex/spinlock free) / **零 TLS** / **零引用计数**
- **Region #0 bit 0 保留** (ID 0 = `FUN_IDPOOL_INVALID_ID` 永不分配)
- **Region 内 ID 单调递增**, 耗尽后才复用已释放 ID
- **Zone 数量无限制** (安全上限 1024, 内存分配精确 / 运算对齐 2 的幂)
- **懒加载**: bitmap / values / slots / registry 全部按需分配
- **柔性数组 zones** (`zones[]` 柔性数组成员, 精确 numa_nodes 内存)
- **Zone 属性重排**: 热数据 / 统计 / 全局位图 / 动态数组, 独立 cache line
- **双模式**: `WITH_VALUE` (绑定指针) / `NO_VALUE` (省内存)
- **INIT_CAP = 64** (Region #0 可用 63 个 ID, bit 0 保留)
- **永不返回失败** (OOM 则 `abort`)
- **跨平台**: x86_64 / x86_32 / AArch64 / ARMv7 / LoongArch64

## v9 设计要点

相比 v8 的核心改进:

| 改动 | 说明 |
|---|---|
| **Zone 无限制** | 移除硬编码 16 上限, 安全上限 1024 |
| **分配精确值** | Zone 数量用于内存分配时是精确值 (flexible array) |
| **编码对齐值** | 用于 ID 编码时对齐到 2 的幂 (zone_mask / zone_shift) |

## ID 编码格式

```
┌──────────────────────────────┬──────────────────┐
│         ID Index            │   Zone ID (低)   │
└──────────────────────────────┴──────────────────┘
         >> zone_shift              & zone_mask

ID = (region_base + bit_offset) << zone_shift | zone_id

Region #0: [bit0=保留] bit1→ID1 ... bit63→ID63     (63 个 ID)
Region #1: bit0→ID64  ... bit127→ID191              (128 个 ID)
Region #2: bit0→ID192 ... bit447→ID447             (256 个 ID)
...
```

## Zone 数量对齐示例

| 指定 Zone 数 | 实际 Zone 数 | 对齐后 | zone_shift | zone_mask |
|---|---|---|---|---|
| 1 | 1 | 1 | 0 | 0 |
| 2 | 2 | 2 | 1 | 1 |
| 4 | 4 | 4 | 2 | 3 |
| 17 | 17 | 32 | 5 | 31 |
| 31 | 31 | 32 | 5 | 31 |
| 33 | 33 | 64 | 6 | 63 |
| 64 | 64 | 64 | 6 | 63 |
| 1024 | 1024 | 1024 | 10 | 1023 |

> Zone 数量用于内存分配时是精确值, 用于 ID 编码时对齐到 2 的幂。

## Region #0 bit 0 保留策略

`ID 0` 作为 `FUN_IDPOOL_INVALID_ID` 永不分配, 调用方可用 `id == 0` 直接判定"无效/未分配", 无需额外标志位。

**3 处协同实现**:

| 位置 | 行为 |
|---|---|
| `ensure_region` | 分配 bitmap 后, `bit_tas(bm, 0)` 预设 bit 0 占用 |
| `create_region` | Region #0 的 `cursor` 初始值设为 1 (跳过 bit 0) |
| `get_value` / `release_id` | 显式拒绝 `id == 0`, 直接返回 NULL |

**安全性**: bit 0 预设 + cursor 起点跳过, 双保险。

## 文件

| 文件 | 说明 |
|---|---|
| `arch_defs.h` | 平台抽象层 (cache line / 原子 / 对齐 / NUMA) |
| `fun_idpool.h` | 公共 API (8 个函数 + 模式枚举 + 统计结构体) |
| `fun_idpool.c` | 完整实现 (~720 行, v9: Zone 数量无限制) |
| `test_v9.c` | 8 项综合测试 |
| `Makefile` | 构建脚本 (all / run / debug / asan) |
| `Makefile.mem` | 旧版内存分析 Makefile (test_mem) |
| `Makefile.memopt` | 双模式测试 Makefile (test_memopt) |

## API

```c
#include "fun_idpool.h"

/* ===== 创建 (Zone 数量无限制, 0 = 自动检测, 上限 1024) ===== */
fun_idpool_t pool = fun_idpool_create(4);             /* 默认 WITH_VALUE */
fun_idpool_t pool_nv = fun_idpool_create_ex(4, FUN_IDPOOL_MODE_NO_VALUE);
fun_idpool_t pool_wv = fun_idpool_create_ex(4, FUN_IDPOOL_MODE_WITH_VALUE);

/* ===== 分配/查询/释放 ===== */
uint32_t id = fun_idpool_gen_id(pool, my_ptr);         /* 永不返回 0 */
void *ptr  = fun_idpool_get_value(pool, id);           /* 取回指针 */
void *ptr  = fun_idpool_release_id(pool, id);          /* 释放 */

/* ===== 统计 ===== */
fun_idpool_stats_s stats;
fun_idpool_get_stats(pool, &stats);

/* ===== 销毁 ===== */
fun_idpool_destroy(pool);
```

### 模式行为对比

| 操作 | `WITH_VALUE` | `NO_VALUE` |
|---|---|---|
| `gen_id(pool, ptr)` | `ptr` 存入 values 数组 | `ptr` 被忽略 |
| `gen_id` 返回 | `ID >= 1` | `ID >= 1` |
| `get_value(id)` 存在 | 返回绑定的 ptr (可为 NULL) | 返回 `FUN_IDPOOL_EXISTS = (void*)1` |
| `get_value(id)` 不存在 | `NULL` | `NULL` |
| `release_id(id)` 成功 | 返回原 ptr | 返回 `FUN_IDPOOL_EXISTS` |
| `release_id(id)` 无效 | `NULL` | `NULL` |

## 构建与测试

```bash
make                # Release 编译 (test_v9)
make run            # 运行主测试 (默认 16 线程 × 5000 ops)
make run THREADS=32 OPS=10000
make debug          # Debug 构建 (-O0 -g)
make asan           # AddressSanitizer
make info           # 显示构建配置
make clean          # 清理

# 旧版测试 (Makefile.mem / Makefile.memopt)
make -f Makefile.mem
make -f Makefile.memopt
```

### 测试覆盖 (test_v9 8 项)

```
1. bit0 reserved + INIT_CAP=64                → PASS
2. ID encode/decode 100K                     → 0 mismatches
3. Multithread (16 × 5000)                     → ~16 M ops/s, 0 errors
4. Extreme (32 × 2000)                         → 0 errors
5. NO_VALUE mode (values_mem=0)              → PASS
6. Dynamic zones/slots (2 zones)             → PASS
7. Unlimited zones (1/17/31/64/1024)         → ALL PASS
8. ID encoding with large zone count         → 0 mismatches
```

## 模式选择建议

| 需求 | 推荐 |
|---|---|
| 需要通过 ID 取回绑定的指针 | `WITH_VALUE` |
| 只需要"ID 是否存在" | `NO_VALUE` |
| 嵌入式 / 内存敏感 | `NO_VALUE` |
| 大规模 ID (百万以上) | `NO_VALUE` |
| 兼容旧代码 | `WITH_VALUE` (默认) |

**注意**: 模式在 `create_ex` 时确定, **创建后不可更改**。

## 统计字段

`fun_idpool_get_stats` 填充 `fun_idpool_stats_s`:

| 字段 | 含义 |
|---|---|
| `total_alloc` / `total_freed` | 跨 zone 累计分配 / 释放次数 |
| `scan_retries` | TAS 失败回退次数 (cursor 推进计数) |
| `reuse_count` | 复用已释放 ID 的次数 |
| `abort_count` | 内部错误中止次数 (预留) |
| `bitmap_memory` | bitmap 实际占用字节数 (跨 zone) |
| `values_memory` | values 数组实际占用字节数 (NO_VALUE = 0) |
| `numa_nodes` | 实际 zone 数 |
| `total_regions` | 当前已发布 Region 总数 (跨 zone) |
| `mode` | 池模式 (创建时确定, 不可变) |

可与 `mallinfo2()` 交叉验证内存占用。

## 约束与限制

- **Zone 数量**: 1 ~ 1024, 超出会被截断 (实际系统远小于此值)
- **ID 编码**: `ID = (region_base + bit_offset) << zone_shift | zone_id`
- **Region 数量**: `MAX_REGIONS = 256` (达到时 `create_region` 会 `abort`)
- **Region 大小**: 单 Region 最大 `1 << 20 = 1M` 个 ID
- **线程安全**:
  - `gen_id` / `release_id` / `get_value` 多线程安全 (lock-free)
  - `create` / `destroy` **不可与 gen_id 并发**
  - 跨池不保证亲和性, 每个池独立管理自己的 zone
- **Region #0 特殊**: bit 0 永久保留 (ID 0 永不分配)
- **Zone 数量分配 vs 编码**:
  - 内存分配: 精确分配 `numa_nodes` 个 zone (柔性数组)
  - ID 编码: 对齐到 2 的幂 (zone_mask / zone_shift)

## 平台支持

| 平台 | 状态 |
|---|---|
| x86_64 | ✅ |
| x86_32 | ✅ (-m32 -march=i586 + -latomic) |
| AArch64 | ✅ (-march=armv8-a+lse) |
| ARMv7 | ✅ (-march=armv7-a -mfpu=neon + -latomic) |
| LoongArch64 | ✅ (-march=la664) |

## License

Public domain / MIT (your choice).
