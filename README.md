# fun_idpool — 跨平台无锁 ID 池

## 特性

- **严格 C99**，零依赖（仅 POSIX + GCC 内建原子）
- **零锁**：无 mutex / spinlock
- **零 TLS**：不用 `__thread` / `pthread_key`
- **零引用计数**
- **懒加载**：Region 内存首次使用时才分配，ID 用量少时极小内存
- **单调递增**：同一 Region 内 ID 单向递增，耗尽后才复用已释放 ID
- **永不返回失败**：内存不足直接 `abort()`（无静默失败）
- **NUMA 感知**：实际检测节点数，内存分配用实际值，位运算用对齐值
- **跨平台**：x86_64 / x86_32 / AArch64 / ARMv7 / LoongArch64 v1.0 & v1.1

## 文件

| 文件 | 说明 |
|------|------|
| `arch_defs.h` | 平台抽象层（cache line、原子操作、slot、对齐、getcpu、NUMA 检测） |
| `fun_idpool.h` | 公共 API（5 个函数 + 常量 + 统计结构体） |
| `fun_idpool.c` | 完整实现 |
| `test_idpool.c` | 综合测试（5 项：单调、懒加载、编解码、多线程、极端并发） |
| `Makefile` | 跨平台构建脚本 |

## API

```c
#include "fun_idpool.h"

fun_idpool *pool = fun_idpool_create(4);              // 创建池（4 个 Zone，0=自动检测）
uint32_t id = fun_idpool_gen_id(pool, my_ptr);      // 生成 ID（永不失败，OOM 则 abort）
void *ptr = fun_idpool_get_value(pool, id);        // 取回指针（ID 无效返回 NULL）
void *ptr = fun_idpool_release_id(pool, id);       // 释放 ID（返回绑定指针）
fun_idpool_destroy(pool);                           // 销毁池
```

## 构建与测试

```bash
make                # 编译
make run            # 运行（默认 16 线程 × 5000 ops）
make run THREADS=32 OPS=10000 ZONES=4
make debug          # Debug 构建
make asan           # AddressSanitizer 内存检查
make info          # 显示架构与编译参数
make clean         # 清理
```

## ID 编码格式

```
┌──────────────────────────────┬──────────────┐
│         ID Index            │  Zone ID    │
└──────────────────────────────┴──────────────┘
         >> zone_shift             低位
```

- **Zone ID** = `cpu & zone_mask`（NUMA 亲和）
- **ID Index** = `region_base + bit_offset`（单调递增）
- 同一 Region 内，ID 严格递增直到 cap-1，然后才复用

## Region 容量规划

| Region # | cap（INIT_CAP=64） | base |
|----------|----------------------|------|
| 0 | 64 | 0 |
| 1 | 128 | 64 |
| 2 | 256 | 192 |
| 3 | 512 | 448 |
| 4 | 1024 | 960 |
| ... | ... | ... |
| k | 64 × 2^k | 64 × (2^k - 1) |

公式：
- `cap(k)   = INIT_CAP × 2^k`（钳制到 MAX_CAP）
- `base(k)  = INIT_CAP × (2^k - 1)`

## 平台支持矩阵

| 平台 | 编译器 flags | 64-bit 原子 | 128-bit 原子 | libatomic |
|------|-------------|-------------|--------------|-----------|
| x86_64 | `-march=native` | ✅ | ⚠️ CMPXCHG16B | 不需要 |
| x86_32 (i586+) | `-m32 -march=i586` | ✅ | ❌ | 不需要 |
| x86_32 (i386) | `-m32 -march=i386` | ❌ | ❌ | **需要** |
| AArch64 | `-march=armv8-a+lse` | ✅ | ⚠️ | 不需要 |
| ARMv7 | `-march=armv7-a -mfpu=neon` | ⚠️ LDREXD* | ❌ | **需要** |
| LoongArch64 v1.0 (LA464) | `-march=la464` | ✅ LL.D/SC.D | ❌ SC.Q | **需要(128位)** |
| LoongArch64 v1.1 (LA664) | `-march=la664` | ✅ | ✅ SC.Q | 不需要 |

> ARMv7 的 LDREXD/STREXD 要求 8 字节对齐，`aligned_u64` 已保证。

## 设计要点

### 三级位图
1. **Global Bitmap**：快速跳过 FULL Region
2. **Summary Bitmap**：快速跳过全满的 64-bit 字
3. **Main Bitmap**：实际分配/回收 bit

### 两阶段分配
- **Phase 0**：从 cursor 往后找空闲位（单调递增）
- **Phase 1**：cursor 到末尾后，回绕扫描（复用已释放 ID）

### 懒加载
- Region 结构体在 `create_region` 时分配（很小，~64 字节）
- bitmap / summary / values 在首次 `ensure_region` 时才分配
- 小容量场景（如只分配几个 ID）几乎不占内存

### NUMA 优化
- `detect_numa_nodes()` 读 `/sys/devices/system/node/online`
- 内存分配用**实际节点数**
- 位运算用**对齐到 2 的幂**（shift + mask）
- 每 Zone 独立统计计数器（减少跨核 cache bouncing）

## 测试结果

```
=== Test 2: Lazy Allocation ===
  After 10 allocs: regions=4 alloc=10  → PASS

=== Test 1: Same-Region Monotonic ===
  Same-region monotonic: PASS
  Reuse after free: PASS

=== Test 5: ID Encode/Decode ===
  Verified 100000 IDs, 0 mismatches  → PASS

=== Test 3: Multithread (16 × 5000) ===
  throughput: 15.68 M ops/s
  alloc: 100001  freed: 40000  reuse: 98467
  thread errs: 0  → PASS

=== Test 4: Extreme Concurrency (32 × 2000) ===
  alloc: 64001  freed: 64000  reuse: 63998
  → PASS (AddressSanitizer clean)

*** ALL TESTS PASSED ***
```

## License

Public domain / MIT (your choice).
