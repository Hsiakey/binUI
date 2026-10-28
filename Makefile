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
BIN_DIR  := $(BUILD_DIR)/bin

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

$(BUILD_DIR)/examples/app_win32.o: $(SRC_DIR)/platform/app_win32.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -Isrc -c $< -o $@

$(LIB_A): $(LIB_OBJS)
	ar rcs $@ $^

# =============================================
# 组件二进制：.bum.xml -> .bum（产物落 build 目录，不污染源码树）
# =============================================
COMPONENT_BUMS := \
    $(BUILD_DIR)/assets/components/card.bum \
    $(BUILD_DIR)/assets/components/dock.bum

# 静态链接：构建脚本经常在 sh/PowerShell 等不同环境里调用它，
# 动态 MinGW 运行时（libstdc++-6.dll 等）在某些 PATH 下找不到（sh 下表现为退出码 127）
$(BIN_DIR)/binui_compile$(EXT): tools/binui_compile.cpp $(LIB_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -o $@ $< $(LIB_A)

$(BUILD_DIR)/assets/components/%.bum: $(EXA_DIR)/features/%.bum.xml $(BIN_DIR)/binui_compile$(EXT)
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
# =============================================
TEST_BINS := \
    $(BIN_DIR)/test_binui$(EXT) \
    $(BIN_DIR)/test_renderer$(EXT) \
    $(BIN_DIR)/test_renderer_real$(EXT) \
    $(BIN_DIR)/test_compiler$(EXT) \
    $(BIN_DIR)/test_anim$(EXT) \
    $(BIN_DIR)/test_hot_reload$(EXT) \
    $(BIN_DIR)/test_font$(EXT)

$(BIN_DIR)/test_binui$(EXT): tests/test_binui.cpp $(LIB_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -o $@ $< $(LIB_A)

$(BIN_DIR)/test_renderer$(EXT): tests/test_renderer.cpp $(LIB_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -o $@ $< $(LIB_A)

$(BIN_DIR)/test_renderer_real$(EXT): tests/test_renderer_real.cpp $(LIB_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -o $@ $< $(LIB_A)

$(BIN_DIR)/test_compiler$(EXT): tests/test_compiler.cpp $(LIB_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -o $@ $< $(LIB_A)

$(BIN_DIR)/test_anim$(EXT): tests/test_anim.cpp $(LIB_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -o $@ $< $(LIB_A)

# test_hot_reload 需要 examples/ 下的 binui_paths.h / binui_hot_reload.h
$(BIN_DIR)/test_hot_reload$(EXT): tests/test_hot_reload.cpp $(LIB_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -I$(EXA_DIR) -o $@ $< $(LIB_A)

# test_font 需要 FreeType/HarfBuzz 头与预编译静态库
$(BIN_DIR)/test_font$(EXT): tests/test_font.cpp $(LIB_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RTLINK) -I$(INC_DIR) -Ithird_party/freetype/include -Ithird_party/harfbuzz/src \
	    -o $@ $< $(LIB_A) -Lthird_party/freetype/build -lfreetype \
	    -Lthird_party/harfbuzz/build-static/src -lharfbuzz

.PHONY: test
test: $(TEST_BINS)
	@echo "--- test_binui ---"
	$(BIN_DIR)/test_binui$(EXT)
	@echo "--- test_renderer ---"
	$(BIN_DIR)/test_renderer$(EXT)
	@echo "--- test_renderer_real (run from project root) ---"
	$(BIN_DIR)/test_renderer_real$(EXT)
	@echo "--- test_compiler (run from project root) ---"
	$(BIN_DIR)/test_compiler$(EXT)
	@echo "--- test_anim ---"
	$(BIN_DIR)/test_anim$(EXT)
	@echo "--- test_hot_reload (run from project root) ---"
	$(BIN_DIR)/test_hot_reload$(EXT)
	@echo "--- test_font ---"
	$(BIN_DIR)/test_font$(EXT)

# =============================================
# Win32 demos
# =============================================
ifdef ComSpec

# OpenGL 后端（WGL + FreeType/HarfBuzz 静态库）
GL_LIBS := -lopengl32 -ldwmapi -Lthird_party/freetype/build -lfreetype -Lthird_party/harfbuzz/build-static/src -lharfbuzz

# 6 个示例 = 6 组：功能 / 动画 / 字体 / 2D 渲染 / 3D 渲染 / 纹理
DEMO_BINS := \
    $(BIN_DIR)/features$(EXT) \
    $(BIN_DIR)/animshow$(EXT) \
    $(BIN_DIR)/fontshow$(EXT) \
    $(BIN_DIR)/svgdemo$(EXT) \
    $(BIN_DIR)/cubedemo$(EXT) \
    $(BIN_DIR)/texturedemo$(EXT)

# cmd 的 copy 不识别正斜杠路径，转成反斜杠
# 资源拷贝一律用 cp + 正斜杠路径。
# 原先写 `cmd /c copy /Y examples\x\y build-mingw\bin\x\y`：SHELL=sh 时反斜杠被 sh 当转义吃掉，
# cmd 收到不存在的路径，只打印 banner 就退出 —— 演示 UI 其实从没被拷到 bin/ 下（静默失败）。
WIN_BIN := $(BIN_DIR)

# 标准组件库：demo 的 main.html 用 ../../components/xxx.bum.xml 引用，
# 从 $(BIN_DIR)/<demo>/ 往上两级正好是 $(BUILD_DIR)/，所以拷到这里
STD_LIB_SRCS  := $(wildcard components/*.bum.xml)
STD_LIB_STAMP := $(BUILD_DIR)/components/.stamp

$(STD_LIB_STAMP): $(STD_LIB_SRCS)
	@mkdir -p $(BUILD_DIR)/components
	@cp -f $(STD_LIB_SRCS) $(BUILD_DIR)/components/
	@touch $@

$(BUILD_DIR)/examples/%.o: $(EXA_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -Isrc -I$(EXA_DIR) -c $< -o $@

$(BIN_DIR)/features$(EXT): $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/features/main.o $(LIB_A) $(COMPONENT_BUMS)
	@mkdir -p $(BIN_DIR)/features
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/features/main.o $(LIB_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/features/main.html $(WIN_BIN)/features/main.html
	cp -f $(EXA_DIR)/features/button.bum.xml $(WIN_BIN)/features/button.bum.xml
	cp -f $(BUILD_DIR)/assets/components/card.bum $(WIN_BIN)/features/card.bum
	cp -f $(BUILD_DIR)/assets/components/dock.bum $(WIN_BIN)/features/dock.bum

$(BIN_DIR)/animshow$(EXT): $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/animshow/main.o $(LIB_A)
	@mkdir -p $(BIN_DIR)/animshow
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/animshow/main.o $(LIB_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/animshow/main.html $(WIN_BIN)/animshow/main.html

$(BIN_DIR)/fontshow$(EXT): $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/fontshow/main.o $(LIB_A)
	@mkdir -p $(BIN_DIR)/fontshow
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/fontshow/main.o $(LIB_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/fontshow/main.html $(WIN_BIN)/fontshow/main.html

$(BIN_DIR)/svgdemo$(EXT): $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/svgdemo/main.o $(LIB_A)
	@mkdir -p $(BIN_DIR)/svgdemo
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/svgdemo/main.o $(LIB_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/svgdemo/main.html $(WIN_BIN)/svgdemo/main.html

$(BIN_DIR)/cubedemo$(EXT): $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/cubedemo/main.o $(LIB_A)
	@mkdir -p $(BIN_DIR)/cubedemo
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/cubedemo/main.o $(LIB_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/cubedemo/main.html $(WIN_BIN)/cubedemo/main.html

$(BIN_DIR)/texturedemo$(EXT): $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/texturedemo/main.o $(LIB_A)
	@mkdir -p $(BIN_DIR)/texturedemo
	$(CXX) $(LDFLAGS) $(RTLINK) -o $@ $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/texturedemo/main.o $(LIB_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/texturedemo/main.html $(WIN_BIN)/texturedemo/main.html

all: $(LIB_A) $(TEST_BINS) $(DEMO_BINS) $(STD_LIB_STAMP)

# 调试工具（可选，不入 all）：离屏渲染 / 窗口抓取 / 窗口枚举
TOOL_BINS := \
    $(BIN_DIR)/binui_compile$(EXT) \
    $(BIN_DIR)/offscreen_render$(EXT) \
    $(BIN_DIR)/window_capture$(EXT) \
    $(BIN_DIR)/enum_windows$(EXT) \
    $(BIN_DIR)/check_examples$(EXT)

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

.PHONY: tools
tools: $(TOOL_BINS)
	@echo "tools -> $(TOOL_BINS)"

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
         $(wildcard $(BUILD_DIR)/examples/*.d) \
         $(wildcard $(BUILD_DIR)/examples/*/*.d)