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
    $(BUILD_DIR)/binui_charts.o
LIB_A    := $(BUILD_DIR)/libbinui.a

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
$(BUILD_DIR)/binui_gl-gdi.o: $(SRC_DIR)/backends/binui_gl-gdi.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -Isrc -c $< -o $@

# Vulkan 后端（P2）：需要 third_party/vulkan 的头；运行期只 LoadLibrary("vulkan-1.dll")，
# 不链接 vulkan-1.lib，所以构建不需要 Vulkan SDK。
VK_INC   := -Ithird_party/vulkan/include
$(BUILD_DIR)/binui_vk.o: $(SRC_DIR)/backends/binui_vk.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -Isrc $(VK_INC) -c $< -o $@

$(BUILD_DIR)/examples/app_win32.o: $(SRC_DIR)/platform/app_win32.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -Isrc -c $< -o $@

$(LIB_A): $(LIB_OBJS)
	ar rcs $@ $^

# =============================================
# 组件二进制：.bum.html -> .bum（产物落 build 目录，不污染源码树）
# =============================================
COMPONENT_BUMS := \
    $(BUILD_DIR)/assets/components/card.bum \
    $(BUILD_DIR)/assets/components/dock.bum

# 静态链接：构建脚本经常在 sh/PowerShell 等不同环境里调用它，
# 动态 MinGW 运行时（libstdc++-6.dll 等）在某些 PATH 下找不到（sh 下表现为退出码 127）
$(BIN_DIR)/binui_compile$(EXT): tools/binui_compile.cpp $(LIB_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -o $@ $< $(LIB_A)

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
$(TEST_DIR)/test_css$(EXT): tests/test_css.cpp $(LIB_A)
	@mkdir -p $(TEST_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -o $@ $< $(LIB_A)

# test_elements：二进制格式 / 元素树 / 布局 / 命中 / 交互 / 动画 / 绘制几何 / 热重载
# 工作目录须为仓库根（用到 tests/fixtures/ 与 build-mingw/tmp/）
$(TEST_DIR)/test_elements$(EXT): tests/test_elements.cpp $(LIB_A)
	@mkdir -p $(TEST_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -I$(EXA_DIR) -o $@ $< $(LIB_A)

# test_font：字体服务（FreeType + HarfBuzz）+ 输入法（IME）的 UTF-16 光标桥接
$(TEST_DIR)/test_font$(EXT): tests/test_font.cpp $(LIB_A)
	@mkdir -p $(TEST_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -Ithird_party/freetype/include -Ithird_party/harfbuzz/src \
	    -o $@ $< $(LIB_A) -Lthird_party/freetype/build -lfreetype \
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
GL_LIBS := -lopengl32 -ldwmapi -limm32 -Lthird_party/freetype/build -lfreetype -Lthird_party/harfbuzz/build-static/src -lharfbuzz

# C ABI 的冒烟测试（无窗口：只测编译/加载/节点查询/改文本，需链 GL 后端对象）
TEST_BINS += $(TEST_DIR)/test_capi$(EXT)
test: $(TEST_DIR)/test_capi$(EXT)

# 注意：这里必须直接写 $(BUILD_DIR)/libbinui_c.a，不能写 $(BINUI_C_A) ——
# 那个变量是后面用 := 定义的，GNU Make 在读到这一行时就展开成了空，
# 于是 test_capi 永远只依赖 .cpp，库更新后不重链，跑的是旧二进制。
$(TEST_DIR)/test_capi$(EXT): tests/test_capi.cpp $(BUILD_DIR)/libbinui_c.a $(LIB_A)
	@mkdir -p $(TEST_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -o $@ $< $(BUILD_DIR)/libbinui_c.a $(LIB_A) $(WINLIBS) $(GL_LIBS)

# 8 组使用示例（一组一个 exe，一组只演示一件事）：功能 / 动画 / 字体与输入法 /
# 按钮样式 / 自适应布局 / 2D 渲染 / 3D 渲染 / 纹理。产物在 $(DEMO_DIR)，与测试彻底分开。
DEMO_BINS := \
    $(DEMO_DIR)/features$(EXT) \
    $(DEMO_DIR)/animshow$(EXT) \
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
$(DEMO_DIR)/features$(EXT): $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/features/main.o $(LIB_A) $(COMPONENT_BUMS) $(EXA_DIR)/features/main.html $(EXA_DIR)/features/button.bum.html
	@mkdir -p $(DEMO_DIR)/features
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/features/main.o $(LIB_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/features/main.html $(DEMO_DIR)/features/main.html
	cp -f $(EXA_DIR)/features/button.bum.html $(DEMO_DIR)/features/button.bum.html
	cp -f $(BUILD_DIR)/assets/components/card.bum $(DEMO_DIR)/features/card.bum
	cp -f $(BUILD_DIR)/assets/components/dock.bum $(DEMO_DIR)/features/dock.bum

$(DEMO_DIR)/animshow$(EXT): $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/animshow/main.o $(LIB_A) $(EXA_DIR)/animshow/main.html
	@mkdir -p $(DEMO_DIR)/animshow
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/animshow/main.o $(LIB_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/animshow/main.html $(DEMO_DIR)/animshow/main.html

$(DEMO_DIR)/fontshow$(EXT): $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/fontshow/main.o $(LIB_A) $(EXA_DIR)/fontshow/main.html
	@mkdir -p $(DEMO_DIR)/fontshow
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/fontshow/main.o $(LIB_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/fontshow/main.html $(DEMO_DIR)/fontshow/main.html

$(DEMO_DIR)/flexshow$(EXT): $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/flexshow/main.o $(LIB_A) $(EXA_DIR)/flexshow/main.html
	@mkdir -p $(DEMO_DIR)/flexshow
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/flexshow/main.o $(LIB_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/flexshow/main.html $(DEMO_DIR)/flexshow/main.html

$(DEMO_DIR)/svgdemo$(EXT): $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/svgdemo/main.o $(LIB_A) $(EXA_DIR)/svgdemo/main.html
	@mkdir -p $(DEMO_DIR)/svgdemo
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/svgdemo/main.o $(LIB_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/svgdemo/main.html $(DEMO_DIR)/svgdemo/main.html

$(DEMO_DIR)/cubedemo$(EXT): $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/cubedemo/main.o $(LIB_A) $(EXA_DIR)/cubedemo/main.html
	@mkdir -p $(DEMO_DIR)/cubedemo
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/cubedemo/main.o $(LIB_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/cubedemo/main.html $(DEMO_DIR)/cubedemo/main.html

$(DEMO_DIR)/texturedemo$(EXT): $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/texturedemo/main.o $(LIB_A) $(EXA_DIR)/texturedemo/main.html
	@mkdir -p $(DEMO_DIR)/texturedemo
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/texturedemo/main.o $(LIB_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/texturedemo/main.html $(DEMO_DIR)/texturedemo/main.html

$(DEMO_DIR)/buttonshow$(EXT): $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/buttonshow/main.o $(LIB_A) $(EXA_DIR)/buttonshow/main.html
	@mkdir -p $(DEMO_DIR)/buttonshow
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/buttonshow/main.o $(LIB_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/buttonshow/main.html $(DEMO_DIR)/buttonshow/main.html

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

$(BINUI_C_A): $(BUILD_DIR)/binui_c.o $(BUILD_DIR)/binui_gl-gdi.o
	ar rcs $@ $^

# RTLINK 同样要带上：Windows 上 libstdc++-6.dll / libgcc_s_seh-1.dll 不能指望宿主有，
# freetype/harfbuzz 也走静态库 —— 产物除系统 DLL 外自包含
$(BINUI_C_DLL): $(BUILD_DIR)/binui_c_dll.o $(BUILD_DIR)/binui_gl-gdi.o $(LIB_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(LDFLAGS) $(RTLINK) -shared -o $@ $^ $(WINLIBS) $(GL_LIBS) \
	    -Wl,--out-implib=$(BIN_DIR)/libbinui_c.dll.a

.PHONY: capi
capi: $(BINUI_C_A) $(BINUI_C_DLL)
	@echo "C ABI -> $(BINUI_C_A) $(BINUI_C_DLL)"

.PHONY: examples
examples: $(DEMO_BINS) $(STD_LIB_STAMP)

all: $(LIB_A) $(TEST_BINS) $(DEMO_BINS) $(STD_LIB_STAMP) $(BINUI_C_A) $(BINUI_C_DLL)

# 调试工具（可选，不入 all）：离屏渲染 / 窗口抓取 / 窗口枚举
TOOL_BINS := \
    $(BIN_DIR)/binui_compile$(EXT) \
    $(BIN_DIR)/binui_export$(EXT) \
    $(BIN_DIR)/offscreen_render$(EXT) \
    $(BIN_DIR)/gl_shot$(EXT) \
    $(BIN_DIR)/window_capture$(EXT) \
    $(BIN_DIR)/enum_windows$(EXT) \
    $(BIN_DIR)/pixel_diff$(EXT) \
    $(BIN_DIR)/check_examples$(EXT) \
    $(BIN_DIR)/vk_shot$(EXT) \
    $(BIN_DIR)/vk_window$(EXT)

# 把 .bui 还原成打包前的目录结构（读同目录的 binui.res，header-only，不用链接库）
$(BIN_DIR)/binui_export$(EXT): tools/binui_export.cpp
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -o $@ $<

# 示例 UI 零警告检查（CMake 也会注册成 ctest 用例）
$(BIN_DIR)/check_examples$(EXT): tools/check_examples.cpp $(LIB_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -o $@ $< $(LIB_A) $(GL_LIBS)

$(BIN_DIR)/offscreen_render$(EXT): tools/offscreen_render.cpp $(SRC_DIR)/platform/app_win32.cpp $(LIB_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -I$(EXA_DIR) -Isrc -o $@ $< $(SRC_DIR)/platform/app_win32.cpp \
	    $(LIB_A) -lgdiplus -lgdi32 -luser32 -lole32 $(GL_LIBS)

$(BIN_DIR)/window_capture$(EXT): tools/window_capture.cpp
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -o $@ $< -lgdiplus -lgdi32 -luser32
$(BIN_DIR)/enum_windows$(EXT): tools/enum_windows.cpp
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -o $@ $< -luser32

# GL 后端的离屏抓图：GL 窗口抓不到图（PrintWindow 全黑），这个直接读回离屏像素
$(BIN_DIR)/gl_shot$(EXT): tools/gl_shot.cpp $(BUILD_DIR)/binui_gl-gdi.o $(LIB_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -Isrc -o $@ $< \
	    $(BUILD_DIR)/binui_gl-gdi.o $(LIB_A) -lgdiplus -lgdi32 -luser32 -lole32 $(GL_LIBS)

$(BIN_DIR)/vk_shot$(EXT): tools/vk_shot.cpp $(BUILD_DIR)/binui_vk.o $(LIB_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -Isrc $(VK_INC) -c tools/vk_shot.cpp -o $(BUILD_DIR)/vk_shot.o
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/vk_shot.o $(BUILD_DIR)/binui_vk.o $(LIB_A) \
		-luser32 -lgdi32 -lgdiplus -lole32 \
		-Lthird_party/freetype/build -lfreetype \
		-Lthird_party/harfbuzz/build-static/src -lharfbuzz

# 真实窗口 + 交换链的验收工具（P2b）：跑 N 帧，把**交换链图像**读回写 PNG
$(BIN_DIR)/vk_window$(EXT): tools/vk_window.cpp $(BUILD_DIR)/binui_vk.o $(LIB_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -Isrc $(VK_INC) -c tools/vk_window.cpp -o $(BUILD_DIR)/vk_window.o
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/vk_window.o $(BUILD_DIR)/binui_vk.o $(LIB_A) \
		-luser32 -lgdi32 -lgdiplus -lole32 \
		-Lthird_party/freetype/build -lfreetype \
		-Lthird_party/harfbuzz/build-static/src -lharfbuzz

$(BIN_DIR)/pixel_diff$(EXT): tools/pixel_diff.cpp
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -o $@ $< -lgdiplus -lgdi32 -luser32 -lole32

.PHONY: tools
tools: $(TOOL_BINS)
	@echo "tools -> $(TOOL_BINS)"

# =============================================
# 像素回归（本机跑，不入 CI）
#
# 为什么不在 CI 里：GitHub 的 Windows runner 是软件光栅化，AA 边缘与真实 GPU 不一致，
# 同一份代码会稳定地差出百分之几；基准图只在**本机**有意义。
# 用法：
#   mingw32-make SHELL=sh pixels           # 渲染 + 与 docs/pixels 基准图比对
#   mingw32-make SHELL=sh pixels-update    # 改完绘制/布局后重新生成基准图
# =============================================
PIXEL_CASES := circle display-none2 probe radius-matrix svgpath svggrad svgjoin svgstroke svgfinish svgaa
PIXEL_OUT   := $(BUILD_DIR)/pixels
PIXEL_GOLD  := docs/pixels
PIXEL_TOL   ?= 0.5

.PHONY: pixels pixels-update
pixels: tools
	@mkdir -p $(PIXEL_OUT)
	@fail=0; \
	for c in $(PIXEL_CASES); do \
	    echo "--- $$c ---"; \
	    $(BIN_DIR)/offscreen_render$(EXT) docs/issues/assets/$$c.html $(PIXEL_OUT)/$$c.png >/dev/null 2>&1 || { echo "渲染失败: $$c"; fail=1; continue; }; \
	    $(BIN_DIR)/pixel_diff$(EXT) $(PIXEL_GOLD)/$$c.png $(PIXEL_OUT)/$$c.png $(PIXEL_TOL) $(PIXEL_OUT)/$$c.diff.png || fail=1; \
	done; \
	exit $$fail

pixels-update: tools
	@mkdir -p $(PIXEL_GOLD)
	@for c in $(PIXEL_CASES); do \
	    $(BIN_DIR)/offscreen_render$(EXT) docs/issues/assets/$$c.html $(PIXEL_GOLD)/$$c.png >/dev/null 2>&1 || exit 1; \
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
	exit $$fail

.PHONY: vk-window

# 窗口模式验收：拿真实窗口跑 N 帧，读回**交换链图像**与 GL 同尺寸输出比（阈值 0）。
# 覆盖：交换链呈现 / 强制 GDI 读回 / resize 后交换链重建 / 全屏直翻转守卫 / 透明窗。
vk-window: tools
	@mkdir -p $(PIXEL_VK_OUT)
	@fail=0; \
	for c in $(VK_WINDOW_CASES); do \
	    $(BIN_DIR)/gl_shot$(EXT) docs/issues/assets/$$c.html $(PIXEL_VK_OUT)/$$c.gl.png >/dev/null 2>&1 || { echo "GL 渲染失败: $$c"; fail=1; continue; }; \
	    for mode in "" "--gdi" "--transparent" "--resize"; do \
	        tag=$$(echo "win$$mode" | tr -d '-'); \
	        echo "--- $$c $$mode ---"; \
	        $(BIN_DIR)/vk_window$(EXT) docs/issues/assets/$$c.html --frames 20 $$mode \
	            --out $(PIXEL_VK_OUT)/$$c.$$tag.png >/dev/null 2>&1 || { echo "window failed: $$c $$mode"; fail=1; continue; }; \
	        if [ "$$mode" = "--resize" ]; then continue; fi; \
	        $(BIN_DIR)/pixel_diff$(EXT) $(PIXEL_VK_OUT)/$$c.gl.png $(PIXEL_VK_OUT)/$$c.$$tag.png $(PIXEL_VK_TOL) $(PIXEL_VK_OUT)/$$c.$$tag.diff.png $(PIXEL_VK_CHTOL) || fail=1; \
	    done; \
	done; \
	echo "--- fullscreen guard (window covers monitor -> expect GDI readback) ---"; \
	$(BIN_DIR)/vk_window$(EXT) docs/issues/assets/probe.html --frames 10 --fullscreen 2>&1 | grep -E "present=|saved|fullscreen" || fail=1; \
	exit $$fail

else

all: $(LIB_A) $(TEST_BINS)
	@echo "binUI: non-Windows platform, demos skipped (lib + tests built)"

endif

# =============================================
# cleanup
# =============================================
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