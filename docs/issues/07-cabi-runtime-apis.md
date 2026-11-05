> **状态：已修复：新增 binui_scroll_to / scroll_by / content_height / set_node_bounds / add_node / remove_node；Python/Go/Rust 绑定同步补齐。**

# C ABI 缺运行期能力：`scrollTo` / `setNodeBounds` / `addNode`，Go 绑定还漏了 `set_image`

> 建议标签：`enhancement` · 面积：C ABI / 绑定
> 版本：binUI 0.2.0，`BINUI_VERSION=14`，commit `88931ee`
> 用途说明：我用 Go + C ABI 写一个聊天式 GUI（`docs/issues/assets/gui-dark-chat.png`），
> 下面这几项是**按需求排的**，不是"能不能加"的问题。

## 场景

C ABI 的设计是"布局编译期定死，运行期只改内容"（`set_text` / `set_visible` /
`set_checked` / `set_image`），这对静态面板完全够用。但一写成**列表/聊天流**，
三件事就绕不过去：

1. **追加一条消息后要滚到底**。现在没有 `scrollTo`，只能让内容总高不超过视口
   （等于放弃滚动）或者让用户自己滚。
2. **消息高度不固定**。`set_text` 不会重算高度（见
   [06-settext-no-reflow.md](06-settext-no-reflow.md)），宿主的补救办法是
   自己算高度 + 挪后续节点 —— 这需要一个 `setNodeBounds`。
3. **列表项数量运行时才知道**。现在必须在布局里预留 N 个槽位（我是留 8 行智能体 +
   1 条"待发送"气泡），条数一变就得重新编译布局。

## 现状对照

核心渲染器里其实**都有**（`include/binui_renderer.h`）：

```cpp
146: uint16_t addNode(uint16_t parentId, uint8_t type, float x, float y, float w, float h, …);
150: void removeNode(uint16_t nodeId);     // 隐藏（不可见），节点保留供复用
157: void setSize(int width, int height);
197: void scrollTo(uint16_t nodeId, float x, float y);
198: void scrollBy(uint16_t nodeId, float dx, float dy);
201: float contentHeight(uint16_t nodeId) const;
342: void relayout(int width, int height);
```

但 `include/binui_c.h` 一个都没导出来。C ABI 现有 34 个函数里，节点写操作只有
`set_text` / `set_visible` / `set_checked` / `invalidate_*` / `set_image*`。

同样，`bindings/go/binui/binui.go` 也没封装 `binui_set_image_raw` /
`binui_set_image` / `binui_mount_components` —— 这三个 C ABI 里**有**，Go 里没有。
（想要正圆头像只能靠 `set_image_raw` 传位图，现在 Go 侧得自己写 cgo。）

## 建议补的 C ABI（按优先级）

```c
/* 1. 滚动：已有实现，纯导出 */
BINUI_C_API void BINUI_C_CALL binui_scroll_to(void* app, int32_t node_id, float x, float y);
BINUI_C_API void BINUI_C_CALL binui_scroll_by(void* app, int32_t node_id, float dx, float dy);
BINUI_C_API float BINUI_C_CALL binui_content_height(void* app, int32_t node_id);  /* 配合"贴底" */

/* 2. 节点几何：宿主自己排版/撑高用（配合 issue 06） */
BINUI_C_API int32_t BINUI_C_CALL binui_set_node_bounds(void* app, int32_t node_id,
                                                       float x, float y, float w, float h);

/* 3. 动态节点：列表/日志流用 */
BINUI_C_API int32_t BINUI_C_CALL binui_add_node(void* app, int32_t parent_id, int32_t type,
                                                float x, float y, float w, float h,
                                                uint32_t color, int32_t radius, int32_t font_size);
BINUI_C_API int32_t BINUI_C_CALL binui_remove_node(void* app, int32_t node_id);
```

配套注意：

- `binui_add_node` 返回的是新节点 id，但 `set_text` 之类要的是 `uint16_t`；ABI 里统一用
  `int32_t` 传没问题的，内部转就行（现有函数已经是这个风格）。
- 动态节点会破坏"百分比/flex 重排"的假设（`relayout` 只认静态布局里的规格段），
  文档要写清楚：运行时加的节点不参与 `setSize` 重排 —— 这点 `ui_scale="proportional"`
  那节其实已经提过同类限制，照着写就行。
- 加完记得同步三处：`binui_c.h` 的 ABI 注释、`docs/bindings.md`、`bindings/{go,python,rust}`；
  以及 CONTRIBUTING 里说的"改格式必须做的三件事"（这里不改二进制格式，但要升
  `BINUI_C_ABI_VERSION` 吗？—— 只加函数不破坏既有布局，建议**不升**，在头注释里说明是增量）。

## 顺带：Go 绑定补齐

`bindings/go/binui/binui.go` 补三个方法就够（`SetImageRaw` / `SetImage` / `MountComponents`），
都是现成的 C 函数翻译一行的事。另外：

- `go vet` 在 `binui.go` 会报 `possible misuse of unsafe.Pointer`
  （`unsafe.Pointer(&layout[0])` 传给 `binui_app_create`），CI 里如果开 `go vet ./...` 会红；
  改成 `unsafe.SliceData(layout)` 即可（Go 1.20+）。
- `README.md` 里 `bindings/go` 的构建步骤还写着 `mingw32-make SHELL=sh capi`，
  但 `build.ps1` / Go 侧的 cgo 路径写死了 `../../../build-mingw/bin` ——
  如果用 CMake 构建（产出在 `build-cmake/`），Go 绑定就链接不上。建议改成可用
  `CGO_LDFLAGS` 覆盖，或在 README 里点明"Go 绑定只认 build-mingw"。
