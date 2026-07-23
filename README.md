# fun_idpool v8 — 无锁 ID 池

## 核心特性

- **严格 C99**, 零依赖 (仅 POSIX + GCC 内建原子)
- **零锁** (mutex/spinlock free) / **零 TLS** / **零引用计数**
- **Region #0 bit 0 保留**: ID 0 = `FUN_IDPOOL_INVALID_ID` 永不分配
- **Region 内 ID 单调递增**, 耗尽后才复用
- **懒加载**: bitmap / values / slots / registry 全部按需分配
- **柔性数组 zones** (`zones[]` 柔性数组成员): 精确 numa_nodes 内存, 消除固定数组浪费
- **Zone 属性重排**: 热数据 / 统计 / 全局位图 / 动态数组 分别独立 cache line 对齐
- **动态 slots** (初始 `SLOT_INIT_CAP = 4`, 2 倍扩容)
- **Zone Registry 动态数组** (per-zone 懒加载, 旧数组延迟释放)
- **双模式**: `WITH_VALUE` (绑定指针) / `NO_VALUE` (省内存)
- **INIT_CAP = 64** (Region #0 可用 63 个 ID, bit 0 保留)
- **永不返回失败** (OOM 则 `abort`)

## v8 设计要点

相比前版本,v8 主要改动:

| 改动 | 说明 |
|---|---|
| **柔性数组 zones** | `zones[0..numa_nodes-1]` 替代固定 `zones[16]` 数组, 精确分配内存 |
| **动态 slots** | `region_slot *slots` 替代固定 `slots[MAX_REGIONS]`, 懒加载扩容 |
| **动态 registry** | `idpool_region_base **regions_ptr` 替代全局 `zone_registry[][]`, per-zone 管理 |
| **属性重排** | Zone 内字段按 cache line 分组: 热数据 / 统计 / 大块指针 / 动态数组 |
| **region_slot 内聚** | 从 `arch_defs.h` 移到 `fun_idpool.c` (业务概念非平台抽象) |

## Region #0 bit 0 保留策略

`ID 0` 作为 `FUN_IDPOOL_INVALID_ID` 永不分配, 调用方可用 `id == 0` 直接判定"无效/未分配", 无需额外标志位。

**3 处协同实现**:

| 位置 | 行为 |
|---|---|
| `ensure_region` | 分配 bitmap 后, `bit_tas(bm, 0)` 预设 bit 0 占用 |
| `create_region` | Region #0 的 `cursor` 初始值设为 1 (跳过 bit 0) |
| `get_value` / `release_id` | 显式拒绝 `id == 0`, 直接返回 NULL |

**安全性**: bit 0 预设 + cursor 起点跳过, 双保险。

## ID 编码

```
ID = (region_base + bit_offset) << zone_shift | zone_id

Region #0: base=0,   bit 1~63   → ID 1~63     (bit 0 永久保留)
Region #1: base=64,  bit 0~63   → ID 64~127
Region #2: base=128, bit 0~63   → ID 128~191
...
Region #k: base=64*(2^k - 1), cap=64*2^k
```

## 文件

| 文件 | 说明 |
|---|---|
| `arch_defs.h` | 平台抽象层 (cache line / 原子 / 对齐 / NUMA) |
| `fun_idpool.h` | 公共 API (8 个函数 + 模式枚举 + 统计结构体) |
| `fun_idpool.c` | 完整实现 (~720 行, v8: 柔性数组 + 动态 slots) |
| `test_v8.c` | 6 项综合测试 (单 Region / ID 编解码 / MT / 极端并发 / 长生命周期 / 内存统计) |
| `Makefile` | 构建脚本 (all / run / debug / asan / info / clean) |
| `Makefile.mem` | 旧版内存测试 Makefile (test_mem) |
| `Makefile.memopt` | 双模式内存测试 Makefile (test_memopt) |

## API

```c
#include "fun_idpool.h"

/* ===== 创建 ===== */
fun_idpool_t pool = fun_idpool_create(4);             /* 默认 WITH_VALUE */
fun_idpool_t pool_nv = fun_idpool_create_ex(4, FUN_IDPOOL_MODE_NO_VALUE);
fun_idpool_t pool_wv = fun_idpool_create_ex(4, FUN_IDPOOL_MODE_WITH_VALUE);

/* ===== 分配/查询/释放 ===== */
uint32_t id = fun_idpool_gen_id(pool, my_ptr);         /* 永不返回 0 */
void *ptr  = fun_idpool_get_value(pool, id);           /* WITH_VALUE: ptr; NO_VALUE: FUN_IDPOOL_EXISTS / NULL */
void *ptr  = fun_idpool_release_id(pool, id);          /* 释放, 返回原值 */

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
make           # Release 编译 (test_v8)
make run       # 运行 test_v8
make debug     # Debug 构建 (-O0 -g)
make asan      # AddressSanitizer 构建
make info      # 显示构建配置
make clean     # 清理

# 旧版内存测试
make -f Makefile.mem
make -f Makefile.memopt
```

### 测试覆盖 (test_v8 6 项)

```
1. Same-Region Monotonic    [PASS]  alloc[0..62]=1..63, alloc[63]=64 (跳 Region #0 bit 0)
2. ID Encoding               [PASS]  100K IDs 全量回环, 0 mismatches
3. Multithread (16 × 5000)   [PASS]  ~16 M ops/s, 0 errors
4. Extreme Concurrency       [PASS]  极端并发场景
5. NO_VALUE vs WITH_VALUE    [PASS]  两种模式独立验证
6. Memory Statistics         [PASS]  bitmap/values 统计自洽
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

## 约束与限制

- **ID 上限**: 单 zone 单 Region 最大 `1 << 20 = 1M` 个 ID
- **Zone 数量**: 1 ~ 16, 超出会被截断
- **MAX_REGIONS**: 256 (达到时 `create_region` 会 `abort`)
- **线程安全**:
  - `gen_id` / `release_id` / `get_value` 多线程安全 (无锁)
  - `create` / `destroy` **不可与 gen_id 并发**
- **ID 编码**: `ID = (region_base + bit_offset) << zone_shift | zone_id`
- **Region #0**: bit 0 永久保留 (ID 0 永不分配)

## 平台支持

| 平台 | 状态 |
|---|---|
| x86_64 | ✅ |
| x86_32 | ✅ (-m32 -march=i586 + -latomic) |
| AArch64 | ✅ (-march=armv8-a+lse) |
| ARMv7 | ✅ (-march=armv7-a -mfpu=neon + -latomic) |
| LoongArch64 | ✅ (-march=la664) |

## License

Public domain / MIT — see [`LICENSE`](LICENSE) for full text.
