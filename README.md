# fun_idpool — 跨平台无锁 ID 池

## 核心特性

- **严格 C99**，零锁，零 TLS，零引用计数
- **双模式**：`WITH_VALUE`（绑定指针）/ `NO_VALUE`（极致省内存）
- **拆分结构体**：`idpool_region_base`（无 values）+ `idpool_region`（继承 + values）
- NO_VALUE 模式 **省 97.7% 内存**（1M IDs：199 KB vs 8.5 MB）
- Region 内 ID 单调递增，耗尽后才复用
- 永不返回失败（OOM 直接 abort）
- 懒加载（bitmap/values 按需分配）
- 跨平台（x86_64/x86_32/AArch64/ARMv7/LoongArch64）

## 内存对比（4 Zone，实测）

| IDs | NO_VALUE | WITH_VALUE | 节省 |
|-----|---------|-----------|------|
| 10 | 68 KB | 69 KB | 0.9% |
| 100 | 68 KB | 70 KB | 2.7% |
| 1,000 | 69 KB | 85 KB | 19.2% |
| 10,000 | 76 KB | 232 KB | 67.2% |
| 100,000 | 100 KB | 641 KB | 84.4% |
| **1,000,000** | **199 KB** | **8.5 MB** | **97.7%** |

> 节省来源（拆分结构体的两层优化）：
> 1. **`values[]` 数组**：每个 ID 8 字节指针，1M IDs 省 ~8 MB
> 2. **`idpool_region` 的对齐 padding**：`idpool_region` 末尾的 `void **values` 字段对齐到 8 字节需要 8 字节 padding；NO_VALUE 模式下根本没有这个字段，Region 结构体本身更紧凑
> 3. **`bitmap + summary`**：两种模式都需要，不受影响

## 文件

| 文件 | 说明 |
|------|------|
| `arch_defs.h` | 平台抽象层（cache line、原子操作、slot、对齐、NUMA 检测） |
| `fun_idpool.h` | 公共 API（双模式 + create_ex + FUN_IDPOOL_EXISTS） |
| `fun_idpool.c` | 完整实现（拆分结构体 + 懒加载 + 两阶段分配） |
| `test_split.c` | 8 项综合测试 |
| `Makefile` | 跨平台构建（make/run/debug/asan） |

## API

```c
#include "fun_idpool.h"

/* 双模式创建 */
fun_idpool_t pool_nv = fun_idpool_create_ex(4, FUN_IDPOOL_MODE_NO_VALUE);
fun_idpool_t pool_wv = fun_idpool_create_ex(4, FUN_IDPOOL_MODE_WITH_VALUE);

/* 兼容旧 API（默认 WITH_VALUE） */
fun_idpool_t pool = fun_idpool_create(4);

/* 生成 ID（永不失败） */
uint32_t id = fun_idpool_gen_id(pool, my_ptr);

/* 取回指针 */
void *ptr = fun_idpool_get_value(pool, id);
/* NO_VALUE 模式：返回 FUN_IDPOOL_EXISTS（ID 存在）或 NULL（不存在） */

/* 释放 ID */
void *ptr = fun_idpool_release_id(pool, id);

/* 销毁 */
fun_idpool_destroy(pool);

/* 查询模式 */
fun_idpool_mode_t m = fun_idpool_get_mode(pool);
```

## 构建与测试

```bash
make              # Release 编译
make run          # 运行（默认 16 线程 × 5000 ops）
make run THREADS=32 OPS=10000   # 自定义参数
make debug        # Debug 构建（-O0 -g）
make asan         # AddressSanitizer
make info         # 显示架构信息
make clean        # 清理构建产物
```

## 测试结果

```
=== SUMMARY ===
Total real errors: 0
*** ALL TESTS PASSED ***

Test 1: 同 Region 单调递增     — PASS (两种模式)
Test 2: 内存精确统计           — PASS (values_memory 自洽)
Test 3: ID 编解码 10万次       — PASS (0 errors)
Test 4: 多线程 16×5000         — PASS (13~16 M ops/s)
Test 5: 极端并发 16×2000       — PASS (0 errors)
Test 6: 长生命周期 10%         — PASS (零内存增长)
Test 7: NO_VALUE vs WITH_VALUE — PASS (省 97.7%)
Test 8: 模式行为验证           — PASS
AddressSanitizer               — PASS (0 内存违规)
```

## 拆分结构体设计

```
idpool_region_base {          idpool_region {
    uint32_t base;    ─┐           idpool_region_base base;  ← 完全复用
    uint32_t cap;     │           void **values;    ← 仅 WITH_VALUE
    uint32_t used;    │           }
    uint32_t cursor;  │
    ...               │ 热数据（紧凑）
    uint64_t *bitmap;─┘
    uint64_t *summary;
};

NO_VALUE 模式：分配 sizeof(idpool_region_base)  ← 小
WITH_VALUE 模式：分配 sizeof(idpool_region)     ← 大（多 8 字节指针）
```

**设计要点：**
- `idpool_region_base` 是所有模式共享的"基础 Region"，紧凑布局仅含分配所需字段
- `idpool_region` 嵌入 `idpool_region_base base` + 追加 `void **values`（仅 WITH_VALUE 模式有意义）
- 通过 `_Static_assert` 静态断言保证 `sizeof(((idpool_region*)0)->base) == sizeof(idpool_region_base)`，类型转换安全
- `ensure_region` / `create_region` 接受 `fun_idpool_mode_t` 参数，按模式分配不同大小的结构体
- NO_VALUE 模式下 `r` 实际只有 `idpool_region_base` 大小，`values` 字段不存在；访问路径走 `(idpool_region*)r` 强制转换时由 mode 守卫保护，永不解引用

**为什么 NO_VALUE 能省那么多：**
1. 1M ID 场景 `values[]` 数组本身占用 `cap * 8B ≈ 8MB`
2. `idpool_region` 因 `void **values` 字段对齐导致末尾 8 字节 padding
3. 拆分后 NO_VALUE 模式连结构体本身都更小，省去 padding

## 模式选择建议

| 你的需求 | 推荐模式 | 原因 |
|---------|---------|------|
| 需要通过 ID 取回绑定的指针 | `WITH_VALUE` | 唯一选择 |
| 只需要"ID 是否存在"的查询 | `NO_VALUE` | 省 97% 内存 |
| 嵌入式 / 内存敏感场景 | `NO_VALUE` | 省内存是核心收益 |
| 大规模 ID（百万以上） | `NO_VALUE` | 内存节省指数级放大 |
| 需要区分"绑 NULL" 和 "ID 不存在" | `NO_VALUE` | `FUN_IDPOOL_EXISTS` 哨兵解决歧义 |
| 兼容旧代码（用 `fun_idpool_create(int)`） | `WITH_VALUE` | 默认模式 |

**注意：**
- 模式在 `fun_idpool_create_ex` 时确定，**创建后不可更改**
- 两种模式的 API 表面相同，但 `get_value` / `release_id` 返回值语义不同
- 同一进程内可同时存在两种模式的池，互不影响

## 约束与限制

- **ID 上限**：单 zone 单 Region 最大 `1 << 20 = 1,048,576` 个 ID；每个 zone 可扩展多个 Region（默认 `MAX_REGIONS = 256`），理论上限 ≈ `256 × 2^20 = 2.6 亿` 个 ID
- **Region 数量**：`MAX_REGIONS = 256` 达到时 `create_region` 会 abort（理论极限，不是实际问题）
- **Zone 数量**：1 ~ 16，超出会被截断；自动检测从 `/sys/devices/system/node/online` 读取
- **线程安全**：
  - 多线程并发 `gen_id` / `release_id` / `get_value` 安全（无锁）
  - `fun_idpool_create` / `fun_idpool_destroy` **不可与 gen_id 并发调用**
  - 跨池不保证亲和性，每个池独立管理自己的 zone
- **ID 编码**：`ID = (base + bit) << zone_shift | zone_id`，因此不同 zone 的 ID 不会冲突，但同 zone 内 bit 不复用直至 Region 满
- **内存释放**：`fun_idpool_destroy` 释放所有 Region 的 bitmap / summary / values；NO_VALUE 模式不分配 values 故无对应 free

## 平台支持

| 平台 | flags | 状态 |
|------|-------|------|
| x86_64 | `-march=native` | ✅ |
| x86_32 | `-m32 -march=i586` | ✅ |
| AArch64 | `-march=armv8-a+lse` | ✅ |
| ARMv7 | `-march=armv7-a -mfpu=neon -latomic` | ✅ |
| LoongArch64 v1.0 | `-march=la464 -latomic` | ✅ |
| LoongArch64 v1.1 | `-march=la664` | ✅ |

## License

Public domain / MIT — see [`LICENSE`](LICENSE) for full text.
