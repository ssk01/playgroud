# playgroud for Kaizen

Learning by doing. Each sub-directory is an independent experiment.

> 建这个 repo 的初衷是自己动手写每一行代码来加深理解，但说实话，里面相当一部分项目已经是 vibe coding 了——和 AI 结对完成的。时代变了，Kaizen 的方式也在变。

## 2026

| Project | Language | Description |
|---------|----------|-------------|
| [autograd](2026/autograd/) | Python | 动态图 vs 静态图 autograd 玩具实现 — 手写计算图、反向传播、if 分支对比，演示 PyTorch vs TF 1.x 的核心差异 |
| [cache-friendly-layout](2026/cache-friendly-layout/) | C++ | 验证 [Bun Install 博客](https://bun.com/blog/behind-the-scenes-of-bun-install) 中 cache-friendly 数据布局的优化效果：AoS vs SoA、HashMap vs flat array、JSON 对象树 vs SoA+字符串池、真实依赖树遍历 |
| [go](2026/go/) | Python | AlphaGo Zero 风格的围棋 AI — MCTS + 神经网络自我对弈训练 |
| [microgpt.py](2026/microgpt.py) | Python | 从零实现的微型 GPT — 纯 Python 无依赖，手写 autograd + 字符级语言模型 |
| [bili-views](2026/bili-views/) | TypeScript | B站实时在线观看人数统计系统 — Express + Redis + WebSocket |

## 2025

| Project | Language | Description |
|---------|----------|-------------|
| [branchless_sort.cpp](2025/branchless_sort.cpp) | C++ | Branchless quicksort — 用 conditional move 消除分支预测失败，对比 std::sort 实现 2.5x 加速 |
| [sort.ipynb](2025/sort.ipynb) | Python | 分析 branchless sorting 为什么快：分支预测代价量化，验证 Jeff Dean 的性能估算模型 |
| [leet_168](2025/leet_168_excel-sheel-column-title.ipynb) | Python | LeetCode 168: Excel 列号转换 (整数 → "ZY") |
| [leet_365](2025/leet_365_water-and-jug-problem.ipynb) | Python | LeetCode 365: 水壶问题 (BFS 搜索) |
