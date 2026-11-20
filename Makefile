# Makefile - binUI (MinGW / GCC / Clang)
# Core lib binui (pure C++) builds on any platform; Win32 demos build on Windows only.
# CMake is also supported.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -MMD -MP
LDFLAGS  ?=
# 静态链接 C++ 运行时：产物常在 sh 下被执行（make 的 SHELL=sh），而 Git for Windows
# 的 /mingw64/bin 排在本机 MinGW 前面，会先命中版本不匹配的 libstdc++-6.dll，
# 表现为莫名其妙的退出码 127。静态链接后产物自包含，换环境也不会炸。
RTLINK   ?= -static-libgcc -static-libstdc++

SRC_DIR  := src
INC_DIR  := include
EXA_DIR  := examples
# MinGW 构建目录与 CMake 的 build/ 分开，避免 make clean 误删 CMake 产物
BUILD_DIR := build-mingw
# 产物分三处，别再混在一起看一眼分不清谁是谁：
#   bin/           库 + 命令行工具（binui_c.dll、binui_compile、offscreen_render…）
#   bin/tests/     测试用例（只有 test_css / test_elements / test_font / test_capi）
#   bin/examples/  使用示例（一组一个 exe，UI 在同名子目录里）
BIN_DIR  := $(BUILD_DIR)/bin
TEST_DIR := $(BIN_DIR)/tests
DEMO_DIR := $(BIN_DIR)/examples

# platform detection
ifdef ComSpec
    PLATFORM := win32
    WINLIBS  := -luser32 -lgdi32 -lgdiplus -lole32
    EXT      := .exe
else
    PLATFORM := posix
    EXT      :=
endif

# =============================================
# core lib binui
# =============================================
LIB_OBJS := \
    $(BUILD_DIR)/compiler/binui_compiler.o \
    $(BUILD_DIR)/renderer/binui_renderer.o \
    $(BUILD_DIR)/font/binui_font_host.o \
    $(BUILD_DIR)/common/binui_link.o \
    $(BUILD_DIR)/binui_paint.o \
    $(BUILD_DIR)/binui_charts.o
LIB_A    := $(BUILD_DIR)/libbinui.a

# bin2d（P2 迁移）：<path> 的填充光栅化在 bin2d 里做（`bin2d::RasterizeCoverage`），
# 见 src/backends/binui_paint.cpp 与 docs/migrate-to-bin2d.md。所以 bin2d 成了**硬依赖**：
# 没构建过就明确报错，而不是静默少一堆符号。先 `make -C ../bin2D lib`。
BIN2D_DIR := ../bin2D
BIN2D_A   := $(BIN2D_DIR)/build-mingw/libbin2d.a
# 文字模块（0.38.x）：字形光栅 / 整形 / 图集 / 文字阴影从 binUI 搬到 bin2d 的可选
# 静态库。核心库不链它（"纯 CPU、零第三方依赖"那条保持不变），而 binUI 的绘制
# 后端要画字，所以这里是 binUI 的硬依赖 —— 先 `make -C ../bin2D text-lib`。
BIN2D_TEXT_A := $(BIN2D_DIR)/build-mingw/libbin2d_text.a
LIBS_A    := $(LIB_A) $(BIN2D_A) $(BIN2D_TEXT_A)

# binGPU（0.26.1）：窗口呈现走 bin2d::GpuCompositor → bingpu 交换链，也就是
# master 那条分层里的 "bin2d 的后端交给 binGPU"。
# 与 bin2D 对 binGPU 的处理一致：**有才用**。没有 libbingpu.a 时 bin2d 的 gpu.cpp
# 根本不进库，binUI 也就只走 "CPU 出图 + GDI 贴屏" —— 窗口照样能开，只是没 GPU 上屏。
BINGPU_DIR := ../binGPU
BINGPU_A   := $(wildcard $(BINGPU_DIR)/build-mingw/libbingpu.a)
ifneq ($(BINGPU_A),)
BINGPU_INC := -I$(BINGPU_DIR)/include
BINGPU_LNK := $(BINGPU_A) -ld3d11 -ldxgi -ldwmapi
else
BINGPU_INC := -DBINUI_NO_BINGPU
BINGPU_LNK :=
endif

# 通用源规则（-Isrc：核心源里用 "backends/xxx.h" 这种相对 src/ 的路径引用后端头）
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -Isrc -c $< -o $@

# 平台字体发现（0.38.x）：只有"家族名 → 字体文件路径"这一层留在 binUI，
# 字形光栅/整形在图集模块里（bin2d 的 libbin2d_text）。所以这个 .o **不需要**
# FreeType/HarfBuzz 的头 —— 这正是"binUI 不再出现 ft2build/harfbuzz"的落点。
$(BUILD_DIR)/font/%.o: $(SRC_DIR)/font/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -Isrc -I$(BIN2D_DIR)/include -c $< -o $@

# 后端（src/backends/）与 Win32 外壳（src/platform/）：对象路径保持在 build-mingw 根下，
# 演示程序的链接规则不用跟着改
#
# 0.37.x：**只到这里为止**。binUI 的绘制路径只剩一条
#
#     binUI  ──►  bin2d / bin3d  ──►  binGPU  ──►  GL / Vulkan / D3D11
#
# 老的三份 GPU 后端（binui_gl / binui_vk / binui_d3d11）、设备层那份（binui_gpu）
# 和运行期选择器（binui_auto）已整份删除（共 ~12k 行，含 4 份 GLSL/HLSL），
# 所以这里没有 VK_INC、没有 D3D11_LIBS、没有 -lopengl32/-ld3dcompiler 之类的链接项。
# 那份删除的名单与理由见 src/backends/binui_ui.h 顶部与 docs/backend_layering.md 第 3c 步。

# 路径填充：覆盖率 → 条带。需要 bin2d 头/库（P2）
$(BUILD_DIR)/binui_paint.o: $(SRC_DIR)/backends/binui_paint.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -I$(INC_DIR) -Isrc -I$(BIN2D_DIR)/include -c $< -o $@

# bin2d 后端（0.26.0）：IBinUIGfx 的**唯一**实现，几何全部交给 bin2d::Canvas / GpuExec。
# 分开编成 .o 而不是并进核心库：不带窗口的调用方（bin2d_shot、纯离屏出图）
# 不该被拖上一个窗口外壳（binui_app.h）。
$(BUILD_DIR)/binui_bin2d.o: $(SRC_DIR)/backends/binui_bin2d.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -I$(INC_DIR) -Isrc -I$(BIN2D_DIR)/include $(BINGPU_INC) -c $< -o $@

$(BIN2D_A):
	@echo "[binui] missing bin2d static lib: $(BIN2D_A)"
	@echo "[binui] path fill uses bin2d coverage (P2 migration); run: make -C ../bin2D lib"
	@exit 1

$(BIN2D_TEXT_A):
	@echo "[binui] missing bin2d text module: $(BIN2D_TEXT_A)"
	@echo "[binui] glyph raster/shaping/atlas live in libbin2d_text; run: make -C ../bin2D text-lib"
	@exit 1

# 运行期后端选择（0.33.0 的 BinUIAuto）已在 0.37.x 删除：只剩 bin2d 一条路，
# 没有可挑的了。所以演示程序只链**一个**后端对象。
BACKEND_OBJS := $(BUILD_DIR)/binui_bin2d.o

$(LIB_A): $(LIB_OBJS) $(BIN2D_A)
	ar rcs $@ $(filter %.o,$^)

# =============================================
# 组件二进制：.bum.html -> .bum（产物落 build 目录，不污染源码树）
# =============================================
COMPONENT_BUMS := \
    $(BUILD_DIR)/assets/components/card.bum \
    $(BUILD_DIR)/assets/components/dock.bum

# 静态链接：构建脚本经常在 sh/PowerShell 等不同环境里调用它，
# 动态 MinGW 运行时（libstdc++-6.dll 等）在某些 PATH 下找不到（sh 下表现为退出码 127）
$(BIN_DIR)/binui_compile$(EXT): tools/binui_compile.cpp $(LIBS_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -o $@ $< $(LIBS_A)

$(BUILD_DIR)/assets/components/%.bum: $(EXA_DIR)/features/%.bum.html $(BIN_DIR)/binui_compile$(EXT)
	@mkdir -p $(dir $@)
	$(BIN_DIR)/binui_compile$(EXT) $< -o $@

.PHONY: components
components: $(COMPONENT_BUMS)

# =============================================
# 第三方源码（FreeType / HarfBuzz，体积大不入库）
# =============================================
.PHONY: deps
deps:
	@sh scripts/fetch-deps.sh

# =============================================
# tests (cross-platform)
#
# 只有三个主题套件 + C ABI 冒烟，一个套件一个二进制；
# 套件内部按"元素 / 动画 / 热重载…"分节，新增用例往对应文件的 namespace 里加，
# 不要为一个新特性再开一个 test_xxx.exe（历史包袱见 CHANGELOG 里 8 -> 4 的那次合并）。
# =============================================
TEST_BINS := \
    $(TEST_DIR)/test_css$(EXT) \
    $(TEST_DIR)/test_elements$(EXT) \
    $(TEST_DIR)/test_font$(EXT)

# test_css：CSS 属性 / 选择器 / 伪元素 / 编译器告警（只依赖核心库）
$(TEST_DIR)/test_css$(EXT): tests/test_css.cpp $(LIBS_A)
	@mkdir -p $(TEST_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -o $@ $< $(LIBS_A)

# test_elements：二进制格式 / 元素树 / 布局 / 命中 / 交互 / 动画 / 绘制几何 / 热重载
# 工作目录须为仓库根（用到 tests/fixtures/ 与 build-mingw/tmp/）
$(TEST_DIR)/test_elements$(EXT): tests/test_elements.cpp $(LIBS_A)
	@mkdir -p $(TEST_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -I$(EXA_DIR) -o $@ $< $(LIBS_A)

# test_font：字体服务（bin2d 的文字模块：FreeType 光栅 + HarfBuzz 整形）+ 输入法（IME）
# 头文件：bin2d/text.h（模块）+ font/binui_font_host.h（平台发现）
$(TEST_DIR)/test_font$(EXT): tests/test_font.cpp $(LIBS_A)
	@mkdir -p $(TEST_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -Isrc -I$(BIN2D_DIR)/include \
	    -o $@ $< $(LIBS_A) $(FONT_LIBS)

.PHONY: test
test: $(TEST_BINS)
	@echo "--- test_css ---"
	$(TEST_DIR)/test_css$(EXT)
	@echo "--- test_elements (run from project root) ---"
	$(TEST_DIR)/test_elements$(EXT)
	@echo "--- test_font ---"
	$(TEST_DIR)/test_font$(EXT)
ifdef ComSpec
	@echo "--- test_capi ---"
	$(TEST_DIR)/test_capi$(EXT)
	@echo "--- test_shell (shell x bin2d backend) ---"
	$(TEST_DIR)/test_shell$(EXT)
endif

# =============================================
# Win32 demos
# =============================================
ifdef ComSpec

# 外壳/字体的链接项。0.37.x 只留 bin2d 一条路，所以这里**没有** -lopengl32 /
# -ld3d11 / -ldxgi / -ld3dcompiler 了（那几项是 binui_gl / binui_vk / binui_d3d11 的）。
#   imm32   ：输入法候选窗定位（ImmGetContext / ImmSetCompositionWindow）
#   msimg32 ：AlphaBlend —— GDI 贴"预乘 alpha 位图"那条降级路径靠它
#   dwmapi  ：全屏守卫 / 远程会话判定
# FONT_LIBS 现在是 **libbin2d_text.a 的传递依赖**（字形光栅与整形在那个模块里，
# binUI 自己的源码已经一行 ft2build/harfbuzz 都没有了）。静态链接，产物自包含。
SHELL_LIBS := -ldwmapi -limm32 -lmsimg32
FONT_LIBS  := -Lthird_party/freetype/build -lfreetype \
              -Lthird_party/harfbuzz/build-static/src -lharfbuzz

# C ABI 的冒烟测试（无窗口：只测编译/加载/节点查询/改文本，需链 bin2d 后端对象）
TEST_BINS += $(TEST_DIR)/test_capi$(EXT)
test: $(TEST_DIR)/test_capi$(EXT)

# 外壳 × bin2d 后端（0.27.0）：子窗口模式下的消息/IME/脏区/图层/裁剪栈契约。
# 它守的是"三份 GPU 后端能不能删"——外壳只认 IBinUIGfx，换后端不该丢任何能力。
# 0.37.x 之后它守的就是**唯一**那条路。
# 需要一个窗口站（建的是隐藏父窗口下的子窗口），所以归在 Windows 段里。
TEST_BINS += $(TEST_DIR)/test_shell$(EXT)
test: $(TEST_DIR)/test_shell$(EXT)

$(TEST_DIR)/test_shell$(EXT): tests/test_shell.cpp $(BUILD_DIR)/binui_bin2d.o $(LIBS_A)
	@mkdir -p $(TEST_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -Isrc -I$(BIN2D_DIR)/include -o $@ $< \
	    $(BUILD_DIR)/binui_bin2d.o $(LIBS_A) $(BINGPU_LNK) $(WINLIBS) $(SHELL_LIBS) $(FONT_LIBS)

# 注意：这里必须直接写 $(BUILD_DIR)/libbinui_c.a，不能写 $(BINUI_C_A) ——
# 那个变量是后面用 := 定义的，GNU Make 在读到这一行时就展开成了空，
# 于是 test_capi 永远只依赖 .cpp，库更新后不重链，跑的是旧二进制。
$(TEST_DIR)/test_capi$(EXT): tests/test_capi.cpp $(BUILD_DIR)/libbinui_c.a $(LIBS_A)
	@mkdir -p $(TEST_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -o $@ $< $(BUILD_DIR)/libbinui_c.a $(LIBS_A) $(BINGPU_LNK) $(WINLIBS) $(SHELL_LIBS) $(FONT_LIBS)

# 9 组使用示例（一组一个 exe，一组只演示一件事）：功能 / 动画 / 应用壳 / 字体与输入法 /
# 按钮样式 / 自适应布局 / 2D 渲染 / 3D 渲染 / 纹理。产物在 $(DEMO_DIR)，与测试彻底分开。
DEMO_BINS := \
    $(DEMO_DIR)/features$(EXT) \
    $(DEMO_DIR)/animshow$(EXT) \
    $(DEMO_DIR)/appshow$(EXT) \
    $(DEMO_DIR)/fontshow$(EXT) \
    $(DEMO_DIR)/buttonshow$(EXT) \
    $(DEMO_DIR)/flexshow$(EXT) \
    $(DEMO_DIR)/svgdemo$(EXT) \
    $(DEMO_DIR)/cubedemo$(EXT) \
    $(DEMO_DIR)/texturedemo$(EXT) \

# 资源拷贝一律用 cp + 正斜杠路径。
# 原先写 `cmd /c copy /Y examples\x\y build-mingw\bin\x\y`：SHELL=sh 时反斜杠被 sh 当转义吃掉，
# cmd 收到不存在的路径，只打印 banner 就退出 —— 演示 UI 其实从没被拷到 bin/ 下（静默失败）。

# 标准组件库：demo 的 main.html 用 ../../components/xxx.bum.html 引用，
# UI 在 $(DEMO_DIR)/<demo>/main.html，往上两级正好是 $(BIN_DIR)/，所以拷到这里
STD_LIB_SRCS  := $(wildcard components/*.bum.html components/*.bum.xml)
STD_LIB_STAMP := $(BIN_DIR)/components/.stamp

$(STD_LIB_STAMP): $(STD_LIB_SRCS)
	@mkdir -p $(BIN_DIR)/components
	@cp -f $(STD_LIB_SRCS) $(BIN_DIR)/components/
	@touch $@

$(BUILD_DIR)/examples/%.o: $(EXA_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -Isrc -I$(EXA_DIR) -c $< -o $@

# UI 源文件也算依赖：不然改完 main.html，exe 不用重链，构建期那次 cp 不会重跑，
# 跑起来还是旧界面（只有 --ui 指到源码树才看到新界面）。
$(DEMO_DIR)/features$(EXT): $(BACKEND_OBJS) $(BUILD_DIR)/examples/features/main.o $(LIBS_A) $(COMPONENT_BUMS) $(EXA_DIR)/features/main.html $(EXA_DIR)/features/button.bum.html
	@mkdir -p $(DEMO_DIR)/features
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BACKEND_OBJS) $(BUILD_DIR)/examples/features/main.o $(LIBS_A) $(BINGPU_LNK) $(WINLIBS) $(SHELL_LIBS) $(FONT_LIBS)
	cp -f $(EXA_DIR)/features/main.html $(DEMO_DIR)/features/main.html
	cp -f $(EXA_DIR)/features/button.bum.html $(DEMO_DIR)/features/button.bum.html
	cp -f $(BUILD_DIR)/assets/components/card.bum $(DEMO_DIR)/features/card.bum
	cp -f $(BUILD_DIR)/assets/components/dock.bum $(DEMO_DIR)/features/dock.bum

$(DEMO_DIR)/animshow$(EXT): $(BACKEND_OBJS) $(BUILD_DIR)/examples/animshow/main.o $(LIBS_A) $(EXA_DIR)/animshow/main.html
	@mkdir -p $(DEMO_DIR)/animshow
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BACKEND_OBJS) $(BUILD_DIR)/examples/animshow/main.o $(LIBS_A) $(BINGPU_LNK) $(WINLIBS) $(SHELL_LIBS) $(FONT_LIBS)
	cp -f $(EXA_DIR)/animshow/main.html $(DEMO_DIR)/animshow/main.html

$(DEMO_DIR)/appshow$(EXT): $(BACKEND_OBJS) $(BUILD_DIR)/examples/appshow/main.o $(LIBS_A) $(EXA_DIR)/appshow/main.html $(EXA_DIR)/appshow/appshow-navitem.bum.html $(EXA_DIR)/appshow/appshow-card.bum.html $(EXA_DIR)/appshow/appshow-dialog.bum.html $(EXA_DIR)/appshow/appshow-bubble.bum.html
	@mkdir -p $(DEMO_DIR)/appshow
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BACKEND_OBJS) $(BUILD_DIR)/examples/appshow/main.o $(LIBS_A) $(BINGPU_LNK) $(WINLIBS) $(SHELL_LIBS) $(FONT_LIBS)
	cp -f $(EXA_DIR)/appshow/main.html $(DEMO_DIR)/appshow/main.html
	cp -f $(EXA_DIR)/appshow/appshow-navitem.bum.html $(DEMO_DIR)/appshow/appshow-navitem.bum.html
	cp -f $(EXA_DIR)/appshow/appshow-card.bum.html $(DEMO_DIR)/appshow/appshow-card.bum.html
	cp -f $(EXA_DIR)/appshow/appshow-dialog.bum.html $(DEMO_DIR)/appshow/appshow-dialog.bum.html
	cp -f $(EXA_DIR)/appshow/appshow-bubble.bum.html $(DEMO_DIR)/appshow/appshow-bubble.bum.html

$(DEMO_DIR)/fontshow$(EXT): $(BACKEND_OBJS) $(BUILD_DIR)/examples/fontshow/main.o $(LIBS_A) $(EXA_DIR)/fontshow/main.html
	@mkdir -p $(DEMO_DIR)/fontshow
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BACKEND_OBJS) $(BUILD_DIR)/examples/fontshow/main.o $(LIBS_A) $(BINGPU_LNK) $(WINLIBS) $(SHELL_LIBS) $(FONT_LIBS)
	cp -f $(EXA_DIR)/fontshow/main.html $(DEMO_DIR)/fontshow/main.html

$(DEMO_DIR)/flexshow$(EXT): $(BACKEND_OBJS) $(BUILD_DIR)/examples/flexshow/main.o $(LIBS_A) $(EXA_DIR)/flexshow/main.html
	@mkdir -p $(DEMO_DIR)/flexshow
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BACKEND_OBJS) $(BUILD_DIR)/examples/flexshow/main.o $(LIBS_A) $(BINGPU_LNK) $(WINLIBS) $(SHELL_LIBS) $(FONT_LIBS)
	cp -f $(EXA_DIR)/flexshow/main.html $(DEMO_DIR)/flexshow/main.html

$(DEMO_DIR)/svgdemo$(EXT): $(BACKEND_OBJS) $(BUILD_DIR)/examples/svgdemo/main.o $(LIBS_A) $(EXA_DIR)/svgdemo/main.html
	@mkdir -p $(DEMO_DIR)/svgdemo
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BACKEND_OBJS) $(BUILD_DIR)/examples/svgdemo/main.o $(LIBS_A) $(BINGPU_LNK) $(WINLIBS) $(SHELL_LIBS) $(FONT_LIBS)
	cp -f $(EXA_DIR)/svgdemo/main.html $(DEMO_DIR)/svgdemo/main.html

$(DEMO_DIR)/cubedemo$(EXT): $(BACKEND_OBJS) $(BUILD_DIR)/examples/cubedemo/main.o $(LIBS_A) $(EXA_DIR)/cubedemo/main.html
	@mkdir -p $(DEMO_DIR)/cubedemo
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BACKEND_OBJS) $(BUILD_DIR)/examples/cubedemo/main.o $(LIBS_A) $(BINGPU_LNK) $(WINLIBS) $(SHELL_LIBS) $(FONT_LIBS)
	cp -f $(EXA_DIR)/cubedemo/main.html $(DEMO_DIR)/cubedemo/main.html

$(DEMO_DIR)/texturedemo$(EXT): $(BACKEND_OBJS) $(BUILD_DIR)/examples/texturedemo/main.o $(LIBS_A) $(EXA_DIR)/texturedemo/main.html
	@mkdir -p $(DEMO_DIR)/texturedemo
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BACKEND_OBJS) $(BUILD_DIR)/examples/texturedemo/main.o $(LIBS_A) $(BINGPU_LNK) $(WINLIBS) $(SHELL_LIBS) $(FONT_LIBS)
	cp -f $(EXA_DIR)/texturedemo/main.html $(DEMO_DIR)/texturedemo/main.html

$(DEMO_DIR)/buttonshow$(EXT): $(BACKEND_OBJS) $(BUILD_DIR)/examples/buttonshow/main.o $(LIBS_A) $(EXA_DIR)/buttonshow/main.html
	@mkdir -p $(DEMO_DIR)/buttonshow
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BACKEND_OBJS) $(BUILD_DIR)/examples/buttonshow/main.o $(LIBS_A) $(BINGPU_LNK) $(WINLIBS) $(SHELL_LIBS) $(FONT_LIBS)
	cp -f $(EXA_DIR)/buttonshow/main.html $(DEMO_DIR)/buttonshow/main.html

# =============================================
# 示例后端 = bin2d（唯一一条路）
#
# 0.27.0 曾把 bin2d 版做成并列的一套 `*_2d.exe`（`make demos-2d`）；0.29.0 起
# **默认就是它**；0.37.x 把另外四份（gl / vk / d3d11 / gpu + BinUIAuto）删干净之后，
# `examples/*.exe` 只链 `binui_bin2d.o`，`demos-vk` / `demos-d3d11` 两个对照构建
# 连同 `-DBINUI_GPU_VK|D3D11` 宏一起退场。
#
# `make demos-2d` 保留成别名（脚本、文档、CI 里那批命令继续能用）。产物名从不带
# `_2d` 后缀（`appshow.exe` 而不是 `appshow_2d.exe` —— 用户是要双击那个的）。
# =============================================

.PHONY: demos-2d
demos-2d: $(DEMO_BINS)
	@echo "bin2d demos (default) -> $(DEMO_BINS)"

# =============================================
# C ABI：给 Python / Go / Rust 等宿主语言的绑定入口（include/binui_c.h）
#   build-mingw/libbinui_c.a    静态库：C / C++ / Rust 直接链
#   build-mingw/bin/binui_c.dll 动态库：Python ctypes / Go cgo（免编译，随包分发）
# =============================================
BINUI_C_A   := $(BUILD_DIR)/libbinui_c.a
BINUI_C_DLL := $(BIN_DIR)/binui_c.dll

$(BUILD_DIR)/binui_c.o: $(SRC_DIR)/binui_c.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -Isrc -c $< -o $@

# DLL 版单独编一份：-DBINUI_C_BUILD_DLL 让头文件把函数标成 __declspec(dllexport)
$(BUILD_DIR)/binui_c_dll.o: $(SRC_DIR)/binui_c.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -DBINUI_C_BUILD_DLL -I$(INC_DIR) -Isrc -c $< -o $@

# 0.28.1：C ABI 的默认后端从 GL 换成 bin2d（见 src/binui_c.cpp:211）。静态库要把
# binui_bin2d.o 打进去，动态库要把它链进来 —— 两处都必须换，只换一处会在 DLL 版
# 上保留一条"还依赖 GL"的隐式路径。
$(BINUI_C_A): $(BUILD_DIR)/binui_c.o $(BACKEND_OBJS)
	ar rcs $@ $^

# RTLINK 同样要带上：Windows 上 libstdc++-6.dll / libgcc_s_seh-1.dll 不能指望宿主有，
# freetype/harfbuzz 也走静态库 —— 产物除系统 DLL 外自包含
$(BINUI_C_DLL): $(BUILD_DIR)/binui_c_dll.o $(BACKEND_OBJS) $(LIBS_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(LDFLAGS) $(RTLINK) -shared -o $@ $^ $(BINGPU_LNK) $(WINLIBS) $(SHELL_LIBS) $(FONT_LIBS) \
	    -Wl,--out-implib=$(BIN_DIR)/libbinui_c.dll.a

.PHONY: capi
capi: $(BINUI_C_A) $(BINUI_C_DLL)
	@echo "C ABI -> $(BINUI_C_A) $(BINUI_C_DLL)"

.PHONY: examples
examples: $(DEMO_BINS) $(STD_LIB_STAMP)

all: $(LIBS_A) $(TEST_BINS) $(DEMO_BINS) $(STD_LIB_STAMP) $(BINUI_C_A) $(BINUI_C_DLL)

# 调试工具（可选，不入 all）：离屏渲染 / 窗口抓取 / 窗口枚举
#
# 0.37.x 退场的工具（都绑在被删的后端上）：
#   gl_shot / vk_shot / d3d11_shot —— 三份后端的离屏抓图（原 `pixels` / `pixels-vk` /
#                                    `pixels-d3d11` 三条门禁的执行体）
#   gpu_shot                      —— binUI 自己那份 binui_gpu 的离屏抓图（原 `pixels-gpu`）
#   vk_window / vk_stress         —— Vulkan 的交换链验收与鲁棒性压测
# 现在出图只剩 bin2d_shot（CPU 权威）与 bin2d_window（真窗口呈现），
# GPU 侧的像素门禁是 `pixels-bin2d-gpu`（bin2d_shot + BINUI2D_GPU=1）。
TOOL_BINS := \
    $(BIN_DIR)/binui_compile$(EXT) \
    $(BIN_DIR)/binui_export$(EXT) \
    $(BIN_DIR)/bin2d_shot$(EXT) \
    $(BIN_DIR)/bin2d_window$(EXT) \
    $(BIN_DIR)/partial_shot$(EXT) \
    $(BIN_DIR)/draw_bench$(EXT) \
    $(BIN_DIR)/path_bench$(EXT) \
    $(BIN_DIR)/window_capture$(EXT) \
    $(BIN_DIR)/enum_windows$(EXT) \
    $(BIN_DIR)/pixel_diff$(EXT) \
    $(BIN_DIR)/baseline_snapshot$(EXT) \
    $(BIN_DIR)/check_examples$(EXT)

# 把 .bui 还原成打包前的目录结构（读同目录的 binui.res，header-only，不用链接库）
$(BIN_DIR)/binui_export$(EXT): tools/binui_export.cpp
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -o $@ $<

# 示例 UI 零警告检查（CMake 也会注册成 ctest 用例）
$(BIN_DIR)/check_examples$(EXT): tools/check_examples.cpp $(LIBS_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -o $@ $< $(LIBS_A) $(FONT_LIBS)

# "电篱笆"版的零警告检查：每个 C++ 堆块末尾挂一个不可访问页，越界写/读已释放内存
# 立刻崩并打印分配栈。本机 mingw 没有 libasan、也没权限开 page heap，这是替代品 ——
# v0.16.46 就是靠它定位到编译器里一处悬空引用（栈引用被 push_back 扩容搞失效）。
# 慢两个数量级，所以单独成目标，不进 all。
$(BIN_DIR)/check_examples_guard$(EXT): tools/check_examples.cpp tools/guard_alloc.cpp $(LIBS_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -o $@ tools/check_examples.cpp \
	    tools/guard_alloc.cpp $(LIBS_A) $(FONT_LIBS)

.PHONY: check-guard
check-guard: $(BIN_DIR)/check_examples_guard$(EXT)
	$(BIN_DIR)/check_examples_guard$(EXT) examples

# ★ 这条规则（以及下面几个直接编译 .cpp 的工具）**没有头依赖**：
#   改了 include/binui_paint.h、src/backends/binui_draw_nodes.h 这类共享头之后，
#   这些 exe 不会重编，验出来的还是旧行为（本仓库在 §7.6.4 踩过一次，0.16.13 又踩了一次）。
#   所以离屏工具改成"先编 .o（带 -MMD）再链接"。
$(BIN_DIR)/window_capture$(EXT): tools/window_capture.cpp
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -o $@ $< -lgdiplus -lgdi32 -luser32
$(BIN_DIR)/enum_windows$(EXT): tools/enum_windows.cpp
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -o $@ $< -luser32

# bin2d 后端的离屏出图：**不需要窗口**（像素落在 bin2d::Surface 上，写完就存 PNG）。
# 这是"binUI 后端交给 bin2d"最直接的验证工具 —— 无 GPU、无驱动、可进 CI。
# `pixels-bin2d`（CPU 权威，0 容差）与 `pixels-bin2d-gpu`（BINUI2D_GPU=1，对 docs/pixels）
# 两条像素门禁都靠它出图。
$(BUILD_DIR)/bin2d_shot.o: tools/bin2d_shot.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -MMD -MP -I$(INC_DIR) -Isrc -c $< -o $@

$(BIN_DIR)/bin2d_shot$(EXT): $(BUILD_DIR)/bin2d_shot.o $(BUILD_DIR)/binui_bin2d.o $(LIBS_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -o $@ $^ -lgdiplus -lgdi32 -luser32 -lole32 \
	    $(FONT_LIBS) $(BINGPU_LNK)

# bin2d 后端的**真窗口**验证：窗口 + 呈现 + 帧泵。这是"能不能当窗口后端用"的证据，
# 也是 bin2d_shot 的对照（离屏能出图 ≠ 能上屏）。
$(BUILD_DIR)/bin2d_window.o: tools/bin2d_window.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -MMD -MP -I$(INC_DIR) -Isrc -c $< -o $@

$(BIN_DIR)/bin2d_window$(EXT): $(BUILD_DIR)/bin2d_window.o $(BUILD_DIR)/binui_bin2d.o $(LIBS_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -o $@ $^ -lgdiplus -lgdi32 -luser32 -lole32 \
	    $(FONT_LIBS) $(BINGPU_LNK)

# partial_shot：局部重绘一致性检查（hover 脏矩形 vs 整帧）。走 .o + -MMD，
# 免得改了 binui_draw_nodes.h / binui_paint.h 之后工具还跑旧行为（这个仓库在
# gl_shot 上踩过两次，见下文注释）。
# 0.37.x：工具改成离屏 BinUI2D（原来是隐藏窗口 + GL 上下文 + BitBlt 抓像素），
# 所以 **0 容差** —— 纯 CPU 光栅同状态必须逐位相同。
$(BUILD_DIR)/partial_shot.o: tools/partial_shot.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -MMD -MP -I$(INC_DIR) -Isrc -c $< -o $@

$(BIN_DIR)/partial_shot$(EXT): $(BUILD_DIR)/partial_shot.o $(BUILD_DIR)/binui_bin2d.o $(LIBS_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -o $@ $^ \
	    -lgdiplus -lgdi32 -luser32 -lole32 $(BINGPU_LNK) $(SHELL_LIBS) $(FONT_LIBS)

# draw_bench：绘制遍历的 CPU 基准（无 GPU / 无窗口 / 确定性）。
# 不带任何后端 .o —— 遍历只认 IBinUIGfx，计数后端在工具里就地实现，
# 所以同一个数字在 GL/VK/D3D11 三端之间可比。--hash 输出整个原语流的哈希，
# 用来证明"缓存/重构没有改变发出的几何"（比像素更严格）。
$(BUILD_DIR)/draw_bench.o: tools/draw_bench.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -MMD -MP -I$(INC_DIR) -Isrc -c $< -o $@

$(BIN_DIR)/draw_bench$(EXT): $(BUILD_DIR)/draw_bench.o $(LIBS_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -o $@ $^

# ★ 工具的 .o 必须**单独成规则**：以前把编译写进 exe 的 recipe，于是 -MMD 生成的
#   build-mingw/vk_window.d 谁都不依赖，结果"改了共享头（binui_draw_nodes.h 等）但工具
#   不重编"——直接链了旧 .o，验出来的行为是旧的（本轮就在 BINUI_DRAW_PROF 上踩到）。
#   （vk_window 本身已在 0.37.x 删除，这条教训留给剩下的工具。）

$(BIN_DIR)/pixel_diff$(EXT): tools/pixel_diff.cpp
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -o $@ $< -lgdiplus -lgdi32 -luser32 -lole32

# 迁移基线快照（P1）：把三端像素、原语流哈希、bin2d 画质数字压成一份可提交的清单
$(BIN_DIR)/baseline_snapshot$(EXT): tools/baseline_snapshot.cpp
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -o $@ $< -lgdiplus -lgdi32 -luser32 -lole32

.PHONY: tools
tools: $(TOOL_BINS)

# 局部重绘一致性（P0 回归）：hover 只重画脏矩形后，画面必须与"同样状态的整帧重绘"
# 逐像素一致。这类错在整帧像素基准里看不见（clip=整屏时剔除判据的边缘情况碰不到
# 任何节点），只能靠这里。见 docs/issues/10-partial-redraw-cull.md。
#
# 0.37.x 现状（如实记账）：工具已从 GL 后端移植到离屏 BinUI2D（不需要窗口/GL），
# 判据从"diff<=64 且 max<=1"收紧到 **0 容差**。
#
# 0.38.0 起它是**绿的**（4 个 hover 点全 diff=0）。此前的红是这样一份欠账：
#   278,222 -> diff=1825 (clip 内 39 / clip 外 1786) max=15
# 落点不在"阴影/模糊图层只按元素盒裁剪"，而在 bin2d 的**圆角矩形 CPU 快路径**：
#   · raster.cpp FillRoundedSolidDirect：裁剪切掉形状中段时 midBegin/midEnd 倒挂，
#     下圆角带从裁剪区**上方**开画 → 脏矩形外那条 ~12px 的半透明带（1786 个像素）；
#   · raster.cpp CompositeRowFused：c>=1 那一支用前缀和 run 代替本像素的覆盖率，
#     裁剪边界那一列（run 还是 0）被整列判掉 → clip 内的单列差异（39 个像素）；
#   · canvas.cpp compositeSurface：合成只按 target 边界裁，没和当前 clipRect 求交 ——
#     外阴影按偏移贴回时会溢出脏矩形（GPU 那条出口本来就是按 scissor 裁的）。
# 判据本身不动：**不要靠放宽容差让它变绿**。
PARTIAL_CASES := 278,222 358,221 99,116 830,117
.PHONY: partial
partial: $(BIN_DIR)/partial_shot$(EXT)
	@fail=0; for c in $(PARTIAL_CASES); do \
	    x=$$(echo $$c | cut -d, -f1); y=$$(echo $$c | cut -d, -f2); \
	    echo "--- buttonshow hover ($$x,$$y) ---"; \
	    $(BIN_DIR)/partial_shot$(EXT) --check $(EXA_DIR)/buttonshow/main.html - $$x $$y 40 || fail=1; \
	done; \
	echo "--- svgdemo hover (any in-screen button) ---"; \
	exit $$fail
	@echo "tools -> $(TOOL_BINS)"

# =============================================
# 绘制基准（P0-1 / P0-4 的量化入口，v0.16.33）
#
# 以前 docs/future.md 里只有几句一次性手测数字（"约 1052 µs/帧、约 9800 个四边形"），
# 优化完没法证明真的快了，重构也没法证明没改坏。现在：
#   make bench-draw  → 纯 CPU 遍历（计数后端，无 GPU/无窗口/确定性，跨后端可比）
# （`make bench-gl` 已在 0.37.x 删除：它量的那一个后端 binui_gl 整份退场了。现在
#   "一帧到底花在遍历还是发射上"的对照数字看 probe_cost 的 bin2d/GPU 两行。）
# `draw_bench <ui> --hash` 输出整个原语流的哈希：缓存/重构前后必须一致。
# =============================================
BENCH_CASES := features animshow appshow fontshow flexshow svgdemo cubedemo texturedemo buttonshow
BENCH_ASSET := docs/issues/assets

.PHONY: bench-draw
bench-draw: $(BIN_DIR)/draw_bench$(EXT)
	@echo "--- traversal (CPU only: layout + bands + cull + primitive dispatch) ---"
	@for d in $(BENCH_CASES); do \
	    $(BIN_DIR)/draw_bench$(EXT) $(EXA_DIR)/$$d/main.html --frames 120 --warm 10; \
	done
	@echo "--- band blow-up case (12 x 135deg gradient cards) ---"
	@$(BIN_DIR)/draw_bench$(EXT) $(BENCH_ASSET)/bench-grad.html --frames 120 --warm 10

$(BUILD_DIR)/path_bench.o: tools/path_bench.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -MMD -MP -I$(INC_DIR) -Isrc -c $< -o $@

$(BIN_DIR)/path_bench$(EXT): $(BUILD_DIR)/path_bench.o $(LIBS_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -o $@ $^

# 路径填充几何的 CPU 成本（P0-3）：纯 CPU、无窗口、确定性，硬边/AA 分开报
.PHONY: bench-path
bench-path: $(BIN_DIR)/path_bench$(EXT)
	@$(BIN_DIR)/path_bench$(EXT) 50

# ---- 迁移开工条件：bin2d 的 FillPath 与现实现（条带 + fillRect）同口径对拍 ----
# 见 docs/migrate-to-bin2d.md：这条数字不达标就不动 binUI 的路径光栅。
# BIN2D_DIR / BIN2D_A 已在文件上方定义（P2 起 bin2d 是硬依赖）。
.PHONY: gate-migrate
gate-migrate: $(LIB_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -I$(BIN2D_DIR)/include $(RTLINK) \
	    -o $(BIN_DIR)/migrate_gate$(EXT) tools/migrate_gate.cpp $(LIBS_A)
	@$(BIN_DIR)/migrate_gate$(EXT) 200



# =============================================
# 像素回归（本机跑，不入 CI）
#
# 为什么不在 CI 里：GitHub 的 Windows runner 是软件光栅化，GPU 那一条（pixels-bin2d-gpu）
# 的 AA 边缘与真实 GPU 不一致，同一份代码会稳定地差出百分之几；基准图只在**本机**有意义。
# （pixels-bin2d 是纯 CPU，CI 里也能跑。）
#
# **2026-11-20 之后只剩两条门禁**：
#   make pixels-bin2d      CPU 权威（bin2d 的扫描线覆盖率，对 tests/pixels-bin2d，**0 容差**）
#   make pixels-bin2d-gpu  GPU 唯一门禁（BINUI2D_GPU=1 走 binUI → bin2d → binGPU，
#                          对 docs/pixels，0.5% / 通道 8）
#
# 一起退场的三条（`pixels` / `pixels-vk` / `pixels-d3d11` / `pixels-gpu` 与它们依赖的
# gl_shot / vk_shot / d3d11_shot / gpu_shot）：那几条是"三个后端互拍"的门禁，
# 后端删了门禁自然没有执行体。docs/pixels 还留着，但它的角色变了 ——
# 不再是"GL 自回归的基准"，而是 **GPU 那条路的参照图**（当年 GL 4× MSAA 出的基线）。
# 详见 docs/backend_layering.md 第 3c 步。
# =============================================
PIXEL_CASES := circle display-none2 probe radius-matrix svgpath svggrad svgjoin svgstroke svgfinish svgaa opacity grad-angle shadow image-round image-mip text-shadow text-shadow-multi svg-filter
PIXEL_GOLD  := docs/pixels
PIXEL_TOL   ?= 0.5

# =============================================
# bin2d 后端的像素回归（0.26.0）—— CPU 像素权威
#
# **容差 0**。bin2d 后端是纯 CPU 光栅，没有驱动、没有 GPU、没有线程调度参与，
# 同一份输入必须逐位相同 —— 有差异就是真回归。
#
# 为什么不和 docs/pixels 比：两条路的覆盖率 AA 实现**本质不同**
# （bin2d 用 8× 子采样解析覆盖率，GPU 用 1px 模板/SDF），边缘必然差几级。
# 那是换实现的标准差，不是 bug。
#
# 为什么不需要窗口：bin2d 后端没有交换链，像素直接落在 Surface 上（见 tools/bin2d_shot.cpp）。
# CI 里也能跑。
# =============================================
PIXEL2D_OUT  := $(BUILD_DIR)/pixels-bin2d
PIXEL2D_GOLD := tests/pixels-bin2d

.PHONY: pixels-bin2d pixels-bin2d-update
pixels-bin2d: tools
	@mkdir -p $(PIXEL2D_OUT)
	@if [ ! -d $(PIXEL2D_GOLD) ]; then \
	    echo "[pixels-bin2d] no baseline yet - run: make pixels-bin2d-update"; exit 1; fi
	@fail=0; \
	for c in $(PIXEL_CASES); do \
	    $(BIN_DIR)/bin2d_shot$(EXT) docs/issues/assets/$$c.html $(PIXEL2D_OUT)/$$c.png >/dev/null 2>&1 \
	        || { echo "render failed: $$c"; fail=1; continue; }; \
	    $(BIN_DIR)/pixel_diff$(EXT) $(PIXEL2D_GOLD)/$$c.png $(PIXEL2D_OUT)/$$c.png 0 \
	        $(PIXEL2D_OUT)/$$c.diff.png >/dev/null 2>&1 \
	        || { echo "--- changed: $$c ---"; \
	             $(BIN_DIR)/pixel_diff$(EXT) $(PIXEL2D_GOLD)/$$c.png $(PIXEL2D_OUT)/$$c.png 0 \
	                 $(PIXEL2D_OUT)/$$c.diff.png; fail=1; }; \
	done; \
	if [ $$fail -eq 0 ]; then echo "[pixels-bin2d] all cases pixel-identical (tolerance 0)"; fi; \
	exit $$fail

pixels-bin2d-update: tools
	@mkdir -p $(PIXEL2D_GOLD)
	@for c in $(PIXEL_CASES); do \
	    $(BIN_DIR)/bin2d_shot$(EXT) docs/issues/assets/$$c.html $(PIXEL2D_GOLD)/$$c.png >/dev/null 2>&1 || exit 1; \
	    echo "updated baseline: $(PIXEL2D_GOLD)/$$c.png"; \
	done

# =============================================
# bin2d **GPU 执行体**的像素回归（2026-11-20）：binUI → bin2d → binGPU
#
# **这是 0.37.x 之后唯一的 GPU 像素门禁**。这条路画出来的像素，正确的参照是
# docs/pixels（当年 GL 4× MSAA 出的基线），**不是** tests/pixels-bin2d —— 后者是
# bin2d 的 CPU 光栅（8 子采样扫描线），拿它当参照等于在比"两个不同的滤波器"。
# 所以判据沿用当年那三条跨后端门禁的口径：0.5% 容差、单通道 8 级，逐页打印数字。
#
# BINUI2D_GPU=1 让 bin2d_shot 建一个 HEADLESS 的 bin2d::GpuExec（绘制在设备上，
# present() 之后 readback 回 Surface），像素照样从 pixels() 出，PNG 编码不变。
# 后端与 MSAA 档由 binGPU 的 bingpu_render_plan 定（VK → 4×，GL/D3D11 → 1×），
# bin2d 这边不再自己写"哪个后端能开"。
#
# 与已删除的 pixels-gpu（binUI 自己那份 binui_gpu）的差别只有一处：**语义层怎么切图元**。
# 两者共用同一套 binGPU 设备/管线/片元，所以"边缘 + 缩小图片"的页（circle、
# radius-matrix、image-round、image-mip、grad-angle）在容差内接近 0。
# =============================================
PIXEL2D_GPU_OUT := $(BUILD_DIR)/pixels-bin2d-gpu

# 已知会不一样的一页。**不是质量档的账**（MSAA 1× 与 4× 给出同一组数字），
# 而是 binUI 的 bin2d 后端在这一层的语义缺口，记在这里当欠账而不是当容差：
#
#   text-shadow-multi —— bin2d 的文字阴影走自己的 BlurSigma（σ 语义），与 binUI 老 GL
#                        后端的阴影模糊不是同一个核（边缘几级到十几级的差）。同一页
#                        CPU 路径差 1.344%，GPU 执行体反而更近（1.277%）。
#
# svg-filter 曾经在这张名单上（12.921% / max 204）：那时 GPU 执行体**没有离屏层**，
# bin2d 的 pushOffscreen 出口是 Surface，于是 `filter: blur()` / 外阴影在
# binui_bin2d.cpp 的 pushLayerEx 里降级成"纯组 alpha"。bin2d 0.22.5 补上设备侧内容层
# （Canvas::pushContentLayer/popContentLayer + draw/fillContentLayer）之后，那一页是
# 0.000% / max 6 —— 比 CPU 路径（0.511%）还近，于是从名单里摘掉。
#
# 名单里的页照常打印数字，只不判失败；**任何新页越线都算红**（门禁仍然有效）。
PIXEL2D_GPU_KNOWN := text-shadow-multi

.PHONY: pixels-bin2d-gpu
pixels-bin2d-gpu: tools
	@mkdir -p $(PIXEL2D_GPU_OUT)
	@fail=0; \
	for c in $(PIXEL_CASES); do \
	    printf '%-20s ' "$$c"; \
	    BINUI2D_GPU=1 $(BIN_DIR)/bin2d_shot$(EXT) docs/issues/assets/$$c.html $(PIXEL2D_GPU_OUT)/$$c.png >/dev/null 2>&1 \
	        || { echo "render failed"; fail=1; continue; }; \
	    if ! $(BIN_DIR)/pixel_diff$(EXT) $(PIXEL_GOLD)/$$c.png $(PIXEL2D_GPU_OUT)/$$c.png $(PIXEL_TOL) $(PIXEL2D_GPU_OUT)/$$c.diff.png; then \
	        if echo " $(PIXEL2D_GPU_KNOWN) " | grep -q " $$c "; then \
	            echo "  ^ known diff (reason in Makefile)"; \
	        else \
	            fail=1; \
	        fi; \
	    fi; \
	done; \
	exit $$fail


# 注意（踩过）：recipe 的**命令文本**一律用 ASCII。mingw32-make 调 Git 的 sh 时，
# 某些中文字符（实测「没构建设」「（）」）会让 sh 报 "unexpected EOF while looking for
# matching `"`" —— 整行命令解析失败、目标直接不动，报错还指向 Makefile 行号，极难定位。
# 中文只放在注释里；要在命令里输出中文，改成 ASCII（或者写进脚本文件再调用）。
# =============================================

else

all: $(LIBS_A) $(TEST_BINS)
	@echo "binUI: non-Windows platform, demos skipped (lib + tests built)"

endif

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)

# 自动头文件依赖（-MMD -MP 生成 .d，头文件变更时正确重编对应 .o）
# 注意：每个输出子目录都要列进来，漏一个就会出现"改了头文件但这个目录不重编"
-include $(wildcard $(BUILD_DIR)/*.d) \
         $(wildcard $(BUILD_DIR)/compiler/*.d) \
         $(wildcard $(BUILD_DIR)/renderer/*.d) \
         $(wildcard $(BUILD_DIR)/font/*.d) \
         $(wildcard $(BUILD_DIR)/backends/*.d) \
         $(wildcard $(BUILD_DIR)/platform/*.d) \
         $(wildcard $(BUILD_DIR)/common/*.d) \
         $(wildcard $(BUILD_DIR)/bin/*.d) \
         $(wildcard $(BUILD_DIR)/bin/tests/*.d) \
         $(wildcard $(BUILD_DIR)/examples/*.d) \
         $(wildcard $(BUILD_DIR)/examples/*/*.d)
