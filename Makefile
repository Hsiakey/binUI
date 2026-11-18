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
    $(BUILD_DIR)/font/binui_font_renderer.o \
    $(BUILD_DIR)/common/binui_link.o \
    $(BUILD_DIR)/binui_paint.o \
    $(BUILD_DIR)/binui_charts.o
LIB_A    := $(BUILD_DIR)/libbinui.a

# bin2d（P2 迁移）：<path> 的填充光栅化在 bin2d 里做（`bin2d::RasterizeCoverage`），
# 见 src/backends/binui_paint.cpp 与 docs/migrate-to-bin2d.md。所以 bin2d 成了**硬依赖**：
# 没构建过就明确报错，而不是静默少一堆符号。先 `make -C ../bin2D lib`。
BIN2D_DIR := ../bin2D
BIN2D_A   := $(BIN2D_DIR)/build-mingw/libbin2d.a
LIBS_A    := $(LIB_A) $(BIN2D_A)

# 通用源规则（-Isrc：核心源里用 "backends/xxx.h" 这种相对 src/ 的路径引用后端头）
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -Isrc -c $< -o $@

# binui_font_renderer：需要 third_party 的 FreeType/HarfBuzz 头
$(BUILD_DIR)/font/%.o: $(SRC_DIR)/font/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -Ithird_party/freetype/include -Ithird_party/harfbuzz/src -c $< -o $@

# 后端（src/backends/）与 Win32 外壳（src/platform/）：对象路径保持在 build-mingw 根下，
# 演示程序的链接规则不用跟着改
$(BUILD_DIR)/binui_gl.o: $(SRC_DIR)/backends/binui_gl.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -Isrc -c $< -o $@

# Vulkan 后端（P2）：需要 third_party/vulkan 的头；运行期只 LoadLibrary("vulkan-1.dll")，
# 不链接 vulkan-1.lib，所以构建不需要 Vulkan SDK。
VK_INC   := -Ithird_party/vulkan/include
$(BUILD_DIR)/binui_vk.o: $(SRC_DIR)/backends/binui_vk.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -Isrc $(VK_INC) -c $< -o $@

# D3D11 后端（P7）：与 GL / Vulkan 同构，实现同一份 IBinUIGfx。
# 链接 d3d11 / dxgi / d3dcompiler（运行期着色器编译）；dwmapi 供全屏守卫与远程会话判定。
D3D11_LIBS := -ld3d11 -ldxgi -ld3dcompiler -ldwmapi
$(BUILD_DIR)/binui_d3d11.o: $(SRC_DIR)/backends/binui_d3d11.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -Isrc -c $< -o $@

# 路径填充：覆盖率 → 条带。**唯一**需要 bin2d 头/库的目标（P2）
$(BUILD_DIR)/binui_paint.o: $(SRC_DIR)/backends/binui_paint.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -I$(INC_DIR) -Isrc -I$(BIN2D_DIR)/include -c $< -o $@

$(BIN2D_A):
	@echo "[binui] 缺 bin2d 静态库：$(BIN2D_A)"
	@echo "[binui] 路径填充已经改用 bin2d 的覆盖率（P2 迁移），先跑：make -C ../bin2D lib"
	@exit 1

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

# test_font：字体服务（FreeType + HarfBuzz）+ 输入法（IME）的 UTF-16 光标桥接
$(TEST_DIR)/test_font$(EXT): tests/test_font.cpp $(LIBS_A)
	@mkdir -p $(TEST_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -Ithird_party/freetype/include -Ithird_party/harfbuzz/src \
	    -o $@ $< $(LIBS_A) -Lthird_party/freetype/build -lfreetype \
	    -Lthird_party/harfbuzz/build-static/src -lharfbuzz

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
endif

# =============================================
# Win32 demos
# =============================================
ifdef ComSpec

# OpenGL 后端（WGL + FreeType/HarfBuzz 静态库）
# imm32：输入法候选窗定位（ImmGetContext / ImmSetCompositionWindow）
# msimg32：AlphaBlend —— GDI 画布的 box-shadow 靠它贴"预乘 alpha 位图"（v23）
GL_LIBS := -lopengl32 -ldwmapi -limm32 -lmsimg32 -Lthird_party/freetype/build -lfreetype -Lthird_party/harfbuzz/build-static/src -lharfbuzz

# C ABI 的冒烟测试（无窗口：只测编译/加载/节点查询/改文本，需链 GL 后端对象）
TEST_BINS += $(TEST_DIR)/test_capi$(EXT)
test: $(TEST_DIR)/test_capi$(EXT)

# 注意：这里必须直接写 $(BUILD_DIR)/libbinui_c.a，不能写 $(BINUI_C_A) ——
# 那个变量是后面用 := 定义的，GNU Make 在读到这一行时就展开成了空，
# 于是 test_capi 永远只依赖 .cpp，库更新后不重链，跑的是旧二进制。
$(TEST_DIR)/test_capi$(EXT): tests/test_capi.cpp $(BUILD_DIR)/libbinui_c.a $(LIBS_A)
	@mkdir -p $(TEST_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -o $@ $< $(BUILD_DIR)/libbinui_c.a $(LIBS_A) $(WINLIBS) $(GL_LIBS)

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
$(DEMO_DIR)/features$(EXT): $(BUILD_DIR)/binui_gl.o $(BUILD_DIR)/examples/features/main.o $(LIBS_A) $(COMPONENT_BUMS) $(EXA_DIR)/features/main.html $(EXA_DIR)/features/button.bum.html
	@mkdir -p $(DEMO_DIR)/features
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/binui_gl.o $(BUILD_DIR)/examples/features/main.o $(LIBS_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/features/main.html $(DEMO_DIR)/features/main.html
	cp -f $(EXA_DIR)/features/button.bum.html $(DEMO_DIR)/features/button.bum.html
	cp -f $(BUILD_DIR)/assets/components/card.bum $(DEMO_DIR)/features/card.bum
	cp -f $(BUILD_DIR)/assets/components/dock.bum $(DEMO_DIR)/features/dock.bum

$(DEMO_DIR)/animshow$(EXT): $(BUILD_DIR)/binui_gl.o $(BUILD_DIR)/examples/animshow/main.o $(LIBS_A) $(EXA_DIR)/animshow/main.html
	@mkdir -p $(DEMO_DIR)/animshow
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/binui_gl.o $(BUILD_DIR)/examples/animshow/main.o $(LIBS_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/animshow/main.html $(DEMO_DIR)/animshow/main.html

$(DEMO_DIR)/appshow$(EXT): $(BUILD_DIR)/binui_gl.o $(BUILD_DIR)/examples/appshow/main.o $(LIBS_A) $(EXA_DIR)/appshow/main.html $(EXA_DIR)/appshow/appshow-navitem.bum.html $(EXA_DIR)/appshow/appshow-card.bum.html $(EXA_DIR)/appshow/appshow-dialog.bum.html $(EXA_DIR)/appshow/appshow-bubble.bum.html
	@mkdir -p $(DEMO_DIR)/appshow
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/binui_gl.o $(BUILD_DIR)/examples/appshow/main.o $(LIBS_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/appshow/main.html $(DEMO_DIR)/appshow/main.html
	cp -f $(EXA_DIR)/appshow/appshow-navitem.bum.html $(DEMO_DIR)/appshow/appshow-navitem.bum.html
	cp -f $(EXA_DIR)/appshow/appshow-card.bum.html $(DEMO_DIR)/appshow/appshow-card.bum.html
	cp -f $(EXA_DIR)/appshow/appshow-dialog.bum.html $(DEMO_DIR)/appshow/appshow-dialog.bum.html
	cp -f $(EXA_DIR)/appshow/appshow-bubble.bum.html $(DEMO_DIR)/appshow/appshow-bubble.bum.html

$(DEMO_DIR)/fontshow$(EXT): $(BUILD_DIR)/binui_gl.o $(BUILD_DIR)/examples/fontshow/main.o $(LIBS_A) $(EXA_DIR)/fontshow/main.html
	@mkdir -p $(DEMO_DIR)/fontshow
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/binui_gl.o $(BUILD_DIR)/examples/fontshow/main.o $(LIBS_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/fontshow/main.html $(DEMO_DIR)/fontshow/main.html

$(DEMO_DIR)/flexshow$(EXT): $(BUILD_DIR)/binui_gl.o $(BUILD_DIR)/examples/flexshow/main.o $(LIBS_A) $(EXA_DIR)/flexshow/main.html
	@mkdir -p $(DEMO_DIR)/flexshow
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/binui_gl.o $(BUILD_DIR)/examples/flexshow/main.o $(LIBS_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/flexshow/main.html $(DEMO_DIR)/flexshow/main.html

$(DEMO_DIR)/svgdemo$(EXT): $(BUILD_DIR)/binui_gl.o $(BUILD_DIR)/examples/svgdemo/main.o $(LIBS_A) $(EXA_DIR)/svgdemo/main.html
	@mkdir -p $(DEMO_DIR)/svgdemo
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/binui_gl.o $(BUILD_DIR)/examples/svgdemo/main.o $(LIBS_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/svgdemo/main.html $(DEMO_DIR)/svgdemo/main.html

$(DEMO_DIR)/cubedemo$(EXT): $(BUILD_DIR)/binui_gl.o $(BUILD_DIR)/examples/cubedemo/main.o $(LIBS_A) $(EXA_DIR)/cubedemo/main.html
	@mkdir -p $(DEMO_DIR)/cubedemo
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/binui_gl.o $(BUILD_DIR)/examples/cubedemo/main.o $(LIBS_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/cubedemo/main.html $(DEMO_DIR)/cubedemo/main.html

$(DEMO_DIR)/texturedemo$(EXT): $(BUILD_DIR)/binui_gl.o $(BUILD_DIR)/examples/texturedemo/main.o $(LIBS_A) $(EXA_DIR)/texturedemo/main.html
	@mkdir -p $(DEMO_DIR)/texturedemo
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/binui_gl.o $(BUILD_DIR)/examples/texturedemo/main.o $(LIBS_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/texturedemo/main.html $(DEMO_DIR)/texturedemo/main.html

$(DEMO_DIR)/buttonshow$(EXT): $(BUILD_DIR)/binui_gl.o $(BUILD_DIR)/examples/buttonshow/main.o $(LIBS_A) $(EXA_DIR)/buttonshow/main.html
	@mkdir -p $(DEMO_DIR)/buttonshow
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/binui_gl.o $(BUILD_DIR)/examples/buttonshow/main.o $(LIBS_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/buttonshow/main.html $(DEMO_DIR)/buttonshow/main.html

# =============================================
# Vulkan 版示例（P5）：同一份 demo 源码 + -DBINUI_GPU_VK，链接 binui_vk.o。
#   make demos-vk        → examples/*_vk.exe（与 GL 版并存，便于 A/B）
#   make GPU=vk all      → 额外产出 vk 版示例（仍保留 GL 版）
# 默认 `make all` 不受影响（GPU=gl）。
# =============================================
VK_DEMOS := svgdemo fontshow animshow buttonshow flexshow cubedemo texturedemo

define VK_DEMO_RULE
$(DEMO_DIR)/$(1)_vk$(EXT): $(BUILD_DIR)/binui_vk.o $(BUILD_DIR)/examples/$(1)/main_vk.o $(LIBS_A) $(EXA_DIR)/$(1)/main.html
	@mkdir -p $(DEMO_DIR)/$(1)
	$(CXX) $(LDFLAGS) $(RTLINK) -o $$@ $(BUILD_DIR)/binui_vk.o $(BUILD_DIR)/examples/$(1)/main_vk.o $(LIBS_A) \
	    $(WINLIBS) -ldwmapi -limm32 \
	    -Lthird_party/freetype/build -lfreetype \
	    -Lthird_party/harfbuzz/build-static/src -lharfbuzz
	cp -f $(EXA_DIR)/$(1)/main.html $(DEMO_DIR)/$(1)/main.html

$(BUILD_DIR)/examples/$(1)/main_vk.o: examples/$(1)/main.cpp
	@mkdir -p $(BUILD_DIR)/examples/$(1)
	$(CXX) $(CXXFLAGS) -DBINUI_GPU_VK -I$(INC_DIR) -Isrc $(VK_INC) -c examples/$(1)/main.cpp -o $$@
endef

$(foreach d,$(VK_DEMOS),$(eval $(call VK_DEMO_RULE,$(d))))

# features 的 vk 版目标文件（宏里那条只覆盖 VK_DEMOS 里的示例）
$(BUILD_DIR)/examples/features/main_vk.o: examples/features/main.cpp
	@mkdir -p $(BUILD_DIR)/examples/features
	$(CXX) $(CXXFLAGS) -DBINUI_GPU_VK -I$(INC_DIR) -Isrc $(VK_INC) -c examples/features/main.cpp -o $@
# features 有外挂组件（.bum），单独写规则：宏里那条只拷 main.html
$(DEMO_DIR)/features_vk$(EXT): $(BUILD_DIR)/binui_vk.o $(BUILD_DIR)/examples/features/main_vk.o $(LIBS_A) \
		$(COMPONENT_BUMS) $(EXA_DIR)/features/main.html $(EXA_DIR)/features/button.bum.html
	@mkdir -p $(DEMO_DIR)/features
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/binui_vk.o $(BUILD_DIR)/examples/features/main_vk.o $(LIBS_A) \
	    $(WINLIBS) -ldwmapi -limm32 \
	    -Lthird_party/freetype/build -lfreetype \
	    -Lthird_party/harfbuzz/build-static/src -lharfbuzz
	cp -f $(EXA_DIR)/features/main.html $(DEMO_DIR)/features/main.html
	cp -f $(EXA_DIR)/features/button.bum.html $(DEMO_DIR)/features/button.bum.html
	cp -f $(BUILD_DIR)/assets/components/card.bum $(DEMO_DIR)/features/card.bum
	cp -f $(BUILD_DIR)/assets/components/dock.bum $(DEMO_DIR)/features/dock.bum

DEMO_VK_BINS := $(DEMO_DIR)/features_vk$(EXT) $(foreach d,$(VK_DEMOS),$(DEMO_DIR)/$(d)_vk$(EXT))

.PHONY: demos-vk
demos-vk: $(DEMO_VK_BINS)
	@echo "vk demos -> $(DEMO_VK_BINS)"

# =============================================
# D3D11 版示例（P7）：同一份 demo 源码 + -DBINUI_GPU_D3D11，链接 binui_d3d11.o。
#   make demos-d3d11     → examples/*_d3d11.exe（与 GL / VK 版并存，便于三方 A/B）
# =============================================
D3D11_DEMOS := svgdemo fontshow animshow buttonshow flexshow cubedemo texturedemo features

define D3D11_DEMO_RULE
$(DEMO_DIR)/$(1)_d3d11$(EXT): $(BUILD_DIR)/binui_d3d11.o $(BUILD_DIR)/examples/$(1)/main_d3d11.o $(LIBS_A) $(EXA_DIR)/$(1)/main.html
	@mkdir -p $(DEMO_DIR)/$(1)
	$(CXX) $(LDFLAGS) $(RTLINK) -o $$@ $(BUILD_DIR)/binui_d3d11.o $(BUILD_DIR)/examples/$(1)/main_d3d11.o $(LIBS_A) \
	    $(WINLIBS) $(D3D11_LIBS) -limm32 \
	    -Lthird_party/freetype/build -lfreetype \
	    -Lthird_party/harfbuzz/build-static/src -lharfbuzz
	cp -f $(EXA_DIR)/$(1)/main.html $(DEMO_DIR)/$(1)/main.html

$(BUILD_DIR)/examples/$(1)/main_d3d11.o: examples/$(1)/main.cpp
	@mkdir -p $(BUILD_DIR)/examples/$(1)
	$(CXX) $(CXXFLAGS) -DBINUI_GPU_D3D11 -I$(INC_DIR) -Isrc -c examples/$(1)/main.cpp -o $$@
endef

$(foreach d,$(D3D11_DEMOS),$(eval $(call D3D11_DEMO_RULE,$(d))))

DEMO_D3D11_BINS := $(foreach d,$(D3D11_DEMOS),$(DEMO_DIR)/$(d)_d3d11$(EXT))

.PHONY: demos-d3d11
demos-d3d11: $(DEMO_D3D11_BINS)
	@echo "d3d11 demos -> $(DEMO_D3D11_BINS)"

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

$(BINUI_C_A): $(BUILD_DIR)/binui_c.o $(BUILD_DIR)/binui_gl.o
	ar rcs $@ $^

# RTLINK 同样要带上：Windows 上 libstdc++-6.dll / libgcc_s_seh-1.dll 不能指望宿主有，
# freetype/harfbuzz 也走静态库 —— 产物除系统 DLL 外自包含
$(BINUI_C_DLL): $(BUILD_DIR)/binui_c_dll.o $(BUILD_DIR)/binui_gl.o $(LIBS_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(LDFLAGS) $(RTLINK) -shared -o $@ $^ $(WINLIBS) $(GL_LIBS) \
	    -Wl,--out-implib=$(BIN_DIR)/libbinui_c.dll.a

.PHONY: capi
capi: $(BINUI_C_A) $(BINUI_C_DLL)
	@echo "C ABI -> $(BINUI_C_A) $(BINUI_C_DLL)"

.PHONY: examples
examples: $(DEMO_BINS) $(STD_LIB_STAMP)

all: $(LIBS_A) $(TEST_BINS) $(DEMO_BINS) $(STD_LIB_STAMP) $(BINUI_C_A) $(BINUI_C_DLL)

# 调试工具（可选，不入 all）：离屏渲染 / 窗口抓取 / 窗口枚举
TOOL_BINS := \
    $(BIN_DIR)/binui_compile$(EXT) \
    $(BIN_DIR)/binui_export$(EXT) \
    $(BIN_DIR)/gl_shot$(EXT) \
    $(BIN_DIR)/partial_shot$(EXT) \
    $(BIN_DIR)/draw_bench$(EXT) \
    $(BIN_DIR)/path_bench$(EXT) \
    $(BIN_DIR)/window_capture$(EXT) \
    $(BIN_DIR)/enum_windows$(EXT) \
    $(BIN_DIR)/pixel_diff$(EXT) \
    $(BIN_DIR)/baseline_snapshot$(EXT) \
    $(BIN_DIR)/check_examples$(EXT) \
    $(BIN_DIR)/vk_shot$(EXT) \
    $(BIN_DIR)/vk_window$(EXT) \
    $(BIN_DIR)/vk_stress$(EXT) \
    $(BIN_DIR)/d3d11_shot$(EXT)

# 把 .bui 还原成打包前的目录结构（读同目录的 binui.res，header-only，不用链接库）
$(BIN_DIR)/binui_export$(EXT): tools/binui_export.cpp
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -o $@ $<

# 示例 UI 零警告检查（CMake 也会注册成 ctest 用例）
$(BIN_DIR)/check_examples$(EXT): tools/check_examples.cpp $(LIBS_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -o $@ $< $(LIBS_A) $(GL_LIBS)

# "电篱笆"版的零警告检查：每个 C++ 堆块末尾挂一个不可访问页，越界写/读已释放内存
# 立刻崩并打印分配栈。本机 mingw 没有 libasan、也没权限开 page heap，这是替代品 ——
# v0.16.46 就是靠它定位到编译器里一处悬空引用（栈引用被 push_back 扩容搞失效）。
# 慢两个数量级，所以单独成目标，不进 all。
$(BIN_DIR)/check_examples_guard$(EXT): tools/check_examples.cpp tools/guard_alloc.cpp $(LIBS_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -o $@ tools/check_examples.cpp \
	    tools/guard_alloc.cpp $(LIBS_A) $(GL_LIBS)

.PHONY: check-guard
check-guard: $(BIN_DIR)/check_examples_guard$(EXT)
	$(BIN_DIR)/check_examples_guard$(EXT) examples

# ★ 这条规则（以及下面 gl_shot/d3d11_shot 直接编译 .cpp 的几条）**没有头依赖**：
#   改了 include/binui_paint.h、src/backends/binui_draw_nodes.h 这类共享头之后，
#   这些 exe 不会重编，验出来的还是旧行为（本仓库在 §7.6.4 踩过一次，0.16.13 又踩了一次）。
#   所以离屏工具改成"先编 .o（带 -MMD）再链接"。
$(BIN_DIR)/window_capture$(EXT): tools/window_capture.cpp
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -o $@ $< -lgdiplus -lgdi32 -luser32
$(BIN_DIR)/enum_windows$(EXT): tools/enum_windows.cpp
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -o $@ $< -luser32

# GL 后端的离屏抓图：GL 窗口抓不到图（PrintWindow 全黑），这个直接读回离屏像素
$(BIN_DIR)/gl_shot$(EXT): tools/gl_shot.cpp $(BUILD_DIR)/binui_gl.o $(LIBS_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -Isrc -o $@ $< \
	    $(BUILD_DIR)/binui_gl.o $(LIBS_A) -lgdiplus -lgdi32 -luser32 -lole32 $(GL_LIBS)

# partial_shot：局部重绘一致性检查（hover 脏矩形 vs 整帧）。走 .o + -MMD，
# 免得改了 binui_draw_nodes.h / binui_paint.h 之后工具还跑旧行为（这个仓库在
# gl_shot 上踩过两次，见下文注释）。
$(BUILD_DIR)/partial_shot.o: tools/partial_shot.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -MMD -MP -I$(INC_DIR) -Isrc -c $< -o $@

$(BIN_DIR)/partial_shot$(EXT): $(BUILD_DIR)/partial_shot.o $(BUILD_DIR)/binui_gl.o $(LIBS_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -o $@ $^ \
	    -lgdiplus -lgdi32 -luser32 -lole32 $(GL_LIBS)

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
$(BUILD_DIR)/vk_shot.o: tools/vk_shot.cpp
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -Isrc $(VK_INC) -c $< -o $@
$(BIN_DIR)/vk_shot$(EXT): $(BUILD_DIR)/vk_shot.o $(BUILD_DIR)/binui_vk.o $(LIBS_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/vk_shot.o $(BUILD_DIR)/binui_vk.o $(LIBS_A) \
		-luser32 -lgdi32 -lgdiplus -lole32 \
		-Lthird_party/freetype/build -lfreetype \
		-Lthird_party/harfbuzz/build-static/src -lharfbuzz

# 真实窗口 + 交换链的验收工具（P2b）：跑 N 帧，把**交换链图像**读回写 PNG
$(BUILD_DIR)/vk_window.o: tools/vk_window.cpp
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -Isrc $(VK_INC) -c $< -o $@
$(BIN_DIR)/vk_window$(EXT): $(BUILD_DIR)/vk_window.o $(BUILD_DIR)/binui_vk.o $(LIBS_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/vk_window.o $(BUILD_DIR)/binui_vk.o $(LIBS_A) \
		-luser32 -lgdi32 -lgdiplus -lole32 \
		-Lthird_party/freetype/build -lfreetype \
		-Lthird_party/harfbuzz/build-static/src -lharfbuzz

# 鲁棒性压测工具（P4）：resize 抖动 / 最小化 / 透明切换 / GDI 切换 / init-shutdown 循环 / 双实例
$(BUILD_DIR)/vk_stress.o: tools/vk_stress.cpp
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -Isrc $(VK_INC) -c $< -o $@
$(BIN_DIR)/vk_stress$(EXT): $(BUILD_DIR)/vk_stress.o $(BUILD_DIR)/binui_vk.o $(LIBS_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/vk_stress.o $(BUILD_DIR)/binui_vk.o $(LIBS_A) \
		-luser32 -lgdi32 -lgdiplus -lole32 -lpsapi \
		-Lthird_party/freetype/build -lfreetype \
		-Lthird_party/harfbuzz/build-static/src -lharfbuzz

# D3D11 出图工具（P7）：隐藏窗口跑 N 帧 + 从离屏纹理读回 → PNG。
# 不走屏幕截屏，所以窗口被遮挡也能抓到真实像素（flip-model 交换链的 PrintWindow 是白屏）。
$(BUILD_DIR)/d3d11_shot.o: tools/d3d11_shot.cpp
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -Isrc -c $< -o $@
$(BIN_DIR)/d3d11_shot$(EXT): $(BUILD_DIR)/d3d11_shot.o $(BUILD_DIR)/binui_d3d11.o $(LIBS_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/d3d11_shot.o $(BUILD_DIR)/binui_d3d11.o $(LIBS_A) \
		-luser32 -lgdi32 -lgdiplus -lole32 $(D3D11_LIBS) \
		-Lthird_party/freetype/build -lfreetype \
		-Lthird_party/harfbuzz/build-static/src -lharfbuzz

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
PARTIAL_CASES := 278,222 358,221 99,116 830,117
.PHONY: partial
partial: $(BIN_DIR)/partial_shot$(EXT)
	@fail=0; for c in $(PARTIAL_CASES); do \
	    x=$$(echo $$c | cut -d, -f1); y=$$(echo $$c | cut -d, -f2); \
	    echo "--- buttonshow hover ($$x,$$y) ---"; \
	    $(BIN_DIR)/partial_shot$(EXT) --check $(EXA_DIR)/buttonshow/main.html - $$x $$y 40 || fail=1; \
	done; \
	echo "--- svgdemo hover (屏内任意按钮) ---"; \
	exit $$fail
	@echo "tools -> $(TOOL_BINS)"

# =============================================
# 绘制基准（P0-1 / P0-4 的量化入口，v0.16.33）
#
# 以前 docs/future.md 里只有几句一次性手测数字（"约 1052 µs/帧、约 9800 个四边形"），
# 优化完没法证明真的快了，重构也没法证明没改坏。现在：
#   make bench-draw  → 纯 CPU 遍历（计数后端，无 GPU/无窗口/确定性，跨后端可比）
#   make bench-gl    → 真实 GL 后端一帧：绘制(遍历+发射) + 提交+呈现 分开量
# 两个数字相减 = 后端发射/上传开销，据此决定该优化哪一头。
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

.PHONY: bench-gl
bench-gl: $(BIN_DIR)/gl_shot$(EXT)
	@echo "--- real GL backend: emit vs submit+present ---"
	@for d in features appshow animshow; do \
	    $(BIN_DIR)/gl_shot$(EXT) $(EXA_DIR)/$$d/main.html - --bench 60; \
	done
	@$(BIN_DIR)/gl_shot$(EXT) $(BENCH_ASSET)/bench-grad.html - --bench 60

# =============================================
# 像素回归（本机跑，不入 CI）
#
# 为什么不在 CI 里：GitHub 的 Windows runner 是软件光栅化，AA 边缘与真实 GPU 不一致，
# 同一份代码会稳定地差出百分之几；基准图只在**本机**有意义。
#
# 基准图由谁渲染（v0.24.0 起）：**GL 后端**（`gl_shot`，隐藏窗口 + 离屏 FBO）。
# 以前是 GDI 后端（`offscreen_render` → `drawNodesGDI`）—— GDI 渲染后端已在 0.24.0 移除，
# 见 CHANGELOG。现在的像素权威是 GL：`pixels` = GL 自回归，`pixels-vk` / `pixels-d3d11`
# = 另两个后端与 GL 逐像素比。CPU 侧的"像素权威"只剩 bin2d（路径/圆角的覆盖率）。
#
# 用法：
#   mingw32-make SHELL=sh pixels           # GL 渲染 + 与 docs/pixels 基准图比对
#   mingw32-make SHELL=sh pixels-update    # 改完绘制/布局后重新生成基准图
# =============================================
PIXEL_CASES := circle display-none2 probe radius-matrix svgpath svggrad svgjoin svgstroke svgfinish svgaa opacity grad-angle shadow image-round image-mip text-shadow text-shadow-multi svg-filter
PIXEL_OUT   := $(BUILD_DIR)/pixels
PIXEL_GOLD  := docs/pixels
PIXEL_TOL   ?= 0.5

.PHONY: pixels pixels-update
pixels: tools
	@mkdir -p $(PIXEL_OUT)
	@fail=0; \
	for c in $(PIXEL_CASES); do \
	    echo "--- $$c ---"; \
	    $(BIN_DIR)/gl_shot$(EXT) docs/issues/assets/$$c.html $(PIXEL_OUT)/$$c.png >/dev/null 2>&1 || { echo "渲染失败: $$c"; fail=1; continue; }; \
	    $(BIN_DIR)/pixel_diff$(EXT) $(PIXEL_GOLD)/$$c.png $(PIXEL_OUT)/$$c.png $(PIXEL_TOL) $(PIXEL_OUT)/$$c.diff.png || fail=1; \
	done; \
	exit $$fail

pixels-update: tools
	@mkdir -p $(PIXEL_GOLD)
	@for c in $(PIXEL_CASES); do \
	    $(BIN_DIR)/gl_shot$(EXT) docs/issues/assets/$$c.html $(PIXEL_GOLD)/$$c.png >/dev/null 2>&1 || exit 1; \
	    echo "更新基准: $(PIXEL_GOLD)/$$c.png"; \
	done

# =============================================
# Vulkan 后端的像素回归（P3 起）：与 GL **逐像素**比。
#
# 判据：不同像素 < $(PIXEL_VK_TOL)%，单通道容差 $(PIXEL_VK_CHTOL)。
# 为什么不是 0：GL 与 Vulkan 的 4x MSAA 在**斜边**上采样归属不同（同一个 GPU、
# 两套驱动前端），实测差值恰好是"一个采样点"量级（≈25% 覆盖率），逐像素正负
# 各半（对称、零净偏）。互不斜边的用例（圆/圆角/文本/纵向 AA 条带）是 0 差异。
# 实测两张后端与 GDI 基准的距离相同（差 ≤ 34 像素 / 109200），所以这里比的不是
# "谁更对"，而是"新后端有没有引入新的绘制差异"。
# =============================================
PIXEL_VK_OUT   := $(BUILD_DIR)/pixels-vk
PIXEL_VK_TOL   ?= 1.5
PIXEL_VK_CHTOL ?= 128
# 真实示例（整页 UI，不是单点卡片）：文本、图片、组件、SVG 混排
VK_DEMO_CASES  := features fontshow svgdemo texturedemo
# 动画中途帧（v0.11.10）：`from { transform: scale(0.77) }` → t=0 就落在**小数缩放**上，
# 专门覆盖"几何是小数时"的路径（亚像素坐标 + 小数圆角半径 + 圆角模板重采样）。
# 这条用例就是 VK 圆角模板从 NEAREST 改 LINEAR 的判据：改之前 252x156 区域有差异，
# 改之后不同像素 0（最大通道差 1）。只进跨后端比对，不建 GDI 基准图。
# roundtex-lru（v0.12.8）：一页里 >48 种圆角半径，专门压"圆角模板 LRU 淘汰"——
# 淘汰时如果当场删纹理，本帧已经录进批次的四边形还在引用它，跨后端比对会看到某几块
# 圆角被画成整块纯色。这条用例只进跨后端比对（GL vs VK / D3D11），不建 GDI 基准图。
# filter-blur（v0.16.42）：五档半径 + 子树模糊 + 渐变圆角 + opacity/blur 合层 + 嵌套两层
# + 小数坐标 + -webkit- 别名 + 贴窗口边缘。只进跨后端比对（GDI 遗留路径不模糊）。
# 实测 GL vs VK 0 不同像素（最大通道差 59）、D3D11 vs GL 0 不同像素（最大通道差 2）；
# drop-shadow（v0.16.43）：外阴影取元素 alpha 上色；本用例覆盖星形轮廓/文字/子树/多阴影/硬阴影/
# 负偏移/颜色前置/opacity 同用/贴窗口边缘/blur+shadow 同写/方角图片。三端 0 不同像素。
# 那 59 来自"层里画圆角图片"的既有单采样差异（见 docs/future.md 的工具链已知问题）。
# blur-quality（v0.16.44）：分离高斯。小元素 + 大半径（旧实现最难看的一组：14px 圆点糊成十字、
# 6px 细条糊成方块）+ 边缘剖面（16px 半径的水平线）+ 文字 blur(3) / drop-shadow(10) + 毛玻璃
# 10~32px。只进跨后端比对；观感差异看 docs/issues/assets/blur-quality.png。
# round-in-layer（v0.16.45）：1× 离屏层里的**圆角图片**（按行切细条 → 每条左右边是小数）。
# 这条用例就是"VK 图层用了 4× MSAA 主管线"的判据：修之前弧线每行差一个像素（17 px / 通道差
# 200+），修之后 GL/VK/D3D11 三端 0 不同像素、最大通道差 0。覆盖 opacity 层、模糊层、
# 小数坐标、嵌套两层，以及层里的圆/渐变/细线（确认修法没波及别的图元）。
# nine-slice（v0.16.51 扩）：九宫格中段四种填充模式 stretch / repeat / round / space 全开，
# 含 space 的"两端也留缝"与"中段比一块还窄时缩成一块"。
# svg-mask（v0.16.53/54 扩）：遮罩内容用**渐变**填充（横向线性 / 竖向三色标 / 径向聚光），
# 外加 mask-type 的 luminance / alpha 对照（同一份 #808080 内容差一倍）。
# 渐变的色标 alpha 存的是亮度，保留趟直接按渐变色带画 —— 三端同一份代码，只进跨后端比对。
# text-shadow-multi（v0.16.50）：多层文字阴影（霓虹 3 层 / 描边 4 层 / 双色偏移 / 继承 /
# currentColor / 贴边大模糊 / 三个不同模糊半径同时活跃 = 三张阴影图集）。三端同一份图集代码，
# 只进跨后端比对（GDI 侧另外烘位图，基准图由它生成）。
# layer-stroke / layer-shadow（v0.17.1）：docs/future.md「已知差异」里那两条未定位差异的判据。
#   layer-stroke —— 裁剪层里**奇数宽度描边**（历史上 VK 每段末端多 1px）。范围拉满：w1/w2/w3/w5/w7、
#     整数与小数 y、斜线、折线、圆、自定义路径裁剪、嵌套裁剪，外加同款"不裁剪"对照。
#     顺带钉住真正的根因：`fillLine` 的细长四边形过 GPU 光栅化，4× MSAA 采样位置在 GL 与
#     Vulkan 上互为镜像 → 浅斜线覆盖率差一档（62/255）。修法是描边改走 CPU 条带覆盖率，
#     所以本页要 0 不同像素。圆描边（a 例）单测时差 320 像素 / 通道 123，修后 0。
#   layer-shadow —— 组透明度图层里的 box-shadow（历史上差 ~34）。三条触发条件缺一不可：
#     卡片底半透明、阴影大模糊低 alpha、底下是渐变。修后 ≤8（预乘色的 8bit 舍入）。
ANIM_CASES := anim-midphase affine-corners roundtex-lru text-subpixel affine-text nine-slice img-filter svg-marker svg-clip svg-pattern pattern-clip svg-mask flex-scroll-shrink glass glass-multi-size grad-quad fixed sticky letter-spacing text-overflow filter-blur drop-shadow blur-quality round-in-layer text-shadow-multi svg-filter layer-stroke layer-shadow
# 窗口模式验收（P2b）：交换链 / GDI 读回 / resize / 全屏守卫 / 透明窗
VK_WINDOW_CASES := probe svgpath circle

.PHONY: pixels-vk

pixels-vk: tools
	@mkdir -p $(PIXEL_VK_OUT)
	@fail=0; \
	for c in $(PIXEL_CASES); do \
	    echo "--- $$c ---"; \
	    $(BIN_DIR)/vk_shot$(EXT) docs/issues/assets/$$c.html $(PIXEL_VK_OUT)/$$c.png >/dev/null 2>&1 || { echo "VK 渲染失败: $$c"; fail=1; continue; }; \
	    $(BIN_DIR)/gl_shot$(EXT) docs/issues/assets/$$c.html $(PIXEL_VK_OUT)/$$c.gl.png >/dev/null 2>&1 || { echo "GL 渲染失败: $$c"; fail=1; continue; }; \
	    $(BIN_DIR)/pixel_diff$(EXT) $(PIXEL_VK_OUT)/$$c.gl.png $(PIXEL_VK_OUT)/$$c.png $(PIXEL_VK_TOL) $(PIXEL_VK_OUT)/$$c.diff.png $(PIXEL_VK_CHTOL) || fail=1; \
	done; \
	for d in $(VK_DEMO_CASES); do \
	    echo "--- demo/$$d ---"; \
	    $(BIN_DIR)/vk_shot$(EXT) examples/$$d/main.html $(PIXEL_VK_OUT)/demo-$$d.png >/dev/null 2>&1 || { echo "VK 渲染失败: $$d"; fail=1; continue; }; \
	    $(BIN_DIR)/gl_shot$(EXT) examples/$$d/main.html $(PIXEL_VK_OUT)/demo-$$d.gl.png >/dev/null 2>&1 || { echo "GL 渲染失败: $$d"; fail=1; continue; }; \
	    $(BIN_DIR)/pixel_diff$(EXT) $(PIXEL_VK_OUT)/demo-$$d.gl.png $(PIXEL_VK_OUT)/demo-$$d.png $(PIXEL_VK_TOL) $(PIXEL_VK_OUT)/demo-$$d.diff.png $(PIXEL_VK_CHTOL) || fail=1; \
	done; \
	for a in $(ANIM_CASES); do \
	    echo "--- anim/$$a ---"; \
	    $(BIN_DIR)/vk_shot$(EXT) docs/issues/assets/$$a.html $(PIXEL_VK_OUT)/anim-$$a.png >/dev/null 2>&1 || { echo "VK 渲染失败: $$a"; fail=1; continue; }; \
	    $(BIN_DIR)/gl_shot$(EXT) docs/issues/assets/$$a.html $(PIXEL_VK_OUT)/anim-$$a.gl.png >/dev/null 2>&1 || { echo "GL 渲染失败: $$a"; fail=1; continue; }; \
	    $(BIN_DIR)/pixel_diff$(EXT) $(PIXEL_VK_OUT)/anim-$$a.gl.png $(PIXEL_VK_OUT)/anim-$$a.png 0 $(PIXEL_VK_OUT)/anim-$$a.diff.png $(PIXEL_VK_CHTOL) || fail=1; \
	done; \
	exit $$fail

# =============================================
# D3D11 后端的像素回归（P7 起）：同样与 GL **逐像素**比。
#
# 现状（P7a）：判据放宽到 $(PIXEL_D3D_TOL)%。已知差异来源有两处，都是"D3D11 还没换成
# 共享实现"：它自己的**圆角模板**（模板边长 max(8,4r) + 自己的覆盖率算法，而 GL/Vulkan
# 用 binui_paint.h 的 RoundTemplateSize/BuildRoundedNineSlice）和自己的**文本光栅化**
# （binui_font_renderer + 自带图集）。实测 circle 2.04%、svgstroke 0.28%、svgaa 0.25%，
# 纯几何且无圆角的用例（probe / radius-matrix）是**零差异**。
# P7b（把这两样也换成共享实现）之后这里收紧到 1.5% → 0。
# =============================================
PIXEL_D3D_OUT := $(BUILD_DIR)/pixels-d3d11
PIXEL_D3D_TOL ?= 3

.PHONY: pixels-d3d11

pixels-d3d11: tools
	@mkdir -p $(PIXEL_D3D_OUT)
	@fail=0; for c in $(PIXEL_CASES); do \
	    echo "--- $$c ---"; \
	    $(BIN_DIR)/d3d11_shot$(EXT) docs/issues/assets/$$c.html $(PIXEL_D3D_OUT)/$$c.png >/dev/null 2>&1 || fail=1; \
	    $(BIN_DIR)/gl_shot$(EXT) docs/issues/assets/$$c.html $(PIXEL_D3D_OUT)/$$c.gl.png >/dev/null 2>&1 || fail=1; \
	    $(BIN_DIR)/pixel_diff$(EXT) $(PIXEL_D3D_OUT)/$$c.gl.png $(PIXEL_D3D_OUT)/$$c.png $(PIXEL_D3D_TOL) $(PIXEL_D3D_OUT)/$$c.diff.png $(PIXEL_VK_CHTOL) || fail=1; \
	done; exit $$fail
	@fail=0; for d in $(VK_DEMO_CASES); do \
	    echo "--- demo/$$d ---"; \
	    $(BIN_DIR)/d3d11_shot$(EXT) examples/$$d/main.html $(PIXEL_D3D_OUT)/demo-$$d.png >/dev/null 2>&1 || fail=1; \
	    $(BIN_DIR)/gl_shot$(EXT) examples/$$d/main.html $(PIXEL_D3D_OUT)/demo-$$d.gl.png >/dev/null 2>&1 || fail=1; \
	    $(BIN_DIR)/pixel_diff$(EXT) $(PIXEL_D3D_OUT)/demo-$$d.gl.png $(PIXEL_D3D_OUT)/demo-$$d.png $(PIXEL_D3D_TOL) $(PIXEL_D3D_OUT)/demo-$$d.diff.png $(PIXEL_VK_CHTOL) || fail=1; \
	done; exit $$fail
	@fail=0; for a in $(ANIM_CASES); do \
	    echo "--- anim/$$a ---"; \
	    $(BIN_DIR)/d3d11_shot$(EXT) docs/issues/assets/$$a.html $(PIXEL_D3D_OUT)/anim-$$a.png >/dev/null 2>&1 || fail=1; \
	    $(BIN_DIR)/gl_shot$(EXT) docs/issues/assets/$$a.html $(PIXEL_D3D_OUT)/anim-$$a.gl.png >/dev/null 2>&1 || fail=1; \
	    $(BIN_DIR)/pixel_diff$(EXT) $(PIXEL_D3D_OUT)/anim-$$a.gl.png $(PIXEL_D3D_OUT)/anim-$$a.png $(PIXEL_D3D_TOL) $(PIXEL_D3D_OUT)/anim-$$a.diff.png $(PIXEL_VK_CHTOL) || fail=1; \
	done; exit $$fail
	@echo "--- 脏矩形重绘 vs 全量重绘（阈值 0）---"
	@$(BIN_DIR)/d3d11_shot$(EXT) docs/issues/assets/probe.html $(PIXEL_D3D_OUT)/probe.dirty.png --frames 8 --dirty >/dev/null
	@$(BIN_DIR)/pixel_diff$(EXT) $(PIXEL_D3D_OUT)/probe.png $(PIXEL_D3D_OUT)/probe.dirty.png 0
	@echo "--- 强制读回 + GDI 贴屏 vs 交换链（阈值 0）---"
	@$(BIN_DIR)/d3d11_shot$(EXT) docs/issues/assets/probe.html $(PIXEL_D3D_OUT)/probe.gdi.png --gdi >/dev/null
	@$(BIN_DIR)/pixel_diff$(EXT) $(PIXEL_D3D_OUT)/probe.png $(PIXEL_D3D_OUT)/probe.gdi.png 0

# 注意（踩过）：recipe 的**命令文本**一律用 ASCII。mingw32-make 调 Git 的 sh 时，
# 某些中文字符（实测「没构建设」「（）」）会让 sh 报 "unexpected EOF while looking for
# matching `"`" —— 整行命令解析失败、目标直接不动，报错还指向 Makefile 行号，极难定位。
# 中文只放在注释里；要在命令里输出中文，改成 ASCII（或者写进脚本文件再调用）。
# =============================================
# 迁移基线快照（P1，v0.20.3）
#
# 为什么要有它：`make pixels` / `pixels-vk` / `pixels-d3d11` 只回答"在容差内吗"，
# 回答不了"哪一端、哪个用例动了"。迁移 `<path>` 到 bin2d 时，像素一定会动一点点，
# 那时最需要的恰恰是"动的正好是路径相关的用例，别的一个都没动"。
#
# 做法：三端各把每个用例渲一遍到 build-mingw/baseline/render/<后端>/，由
# tools/baseline_snapshot.cpp 压成一行一个 FNV-1a 64（原始 ARGB，PNG 编码器换了也不影响），
# 连同 draw_bench 的原语流哈希、bin2d `make quality` 的数字，一起写进
# tests/baseline/manifest.txt 并**提交进仓库**。每步迁移后对一遍。
#
# 判据（两级，别混）：
#   · 硬门禁仍是 pixel_diff 的容差判据（`make pixels` 等），它管"能不能接受"；
#   · 快照管"动了什么"：结构变化（用例增删 / 尺寸变）直接失败；哈希变化默认**只报告**，
#     确认后跑 `make baseline` 重生成快照并在 CHANGELOG 写清为什么变；
#     要求"这一步必须逐位相同"时用 `make baseline-strict`。
#
# 为什么 `draw_bench --hash` 要带 `--frames 1 --warm 0`：哈希是**跨帧累加**的，
# 帧数不同哈希就不同（实测 --frames 1/2/5 三个值）。固定成"第一帧的原语流"最稳定。
# =============================================
BASELINE_DIR    := tests/baseline
BASELINE_RAW    := $(BUILD_DIR)/baseline
BASELINE_MANI   := $(BASELINE_DIR)/manifest.txt
BASELINE_RENDER := $(BASELINE_RAW)/render
BASELINE_DRAW   := $(BASELINE_RAW)/draw_bench.txt
BASELINE_QA     := $(BASELINE_RAW)/quality.txt
BASELINE_CASES  := $(PIXEL_CASES) $(addprefix demo-,$(VK_DEMO_CASES)) $(addprefix anim-,$(ANIM_CASES))
# bench 用例：真实示例 + 渐变卡片压测（条带爆炸那一页）。
# **animshow 不进快照**：它的原语流由动画时钟（wall clock）决定，同一份代码连跑两次
# 哈希都不同（实测三次三个值），拿它当基线只会每次报"变了"。同理，任何"运行中会自己变"
# 的页面都不该进哈希基线 —— 像素快照那边没这个问题（渲染工具是定帧的，实测两次全等）。
BASELINE_DRAW_CASES := $(filter-out animshow,$(BENCH_CASES)) bench-grad

.PHONY: baseline-render
baseline-render: tools
	@rm -rf $(BASELINE_RENDER)
	@mkdir -p $(BASELINE_RENDER)/gl $(BASELINE_RENDER)/vk $(BASELINE_RENDER)/d3d11
	@fail=0; for c in $(BASELINE_CASES); do \
	    src=docs/issues/assets/$$c.html; \
	    case $$c in demo-*) src=examples/$${c#demo-}/main.html ;; anim-*) src=docs/issues/assets/$${c#anim-}.html ;; esac; \
	    $(BIN_DIR)/gl_shot$(EXT) $$src $(BASELINE_RENDER)/gl/$$c.png >/dev/null 2>&1 || { echo "render failed [gl]: $$c"; fail=1; }; \
	    $(BIN_DIR)/vk_shot$(EXT) $$src $(BASELINE_RENDER)/vk/$$c.png >/dev/null 2>&1 || { echo "render failed [vk]: $$c"; fail=1; }; \
	    $(BIN_DIR)/d3d11_shot$(EXT) $$src $(BASELINE_RENDER)/d3d11/$$c.png >/dev/null 2>&1 || { echo "render failed [d3d11]: $$c"; fail=1; }; \
	done; exit $$fail
	@: > $(BASELINE_DRAW); for d in $(BASELINE_DRAW_CASES); do \
	    src=examples/$$d/main.html; \
	    if [ ! -f $$src ]; then src=$(BENCH_ASSET)/$$d.html; fi; \
	    h=$$($(BIN_DIR)/draw_bench$(EXT) $$src --frames 1 --warm 0 --hash 2>/dev/null | tail -1); \
	    echo "draw $$d $$h" >> $(BASELINE_DRAW); \
	done
	# 只收 stdout：bin2d 的 quality 工具把计时行写到 stderr（0.9.1 起），而台账要逐字比较 ——
	# 把 ms/µs 收进来，这台机器每次跑都会红一次。stdout 是确定性内容，可以逐字记账。
	@if [ -x ../bin2D/build-mingw/out/bin2d-quality$(EXT) ]; then ../bin2D/build-mingw/out/bin2d-quality$(EXT) $(BASELINE_RAW)/quality >$(BASELINE_QA) 2>/dev/null || true; else echo "bin2d quality tool not built (run: make -C ../bin2D quality)" >$(BASELINE_QA); fi

.PHONY: baseline
baseline: baseline-render $(BIN_DIR)/baseline_snapshot$(EXT)
	@$(BIN_DIR)/baseline_snapshot$(EXT) write $(BASELINE_MANI) $(BASELINE_RENDER) $(BASELINE_DRAW) $(BASELINE_QA)

.PHONY: baseline-check
baseline-check: baseline-render $(BIN_DIR)/baseline_snapshot$(EXT)
	@$(BIN_DIR)/baseline_snapshot$(EXT) check $(BASELINE_MANI) $(BASELINE_RENDER) $(BASELINE_DRAW) $(BASELINE_QA)

.PHONY: baseline-strict
baseline-strict: baseline-render $(BIN_DIR)/baseline_snapshot$(EXT)
	@$(BIN_DIR)/baseline_snapshot$(EXT) check $(BASELINE_MANI) $(BASELINE_RENDER) $(BASELINE_DRAW) $(BASELINE_QA) --strict

.PHONY: vk-window
# 窗口模式验收：拿真实窗口跑 N 帧，读回**交换链图像**与 GL 同尺寸输出比（阈值 0）。
# 覆盖：交换链呈现 / 强制 GDI 读回 / resize 后交换链重建 / 全屏直翻转守卫 / 透明窗。
vk-window: tools
	@mkdir -p $(PIXEL_VK_OUT)
	@fail=0; \
	for c in $(VK_WINDOW_CASES); do \
	    $(BIN_DIR)/gl_shot$(EXT) docs/issues/assets/$$c.html $(PIXEL_VK_OUT)/$$c.gl.png >/dev/null 2>&1 || { echo "GL 渲染失败: $$c"; fail=1; continue; }; \
	    for mode in "" "--gdi" "--transparent" "--dirty" "--resize"; do \
	        tag=$$(echo "win$$mode" | tr -d '-'); \
	        echo "--- $$c $$mode ---"; \
	        $(BIN_DIR)/vk_window$(EXT) docs/issues/assets/$$c.html --frames 20 $$mode \
	            --out $(PIXEL_VK_OUT)/$$c.$$tag.png >/dev/null 2>&1 || { echo "window failed: $$c $$mode"; fail=1; continue; }; \
	        if [ "$$mode" = "--resize" ]; then continue; fi; \
	        $(BIN_DIR)/pixel_diff$(EXT) $(PIXEL_VK_OUT)/$$c.gl.png $(PIXEL_VK_OUT)/$$c.$$tag.png $(PIXEL_VK_TOL) $(PIXEL_VK_OUT)/$$c.$$tag.diff.png $(PIXEL_VK_CHTOL) || fail=1; \
	        if [ "$$mode" = "--dirty" ]; then \
	            $(BIN_DIR)/pixel_diff$(EXT) $(PIXEL_VK_OUT)/$$c.png $(PIXEL_VK_OUT)/$$c.$$tag.png 0 || fail=1; \
	        fi; \
	    done; \
	done; \
	echo "--- fullscreen guard (window covers monitor) ---"; \
	$(BIN_DIR)/vk_window$(EXT) docs/issues/assets/probe.html --frames 10 --fullscreen 2>&1 | grep -E "present=|saved|fullscreen" || fail=1; \
	exit $$fail

.PHONY: vk-stress

# 鲁棒性压测（P4）：resize 抖动 / 同尺寸早退 / 最小化恢复 / 透明切换 / GDI 切换 /
# init-shutdown 循环 / 双实例共存 / 泄漏复跑。末尾把画面读回与 GL 比（阈值 0）。
# 判据看工具自己的 [ok]/[FAIL] 与 "后端显存记账归零"。
vk-stress: tools
	@mkdir -p $(PIXEL_VK_OUT)
	@fail=0; \
	for c in probe vkcolor; do \
	    echo "--- stress/$$c ---"; \
	    $(BIN_DIR)/gl_shot$(EXT) docs/issues/assets/$$c.html $(PIXEL_VK_OUT)/$$c.gl.png >/dev/null 2>&1 || { echo "GL 渲染失败: $$c"; fail=1; continue; }; \
	    $(BIN_DIR)/vk_stress$(EXT) docs/issues/assets/$$c.html --rounds 2 \
	        --out $(PIXEL_VK_OUT)/stress-$$c.png 2>/dev/null || fail=1; \
	    $(BIN_DIR)/pixel_diff$(EXT) $(PIXEL_VK_OUT)/$$c.gl.png $(PIXEL_VK_OUT)/stress-$$c.png 0 || fail=1; \
	done; \
	exit $$fail

else

all: $(LIBS_A) $(TEST_BINS)
	@echo "binUI: non-Windows platform, demos skipped (lib + tests built)"

endif

# =============================================
# cleanup
# =============================================
.PHONY: resize-bench
# 拖边框 resize 压测（v0.19.0）：三个后端各跑一遍 BINUI_RESIZE_BENCH=60 —— 每帧把窗口
# 宽度抖 ±40px，逼出"每个 WM_SIZE 都重建交换链/后备缓冲"这条最坏路径。判据：
#   ① 不许出现后端报错（ResizeBuffers hr= / 交换链重建失败 / 重建目标失败 / Present hr=）；
#   ② 打印出来的 gfx.resize 耗时要看一眼（本机基准 GL 0.4 / D3D11 0.4 / VK 4.5 ms，
#      退化回几十 ms 说明又走了"先销毁再新建"的老路）。
# 这条能抓住 v0.19.0 之前那两个 bug：D3D 的 ResizeBuffers 每次都返回 0x887A0001（瞬间
# 失败，耗时看着很好），VK 每次 resize 26 ms。
resize-bench: $(DEMO_DIR)/flexshow$(EXT) $(DEMO_DIR)/flexshow_vk$(EXT) $(DEMO_DIR)/flexshow_d3d11$(EXT)
	@fail=0; for e in flexshow flexshow_vk flexshow_d3d11; do \
	    echo "--- $$e ---"; \
	    out=$$(BINUI_RESIZE_BENCH=60 $(DEMO_DIR)/$$e$(EXT) 2>&1); \
	    echo "$$out" | grep -E '^\[resize-bench\]' || { echo "拿不到压测结果: $$e"; fail=1; continue; }; \
	    if echo "$$out" | grep -qE 'ResizeBuffers hr=|交换链重建失败|重建目标失败|Present hr='; then \
	        echo "$$out" | grep -E 'ResizeBuffers hr=|交换链重建失败|重建目标失败|Present hr=' | head -5; \
	        echo "后端重建失败: $$e"; fail=1; \
	    fi; \
	done; exit $$fail

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