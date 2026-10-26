# .bex 容器（binUI execute）—— Linux 原生打包方案评估与格式规范

> **状态：设计归档，暂不实现。**
> 当前采纳的是「原生可执行文件 + 外挂散件资源」形态，见 [`app_layout.md`](app_layout.md)。
> 本文作为将来做单文件分发时的备选方案保留：§2 的 strip 实测结论、§3 的容器布局与结构体、§6/§7 的工具与安全约定仍然有效，接的时候按 `app_layout.md` §6 的映射表增量拼接即可。

> 一句话：**容器值得做，但必须"薄"**——只做「ELF 里装一份 binUI 布局 + 资源 + 元数据」的**打包约定**，核心库全程零感知；不做旧的 xecpack/xecrun 那套动态加载层。

---

## 0. 结论（TL;DR）

| 问题 | 结论 |
|------|------|
| 单文件 ELF 打包这个方向 | **对**，且是 Linux 分发的刚需（一个文件 = 一个应用，`chmod +x` 即可跑） |
| 承载方式 | **真实 ELF 段 `.bex`（`objcopy --add-section`）**，不是尾部追加。理由见 §2 实测 |
| UI 段默认形态 | **只装编译后的 binXHTML**；源文本（XHTML/XML）作为 `--embed-source` 可选，dev 构建才带 |
| 元数据 | 容器内嵌，但**必须同时能生成 `.desktop` + AppStream metainfo**——桌面环境不读 ELF 里的图标和名字 |
| 图标 | **打包期生成多尺寸 PNG 集合**；运行时不做 SVG 栅格化（"系统自动缩放"这个说法要纠正，见 §4.3） |
| 与 README「砍掉 xec」的关系 | 不冲突：容器只在启动器里出现一次，是**打包约定**不是**运行时层** |
| 真实成本大头 | 不是这个容器（1~2 天），是 **Linux 外壳**（X11/Wayland + EGL + fontconfig + 彩色 emoji + IME，周级），见 §10 |

---

## 1. 评估：该做什么、砍什么

### 1.1 值得做（4 条）

1. **分发形态闭环**。Windows 侧现在是 `exe + 同名目录下的 main.html + png`（见 `CMakeLists.txt` 的 POST_BUILD 复制），目录一散就白屏。单文件容器把这个痛点一次性解决，且 Linux 用户对"下载一个可执行文件直接跑"的期待是一致的。
2. **元数据有了单一来源**。程序名/开发者/版本/图标/AppID 集中在容器里，`--install` 时生成 `.desktop`、AppStream metainfo、hicolor 图标，`--version`/About 框也从同一份读，不会三处漂移。
3. **格式复用，不新造轮子**。UI 载荷直接是现有的 binUI 二进制（magic `BINU`/`XHTM`，`binui_format.h` v10），容器只是外面套一层目录表。渲染器 `loadLayout` / `setImageOrder` / `loadResources` 的调用约定不用改。
4. **可调试性不错**。真实 ELF 段意味着 `readelf -x .bex app`、`objdump -s -j .bex app` 直接能看，比尾部二进制块友好得多。

### 1.2 必须砍掉的部分（3 条，都是"顺手就加"的诱惑）

1. **默认不要在运行时编译 UI 源文本。** 技术上可行（`CompileBinUISource(const std::string&)` 已是内存接口，不用落地临时文件），代价是：编译器 + XML/HTML 解析器永远链进每个应用（几十 KB 代码 + 启动时解析 ms 级），而收益只是"开发者能改"。→ 默认只装编译产物；`--embed-source` 显式开启；运行时若同时存在源和编译产物，**以编译产物为准**（避免出现"改了源但没重新编译"的鬼故事）。
2. **不做运行时 SVG 图标栅格化。** 现有 `<svg>` 只是把 line/circle/polygon 编译成普通节点（`binui_format.h` 的 `BINUI_SVG`/`BINUI_LINE`），**不具备"栅格化一个 SVG 文件"的能力**。真在运行时支持要引入 nanosvg/librsvg（新依赖 + 路径求值 + 渐变/滤镜残缺）。→ 打包器接受 `.svg` 输入，**构建期**调用 `rsvg-convert`/`inkscape` 栅格化成 PNG 再入容器；运行时不认 SVG。
3. **不做"动态加载层"。** 即不复活进程内模块加载、不 `dlopen` 资源、不在容器里放可执行代码。README 第 5 行砍掉 xec 的判断对"加载层"是成立的；只保留"打包 + 单文件自解（不落地）"这一层。

### 1.3 和 README 历史决策的关系

README 说砍掉 xecpack/xecrun/动态加载层是"对多数场景负资产"。本次提案在**边界**上做了切割：

- 容器格式定义在 `src/app/`（不进 `include/`），**libbinui 不链接、不感知**；
- 启动器 = 一个平台外壳（`main.cpp` 里 20 行）：读容器 → `loadLayout` → `loadResources` → 跑主循环；
- 开发者绕过容器直接给 `main.html` 依然完全可行（就是现在的写法）。

即"容器是**可选的封装**，不是**必经的运行时**"。这样既不违背原判断，又拿到了单文件分发的收益。

---

## 2. 承载方式选型（附实测证据）

### 2.1 实测：GNU binutils 2.44 对"自定义段"和"尾部追加"的行为

在本机用 mingw64 的 binutils（`C:\ubin\mingw64\bin`，`objcopy --info` 列出支持 `elf64-x86-64`）对一个手工构造的 ELF64 `ET_REL` 做了四组实验：

```
1. objcopy --add-section .binui=payload.bin prog.o add.o
   → readelf -S add.o:
     [ 2] .binui   PROGBITS  0000000000000000 000044 001000 00      0   0  1
     （无 A 标志 = 非 ALLOC，不占进程映像；偏移 0x44、长度 0x1000 正确）

2. strip --strip-all add.o   （原地）
   → .binui 仍在：[ 2] .binui PROGBITS ... 000044 001000   文件大小 4448 不变

3. strip add.o（无参数）
   → .binui 仍在

4. 尾部追加 4096 字节后再 strip --strip-all
   → 文件从 4376 字节被重写成 280 字节，payload_intact = False
     （strip 按段表重建文件，尾部裸数据直接丢失）
```

**结论（这就是本规范最重要的一条工程判断）**：

- `objcopy --add-section` 加进去的**真实段在 strip 下存活**；
- **文件尾部追加的裸载荷活不过一次 strip**（distro 打包流程、`-g` 调试分离、CI 里随意一次 strip 都会把它抹掉）；
- 所以**定位器只能是 ELF 段表**，不能是"扫文件尾"。

> 注：本次实测用的是 `ET_REL` 对象（本机无 Linux 工具链产生 `ET_EXEC/ET_DYN`）。strip 的段处理逻辑对 `ET_DYN` 走同一套代码，但**落地前应在真实 Linux 二进制上复验一次**（脚本见附录 A）。
>
> 另注：上面日志里段名用的是 `.binui`（当时的临时名），结论与段名无关；正式规范把段名统一定为 **`.bex`**（见 §3.7）。

### 2.2 方案对比

| 方案 | 单文件 | 免额外运行时 | strip 存活 | 可 mmap | 工具兼容 | 结论 |
|------|--------|--------------|------------|---------|----------|------|
| **自定义段 `.bex`（`--add-section`）** | ✅ | ✅ | **✅（实测）** | ✅（非 ALLOC，手动 mmap） | `readelf`/`objdump` 直接可读 | **✅ 采用** |
| 尾部追加裸载荷（AppImage 思路的简化版） | ✅ | ✅ | ❌（实测丢失） | ✅ | 无段表记录 | ❌ 只做诊断扫描 |
| ALLOC 段（`ld -b binary` / `objcopy -I binary -O elf64-x86-64`） | ✅ | ✅ | 需 `--set-section-flags` 保住 | 天然映射 | 段表干净 | 备选（常驻内存，不划算） |
| sidecar 目录（现状 Windows 做法） | ❌ | ✅ | — | — | 全兼容 | 开发期保留 |
| AppImage（squashfs + FUSE） | ✅ | ❌ 需 FUSE/libfuse | — | 需解压 | 兼容 | 太重，不做 |
| Flatpak / Snap | ❌ | ❌ | — | — | — | 不做 |

### 2.3 最终选型

```
Linux  : 可执行文件 datapanel.bex + ELF 段 ".bex"（SHT_PROGBITS，无 SHF_ALLOC）+ 容器内 trailer 哨兵
Windows: 可执行文件 datapanel.exe + PE 节 ".bex"（同一容器字节流，定位器分平台）
macOS  : 可执行文件 + Mach-O 段 __DATA,__bex（后续）
独立形态: 无宿主时，容器可直接存成独立文件 app.bex —— 格式同一份，零成本兼容
```

容器字节流与承载完全解耦：同一份容器塞进 `.bex` 段、PE 节、或独立 `.bex` 文件，读出来的东西一样。这样规范只维护一份。

**明确禁止**：打包完成后不要再对 ELF 做壳/压缩（UPX 等）或 `objcopy --only-keep-debug` 分离——非 ALLOC 的自定义段可能被丢弃或位移。

---

## 3. 容器格式规范（.bex v1）

### 3.1 通用约定

| 项 | 规定 |
|----|------|
| 字节序 | **一律小端**（LE）。x86-64/aarch64/riscv64 都是 LE；BE 平台读前做字节交换或直接拒绝 |
| 读写方式 | 按偏移显式 `get32/get64` 读，**不直接 cast 结构体指针**（避免对齐/端序/别名 UB）；结构体只用于生成期 |
| 偏移基准 | **全部相对容器基址 B**（B = 段在文件中的 `sh_offset`），不出现文件绝对偏移——段被移动后依然有效 |
| 对齐 | 基址 8B；header 48B；字符串池紧接 header；TOC 4B 对齐；数据区每项 16B 对齐；`totalSize` 8B 对齐 |
| 字符串 | UTF-8、`\0` 结尾、无 BOM；多语言走 `key[lang]=value` 扩展行，不搞二进制多语言槽 |
| 大小上限 | 单容器 < 4 GiB（偏移 u32）；单资源 < 1 GiB |
| 校验 | CRC32（多项式 `0xEDB88320`，同 zlib）。计算时 `header.crc32` 与 `trailer.crc32` 两字段按 0 参与 |
| 压缩 | **v1 不压缩**（raw）。PNG/JPEG 本身已压缩、布局段很小，压缩收益低而依赖成本高；flag 位预留 |

### 3.2 总布局

```
B+0                          ┌──────────────────────────────────────┐
                             │ BinUIAppHeader            48 B       │  magic 'BEX1' / 版本 / totalSize / TOC / 字符串池 / meta 偏移 / CRC
B+48                         ├──────────────────────────────────────┤
                             │ 字符串池（UTF-8，'\0' 分隔顺序拼接）    │  strOffset/strSize 指向它
                             ├──────────────────────────────────────┤
                             │ BinUIAppMeta              40 B       │  名字/开发者/版本/AppID/分类/图标/UI 入口
                             ├──────────────────────────────────────┤
                             │ BinUIAppEntry × tocCount  32 B each  │  kind / 逻辑名 / dataOff / dataSize / flags / crc / imageIndex
                             ├──────────────────────────────────────┤
                             │ 数据保税区（每项 16B 对齐）             │  binXHTML / XHTML 源 / 图标 PNG / 图片资源 / 其它
B+totalSize-32               ├──────────────────────────────────────┤
                             │ BinUIAppTrailer           32 B       │  magic2 'BEX1TAIL' / totalSize / crc / 保留
B+totalSize                  └──────────────────────────────────────┘
```

### 3.3 结构定义

> 落地位置：`src/app/binui_container.h`（**不进 `include/`**，核心库不感知）。

```cpp
#pragma once
#include <cstdint>

namespace binui {
namespace app {

constexpr uint32_t BEX_MAGIC   = 0x31584542; // 字节 'B','E','X','1'（LE）
constexpr uint32_t BEX_VERSION = 1;

constexpr uint32_t BEX_FLAG_SIGNED       = 1u << 0; // 含签名段（v2，预留）
constexpr uint32_t BEX_FLAG_PORTABLE     = 1u << 1; // 优先便携运行，不主动装桌面项
constexpr uint32_t BEX_FLAG_EMBED_SOURCE = 1u << 2; // 容器内含 UI 源文本（dev 构建）

enum BexEntryKind : uint32_t {
    BEX_KIND_LAYOUT  = 0, // binUI 二进制布局（magic 'BINU'/'XHTM'，binui_format.h v10）
    BEX_KIND_SOURCE  = 1, // UI 源文本（XHTML/XML），可选
    BEX_KIND_ICON    = 2, // 图标位图（PNG，见 §4.3）
    BEX_KIND_IMAGE   = 3, // UI 引用的图片资源，nameOff = 与 <img src> 完全一致的相对路径
    BEX_KIND_DATA    = 4, // 任意附带文件（音频/字体/数据），nameOff = 逻辑路径
    BEX_KIND_SIG     = 5, // 签名（v2 预留）
};

enum BexEntryFlag : uint32_t {
    BEX_ENTRY_OPTIONAL = 1u << 0, // 缺失可忽略（用于可选附加资源）
    BEX_ENTRY_OVERRIDE = 1u << 1, // 允许被外部同名文件覆盖（开发热改/用户覆盖）
    BEX_ENTRY_PNG      = 1u << 2, // 位图为 PNG（BEX_KIND_ICON/IMAGE 时提示，便于不做魔数嗅探）
};

// 元数据里的 UI 入口形态（与 TOC kind 冗余，便于启动器早决策）
enum BexUiFormat : uint16_t {
    BEX_UI_BINARY = 0, // BEX_KIND_LAYOUT（默认，推荐）
    BEX_UI_XHTML  = 1, // BEX_KIND_SOURCE，XHTML/HTML5 源
    BEX_UI_XML    = 2, // BEX_KIND_SOURCE，XML 源
};

#pragma pack(push, 1)

struct BinUIAppHeader {
    uint32_t magic;          // BEX_MAGIC
    uint16_t verMajor;       // 1
    uint16_t verMinor;       // 0
    uint32_t headerSize;     // 48
    uint32_t flags;          // BEX_FLAG_*
    uint32_t totalSize;      // 容器总长（含 header 与 trailer），必须等于承载段的 sh_size
    uint32_t tocOffset;      // TOC 起始（rel B）
    uint32_t tocCount;
    uint32_t strOffset;      // 字符串池（rel B）
    uint32_t strSize;
    uint32_t metaOffset;     // BinUIAppMeta（rel B）
    uint32_t crc32;          // 全容器 CRC32（本字段与 trailer.crc32 按 0 参与计算）
    uint32_t reserved;       // 0
};

struct BinUIAppMeta {
    uint16_t metaVersion;    // 1
    uint16_t nameOff;        // 显示名（窗口标题 / Name）
    uint16_t devOff;         // 开发者/组织名
    uint16_t verOff;         // 版本字符串（"1.4.2"，给人看）
    uint16_t exeNameOff;     // 建议安装名（见 §3.5，非权威）
    uint16_t appIdOff;       // 反向域名 AppID（"io.github.dev.app"）；0 = 由 dev+exeName 推导
    uint16_t commentOff;     // 一句话简介（.desktop Comment / AppStream summary）
    uint16_t categoriesOff;  // ';' 分隔的 freedesktop 分类（"Utility;Graphics;"）
    uint16_t licenseOff;     // SPDX 标识（"MIT"）
    uint16_t homepageOff;    // 项目主页 URL
    uint16_t extraOff;       // 'key=value\n' 多行扩展块（见 §3.6）
    uint16_t uiFormat;       // BexUiFormat
    uint32_t uiEntryIdx;     // UI 载荷在 TOC 中的下标
    uint32_t iconEntryIdx;   // 主图标（最大那张）在 TOC 中的下标；0xFFFFFFFF = 无
    uint32_t flags;          // BEX_FLAG_*
    uint32_t minRuntimeVer;  // 要求的 BINUI_VERSION 下限；0 = 不限
};

struct BinUIAppEntry {
    uint32_t kind;           // BexEntryKind
    uint32_t nameOff;        // 逻辑名/路径（字符串池偏移，0 = 无名）
    uint32_t dataOff;        // 数据起始（rel B）
    uint32_t dataSize;
    uint32_t flags;          // BexEntryFlag
    uint32_t crc32;          // 该资源 CRC32；0 = 不校验
    uint16_t imageIndex;     // kind=IMAGE 时的槽位提示，0xFFFF = 由路径顺序推导
    uint16_t align;          // 数据对齐（生成器写 16）
    uint32_t reserved;       // 0
};

struct BinUIAppTrailer {     // 固定位于 B+totalSize-32，容器截断/拼接哨兵
    uint64_t magic2;         // 字节 'B','E','X','1','T','A','I','L'
    uint32_t totalSize;      // 必须与 header.totalSize 一致
    uint32_t crc32;          // 必须与 header.crc32 一致
    uint64_t reserved[2];
};

#pragma pack(pop)

static_assert(sizeof(BinUIAppHeader)  == 48, "header must be 48 bytes");
static_assert(sizeof(BinUIAppMeta)    == 40, "meta must be 40 bytes");
static_assert(sizeof(BinUIAppEntry)   == 32, "entry must be 32 bytes");
static_assert(sizeof(BinUIAppTrailer) == 32, "trailer must be 32 bytes");

} // namespace app
} // namespace binui
```

### 3.4 加载算法

```cpp
// 1) 定位自身文件：readlink("/proc/self/exe")；若指向 "(deleted)" → 报错退出（不要读半截）
// 2) 解析自身 ELF：读 e_shoff/e_shnum/e_shstrndx → 找名为 ".bex" 的 SHT_PROGBITS 段
//    → B = sh_offset, N = sh_size
// 3) 无该段 → 无容器，回退 sidecar（同级 main.html / main.binui / 用户 --ui 参数）
// 4) mmap(B, N, PROT_READ, MAP_PRIVATE)  ← 全程只读，绝不 PROT_EXEC
// 5) 校验：magic == BEX_MAGIC；headerSize == 48；totalSize == N；
//          metaOffset + 40 <= tocOffset；tocOffset + tocCount*32 <= 数据区；
//          末尾 32B trailer 的 magic2/totalSize/crc32 与 header 一致；
//          CRC32 全容器匹配 → 失败**直接退出**（不要带着半截布局继续跑）
// 6) 取 UI 载荷 → renderer.loadLayout(view.ptr, view.size)
// 7) 资源：遍历 kind=IMAGE 项 → 以 nameOff 指向的逻辑路径为键喂给渲染器
// 8) 若 header.flags & EMBED_SOURCE 且 kind=SOURCE 存在且 uiFormat != BINARY：
//    优先用源？——不。见 §1.2：编译产物存在时永远用产物；源仅用于 --dump 与 dev 覆盖
```

> 生产路径**不做**"全文件扫 magic"。那是 `--scan` 诊断模式的事。

### 3.5 关于"可执行文件名"字段的正确用法

提案里列了"可执行文件名"，这里给个明确边界：

- **权威永远是 `argv[0]` 的文件名**（用户可能 `mv`、可能软链）。容器里的 `exeName` 只是"建议安装名"，用来生成 `.desktop` 的 `Exec=` 与 hicolor 图标名。
- 两者不一致时不报错、不改名，只 `log` 一行。反过来（把容器里的名字当真名）会出现"改了文件名但 .desktop 指向旧名"的死链。

### 3.6 `other` 扩展块（`extraOff`）

纯文本，每行 `key=value`，`#` 注释，UTF-8。约定可识别的键：

```
name[zh_CN]=数据面板          # 本地化显示名（.desktop 的 Name[zh_CN]）
comment[zh_CN]=实时监控演示
keywords=chart;monitor;       # .desktop Keywords
startup-wm-class=binui-app    # X11 任务栏图标合并，必须与窗口 WM_CLASS 一致
mime-type=text/x-binui;       # 可注册的文件关联
requires=gpu;glsl-330;        # 运行前提，启动器可据此给友好报错
update-url=https://...        # 预留
```

未知键必须**原样保留、不报错**——这就是 "other" 该有的语义；以后加字段不必升容器版本。

### 3.7 命名、后缀与 MIME

**`.bex` = binUI execute。** 三字母，与 `.exe` 同韵律——看一眼就知道"这是可执行的应用包"，同时避开三个坑：`.binui`（已被布局格式占用）、`.binapp`（易被当成 `.bin` 数据文件）、`.app`（macOS bundle 是目录，直接撞）。

| 对象 | 名字 | 说明 |
|------|------|------|
| 容器文件（带 ELF 宿主） | `datapanel.bex` | Linux 下 `chmod +x` 即可跑；后缀是"可执行应用包"的可视信号 + MIME 关联，不是能不能跑的前提 |
| 容器文件（无宿主，纯载荷） | `datapanel.bex` | 同一后缀；由 `binui-run` 或宿主应用读取 |
| ELF 段名 / PE 节名 | `.bex` | 与后缀同名，`readelf -x .bex datapanel.bex` 读起来自洽；PE 节名 4 字符远低于 8 字符上限 |
| 容器 magic（段内） | 字节 `B`,`E`,`X`,`1` | LE u32 = `0x31584542`；末位 `1` 即容器主版本 |
| 容器尾哨兵 | 字节 `BEX1TAIL` | 防截断，位于 `B+totalSize-32` |
| **编译后 UI 布局** | `main.binui` / `main.xhtml` | **不共用 `.bex`**：单份布局能被 `loadLayout` 直接吃，语义与"应用包"不同，撞名会让"这个文件能直接加载"的约定失效 |
| Windows 侧产物 | `datapanel.exe` | Windows 双击执行必须 `.exe`；容器照旧放在 `.bex` 节里，后缀与承载解耦 |

标配判别规则（一条就够）：

```
offset 0 起 4 字节 == 7F 45 4C 46  ("\x7fELF")  → 自执行包（chmod +x 后直接跑）
offset 0 起 4 字节 == 42 45 58 31  ("BEX1")     → 纯容器（需 binui-run / 宿主应用加载）
其它                                            → 不是 .bex
```

`shared-mime-info` 注册（可直接抄进 `binui-app.xml` 后跑 `update-mime-database`）：

```xml
<mime-type type="application/x-binui-app">
  <comment>binUI application</comment>
  <glob pattern="*.bex"/>
  <magic priority="50"><match value="\x7fELF" offset="0" type="string"/></magic>
  <magic priority="60"><match value="BEX1"     offset="0" type="string"/></magic>
  <sub-class-of type="application/x-executable"/>
  <generic-icon name="application-x-executable"/>
</mime-type>
```

与 `.AppImage` 的关键差别：**后缀只是标签，不是挂载机制**。`.bex` 不需要 FUSE、不需要 `--appimage-extract`，改名成 `datapanel` 照样跑（因为定位靠 ELF 段表，不靠文件名）。

---

## 4. 载荷规则

### 4.1 UI 段

| 形态 | kind | 说明 |
|------|------|------|
| **binXHTML（默认）** | `BEX_KIND_LAYOUT` | `CompileBinUIFileEx()` 的 `layout` 原样写入，magic `BINU`/`XHTM`，`BINUI_VERSION=10` |
| XHTML 源（可选） | `BEX_KIND_SOURCE` | `--embed-source` 才写；运行时仅用于 `--dump-source` 与 dev 覆盖 |

启动器在读布局前先检查 `BinUIHeader.version`（layout 自己的 16B 头），与 `meta.minRuntimeVer` 比对：容器要求的运行时不满足 → 明确报"应用需要 binUI ≥ vN，当前 vM"，而不是解密失败式的花屏。

### 4.2 资源索引（关键：路径键必须与源码一致）

现有渲染器接口是**按路径字符串**索引资源：

```cpp
renderer.setImageOrder(res.images);   // imageIndex -> 路径（顺序与编译输出一致）
renderer.loadResources(map);          // map<路径, vector<uint8_t>>
```

所以容器的 `BEX_KIND_IMAGE` 项，`nameOff` **必须逐字节等于** `<img src="...">` 里的相对路径（含子目录、大小写、`./` 前缀都别归一化）——归一化交给编译器做，容器只做搬运。`imageIndex` 字段是"提示"，生成器按编译产物的顺序填，加载器再以 `setImageOrder(paths)` 重建一遍，两者一致即可。

另一个必须提的现存约束：`loadResources` 收的是 `unordered_map<string, vector<uint8_t>>`，**会整份拷贝像素**。容器是大文件（立绘 1227×1227 的 PNG 常 1~2 MB），mmap 的零拷贝优势在这里被抵消。建议（属于渲染器侧的小改动，不在本规范内）：

```cpp
// 可选：零拷贝视图，后端 DecodeImagePixels 本来就吃 (ptr, size)
void loadResourceView(const std::string& path, const uint8_t* data, size_t size);
```

### 4.3 图标：纠正"系统自动缩放降级绘制"

提案写的是"建议直接放最清晰的（图标），系统自动缩放降级绘制"。这句话在 Linux 上会翻车：

- 桌面环境**按尺寸目录取图**（`hicolor/16x16/apps/`、`24x24`、`32x32`、`48x48`、`64x64`、`128x128`、`256x256`、`scalable/`）。任务栏要 24/32、Dock 要 48/64、概览要 128/256、`--about` 可能要 512。
- 只塞一张 1024×1024，装到 `hicolor/1024x1024/` 后，多数主题**找不到就退化成缺省图标**，而不是"帮你缩"。能缩的只有少数合成器，且是最近才有的行为。
- 所以正确做法：**打包期**从一张源图（≥512，建议 1024 或矢量）生成 **16/24/32/48/64/128/256** 七档 PNG（每档几 KB，总增量 < 200 KB），全部入容器（`BEX_KIND_ICON`，`nameOff` = `"icon/64.png"` 这类逻辑名）；`--install` 时按 `hicolor` 目录铺开。
- 容器内 `iconEntryIdx` 指向**最大那张**（给 About 框、`_NET_WM_ICON`、`xdg-toplevel-icon` 用）。
- 输入格式：**PNG 必选**（无损 + alpha，图标必须带 alpha）；JPEG 可选（无 alpha，图标场景基本不用）；**SVG 由打包器栅格化**，容器里不出现 SVG（理由见 §1.2 第 2 条）。
- "系统自动缩放"只在**一个**地方成立：窗口管理器拿 `_NET_WM_ICON` 的 32bpp ARGB 数组自行缩放——那也要求你至少提供 32/48/64，否则一枚 1024 图标被缩到 24 会糊。

---

## 5. Linux 原生集成（这才是"原生"的分界线）

**桌面环境不会打开你的 ELF 找图标和名字。** GNOME/KDE 读的是：

```
~/.local/share/applications/<appid>.desktop
~/.local/share/icons/hicolor/<size>/apps/<name>.png
~/.local/share/metainfo/<appid>.metainfo.xml
```

容器内元数据的真正价值 = ①应用自报（`--version`、About、窗口标题）②**自注册**（从容器生成上面三份）③便携分发。

### 5.1 `.desktop` 生成的 4 个坑

```ini
[Desktop Entry]
Type=Application
Version=1.5                        # ⚠ 这是「桌面条目规范版本」，不是应用版本！
Name=Data Panel
Name[zh_CN]=数据面板
Comment=Realtime monitor demo
Exec=/home/u/.local/bin/datapanel   # 绝对路径或 PATH 内名
Icon=datapanel                      # ⚠ 图标「名」（不含扩展名），不是路径
Terminal=false
Categories=Utility;Graphics;         # ⚠ 必须有，否则菜单挂不上
Keywords=chart;monitor;
StartupWMClass=binui-app            # ⚠ X11 任务栏图标合并靠它，必须等于窗口 WM_CLASS
StartupNotify=true
```

1. `Version=` 是规范版本（1.0/1.5），**应用版本不在这**。应用版本进 AppStream `<releases><release version="1.4.2"/>`。
2. `Icon=` 是图标名，装到 hicolor 后按名解析；写路径无效。
3. `Categories=` 缺失 → 应用不出现在菜单里（只有 `NoDisplay=true` 时才允许缺）。
4. `StartupWMClass` 与窗口的 WM_CLASS 不一致 → X11 任务栏出现"孤儿图标"。

### 5.2 AppStream metainfo（从同一份元数据生成）

```xml
<component type="desktop-application">
  <id>io.github.dev.datapanel</id>
  <name>Data Panel</name>
  <summary>Realtime monitor demo</summary>
  <metadata_license>MIT</metadata_license>
  <project_license>MIT</project_license>
  <developer id="github.dev"><name>Dev</name></developer>
  <url type="homepage">https://…</url>
  <releases><release version="1.4.2" date="2025-01-01"/></releases>
  <provides><binary>datapanel</binary></provides>
  <launchable type="desktop-id">io.github.dev.datapanel.desktop</launchable>
</component>
```

AppID 建议 `io.github.<user>.<app>` 或 `com.<org>.<app>`（反向域名，和容器 `appIdOff` 一致；`0` 时由 `dev + exeName` 推导）。

### 5.3 安装动作（`--install`）

```
1. 读自身容器（不落地、不解包到临时目录）
2. cp 自身 → ~/.local/bin/<exeName>（或 --prefix 指定），chmod 0755
3. 抽图标 → 每个 ~/.local/share/icons/hicolor/<N>x<N>/apps/<exeName>.png
4. 写 ~/.local/share/applications/<appid>.desktop
5. 写 ~/.local/share/metainfo/<appid>.metainfo.xml
6. update-desktop-database / gtk-update-icon-cache（存在才调，缺失忽略）
```

安全细节：全部写 `~/.local/share` 下，用"临时文件 + `rename`"原子落盘，`O_NOFOLLOW` 防符号链接攻击；`--uninstall` 幂等（缺文件不报错）。

### 5.4 便携模式

不安装直接跑（默认行为）：直接读自身容器启动；窗口图标用容器内最大 PNG 走 `_NET_WM_ICON`。`BEX_FLAG_PORTABLE` 只影响"是否提示可 `--install`"。

### 5.5 开发期覆盖（低成本、高价值）

启动优先级：

```
1. --ui <path> 显式指定         （最高）
2. 可执行文件同级 <name>.binui / main.html   （dev 覆盖，免重新打包）
3. 用户覆盖 ~/.local/share/<appid>/override.binui
4. 容器内 UI 载荷               （发布形态）
```

这条几乎零实现成本，但把"改一行 UI 就得重新打包"的开发摩擦消掉了——也是容器方案能不能被接受的关键。

---

## 6. 工具链

### 6.1 打包器 `binui-pack`

```
binui-pack --exe build/app --ui main.html        # 输入 UI 源 → 内部调用编译器 → 装 binXHTML
           --icon logo.png,logo.svg              # 多尺寸自动生成（SVG 走 rsvg-convert）
           --name "Data Panel" --name-zh "数据面板"
           --dev "Dev" --version 1.4.2 --appid io.github.dev.datapanel
           --category "Utility;Graphics;"
           --res assets/                         # 目录递归 → BEX_KIND_IMAGE，逻辑路径保留相对结构
           --embed-source                        # 可选：连源文本一起装
           --out build/datapanel.bex              # 输出：拷 exe + add-section（后缀 .bex 见 §3.7）
```

**关键点：打包器可以在 Windows 上产出 Linux ELF。** `objcopy` 只是搬字节，本机已实测 `objcopy --info` 含 `elf64-x86-64` 目标，`--add-section` 对 ELF 正常工作。所以 CI 里不需要 Linux runner 就能打 Linux 包（除非要用 rsvg 栅格化 SVG——那个是另一回事，可用预生成 PNG 规避）。

CMake 集成（照现有 `add_binui_demo` 的风格）：

```cmake
add_custom_command(TARGET datapanel POST_BUILD
    COMMAND binui-pack --exe "$<TARGET_FILE:datapanel>" --ui ... --out "$<TARGET_FILE_DIR:datapanel>/datapanel.bex")
```

### 6.2 只读检查器 `binui-inspect`

```
binui-inspect dump app            # 元数据 + TOC 全表
binui-inspect extract app:img/logo.png > logo.png
binui-inspect verify app          # CRC / trailer / minRuntimeVer
binui-inspect scan app            # 诊断模式：全文件扫 BEX_MAGIC（定位用，不入生产路径）
```

### 6.3 段名与工具兼容

- 段名统一 `.bex`（4 字符，远低于 PE 节名 8 字符上限；与文件后缀/MIME 同名，`readelf -x .bex` 读起来自洽）。
- 生成时建议 `--set-section-flags .bex=noload,readonly` 明确"不装载"意图（实测不加标志默认也不带 `SHF_ALLOC`）。
- 打包后 `readelf -S datapanel.bex` 应能看到一行 `.bex PROGBITS … 无 A 标志`；`readelf -x .bex datapanel.bex` 可直接 hexdump。

---

## 7. 安全与完整性

| 项 | 规定 |
|----|------|
| 载荷可执行性 | 容器数据段**只读 mmap（`PROT_READ`）**，非 ALLOC 段天然不进进程映像 —— 不满足 W^X 风险 |
| 不 `dlopen` | 容器里绝不放代码，只有数据 |
| CRC 失败 | **硬失败**（打印"容器损坏：offset X 校验不符"并退出），不降级部分加载 |
| trailer 哨兵 | 校验 `totalSize` 与边界，防"文件被截断/被拼接"导致越界读 |
| 路径穿越 | `--install` 解包时逻辑路径禁止 `..` 与绝对路径（PE/ELF 无关，属于 zip-slip 同类） |
| 签名 | v1 只做 CRC（**CRC ≠ 签名 ≠ 系统信任链**）。真签名 v2 用 Ed25519 覆盖整个容器；系统级信任仍应走 IMA/rpm 签名，那是分发侧的事 |
| 已删除的自映像 | `/proc/self/exe` 指向 `(deleted)` 时拒绝运行（不要读半个文件）|

---

## 8. 版本与兼容

三层版本**必须分开**，别混：

| 版本 | 位置 | 语义 | 处理 |
|------|------|------|------|
| 容器版本 | `header.verMajor/verMinor` | 容器结构版本 | 高版本 minor → 忽略未知段；高 major → 拒绝 |
| 布局版本 | UI 载荷内 `BinUIHeader.version`（=10） | binUI 二进制版本 | 与 `meta.minRuntimeVer` 比对，过低给明确报错 |
| 应用版本 | `meta.verOff` 字符串 + AppStream `<release>` | 给人/给包管理器 | 建议同时存机器可比较的 `major.minor.patch` |

向前兼容：新增 TOC kind / 新 `extra` 键 / 新 flag 位都不升版本；改结构布局才升。

---

## 9. 三平台同构（顺手拿到的收益）

一份规范，三种承载，容器字节流完全一致：

| 平台 | 承载 | 定位 |
|------|------|------|
| Linux | ELF 段 `.bex` | `e_shoff` → 段表 → `.bex` |
| Windows | PE 节 `.bex` | PE 节表（4 字符，远低于 8 字符上限） |
| macOS | Mach-O 段（后续） | `__DATA,__bex` |
| 无宿主 | 独立 `.bex` 文件 | 直接当文件读 |

这样 Windows 侧也能顺手治好"exe 旁边散着 html/png"的问题，不必等 Linux。

---

## 10. 落地顺序与真实成本

| 阶段 | 内容 | 估工 |
|------|------|------|
| **1** | 容器格式（`src/app/binui_container.h/.cpp`：打包 + 读取 + 校验）+ `binui-pack` + 启动器 20 行 + `binui-inspect` | **1.5~2 天** |
| **2** | CMake/PE 侧接入（Windows 先上，验证格式闭环） | 0.5 天 |
| **3** | `--install/--uninstall/--appstream` + 图标多尺寸生成 | 1 天 |
| **4** | **Linux 外壳（X11 先行）**：X11 窗口 + 事件 + EGL/GLX 上下文 + 拖边（`_NET_WM_MOVERESIZE`） | 1~2 周 |
| **5** | GL 后端拆分：把图集/四边形/九宫格/FBO/MSAA 抽成 `binui_gl_common`，WGL/EGL/GLX 只做上下文 + swap | 2~3 天 |
| **6** | Wayland 后端（`xdg-shell` + EGL + `text-input-v3`） | 1~1.5 周 |
| **7** | fontconfig 字体发现 + **CBDT/sbix 彩色 emoji**（见下）+ IME + 剪贴板 + PipeWire/Pulse | 1 周 |

**必须先说清的两个技术风险（评估里最该看的部分）**：

1. **彩色 emoji 在 Linux 上大概率开箱失效。** 现有实现走 FreeType 的 **COLR/CPAL 层合成**（README 明确写了）；而 Linux 的默认 emoji 字体 **Noto Color Emoji 是 CBDT/CBLC 位图字形**（内嵌 PNG 位图，`FT_LOAD_COLOR` + `FT_Bitmap` BGRA 路径），不是 COLR。要彩色就得新增一条位图字形路径（不难，但是新代码）；否则 Linux 上 emoji 全变单色/豆腐。另有 Noto-COLRv1 版本可用，但发行版默认不装。
2. **Wayland 下"无边框 + 自绘标题栏 + 拖边缩放"这套交互会被削弱。** Wayland 不允许客户端自行定位窗口，拖窗必须 `xdg_toplevel.move(seat, serial)`（依赖 pointer serial，且合成器**可以拒绝**）；拖边缩放 `xdg_toplevel.resize` 同理。GNOME 对这些支持历来不完整。项目在 Windows 上精心绕开了 NVIDIA 的 `SwapBuffers` 崩溃（拖拽期改走 `StretchDIBits`），到 Linux 上这套绕行逻辑**完全用不上**，但会换一批 Mesa/NVIDIA 的新坑。→ 建议 **X11 先行**（可控性最高，接近 Windows 的做法），Wayland 作为第二阶段；X11 下透明窗口反而比 Windows 简单（ARGB visual 原生支持，不用 `UpdateLayeredWindow` 那套）。

其余好消息：透明/分层窗口、DPI 缩放（X11 手酸，Wayland 由合成器给 scale）都比 Windows 那套自绘+绕驱动简单；文本层（FreeType+HarfBuzz）本来就是跨平台的。

---

## 11. 待定与风险清单

| # | 项 | 需要定 |
|---|----|--------|
| 1 | ~~段名~~ | **已定 `.bex`**（段名 = 后缀 = MIME，PE 侧 4 字符无压力，见 §3.7） |
| 2 | 是否允许**加载时**覆盖容器内资源（`~/.local/share/<appid>/`） | 建议允许，dev 价值高 |
| 3 | 资源是否允许压缩（DEFLATE） | v1 不做，v2 视体积 |
| 4 | `loadResources` 的整份拷贝是否改成视图 | 建议加 `loadResourceView`，独立于本规范 |
| 5 | 是否提供"一个 ELF 装多套 UI（多语言/多主题）" | 建议 `BEX_KIND_DATA` + 路径前缀实现，不生新 kind |
| 6 | AppID 缺失时是否强制推导 | 建议推导 + 警告 |
| 7 | strip 行为需在真实 `ET_DYN` 上复验（本机实测是 `ET_REL`） | 附录 A 脚本，Linux CI 里跑 |

---

## 附录 A：复验脚本（Linux，验证 strip 后 `.bex` 仍在）

```bash
#!/bin/sh
set -e
printf 'BEX1' > payload.bin; head -c 65536 /dev/urandom >> payload.bin
cc -x c -o app - <<'EOF'
int main(void){ return 0; }
EOF
objcopy --add-section .bex=payload.bin app app.bex
strip --strip-all app.bex
echo "--- 段表应有一行 .bex，且 Flg 列无 A ---"
readelf -SW app.bex | grep -E '\.bex|^  \['
./app.bex && echo "OK: 加了段仍然可执行"
set -- $(readelf -SW app.bex | awk '/\.bex/{print $5, $6}')
echo "段偏移=$1 长度=$2 文件大小=$(stat -c%s app.bex)"
```
