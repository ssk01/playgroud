"""
最简 autograd 演示：一维标量，手写计算图。
演示"只有叶子节点存梯度"。
"""


class Val:
    def __init__(self, data, children=(), op=''):
        self.data = data
        self.grad = 0.0
        self.is_leaf = len(children) == 0  # 没有父运算 = 叶子
        self._backward = lambda: None
        self._children = children
        self.op = op

    def __repr__(self):
        leaf = " (LEAF)" if self.is_leaf else ""
        return f"Val({self.data:.4f}, grad={self.grad:.4f}, op={self.op}{leaf})"

    def __mul__(self, other):
        out = Val(self.data * other.data, (self, other), '*')
        def _backward():
            self.grad += out.grad * other.data
            other.grad += out.grad * self.data
        out._backward = _backward
        return out

    def __add__(self, other):
        out = Val(self.data + other.data, (self, other), '+')
        def _backward():
            self.grad += out.grad
            other.grad += out.grad
        out._backward = _backward
        return out

    def square(self):
        out = Val(self.data ** 2, (self,), '²')
        def _backward():
            self.grad += out.grad * 2 * self.data
        out._backward = _backward
        return out

    def backward(self):
        topo = []
        visited = set()
        def build(v):
            if id(v) not in visited:
                visited.add(id(v))
                for c in v._children:
                    build(c)
                topo.append(v)
        build(self)

        self.grad = 1.0
        for v in reversed(topo):
            v._backward()


# === 演示 ===
w = Val(3.0)    # 叶子：权重
x = Val(2.0)    # 叶子：输入
b = Val(1.0)    # 叶子：偏置

# 前向
y = w * x       # 中间节点: 6
z = y + b       # 中间节点: 7
loss = z.square()  # 输出: 49

print("=== 前向 ===")
print(f"w={w.data}, x={x.data}, b={b.data}")
print(f"y = w*x = {y.data}")
print(f"z = y+b = {z.data}")
print(f"loss = z² = {loss.data}")

# 反向
loss.backward()

print("\n=== 反向 ===")
for name, v in [('w', w), ('x', x), ('b', b), ('y', y), ('z', z), ('loss', loss)]:
    print(v)

print("\n=== 只更新叶子 ===")
lr = 0.01
for name, v in [('w', w), ('x', x), ('b', b), ('y', y), ('z', z)]:
    if v.is_leaf:
        v.data -= lr * v.grad
        print(f"  {name}: 更新 ✅  新值={v.data:.4f}")
    else:
        print(f"  {name}: 跳过 ❌  (中间节点)")


# === if 分支演示 ===
# 动态图：直接用 Python if，跟写普通代码一模一样
print("\n" + "="*50)
print("=== if 分支演示（动态图）===")
print("="*50)

for i, (wv, xv, bv) in enumerate([(3.0, 2.0, 1.0), (0.5, 0.3, 0.1)]):
    w = Val(wv)
    x = Val(xv)
    b = Val(bv)
    y = w * x
    z = y + b

    # 就是普通 Python if，完事了
    if z.data > 5:
        loss = z.square()
        branch = "z² (big)"
    else:
        loss = z + z  # 2z
        branch = "2z (small)"

    loss.backward()

    print(f"\n第{i+1}轮: w={wv}, x={xv}, b={bv}")
    print(f"  z = {z.data:.4f} → 走分支: {branch}")
    print(f"  loss = {loss.data:.4f}")
    print(f"  w.grad = {w.grad:.4f}, x.grad = {x.grad:.4f}, b.grad = {b.grad:.4f}")
