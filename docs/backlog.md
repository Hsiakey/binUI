# binUI 待做队列（2026-11-21 建）

> 这份是**执行队列**，不是问题清单：`docs/issues/` 记"为什么"，这里记"下一步干什么、怎么验收、谁能并行"。
> 每条都带来源条目与验收口径；完成一条就在表里划掉并把 commit 记在右边的"结果"列。
> 当前版本基线：binGPU 0.16.0 · bin2D 0.22.9 · binUI 0.38.5。

## 优先级与轨道

三条轨道文件互不重叠，可以同时开工：

| 轨道 | 主题 | 主要落地位置 | 为什么现在做 |
|---|---|---|---|
| **A** | DX：写完能看（[14](issues/14-browser-preview-dx.md)） | `tools/`、`docs/`、`out/preview/` | P0 原型已跑通，收尾成本最低；P1 回答"布局对不对"，比"像不像"更有用 |
| **B** | 语言：合法 HTML（[15](issues/15-valid-html-language.md)） | `src/`（编译器）、`examples/`、`components/` | master 定的长期方向；语法迁移无拦路虎，工程量在预览运行时与语义对齐 |
| **C** | 渲染欠账：GPU 侧还剩什么 | binGPU（`bingpu_gl/vk/d3d11`）、bin2D（`gpu_exec`） | 不阻塞 A/B，但每一条都带确定的验收数字 |

## A 轨道 —— 预览与结构（[14](issues/14-browser-preview-dx.md)）

| ID | 事项 | 来源 | 验收 | 估 | 结果 |
|---|---|---|---|---|---|
| A1 | 镜像生成器收成正式工具：`binui_preview`（输出 `out/preview/`、`--stage` 单组件视图、批量跑 `examples/*`） | 14 §5 轮1 | `components/preview.html` + 3 个示例页各出一组"镜像/真渲染"对照，肉眼可读；工具在 `tools/` 内自洽（`python tools/binui_preview.py --help`） | 0.5–1 天 | |
| A2 | **P1 结构导出 + 布局叠层 viewer**：`binui_compile --dump-nodes` 出 JSON（id/父/class/盒/裁剪/display:none/滚动内容高），单页 viewer 左树右图叠加盒框 | 14 §3 P1 | 能回答"这个盒子为什么不在那"：点树高亮盒、悬停给计算值；JSON 与引擎实际布局同源（不是猜的） | 2–3 天 | |
| A3 | 热重载：改源码 → 浏览器自动刷新（SSE） | 14 §5 轮3 | 保存后 2 秒内刷新，A1/A2 都受益 | 0.5–1 天 | |
| A4 | P2 wasm 真渲染（编译+布局+CPU 光栅进 wasm，canvas 出图） | 14 §3 P2 | 同机同字体下浏览器画面与 `bin2d_shot` 一致 | 2–4 周 | 后置到 A1–A3 之后 |

## B 轨道 —— 合法 HTML（[15](issues/15-valid-html-language.md)）

| ID | 事项 | 来源 | 验收 | 估 | 结果 |
|---|---|---|---|---|---|
| B1 | **M0 语法定稿 + 双跑样例**：`button` 组件用新语法重写（`var()` / `data-component` / `data-param` / `px`），同一份文件浏览器能看、`binui_compile` 能编 | 15 §6 M0 | 同一份 `button.html` 两处都成立；差异清单固化成 §5 表 | 2–3 天 | |
| B2 | **M1 编译器接线**：`var(--name, fallback)` 解析、`data-component` 内联（复用 `<use>` 管线）、`data-param` 文本覆盖、窗口 `data-*`、legacy 告警（`--strict-html` 拒收） | 15 §3 | 新语法样例编出的 `.bui` 与旧语法**像素完全一致**（`pixels-bin2d` 0 容差） | 3–5 天 | |
| B3 | M2 `binui dev`（静态服务 + 监听 + SSE）+ `binui-preview.js` + 组件独立视图（居中 / 1x~8x / 盒边界 / 像素放大） | 15 §4 | 改一行保存 → 浏览器 2 秒内刷新且布局变化可见；单组件 4× 能看清 1px 描边 | 1–2 周 | 依赖 B2 |
| B4 | M3 codemod：`{{}}`→`var()`/`data-param`、`<use>`→`data-component`、补 `px` | 15 §6 M3 | 存量 `examples/` + `components/` 全量迁移，逐页过 `pixels-bin2d` 像素不变 | 3–5 天 | 依赖 B2 绿 |
| B5 | M4/M5 语义对齐（§5 表逐条）+ `binui parity` 门禁 + 删 legacy 语法 | 15 §5/§6 | 示例页浏览器/binUI 差异收敛到"文字换行以内"；编译器只剩一套写法 | 1–2 月 | 持续 |

## C 轨道 —— 渲染欠账（[13](issues/13-gpu-migration-candidates.md) + 各仓 CHANGELOG 欠账）

| ID | 事项 | 来源 | 验收 | 估 | 结果 |
|---|---|---|---|---|---|
| C1 | **GL 窗口路径 4× MSAA still broken**（`circle` 94.6% 差异 / `shadow` 95.9%）——找根因，找不到就在 `render_plan` 里明确 GL=1× 并写进文档 | binGPU 欠账 12 | GL 4× 下 `circle`/`shadow` 与 VK 一致；或文档里写明"GL 无 4×"并让 plan 不再选择 | 1–2 天 | |
| C2 | **per-target 采样数 + 显式 resolve**：`BgpuRenderBeginDesc` 加"这趟不 resolve"，配合显式 `bingpu_cmd_resolve_target`；省掉 `svg-filter` 里 32 次整窗 LOAD blit + 33 次整窗 resolve（约 0.94 ms） | 13 §②、binGPU 0.16.0 欠账 | `svg-filter` 帧时从 ~2.7 ms 降到 ~1.8 ms；18 页像素不变 | 2–3 天 | ABI 要 +1 |
| C3 | **32 次整窗 pass 溯源**：究竟是 bin2D 每层 re-begin/end 还是 binGPU 内部拆的；若在 bin2D 就合帧 | 13 §② | 给出 pass 数归因表（谁开的、为什么），并按归因减少数量 | 1 天 | |
| C4 | D3D11 MSAA / mip / defer-resolve 补齐 | binGPU 欠账 3、13 | D3D11 与 VK 同档；`render_plan` 里 D3D11 可给 4× | 3–5 天 | |
| C5 | **内存 ≤150 MB 决策**：Vulkan 静态下限实测 133–134 MB private（裸窗口+swapchain），1× MSAA 约省 30 MB（会废掉 4× 基线），CPU 光栅演示 38.5 MB | 9:xx 测量 | master 拍板后执行其中一条，并把数字写进 README | 决策 5 分钟 | 等拍板 |

## D 轨道 —— 等 master 拍板（不排期）

| ID | 事项 | 代价 | 状态 |
|---|---|---|---|
| D1 | [11](issues/11-shadow-rings.md) box-shadow 环带化（每帧 fillRounded 的 63.8% 像素） | 性能已实测平账、像素必变（±2 缝隙 / 页面 1.3%），基线要重生成 | 等金标准策略 |
| D2 | [13](issues/13-gpu-migration-candidates.md) ④ AA 口径统一（CPU 扫描线 vs GPU 解析 SDF） | 改基线 | 等拍板 |
| D3 | 三库聚合：CMake `binui::binui` INTERFACE 目标 / `-flto` A/B | 低；结论大概率"分层在源头、聚合在交付" | 待做，量一次 LTO |

## 并行建议（12:00 起跑）

| 时段 | 并行 | 说明 |
|---|---|---|
| 第 1 段 | A2 ∥ B1 ∥ C1 | 文件不重叠：`tools/`+`docs/` / `src/` 编译器 / binGPU-GL。三条都是"当天能出结论"的量级 |
| 第 2 段 | A3 ∥ B2 ∥ C3 | B2 是所有后续 B 项的门；C3 只读调查，不占写权限 |
| 第 3 段 | B3 ∥ C2 | C2 动 ABI，单独一个仓，别和 binUI 改动混在一个提交里 |

规矩不变：GPU > CPU、VK > GL > D3D；先量再算后改；`pixels-bin2d`（0 容差）与 `make partial`（0 差异）是硬门禁；一项做完就把 commit 填进"结果"列，不新开"下一轮"。
