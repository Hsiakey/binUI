# 像素回归基准图

`docs/pixels/*.png` 是 `docs/issues/assets/` 下四份自检布局的**基准渲染结果**（GDI 离屏）。

## 怎么用

```sh
mingw32-make SHELL=sh pixels          # 渲染 + 与基准图逐像素比对
mingw32-make SHELL=sh pixels-update   # 确认改动是"要的"之后，重新生成基准图
```

`pixels` 会打印每个用例的差异率、最大通道差和差异包围盒，并把差异图写到
`build-mingw/pixels/<case>.diff.png`（红色 = 不同像素）。默认容差 0.5%，
单通道阈值 8（容忍 1px 抗锯齿抖动）。可用 `PIXEL_TOL=2.0` 放宽。

## 为什么不在 CI 里

CI 的 Windows runner 是软件光栅化，抗锯齿边缘与真实 GPU 不一致；同一份代码在不同
机器上会稳定地差出百分之几，塞进 CI 只会变成 flaky 红灯。基准图**只在本机有意义**：
它的用途是"我改完绘制/布局，肉眼看不出的地方有没有被带歪"。

真正要进 CI 的回归，靠 `tests/test_paint.cpp`（纯几何/数学断言）和
`check_examples`（编译期警告）—— 那些是确定性的。
