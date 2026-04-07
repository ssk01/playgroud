# 动态图 vs 静态图：Autograd 演示

两个玩具级 autograd 实现，演示动态图和静态图的核心区别。

## 核心区别

一等公民不同：

- **静态图** (`demo_static.py`)：图是一等公民，Node 只描述结构不持有数据，数据从外面 feed 进来沿着图流动
- **动态图** (`demo_simple.py`)：Val 是一等公民，数据和梯度绑在 Val 上，图只是 Val 之间运算的副产品，用完即弃

```
动态图:
  每轮: y = w * x  →  建图  →  backward  →  图扔掉
  每轮: y = w * x  →  建新图  →  backward  →  图扔掉
         ↑ 图每次重建

静态图:
  定义:  g.mul(w, x)  →  图建好了
  第1轮: g.forward(feed)  →  g.backward(vals)  →  图还在
  第2轮: g.forward(feed)  →  g.backward(vals)  →  图还在
         ↑ 图只建一次，反复执行
```

## 对比

| | 动态图 (PyTorch) | 静态图 (TensorFlow 1.x) |
|--------|-------------------------------|-----------------------------------------------|
| 建图 | 每次前向都建新图 | 一次定义，反复执行 |
| 写代码 | 普通 Python，if/for 随便用 | 专用 API，tf.cond、tf.while_loop |
| debug | print(x) 直接看值 | 值在 session.run 时才有，print 看到的是节点描述 |
| 速度 | 建图有开销，略慢 | 不重建，略快 |
| 部署 | 要带 Python 运行时 | 导出图就行，不依赖 Python |
| 灵活性 | 每个样本可以走不同的计算路径 | 路径固定，改结构要重新定义 |

## if 分支：差异最明显的地方

动态图 — 普通 Python if，三行搞定：

```python
if z.data > 5:
    loss = z.square()
else:
    loss = z + z
```

静态图 — 要做四件事：
1. 新增 `add_double` op（图要认识所有操作）
2. 新增 `cond` op（图要知道怎么选分支）
3. **两条分支都要算**，然后才选一个
4. forward/backward 都要加对应的处理逻辑

而且 demo_simple 不是作弊，PyTorch 就是这么干的：

```python
# PyTorch 真实写法
y = w * x
z = y + b

if z.item() > 5:
    loss = z ** 2
else:
    loss = 2 * z

loss.backward()
```

`Val` ≈ `torch.Tensor`，`Val._backward` ≈ `Tensor.grad_fn`，骨架一模一样。

## 动态图免费支持的复杂控制流

Python 有多少种控制流，动态图就免费支持多少种。静态图每遇到一种，就得发明一个对应的 op。

**变长循环** — RNN 处理不等长序列：
```python
# 动态图：普通 for，每条数据循环次数不同
for token in sequence:          # 长度运行时才知道
    h = rnn_cell(h, token)
# 静态图要 tf.while_loop(cond, body, loop_vars)，提前声明循环变量的 shape
```

**递归** — TreeLSTM 在语法树上递归：
```python
# 动态图：普通递归函数
def tree_forward(node):
    if node.is_leaf:
        return embed(node.word)
    children = [tree_forward(c) for c in node.children]  # 每棵树结构不同
    return merge(children)
# 静态图里……基本没法写。树结构每个样本都不一样，图定义不出来
```

**提前退出** — 自适应推理：
```python
for layer in layers:
    x = layer(x)
    if confidence(x) > 0.99:   # 运行时决定算几层
        break
```

**动态选路** — Mixture of Experts：
```python
scores = gate(x)
top_k = scores.topk(2).indices   # 每个样本选不同的专家
output = sum(experts[i](x) for i in top_k)
```

## 为什么 PyTorch 赢了

对研究者来说，灵活性和 debug 便利性远比那点速度差重要。写一个新 idea 试一下，动态图 10 分钟改完跑通，静态图可能要折腾半天处理 `tf.cond` 和 shape 问题。

TF 2.0 最终投降，默认改成 eager mode（动态图）。

## TF 2.0 怎么加的动态图：没有维护两套

底层 C++ op kernel 本来就是同一套（矩阵乘、卷积、relu……），区别只在上层怎么调度：

```
TF 1.x：Python 定义图  →  Graph 数据结构  →  Session.run()  →  C++ kernel 执行
Eager： Python 直接调  →  C++ kernel 立刻执行，立刻返回
```

`tf.function` 做桥梁 — 开发时去掉装饰器用 eager 随便 debug，上线时加上装饰器自动变静态图拿性能：

```python
@tf.function          # 加这个装饰器
def train_step(x):
    y = model(x)      # 第一次调用时 trace 一遍 Python，录成静态图
    loss = loss_fn(y)  # 之后复用这张图，不再走 Python
    return loss
```

三层架构：
1. **C++ op kernel** — 共享，始终同一套
2. **Eager mode** — 默认，Python 直接调 kernel
3. **tf.function** — 可选，trace Python → 静态图，拿回性能

关键限制：`tf.function` 里 Python `if` 仍然有问题 — trace 只走一遍，走了哪条路就录哪条路，另一条丢了。想要真分支还是得用 `tf.cond`。**静态图的 if 问题并没有真正解决，只是让你大部分时候不用碰它了。**

## 为什么静态图没有死

部署场景需要它 — 手机、嵌入式设备不想带 Python 运行时。所以 PyTorch 后来加了 `torch.jit` 和 `torch.compile`，训练时用动态图，部署时转成静态图。两个好处都要。

## 运行输出

### demo_simple.py（动态图）

```
=== 前向 ===
w=3.0, x=2.0, b=1.0
y = w*x = 6.0
z = y+b = 7.0
loss = z² = 49.0

=== 反向 ===
Val(3.0000, grad=28.0000, op= (LEAF))
Val(2.0000, grad=42.0000, op= (LEAF))
Val(1.0000, grad=14.0000, op= (LEAF))
Val(6.0000, grad=14.0000, op=*)
Val(7.0000, grad=14.0000, op=+)
Val(49.0000, grad=1.0000, op=²)

=== 只更新叶子 ===
  w: 更新 ✅  新值=2.7200
  x: 更新 ✅  新值=1.5800
  b: 更新 ✅  新值=0.8600
  y: 跳过 ❌  (中间节点)
  z: 跳过 ❌  (中间节点)

==================================================
=== if 分支演示（动态图）===
==================================================

第1轮: w=3.0, x=2.0, b=1.0
  z = 7.0000 → 走分支: z² (big)
  loss = 49.0000
  w.grad = 28.0000, x.grad = 42.0000, b.grad = 14.0000

第2轮: w=0.5, x=0.3, b=0.1
  z = 0.2500 → 走分支: 2z (small)
  loss = 0.5000
  w.grad = 0.6000, x.grad = 1.0000, b.grad = 2.0000
```

### demo_static.py（静态图）

```
=== 图结构（定义一次，反复用）===
  w: 输入/参数 (LEAF)
  x: 输入/参数 (LEAF)
  b: 输入/参数 (LEAF)
  y = mul(w, x)
  z = add(y, b)
  loss = square(z)

=== 第 1 轮 ===
  w    =   3.0000  grad =  28.0000 (LEAF)
  x    =   2.0000  grad =  42.0000 (LEAF)
  b    =   1.0000  grad =  14.0000 (LEAF)
  y    =   6.0000  grad =  14.0000
  z    =   7.0000  grad =  14.0000
  loss =  49.0000  grad =   1.0000

  更新叶子:
    w: 2.7200
    x: 1.5800
    b: 0.8600

=== 第 2 轮（同一个图，新数据）===
  w    =   2.7200  grad =  16.2980 (LEAF)
  x    =   1.5800  grad =  28.0573 (LEAF)
  b    =   0.8600  grad =  10.3152 (LEAF)
  y    =   4.2976  grad =  10.3152
  z    =   5.1576  grad =  10.3152
  loss =  26.6008  grad =   1.0000

==================================================
=== if 分支演示（静态图）===
==================================================

图结构（注意两条分支都定义了）:
  w: 输入/参数 (LEAF)
  x: 输入/参数 (LEAF)
  b: 输入/参数 (LEAF)
  y = mul(w, x)
  z = add(y, b)
  loss_big = square(z)
  loss_small = add_double(z)
  loss = cond(z, loss_big, loss_small, threshold=5.0)

第1轮: w=3.0, x=2.0, b=1.0
  z = 7.0000 → 走分支: loss_big (z²)
  loss_big = 49.0000 (算了！)
  loss_small = 14.0000 (也算了！)
  loss = 49.0000 (选了一个)
  w.grad = 28.0000, x.grad = 42.0000, b.grad = 14.0000

第2轮: w=0.5, x=0.3, b=0.1
  z = 0.2500 → 走分支: loss_small (2z)
  loss_big = 0.0625 (算了！)
  loss_small = 0.5000 (也算了！)
  loss = 0.5000 (选了一个)
  w.grad = 0.6000, x.grad = 1.0000, b.grad = 2.0000
```
