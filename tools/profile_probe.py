# 沿一条水平线取亮度剖面 —— 用来把"模糊斜坡"从图里量出来（平台/梯形 vs 高斯 S 形）。
# 用法: python tools/profile_probe.py <png> <y> <x0> <x1> [step]
import sys
from PIL import Image

path = sys.argv[1]
y = int(sys.argv[2])
x0 = int(sys.argv[3])
x1 = int(sys.argv[4])
step = int(sys.argv[5]) if len(sys.argv) > 5 else 4

im = Image.open(path).convert("RGB")
row = [im.getpixel((x, y))[0] for x in range(x0, x1)]
print("x  " + " ".join("%4d" % x for x in range(x0, x1, step)))
print("L  " + " ".join("%4d" % row[i] for i in range(0, len(row), step)))
# 一阶差分：梯形的折点在图上是"差分突变"，高斯是平滑的单峰
d = [row[i + 1] - row[i] for i in range(len(row) - 1)]
print("dL " + " ".join("%4d" % d[i] for i in range(0, len(d), step)))
