# Cache-Friendly Data Layout Demo

验证 [Behind The Scenes of Bun Install](https://bun.com/blog/behind-the-scenes-of-bun-install) 博客中 **Cache-Friendly Data Layout** 一节描述的优化效果。

## 背景：为什么 Bun Install 比 npm 快？

当你运行 `npm install` 时，npm 需要解析 lockfile 和 registry manifest 来确定要安装哪些包。这个过程中，npm（以及 yarn、pnpm）使用 JavaScript 的 `JSON.parse()` 把 lockfile/manifest 解析成 JS 对象。这些对象由 V8 引擎在堆上分配，散落在内存各处，互相通过指针引用。

这种内存布局对 CPU 缓存极不友好：
- **CPU 缓存的工作原理**：CPU 不能直接访问内存，而是以 64 字节为单位（cache line）把数据从内存加载到缓存中。L1 缓存最快（~1ns）但最小（32-128KB），L2 (~5ns, 256KB-1MB)，L3 (~10ns, 8-48MB）。如果数据不在任何缓存中，就要去 DRAM 取，需要 ~100-400ns。
- **散落的对象意味着大量 cache miss**：每次访问一个包的元数据，CPU 加载一整条 64 字节的 cache line，但其中可能只有几字节是你需要的数据，其余是 V8 的对象头、GC 标记等无关内容。
- **指针追踪 (pointer chasing)**：访问 `package.dependencies.react` 这样的嵌套结构时，CPU 必须先加载 package 对象，找到 dependencies 指针，再加载 dependencies 对象……每一跳都可能是一次 cache miss。

**Bun 的做法完全不同**：
- 用 **Structure of Arrays (SoA)** 替代传统的 Array of Structures (AoS)，把同类字段存在连续数组中
- 用 **扁平数组 + 自增整数 ID** 替代 HashMap + 字符串 key，消除指针追踪和字符串 hash 开销
- 用 **紧凑的二进制格式** 缓存 registry manifest，避免 JSON 解析

博客给出的理论计算：

> 每次 cache miss ≈ 1200 cycles ≈ 400ns (@3GHz)
> 1000 个包 × 5 个依赖 = 5000 次查找
> 5000 × 400ns = **2ms 纯内存延迟**

本项目通过 4 个 C++ benchmark 验证这些优化的实际效果。

---

## Benchmark 1: AoS vs SoA

### 什么是 AoS 和 SoA？

**Array of Structures (AoS)** 是最自然的数据组织方式——每个包是一个 struct，所有字段紧挨着存储：

```
Package[0]: [name, version, deps, hash, url, scripts, ...]  ← 520 bytes
Package[1]: [name, version, deps, hash, url, scripts, ...]  ← 520 bytes
Package[2]: [name, version, deps, hash, url, scripts, ...]  ← 520 bytes
```

**Structure of Arrays (SoA)** 把每种字段拆成独立的数组：

```
names:    [name0,    name1,    name2,    ...]   ← 连续存储
versions: [ver0,     ver1,     ver2,     ...]   ← 连续存储
deps:     [deps0,    deps1,    deps2,   ...]   ← 连续存储
hashes:   [hash0,    hash1,    hash2,    ...]   ← 连续存储
```

当你只需要扫描一个字段时（比如"统计所有包的依赖数量"），SoA 只需要读 `deps` 这一个数组（100KB 连续内存），而 AoS 要跳过每个包的所有其他字段（总共 49.6MB），每一跳都浪费了大量 cache line。

### 测试设计

模拟 10 万个包的 lockfile 元数据。`PackageAoS` 结构体设为 520 字节（横跨 8 条 cache line），模拟真实 npm 包的完整字段（name, description, homepage, repository, keywords, scripts, 各种 deps map 等）。SoA 把每种字段存在独立的连续数组中。

四组测试访问不同数量的字段，字段故意分布在不同的 cache line 上：
- `num_deps` 在 cache line 0（偏移 9 字节）
- `integrity_hash` 在 cache line 1（偏移 64 字节）
- `publish_time` 在 cache line 7（偏移 448 字节）

**源码**: [aos_vs_soa.cpp](aos_vs_soa.cpp)

### 结果

```
=== AoS vs SoA Benchmark ===
  Packages: 100000    Rounds: 200

  sizeof(PackageAoS) = 520 bytes (8 cache lines per struct)
  SoA num_deps array = 100000 bytes (contiguous)

  AoS total memory: 49.6 MB
  SoA total memory: 2050.8 KB (only fields actually used)

  1. Single-field (num_deps)            AoS:    110.5 us   SoA:      9.0 us   speedup: 12.27x
  2. Two fields (2 cache lines)         AoS:    153.5 us   SoA:     55.2 us   speedup: 2.78x
  3. Three fields (3 cache lines)       AoS:    194.5 us   SoA:     66.7 us   speedup: 2.92x
  4. Filtered (sparse access)           AoS:     63.8 us   SoA:     25.3 us   speedup: 2.52x
```

**结论**：
- 单字段扫描 SoA 快 **12.3x** — AoS 每个包跳 520 字节 (8 条 cache line)，只用其中 1 字节；SoA 只读 100KB 连续数组
- 访问 2-3 个分布在不同 cache line 上的字段，SoA 仍快 **2.8~2.9x** — AoS 每个包要加载多条 cache line，大部分内容被浪费
- 过滤扫描 SoA 快 **2.5x** — SoA 先在紧凑数组上做筛选，命中时才稀疏访问其他字段
- 核心规律：**struct 越大、访问的字段越少，SoA 优势越明显** — 这正是 Bun lockfile 的典型场景

---

## Benchmark 2: HashMap vs Flat Array

### 问题：为什么 npm 用 HashMap，Bun 用数组？

npm/yarn 在内存中用类似 `Map<string, Package>` 的结构存储包信息，查找一个包要：
1. 计算包名的 hash 值（逐字节处理 `"@scope/react"` 这样的字符串）
2. 在 hash 表中找到对应的桶（bucket），可能是链表
3. 沿链表比较字符串找到目标节点
4. 读取节点中的数据

Bun 给每个包分配一个自增整数 ID（0, 1, 2, ...），用 `Array[id]` 直接索引：
1. 计算地址：`base + id × sizeof(Package)`（一次乘法）
2. 读取数据

### 测试设计

模拟 10 万个包，**四组测试统一 100K 次操作**，输出 ns/op 方便横向对比。四种访问模式揭示了 CPU 内存子系统的不同行为：

- **Sequential**：按顺序遍历所有包 — CPU prefetcher 可以预测下一个地址
- **Random (independent)**：100K 次独立的随机查找 — cache 不友好，但 CPU 可以同时发起多个 load
- **Serial Random**：每次查找依赖上一次的结果，但目标地址来自预生成的随机数组
- **Chain walk**：每次查找依赖上一次的结果，且下一个地址藏在当前包的 `dep_ids[0]` 字段中 — CPU 必须等当前 load 完成才能算出下一个地址，**最坏情况**

**源码**: [hashmap_vs_flat.cpp](hashmap_vs_flat.cpp)

### 结果

```
=== HashMap vs Flat Array Benchmark ===
  Packages: 100000    Ops per test: 100000    Rounds: 200

  Flat array memory: 5.3 MB (contiguous)
  HashMap nodes: scattered across heap

  Chain walk unique packages visited: 100000 / 100000 (100.0%)

  1. Sequential traversal       Flat:    40.8 us ( 0.4 ns/op)   HashMap:   468.6 us (  4.7 ns/op)   speedup: 11.5x
  2. Random (independent)       Flat:    79.5 us ( 0.8 ns/op)   HashMap:  2181.2 us ( 21.8 ns/op)   speedup: 27.4x
  3. Chain (serial+structured)  Flat:  1797.1 us (18.0 ns/op)   HashMap:  6154.5 us ( 61.5 ns/op)   speedup: 3.4x
  4. Serial Random (control)    Flat:   812.5 us ( 8.1 ns/op)   HashMap:  2703.3 us ( 27.0 ns/op)   speedup: 3.3x
```

**结论**：

Flat array 的 ns/op 展示了访问模式对延迟的影响：

| 访问模式 | Flat (ns/op) | HashMap (ns/op) | 加速比 | 特征 |
|----------|-------------|-----------------|--------|------|
| Sequential | 0.4 | 4.7 | **11.5x** | prefetcher 预测，流水线满载 |
| Random (independent) | 0.8 | 21.8 | **27.4x** | cache 不友好，但各次 load 独立，MLP 并行 |
| Serial Random | 8.1 | 27.0 | **3.3x** | 串行依赖，MLP 失效 |
| Chain (serial) | 18.0 | 61.5 | **3.4x** | 串行 + 必须读完整 pkg 才知下一跳地址 |

- **sequential (0.4) < random (0.8) < serial random (8.1) < chain (18.0)** 符合预期
- Chain 是最慢的：严格串行依赖 + 随机跳转，下一个地址藏在当前 pkg 的 `dep_ids[0]` 字段中，CPU 必须等完整 load 完成才能发起下一次访问，每次 cache miss 延迟完全暴露
- Random 虽然同样是随机跳转，但各次访问互相独立，CPU 乱序执行可以同时发起多个 miss (Memory-Level Parallelism)，摊薄了延迟
- Flat 比 HashMap 快不是因为"访问更快"，而是**每次查找从 3-4 次访存降到 1 次**：HashMap 的 `find()` 需要读 string → hash → 读 bucket → 比较 → 读 value

---

## Benchmark 3: JSON 对象树 vs AoS vs SoA+字符串池

### 问题：npm 和 Bun 处理包数据的方式到底有什么不同？

当 npm 执行 `JSON.parse(manifest)` 时，V8 引擎不是创建一个大对象，而是创建一**棵由十几个小对象组成的树**，每个对象单独在堆上分配：

```javascript
// JSON.parse() 后 V8 堆上的状态——每个 {} 和 "" 都是独立的堆对象
package = {                     // 对象 1 (堆地址 0x1000)
  name: "@scope/react",         // 字符串对象 (0x5200)
  version: {                    // 对象 2 (0x9000)
    major: 18, minor: 2, patch: 0,
    raw: "18.2.0"               // 字符串对象 (0xA100)
  },
  dist: {                       // 对象 3 (0x12000)
    tarball: "https://...",     // 字符串 (0x15000)
    integrity: "sha512-..."    // 字符串 (0x21000)
  },
  dependencies: {               // 对象 4 (0x31000)
    "loose-envify": "^1.1.0",  //   key (0x35000) + value (0x39000)
    ...
  }
}
```

访问 `pkg.dist.tarball` 需要 **3 次指针追踪**：`pkg → dist → tarball → data`，每一跳都可能 cache miss。

Bun 的做法完全不同——它把数据重新编码成 **SoA (Structure of Arrays) + 字符串池** 的二进制格式：

```
string_pool:       ["react\0", "18.2.0\0", "https://...\0", "sha512-...\0", ...]
                    ^0          ^7           ^14               ^52

tarball_offsets[]:  [14,   60,  99,  ...]    ← 每个包的 tarball 在 pool 中的偏移
version_major[]:   [18,   4,   2,   ...]    ← 纯标量，连续存储
unpack_size[]:     [1024, 512, 256, ...]
dep_list[]:        [1, 2, 3, 4, ...]         ← 依赖用整数 ID
```

访问 `tarball`：`string_pool[tarball_offsets[i]]` — **0 次指针追踪**，2 次数组索引。

### 测试设计

三方对比，所有方存储**相同的逻辑数据**，访问**相同的字段**，区别仅在于内存组织方式：

| 布局 | 模拟什么 | 每个包的存储方式 |
|------|---------|----------------|
| **JSON 对象树** | npm `JSON.parse()` | ~15 个独立 malloc 的小对象，指针互相引用 |
| **AoS** | 朴素的 flat 优化 | 一个 464 字节的大 struct，存在 vector 里 |
| **SoA + 字符串池** | Bun 的实际做法 | 每个字段一个数组 + 去重的字符串池 + CSR 格式依赖图 |

每轮测试前用 64MB 垃圾数据填满缓存，模拟真实工作负载。

**源码**: [pointer_chasing.cpp](pointer_chasing.cpp)

### 结果

```
=== JSON Trees vs AoS vs SoA+StringPool ===
  Three-way comparison: npm (JSON.parse) vs naive flat vs Bun (SoA)
  Scenario: 1000 packages x 5 deps = 5000 lookups

  Memory footprint:
    JSON trees:     ~15,000 scattered heap objects
    AoS:             453.1 KB  (1 vector, 464 bytes/pkg)
    SoA+StringPool:  127.9 KB  (81000 bytes string pool + parallel arrays)

  JSON trees (npm)            cold:    19.3 us (  4 ns/op)   warm:    5.1 us ( 1 ns/op)
  AoS flat struct             cold:    10.6 us (  2 ns/op)   warm:    2.2 us ( 0 ns/op)
  SoA+StringPool (Bun)        cold:     6.7 us (  1 ns/op)   warm:    1.7 us ( 0 ns/op)

  Speedup vs JSON trees:
    AoS:             1.8x (cold)  2.3x (warm)
    SoA+StringPool:  2.9x (cold)  3.0x (warm)
    SoA vs AoS:      1.6x (cold)  1.3x (warm)
```

**结论**：

Bun 的优化实际上是**两层叠加**，每层各有贡献：

| 优化层 | 做了什么 | 加速比 (cold) |
|--------|---------|-------------|
| JSON → AoS | 消除指针追踪，数据连续存储 | **1.8x** |
| AoS → SoA+StringPool | 字符串去重（453KB→128KB）+ 字段分离存储 | **1.6x** |
| JSON → SoA (合计) | Bun 的完整优化 | **2.9x** |

访问模式对比：
```
pkg.dist.tarball[0]:
  JSON:  pkg->dist->tarball->data[0]           3 次指针追踪
  AoS:   aos[i].tarball_url[0]                  1 次数组索引
  SoA:   string_pool[tarball_offsets[i]]        2 次数组索引，但数据更紧凑

dep.unpack_size:
  JSON:  pkg->resolved_deps[d]->dist->unpack_size   3 次指针追踪
  AoS:   aos[aos[i].dep_ids[d]].unpack_size          2 次数组索引
  SoA:   unpack_size[dep_list[dep_offsets[i]+d]]     3 次数组索引，但全在连续内存
```

---

## Benchmark 4: 真实依赖树遍历

### 问题：前面都是人造数据，真实依赖图跑起来怎样？

前面的 benchmark 用的都是人造的数据结构（随机链、随机跳转）。真实的 npm 依赖图是一棵**宽而浅的树**——根节点（你的项目）直接依赖几十个包，每个包又依赖几个包，深度通常不超过 10 层。这跟 100K 步的长链完全不同。

### 数据来源

直接从本地 `kimi-cli/web/package-lock.json` 提取真实依赖图，用 Python 脚本解析出所有包和依赖关系，生成 C++ 头文件 `real_deps.h`：

```
root (depth 0)  ← 1 个包
├── 55 direct deps (depth 1)
├── 166 packages (depth 2)
├── 242 packages (depth 3)  ← 最宽层
├── 215 packages (depth 4)
├── 66 packages (depth 5)
└── ... (depth 6-9, 共 44 个)

总计: 789 个包, 1730 条依赖边, 最深 9 层
```

### 测试设计

对同一个依赖图，用两种数据结构存储，分别执行 BFS（广度优先）和 DFS（深度优先）全量依赖解析：

- **Flat**：包数据存在 `vector<PackageData>` 中，依赖关系用 CSR 格式的两个整数数组 (`dep_offsets` + `dep_list`) 表示，visited 用 `vector<bool>` 按 ID 索引。全部数据 28.3KB，放入 L1 cache。
- **HashMap**：包数据存在 `unordered_map<string, HashPackage>` 中，依赖是 `vector<string>` 存包名，visited 也是 `unordered_map<string, bool>`。每次查找、判重都要做字符串 hash 和比较。

**源码**: [real_tree.cpp](real_tree.cpp)，依赖图数据: [real_deps.h](real_deps.h)

### 结果

```
=== Real Dependency Tree Benchmark ===
  Source: kimi-cli/web/package-lock.json
  Packages: 789    Edges: 1730    Max depth: 9
  Rounds: 500

  Flat graph memory:  28.3 KB (packages + offsets + dep_list)
  HashMap memory:     scattered (string keys + vectors + hash buckets)

  BFS (full resolve)            Flat:     2.7 us (   3 ns/pkg)   HashMap:    49.6 us (   63 ns/pkg)   speedup: 18.3x
  DFS (full resolve)            Flat:     2.6 us (   3 ns/pkg)   HashMap:    39.6 us (   50 ns/pkg)   speedup: 15.3x
  Sequential scan (all pkgs)    Flat:     0.2 us (   0 ns/pkg)   HashMap:     0.6 us (    1 ns/pkg)   speedup: 3.7x
```

**结论**：

真实依赖树上 flat array 比 HashMap 快 **15~18 倍**，是四个 benchmark 中差距最大的。原因：

- 树遍历的核心操作是 **visited 判断**和**边遍历**，flat 中这两个操作极其廉价：`visited[id]`（数组索引 O(1)）和 `dep_list[j]`（连续数组顺序读取）
- HashMap 中每次 visited 判断（`visited.count(string)`）和每次边查找（`map.find(string)`）都要做字符串 hash + 比较，一次 BFS 789 个包要执行上千次字符串操作
- Flat 全部数据 28.3KB，完全热在 L1 cache 里，几乎零 cache miss

---

## 总结

| Benchmark | 对比 | 加速比 | 测了什么 |
|-----------|------|--------|---------|
| AoS vs SoA 单字段 | SoA vs AoS | **12.3x** | struct 越大、访问字段越少，SoA 优势越明显 |
| HashMap vs Flat 顺序 | Flat vs HashMap | **11.5x** | 连续内存让 prefetcher 完美预取 |
| HashMap vs Flat 随机 | Flat vs HashMap | **27.4x** | 整数 ID O(1) vs 字符串 hash+比较 |
| JSON→AoS→SoA 三方 | SoA vs JSON | **2.9x** | Bun 完整优化链的端到端效果 |
| 真实依赖树 BFS | Flat vs HashMap | **18.3x** | 789 包真实依赖图遍历 |
| 真实依赖树 DFS | Flat vs HashMap | **15.3x** | 同上，DFS 策略 |

Bun 的 cache-friendly 优化可以分解为三层，每层独立贡献：

1. **消除指针追踪**（JSON 对象树 → 连续存储）：~1.8x — 不再跳 2-3 次指针
2. **SoA + 字符串池**（AoS → SoA）：~1.6x — 字段分离存储 + 字符串去重，内存压缩 3.5 倍
3. **整数 ID 替代字符串 key**（HashMap → flat array）：3~27x — 消除 hash 计算和字符串比较

这三层叠加在一起，在真实依赖树遍历场景下达到 **15~18 倍**的端到端加速。

## 运行

```bash
make        # 编译全部
./aos_vs_soa
./hashmap_vs_flat
./pointer_chasing
./real_tree
```

环境：macOS (Apple Silicon), g++ -O2 -std=c++17
