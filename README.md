# fun_idpool — 跨平台无锁 ID 池

## 核心特性

- **严格 C99，零依赖** (仅 POSIX + GCC 内建原子)
- **零锁 / 零 TLS / 零引用计数** — 真正的 lock-free
- **双模式**：
  - `WITH_VALUE`: 绑定指针，`get_value` 取回原指针
  - `NO_VALUE`: 仅管理 ID 存在性，省 ~80% 内存
- **Region #0 bit 0 保留** → ID 0 永不分配，`FUN_IDPOOL_INVALID_ID = 0` 天然安全
- **懒加载**: Region bitmap/values 按需分配，小 ID 量时内存极小
- **Zone Registry 懒加载**: 初始 cap=2，按 2 倍扩容，老数组延迟释放
- **永不返回失败**（OOM 直接 `abort`）
- **跨平台**: x86_64 / x86_32 / AArch64 / ARMv7 / LoongArch64 v1.0 & v1.1

## Region #0 bit 0 保留策略

`ID 0` 作为 `FUN_IDPOOL_INVALID_ID` 永不分配，使调用方可以用 `id == 0` 直接判定"无效/未分配"，无需额外标志位。

**实现要点（3 处协同）：**

| 位置 | 行为 |
|---|---|
| `ensure_region_full` | 分配 bitmap 后，`bm[0] \|= 1ULL` 预设 bit 0 为占用 |
| `create_region` / `create_ex` | Region #0 的 `cursor` 初始值设为 1（跳过 bit 0） |
| `get_value` / `release_id` | 显式拒绝 `id == 0`，直接返回 NULL |

**ID 编码**：`ID = (region_base + bit_offset) << zone_shift | zone_id`

```
Region #0: base=0,   bit 1~63   → ID 1~63     (bit 0 永久保留)
Region #1: base=64,  bit 0~63   → ID 64~127
Region #2: base=128, bit 0~63   → ID 128~191
...
Region #k: base=64*(2^k - 1), cap=64*2^k, 单 Region 最大 1M (2^20) 个 ID
```

## 文件

| 文件 | 说明 |
|---|---|
| `fun_idpool.h` | 公共 API（双模式 + 模式枚举 + 统计结构体） |
| `fun_idpool.c` | 完整实现（拆分结构体 + 懒加载 + 两阶段分配 + Zone Registry 懒加载） |
| `arch_defs.h` | 平台抽象（cache line / 原子包装 / slot / 对齐分配 / NUMA 检测） |
| `test_bit0.c` | 6 项综合测试（同 Region 单调、ID 编解码、MT、极端并发、NO_VALUE、内存统计） |
| `verify_bit0.c` | 单线程手工验证（逐步展示 alloc/get_value/release） |
| `Makefile` | 跨平台构建脚本（run / verify / debug / asan） |

## API

```c
#include "fun_idpool.h"

/* ===== 创建 ===== */
fun_idpool_t pool = fun_idpool_create(4);     /* 默认 WITH_VALUE */
fun_idpool_t pool_nv = fun_idpool_create_ex(4, FUN_IDPOOL_MODE_NO_VALUE);
fun_idpool_t pool_wv = fun_idpool_create_ex(4, FUN_IDPOOL_MODE_WITH_VALUE);

/* ===== 分配/查询/释放 ===== */
uint32_t id = fun_idpool_gen_id(pool, my_ptr);      /* 永不返回 0 */
void *ptr  = fun_idpool_get_value(pool, id);        /* WITH_VALUE: ptr; NO_VALUE: FUN_IDPOOL_EXISTS / NULL */
void *ptr  = fun_idpool_release_id(pool, id);       /* 释放, 返回原值 */

/* ===== 统计 ===== */
fun_idpool_stats_s stats;
fun_idpool_get_stats(pool, &stats);
/* stats.total_alloc / total_freed / reuse_count / bitmap_memory / values_memory ... */

/* ===== 销毁 ===== */
fun_idpool_destroy(pool);
```

### 模式行为对比

| 操作 | `WITH_VALUE` | `NO_VALUE` |
|---|---|---|
| `gen_id(pool, ptr)` | `ptr` 存入 values 数组 | `ptr` 被忽略 |
| `gen_id` 返回 | `ID >= 1`（永不返回 0） | `ID >= 1` |
| `get_value(id)` 存在 | 返回绑定的 ptr（可为 NULL） | 返回 `FUN_IDPOOL_EXISTS = (void*)1` |
| `get_value(id)` 不存在 | 返回 `NULL` | 返回 `NULL` |
| `release_id(id)` 成功 | 返回原 ptr | 返回 `FUN_IDPOOL_EXISTS` |
| `release_id(id)` 无效 | 返回 `NULL` | 返回 `NULL` |

**哨兵值**：`FUN_IDPOOL_EXISTS = (void*)1` 解决"绑了 NULL"与"不存在"的歧义。

## 构建与测试

```bash
make              # Release 编译 test_bit0
make run          # 运行 test_bit0（默认 16 线程 × 5000 ops）
make run THREADS=32 OPS=10000   # 自定义参数
make verify       # 编译并运行 verify_bit0（单线程手工验证）
make debug        # Debug 构建（-O0 -g -DDEBUG）
make asan         # AddressSanitizer 构建
make info         # 显示架构信息
make clean        # 清理构建产物
```

### 测试覆盖

`test_bit0`（6 项）+ `verify_bit0`（单线程逐步验证）：

```
=== test_bit0 ===
Test 1: Region #0 bit 0 reserved  → PASS
Test 2: ID Encoding (100K)       → 0 mismatches
Test 3: Multithread (16×5000)     → ~16 M ops/s, 0 errors
Test 4: Extreme (32×2000)         → 0 errors
Test 5: NO_VALUE Mode             → PASS
Test 6: Memory Statistics         → PASS
AddressSanitizer                  → 0 memory errors

=== verify_bit0 ===
alloc[0..62]   = 1..63       (Region #0 bit 1~63)
alloc[63]      = 64          (Region #1 bit 0, 跳过 Region #0 bit 0)
get_value(0)   = NULL        (ID 0 永不分配)
get_value(1)   = 0x1         (原值回读正确)
reuse          = [1,32,63,71,72]   (回绕复用)
```

## 模式选择建议

| 你的需求 | 推荐模式 | 原因 |
|---|---|---|
| 需要通过 ID 取回绑定的指针 | `WITH_VALUE` | 唯一选择 |
| 只需要"ID 是否存在"的查询 | `NO_VALUE` | 省 ~80% 内存 |
| 嵌入式 / 内存敏感场景 | `NO_VALUE` | 省内存是核心收益 |
| 大规模 ID（百万以上） | `NO_VALUE` | 内存节省指数级放大 |
| 需要区分"绑 NULL" 和 "ID 不存在" | `NO_VALUE` | `FUN_IDPOOL_EXISTS` 哨兵解决歧义 |
| 兼容旧代码 | `WITH_VALUE` | 默认模式 |

**注意**：模式在 `fun_idpool_create_ex` 时确定，**创建后不可更改**。

## 内存布局

### 拆分结构体（核心优化）

```
idpool_region_base (无 values):    idpool_region (含 values):
  uint32_t base;                      idpool_region_base base;   ← 完全复用
  uint32_t cap;                       void **values;             ← 仅 WITH_VALUE
  uint32_t used;
  uint32_t cursor;
  uint32_t alloced;
  uint32_t state;
  uint32_t zone_id;
  uint32_t region_idx;
  uint64_t *bitmap;
  uint64_t *summary;
  uint32_t summary_words;
  char _pad[4];
```

通过 `_Static_assert` 保证 base 布局一致；`NO_VALUE` 模式只分配 `sizeof(idpool_region_base)`。

### Zone Registry 懒加载

| 项目 | 详情 |
|---|---|
| 初始容量 | cap = 2（2 个指针 = 16 字节） |
| 扩容策略 | 2 倍增长，上限 MAX_REGIONS = 256 |
| 旧数组处理 | 进入 `old_arrays` 列表延迟释放 |
| 小池收益 | 单 zone 仅 Region #0 → 16 字节（vs 2 KB 全分配） |

### 实测内存（单 zone, 1M ID）

| 组件 | `WITH_VALUE` | `NO_VALUE` |
|---|---|---|
| Region bitmap (1M bit) | ~128 KB | ~128 KB |
| Region summary | ~2 KB | ~2 KB |
| Region values (1M × 8 B) | ~8 MB | **0** |
| Region 结构体（×14） | ~1 KB | ~1 KB |
| Zone 固定开销 | ~2.6 KB | ~2.6 KB |
| **合计** | **~8.5 MB** | **~140 KB** |

## 统计字段

`fun_idpool_get_stats` 填充 `fun_idpool_stats_s`：

| 字段 | 含义 |
|---|---|
| `total_alloc` / `total_freed` | 跨 zone 累计分配 / 释放次数 |
| `scan_retries` | TAS 失败回退次数（cursor 推进计数） |
| `reuse_count` | 复用已释放 ID 的次数 |
| `numa_nodes` | 实际 zone 数 |
| `total_regions` | 当前已发布 Region 总数（跨 zone） |
| `mode` | 池模式（创建时确定，不可变） |
| `bitmap_memory` | bitmap + summary 占用字节（跨 zone 累计） |
| `values_memory` | values[] 占用字节（`NO_VALUE` 模式恒为 0） |
| `region_struct_memory` | Region 结构体占用字节（按 mode 区分 sizeof） |

可与 `mallinfo2()` 交叉验证。

## 约束与限制

- **ID 上限**：单 zone 单 Region 最大 `1 << 20 = 1,048,576` 个 ID；每 zone 可扩展至 `MAX_REGIONS = 256` 个 Region，理论上限 ≈ 2.6 亿个 ID
- **Zone 数量**：1 ~ 16，超出会被截断；自动检测从 `/sys/devices/system/node/online` 读取
- **线程安全**：
  - 多线程并发 `gen_id` / `release_id` / `get_value` 安全（lock-free）
  - `fun_idpool_create` / `fun_idpool_destroy` **不可与 `gen_id` 并发调用**
  - 跨池不保证亲和性，每个池独立管理自己的 zone
- **ID 编码**：`ID = (region_base + bit_offset) << zone_shift | zone_id`，不同 zone 的 ID 不会冲突
- **Region #0 特殊**：bit 0 永远保留（ID 0 = INVALID_ID 永不分配）
- **内存释放**：`fun_idpool_destroy` 释放所有 Region 的 bitmap / summary / values + Zone Registry 当前数组 + 所有延迟释放的旧数组

## 平台支持

| 平台 | flags | 状态 |
|---|---|---|
| x86_64 | `-march=native` | ✅ |
| x86_32 (i386 / i686) | `-m32 -march=i586` + `-latomic` | ✅ |
| AArch64 | `-march=armv8-a+lse` | ✅ |
| ARMv7 | `-march=armv7-a -mfpu=neon` + `-latomic` | ✅ |
| LoongArch64 v1.0 (LA464) | `-march=la464` + `-latomic` | ✅ |
| LoongArch64 v1.1 (LA664) | `-march=la664` | ✅ |

## License

Public domain / MIT — see [`LICENSE`](LICENSE) for full text.
