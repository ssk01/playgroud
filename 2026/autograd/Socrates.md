### Q: 动态图 vs 静态图核心区别是什么？

一等公民不同：
- **静态图**：图是一等公民，Node 只描述结构不持有数据，数据从外面 feed 进来沿着图流动
- **动态图**：Val 是一等公民，数据和梯度绑在 Val 上，图只是 Val 之间运算的副产品，用完即弃

静态图快（不用重建），但不灵活（想加个 if/else 分支要重新定义图）。动态图慢一点，但写代码跟普通 Python 一样自然。

(2026-04-07 00:05)

### Q: demo_simple 有点作弊，我们写 PyTorch 的时候也是这么干的吗？

不是作弊，PyTorch 就是这个套路。`Val` ≈ `torch.Tensor`，`Val._backward` ≈ `Tensor.grad_fn`，`Val._children` ≈ `Tensor.grad_fn.next_functions`，`Val.backward()` 里的拓扑排序 ≈ `torch.autograd.backward()`。区别只是 PyTorch 支持了几百种 op、多维张量、GPU，但骨架一模一样。

(2026-04-07 00:15)

### Q: 动态图里面有啥复杂的 op？语法上的。

Python 有多少种控制流，动态图就免费支持多少种。静态图每遇到一种，就得发明对应的 op：

- **变长循环**：RNN 处理不等长序列，动态图普通 `for`，静态图要 `tf.while_loop`
- **递归**：TreeLSTM 在语法树上递归，每棵树结构不同，静态图基本没法写
- **提前退出**：自适应推理，`confidence > 0.99` 就 `break`
- **动态选路**：Mixture of Experts，每个样本选不同的专家

这也是为什么 TF 2.0 投降了，默认改成 eager mode（动态图）。

(2026-04-07 00:18)

### Q: TF 在原来基础上支持动态图是怎么实现的？不至于维护两个版本吧？

没有维护两套。底层 C++ op kernel 本来就是同一套，区别只在上层调度方式：

- **TF 1.x**：Python 定义图 → Graph 数据结构 → Session.run() → C++ kernel 执行
- **Eager mode**：Python 直接调 → C++ kernel 立刻执行，立刻返回

然后用 `tf.function` 做桥梁：加装饰器时 trace 一遍 Python 代码，把碰到的 tf op 录成静态图缓存。后续同样 signature 直接跑图不再执行 Python。

三层架构：C++ op kernel（共享）→ eager mode（默认）→ tf.function（可选，trace → 静态图）。

关键限制：`tf.function` 里 Python `if` 仍然有问题 — trace 只走一遍，走了哪条路就录哪条。想要真分支还是得 `tf.cond`。**静态图的 if 问题并没有真正解决，只是让你大部分时候不用碰它了。**

(2026-04-07 00:22)

<!-- 以下继续记录 -->
