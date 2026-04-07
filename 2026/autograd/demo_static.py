"""
静态图 autograd 演示。
先定义图，再喂数据执行。对比 demo_simple.py 的动态图。
"""


# === 第一步：定义节点（不算，只描述结构）===

class Node:
    def __init__(self, name, op=None, inputs=None):
        self.name = name
        self.op = op              # 'mul', 'add', 'square', None(叶子)
        self.inputs = inputs or []
        self.is_leaf = op is None

    def __repr__(self):
        return self.name


class Graph:
    """静态计算图：先定义结构，再执行"""

    def __init__(self):
        self.nodes = []

    def variable(self, name):
        """创建叶子节点（参数/输入）"""
        n = Node(name)
        self.nodes.append(n)
        return n

    def mul(self, a, b, name):
        n = Node(name, op='mul', inputs=[a, b])
        self.nodes.append(n)
        return n

    def add(self, a, b, name):
        n = Node(name, op='add', inputs=[a, b])
        self.nodes.append(n)
        return n

    def square(self, a, name):
        n = Node(name, op='square', inputs=[a])
        self.nodes.append(n)
        return n

    def add_double(self, a, name):
        """2a — 用于 else 分支"""
        n = Node(name, op='add_double', inputs=[a])
        self.nodes.append(n)
        return n

    def cond(self, cond_node, threshold, true_node, false_node, name):
        """
        if cond_node > threshold: 用 true_node
        else: 用 false_node

        鬼畜点：两条分支都要算，然后选一个。
        因为图是静态的，不能"不走"某条路。
        """
        n = Node(name, op='cond', inputs=[cond_node, true_node, false_node])
        n.threshold = threshold
        self.nodes.append(n)
        return n

    def forward(self, feed):
        """前向：按定义顺序算一遍"""
        vals = {}
        for n in self.nodes:
            if n.is_leaf:
                vals[n.name] = feed[n.name]
            elif n.op == 'mul':
                vals[n.name] = vals[n.inputs[0].name] * vals[n.inputs[1].name]
            elif n.op == 'add':
                vals[n.name] = vals[n.inputs[0].name] + vals[n.inputs[1].name]
            elif n.op == 'square':
                vals[n.name] = vals[n.inputs[0].name] ** 2
            elif n.op == 'add_double':
                vals[n.name] = vals[n.inputs[0].name] * 2
            elif n.op == 'cond':
                cond_val = vals[n.inputs[0].name]
                true_val = vals[n.inputs[1].name]
                false_val = vals[n.inputs[2].name]
                # 两条路都算了，只是选一个
                n._chose_true = cond_val > n.threshold
                vals[n.name] = true_val if n._chose_true else false_val
        return vals

    def backward(self, vals):
        """反向：按定义逆序算梯度"""
        grads = {n.name: 0.0 for n in self.nodes}
        grads[self.nodes[-1].name] = 1.0  # dloss/dloss = 1

        for n in reversed(self.nodes):
            if n.op == 'square':
                a = n.inputs[0]
                grads[a.name] += grads[n.name] * 2 * vals[a.name]
            elif n.op == 'add':
                a, b = n.inputs
                grads[a.name] += grads[n.name]
                grads[b.name] += grads[n.name]
            elif n.op == 'mul':
                a, b = n.inputs
                grads[a.name] += grads[n.name] * vals[b.name]
                grads[b.name] += grads[n.name] * vals[a.name]
            elif n.op == 'add_double':
                a = n.inputs[0]
                grads[a.name] += grads[n.name] * 2
            elif n.op == 'cond':
                # 梯度只流向被选中的分支
                if n._chose_true:
                    grads[n.inputs[1].name] += grads[n.name]
                else:
                    grads[n.inputs[2].name] += grads[n.name]
        return grads


# === 第二步：定义图（只做一次）===

g = Graph()
w = g.variable('w')
x = g.variable('x')
b = g.variable('b')
y = g.mul(w, x, 'y')
z = g.add(y, b, 'z')
loss = g.square(z, 'loss')

print("=== 图结构（定义一次，反复用）===")
for n in g.nodes:
    if n.is_leaf:
        print(f"  {n.name}: 输入/参数 (LEAF)")
    else:
        inputs = ', '.join(i.name for i in n.inputs)
        print(f"  {n.name} = {n.op}({inputs})")

# === 第三步：喂数据执行（可以反复执行）===

print("\n=== 第 1 轮 ===")
feed = {'w': 3.0, 'x': 2.0, 'b': 1.0}
vals = g.forward(feed)
grads = g.backward(vals)

for name in ['w', 'x', 'b', 'y', 'z', 'loss']:
    node = [n for n in g.nodes if n.name == name][0]
    leaf = " (LEAF)" if node.is_leaf else ""
    print(f"  {name:4s} = {vals[name]:8.4f}  grad = {grads[name]:8.4f}{leaf}")

# 只更新叶子
lr = 0.01
print("\n  更新叶子:")
for name in ['w', 'x', 'b']:
    feed[name] -= lr * grads[name]
    print(f"    {name}: {feed[name]:.4f}")

print("\n=== 第 2 轮（同一个图，新数据）===")
vals = g.forward(feed)
grads = g.backward(vals)

for name in ['w', 'x', 'b', 'y', 'z', 'loss']:
    node = [n for n in g.nodes if n.name == name][0]
    leaf = " (LEAF)" if node.is_leaf else ""
    print(f"  {name:4s} = {vals[name]:8.4f}  grad = {grads[name]:8.4f}{leaf}")


# === if 分支演示 ===
# 静态图：要专门实现 cond 节点，两条分支都要定义，都要算
print("\n" + "="*50)
print("=== if 分支演示（静态图）===")
print("="*50)

g2 = Graph()
w2 = g2.variable('w')
x2 = g2.variable('x')
b2 = g2.variable('b')
y2 = g2.mul(w2, x2, 'y')
z2 = g2.add(y2, b2, 'z')

# 要把两条分支都定义出来
loss_big = g2.square(z2, 'loss_big')       # 分支1: z²
loss_small = g2.add_double(z2, 'loss_small')  # 分支2: 2z

# 然后用一个专门的 cond 节点来选
loss2 = g2.cond(z2, 5.0, loss_big, loss_small, 'loss')

print("\n图结构（注意两条分支都定义了）:")
for n in g2.nodes:
    if n.is_leaf:
        print(f"  {n.name}: 输入/参数 (LEAF)")
    elif n.op == 'cond':
        inputs = ', '.join(i.name for i in n.inputs)
        print(f"  {n.name} = cond({inputs}, threshold={n.threshold})")
    else:
        inputs = ', '.join(i.name for i in n.inputs)
        print(f"  {n.name} = {n.op}({inputs})")

for i, (wv, xv, bv) in enumerate([(3.0, 2.0, 1.0), (0.5, 0.3, 0.1)]):
    feed2 = {'w': wv, 'x': xv, 'b': bv}
    vals2 = g2.forward(feed2)
    grads2 = g2.backward(vals2)

    chose = "loss_big (z²)" if g2.nodes[-1]._chose_true else "loss_small (2z)"
    print(f"\n第{i+1}轮: w={wv}, x={xv}, b={bv}")
    print(f"  z = {vals2['z']:.4f} → 走分支: {chose}")
    print(f"  loss_big = {vals2['loss_big']:.4f} (算了！)")
    print(f"  loss_small = {vals2['loss_small']:.4f} (也算了！)")
    print(f"  loss = {vals2['loss']:.4f} (选了一个)")
    print(f"  w.grad = {grads2['w']:.4f}, x.grad = {grads2['x']:.4f}, b.grad = {grads2['b']:.4f}")
