# Go 绑定文档说错了一件事：「主 goroutine 天然固定线程」

> 建议标签：`documentation`（附带一条 C ABI 加固建议）· 面积：文档 / C ABI / Go 绑定
> 版本：binUI 0.2.0（`20e7a71` 之前之后都一样）
> 平台：Windows + MinGW + Go 1.26（cgo）

## 问题

`bindings/go/README.md:48-49`：

> **别把 `Pump` 丢进子 goroutine**。Win32 窗口有线程亲和；主 goroutine 天然固定
> 线程，子 goroutine 上要自己 `runtime.LockOSThread()`。

`bindings/go/binui/binui.go:21-23` 也一样：

> 窗口必须在同一个 OS 线程上创建和 pump。主 goroutine 天然固定线程，
> 所以别把 Pump 丢进子 goroutine……

**前半句是对的，加粗那句是错的**：Go 的 main goroutine 只在启动时位于主线程，
运行期可以在安全点被调度到别的 M 上。所以"别丢进子 goroutine"并不够 ——
`main` 里也必须 `runtime.LockOSThread()`。

我按这段文档写了宿主（`main` 里直接 `app.Open()` + 循环 `app.Pump(8)`，没有任何
`LockOSThread`），结果是**退出阶段必炸或必挂**，而且原因极难从现象倒推回来。

## 实测数据（三个组合，同一份宿主代码）

宿主：`ovoagent` 的 GUI（Go 1.26 + cgo，1440×900，24 个节点，`-exit-after 900` 后主动
`binui_app_destroy`，此时**窗口还开着**）。每轮 `WaitForExit(6000)` 超时记为挂死。

| binUI 版本 | 宿主是否 `LockOSThread` | 结果 |
|---|---|---|
| `88931ee`（修复前） | ❌ 不锁 | **11/16 崩溃**（`0xc0000005`，写地址恒定 `0x1528`，在 `binui_app_destroy` 里） |
| `88931ee`（修复前） | ✅ 锁 | **16/16 正常** |
| `20e7a71`（修复后） | ❌ 不锁 | **3/12 挂死**（不崩，但进程停在 `binui_app_destroy` 里，6s 不退出） |
| `20e7a71`（修复后） | ✅ 锁 | **12/12 正常** |

补充现象：不锁线程时，主循环里每帧采一次 `GetCurrentThreadId`，900ms 只跑了 **1～2 帧**
（锁线程时是 76～78 帧）——说明 `binui_pump` 在"非建窗线程"上本身就会长时间阻塞，
不只是退出阶段的事。

## 结论（也是对我自己那条 issue 的更正）

- `docs/issues/02-app-destroy-crash.md` 里把「50% 崩溃」归到 `close()` 的顺序上，
  **这个归因是错的**：同样的旧代码，宿主只要 `LockOSThread`，16/16 不崩。
  真正的触发条件是**跨线程调用**（Win32 窗口 + WGL 上下文都有线程亲和）。
- `20e7a71` 对 `close()` 的加固（摘窗口过程 → 隐藏 → 拆 GL → DestroyWindow）
  本身是合理的防御，值得留；但它不是那个崩溃的根因。
- 修完之后，跨线程的宿主观感从"偶尔崩"变成"偶尔挂"——**都没给出任何提示**，
  这是下面这条建议的由来。

## 建议

1. **改两处文档**（最省事、收益最大）：

   ```diff
   - 主 goroutine 天然固定线程，子 goroutine 上要自己 runtime.LockOSThread()。
   + 宿主必须在 main 里调一次 runtime.LockOSThread()：Go 的 main goroutine 只在启动时
   + 位于主线程，运行期可能被调度到别的线程；不锁的话，Open/Pump/Destroy 会在
   + "非建窗线程"上执行，表现为随机崩溃或退出时挂死。
   ```

   `docs/bindings.md` 的「线程与窗口」一节也建议补一句同样的 Go 特有说明
   （那里写了"用 Go 要 `runtime.LockOSThread()`"，但没说 main 也要）。

2. **C ABI 加一道防线**（可选，但能把"随机崩/挂"变成"明确报错"）：

   ```c
   /* binui_app_open 时记下 GetCurrentThreadId()；以下接口进来先比对 */
   int32_t binui_pump(void* app, int32_t timeout_ms);
   /* 返回值语义扩一位：-1 = 调用线程不是建窗线程（宿主写错了，别再继续） */
   ```

   `binui_pump` 返回 `-1` 是向后兼容的（原来只有 0/1），宿主能立刻发现并报错退出，
   总比现在这样 50% 崩 / 25% 挂、日志里一句提示都没有好。

3. **Go 绑定可以顺手提供一个安全入口**，比如：

   ```go
   // RunMain 锁住 OS 线程后执行 fn（窗口必须在同一线程创建与 pump）。
   func RunMain(fn func() int) int {
       runtime.LockOSThread()
       return fn()
   }
   ```

   示例 `bindings/go/demo/main.go` 也改成用它，让"正确用法"默认就是最省事的写法。

## 附：为什么这个坑特别值得写进文档

我在 issue 02 里给的复现是"Go + cgo 聊天界面"，作者用最小 C 程序复现不出来 ——
因为 **C 程序天生只有一个线程在跑窗口循环**，根本不会踩到。这个坑只有"宿主语言有
调度器"时才会出现，所以文档里那句"主 goroutine 天然固定线程"是最容易让人掉进来的地方。
