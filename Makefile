# Makefile - binUI (MinGW / GCC / Clang)
# Core lib binui (pure C++) builds on any platform; Win32 demos build on Windows only.
# CMake is also supported.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -MMD -MP
LDFLAGS  ?=

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
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -o $@ $< $(LIB_A)

$(BIN_DIR)/test_renderer$(EXT): tests/test_renderer.cpp $(LIB_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -o $@ $< $(LIB_A)

$(BIN_DIR)/test_renderer_real$(EXT): tests/test_renderer_real.cpp $(LIB_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -o $@ $< $(LIB_A)

$(BIN_DIR)/test_compiler$(EXT): tests/test_compiler.cpp $(LIB_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -o $@ $< $(LIB_A)

$(BIN_DIR)/test_anim$(EXT): tests/test_anim.cpp $(LIB_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -o $@ $< $(LIB_A)

# test_hot_reload 需要 examples/ 下的 binui_paths.h / binui_hot_reload.h
$(BIN_DIR)/test_hot_reload$(EXT): tests/test_hot_reload.cpp $(LIB_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -I$(EXA_DIR) -o $@ $< $(LIB_A)

# test_font 需要 FreeType/HarfBuzz 头与预编译静态库
$(BIN_DIR)/test_font$(EXT): tests/test_font.cpp $(LIB_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -Ithird_party/freetype/include -Ithird_party/harfbuzz/src \
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

DEMO_BINS := \
    $(BIN_DIR)/vision_monitor$(EXT) \
    $(BIN_DIR)/animshow$(EXT) \
    $(BIN_DIR)/svgdemo$(EXT) \
    $(BIN_DIR)/cssshow$(EXT) \
    $(BIN_DIR)/singlewindow$(EXT) \
    $(BIN_DIR)/multiwindow$(EXT) \
    $(BIN_DIR)/galgamechat$(EXT) \
    $(BIN_DIR)/videodemo$(EXT)

# cmd 的 copy 不识别正斜杠路径，转成反斜杠
# 资源拷贝一律用 cp + 正斜杠路径。
# 原先写 `cmd /c copy /Y examples\x\y build-mingw\bin\x\y`：SHELL=sh 时反斜杠被 sh 当转义吃掉，
# cmd 收到不存在的路径，只打印 banner 就退出 —— 演示 UI 其实从没被拷到 bin/ 下（静默失败）。
WIN_BIN := $(BIN_DIR)

$(BUILD_DIR)/examples/%.o: $(EXA_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -Isrc -I$(EXA_DIR) -c $< -o $@

$(BIN_DIR)/vision_monitor$(EXT): $(BUILD_DIR)/examples/app_win32.o $(BUILD_DIR)/examples/vision_monitor/main.o $(LIB_A)
	@mkdir -p $(BIN_DIR)/vision_monitor
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIB_A) $(WINLIBS)
	cp -f $(EXA_DIR)/vision_monitor/main.binui.xml $(WIN_BIN)/vision_monitor/main.binui.xml

$(BIN_DIR)/animshow$(EXT): $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/animshow/main.o $(LIB_A)
	@mkdir -p $(BIN_DIR)/animshow
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIB_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/animshow/main.html $(WIN_BIN)/animshow/main.html

$(BIN_DIR)/svgdemo$(EXT): $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/svgdemo/main.o $(LIB_A)
	@mkdir -p $(BIN_DIR)/svgdemo
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIB_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/svgdemo/main.html $(WIN_BIN)/svgdemo/main.html

$(BIN_DIR)/cssshow$(EXT): $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/cssshow/main.o $(LIB_A)
	@mkdir -p $(BIN_DIR)/cssshow
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIB_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/cssshow/main.html $(WIN_BIN)/cssshow/main.html

$(BIN_DIR)/singlewindow$(EXT): $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/singlewindow/main.o $(LIB_A)
	@mkdir -p $(BIN_DIR)/singlewindow
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIB_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/singlewindow/main.html $(WIN_BIN)/singlewindow/main.html
	cp -f examples/assets/noela.png $(WIN_BIN)/singlewindow/NOELA_N001_1227x1227.png

$(BIN_DIR)/multiwindow$(EXT): $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/multiwindow/main.o $(LIB_A)
	@mkdir -p $(BIN_DIR)/multiwindow
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIB_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/multiwindow/stage.html $(WIN_BIN)/multiwindow/stage.html
	cp -f $(EXA_DIR)/multiwindow/hero.html $(WIN_BIN)/multiwindow/hero.html
	cp -f $(EXA_DIR)/multiwindow/dialog.html $(WIN_BIN)/multiwindow/dialog.html
	cp -f examples/assets/noela.png $(WIN_BIN)/multiwindow/NOELA_N001_1227x1227.png

$(BIN_DIR)/galgamechat$(EXT): $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/galgamechat/main.o $(LIB_A)
	@mkdir -p $(BIN_DIR)/galgamechat
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIB_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/galgamechat/main.html $(WIN_BIN)/galgamechat/main.html
	cp -f examples/assets/noela.png $(WIN_BIN)/galgamechat/NOELA_N001_1227x1227.png

# videodemo 使用桩视频模块（binui_video_stub），无 FFmpeg 依赖
$(BUILD_DIR)/binui_video_stub.o: $(SRC_DIR)/binui_video_stub.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -c $< -o $@

$(BIN_DIR)/videodemo$(EXT): $(BUILD_DIR)/binui_gl-gdi.o $(BUILD_DIR)/examples/videodemo/main.o $(BUILD_DIR)/binui_video_stub.o $(LIB_A)
	@mkdir -p $(BIN_DIR)/videodemo
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIB_A) $(WINLIBS) $(GL_LIBS)
	cp -f $(EXA_DIR)/videodemo/main.html $(WIN_BIN)/videodemo/main.html
	cp -rf $(EXA_DIR)/videodemo/media $(WIN_BIN)/videodemo/media

all: $(LIB_A) $(TEST_BINS) $(DEMO_BINS)

# 调试工具（可选，不入 all）：离屏渲染 / 窗口抓取 / 窗口枚举
TOOL_BINS := \
    $(BIN_DIR)/offscreen_render$(EXT) \
    $(BIN_DIR)/window_capture$(EXT) \
    $(BIN_DIR)/enum_windows$(EXT) \
    $(BIN_DIR)/check_examples$(EXT)

# 示例 UI 零警告检查（CMake 也会注册成 ctest 用例）
$(BIN_DIR)/check_examples$(EXT): tools/check_examples.cpp $(LIB_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -o $@ $< $(LIB_A) $(GL_LIBS)

$(BIN_DIR)/offscreen_render$(EXT): tools/offscreen_render.cpp $(SRC_DIR)/platform/app_win32.cpp $(LIB_A)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -I$(EXA_DIR) -Isrc -o $@ $< $(SRC_DIR)/platform/app_win32.cpp \
	    $(LIB_A) -lgdiplus -lgdi32 -luser32 -lole32 $(GL_LIBS)

$(BIN_DIR)/window_capture$(EXT): tools/window_capture.cpp
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $< -lgdiplus -lgdi32 -luser32

$(BIN_DIR)/enum_windows$(EXT): tools/enum_windows.cpp
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $< -luser32

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
-include $(wildcard $(BUILD_DIR)/*.d) \
         $(wildcard $(BUILD_DIR)/compiler/*.d) \
         $(wildcard $(BUILD_DIR)/renderer/*.d) \
         $(wildcard $(BUILD_DIR)/font/*.d) \
         $(wildcard $(BUILD_DIR)/examples/*.d) \
         $(wildcard $(BUILD_DIR)/examples/*/*.d)