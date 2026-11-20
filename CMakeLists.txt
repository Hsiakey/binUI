# CMakeLists.txt - binUI 跨平台描述式 UI 核心
# 核心库 binui：XML/HTML5 -> binUI 编译 + binUI 解析/命中测试/事件（纯 C++，无平台依赖）
# 字体渲染：FreeType + HarfBuzz（third_party 源码，静态链接）
cmake_minimum_required(VERSION 3.16)
project(binui
    VERSION 0.20.0
    DESCRIPTION "Cross-platform declarative UI core (binUI compiler + renderer)"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 源码含中文注释（UTF-8 无 BOM）：MSVC 默认按 GBK 读取会误读多字节字符；
# 代码使用 std::min/std::max，需禁用 windows.h 的 min/max 宏
if(MSVC)
    add_compile_options(/utf-8)
    add_compile_definitions(NOMINMAX)
endif()

# =============================================
# 第三方：FreeType（字形解析/光栅化）+ HarfBuzz（整形）
#
# 0.38.x 起 binUI 自己**不再 include** 这两个库：字形光栅 / 整形 / 图集在 bin2d 的
# 可选模块 libbin2d_text 里，binUI 只负责"平台字体发现"（src/font/binui_font_host.cpp）
# 与 CSS 语义。这里保留 subdirectory 是为了让 Makefile 那条 `text-lib` 目标能找到
# 静态库；CMake 侧本身通过 libbin2d_text.a 间接依赖它们。
# =============================================
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
set(FT_DISABLE_LZMA ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/freetype EXCLUDE_FROM_ALL)

set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
set(HB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(HB_BUILD_UTILS OFF CACHE BOOL "" FORCE)
set(HB_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(HB_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(HB_HAVE_FREETYPE OFF CACHE BOOL "" FORCE)
set(HB_HAVE_GLIB OFF CACHE BOOL "" FORCE)
set(HB_HAVE_GRAPHITE2 OFF CACHE BOOL "" FORCE)
set(HB_HAVE_ICU OFF CACHE BOOL "" FORCE)
set(HB_HAVE_CAIRO OFF CACHE BOOL "" FORCE)
set(HB_HAVE_GOBJECT OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/harfbuzz EXCLUDE_FROM_ALL)

# =============================================
# 核心库 binui（纯 C++ + 字体服务，可移植到任意平台）
# =============================================
# bin2d（P2 迁移）：<path> 的填充光栅化在 bin2d 里做，见 src/backends/binui_paint.cpp。
# 所以 bin2d 是硬依赖：先 `make -C ../bin2D lib`（或用 CMake 构建 bin2D）。
set(BIN2D_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../bin2D")
set(BIN2D_LIB "${BIN2D_ROOT}/build-mingw/libbin2d.a")
# 文字模块（0.38.x）：字形光栅 / 整形 / 图集 / 文字阴影。核心库不链它，
# binUI 的绘制后端要画字，所以这里和上面的核心库一样按"预构建的静态库"接。
set(BIN2D_TEXT_LIB "${BIN2D_ROOT}/build-mingw/libbin2d_text.a")

add_library(binui STATIC
target_include_directories(binui PUBLIC "${BIN2D_ROOT}/include")
if(EXISTS "${BIN2D_LIB}")
    target_link_libraries(binui PUBLIC "${BIN2D_LIB}")
else()
    message(WARNING "bin2d 静态库不存在：${BIN2D_LIB}（先 make -C ../bin2D lib）")
endif()
if(EXISTS "${BIN2D_TEXT_LIB}")
    target_link_libraries(binui PUBLIC "${BIN2D_TEXT_LIB}")
else()
    message(WARNING "bin2d 文字模块不存在：${BIN2D_TEXT_LIB}（先 make -C ../bin2D text-lib）")
endif()
    src/compiler/binui_compiler.cpp
    src/renderer/binui_renderer.cpp
    src/font/binui_font_host.cpp
    src/common/binui_link.cpp
    src/binui_charts.cpp
)
target_include_directories(binui PUBLIC ${CMAKE_SOURCE_DIR}/include PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(binui PUBLIC freetype harfbuzz)

# UI 源码 -> 二进制布局（.bui / .bum）的命令行编译器；构建期编组件、CI 里也能单独用
add_executable(binui_compile tools/binui_compile.cpp)
target_link_libraries(binui_compile PRIVATE binui)

# .bui / .bum -> 打包前的目录结构（读同目录的 binui.res；header-only，不依赖库）
add_executable(binui_export tools/binui_export.cpp)

# =============================================
# Win32 演示外壳 + 示例
# =============================================
if(WIN32)
    # 绘制后端（0.37.x 起**只剩一份**）：binUI ──► bin2d ──► binGPU。
    # 以前这里是 gl_shell（src/backends/binui_gl.cpp）；那份与它的两个兄弟
    # （binui_vk / binui_d3d11）、binui_gpu、binui_auto 已在 2026-11-20 整份删除。
    # CMake 侧没接 binGPU 的构建，所以这里按"没有 libbingpu.a"处理（BINUI_NO_BINGPU）：
    # CPU 出图 + GDI 贴屏，窗口照样能开；要 GPU 上屏请用 Makefile 构建。
    add_library(bin2d_backend STATIC
        src/backends/binui_bin2d.cpp
        src/backends/binui_paint.cpp)
    target_compile_definitions(bin2d_backend PRIVATE BINUI_NO_BINGPU)
    target_include_directories(bin2d_backend PUBLIC ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(bin2d_backend PUBLIC binui gdiplus gdi32 user32
        dwmapi imm32 msimg32 ${CMAKE_SOURCE_DIR}/../bin2D/build-mingw/libbin2d.a)

    # C ABI：给 Python / Go / Rust 等宿主语言的唯一入口（include/binui_c.h）。
    # 静态库给 C/C++/Rust 直接链，动态库给 ctypes / cgo（免编译，随包分发）。
    add_library(binui_c STATIC src/binui_c.cpp)
    target_link_libraries(binui_c PUBLIC binui bin2d_backend)

    add_library(binui_c_shared SHARED src/binui_c.cpp)
    target_compile_definitions(binui_c_shared PRIVATE BINUI_C_BUILD_DLL)
    target_link_libraries(binui_c_shared PRIVATE binui bin2d_backend)
    set_target_properties(binui_c_shared PROPERTIES OUTPUT_NAME binui_c)

    # 6 个示例 = 6 组：功能 / 动画 / 字体 / 2D 渲染 / 3D 渲染 / 纹理
    # .bum 由 binui_compile 在构建期从 .bum.html 生成，不进源码树
    set(BINUI_COMPONENT_BUMS "")
    foreach(cname card dock)
        set(bum_out "${CMAKE_BINARY_DIR}/assets/components/${cname}.bum")
        add_custom_command(
            OUTPUT "${bum_out}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/assets/components"
            COMMAND $<TARGET_FILE:binui_compile>
                    "${CMAKE_SOURCE_DIR}/examples/features/${cname}.bum.html" -o "${bum_out}"
            DEPENDS binui_compile "${CMAKE_SOURCE_DIR}/examples/features/${cname}.bum.html"
            COMMENT "binui: ${cname}.bum.html -> ${cname}.bum"
            VERBATIM
        )
        list(APPEND BINUI_COMPONENT_BUMS "${bum_out}")
    endforeach()
    add_custom_target(components_bums ALL DEPENDS ${BINUI_COMPONENT_BUMS})

    # 通用 demo 目标：链接 bin2d_backend，产物落 bin/examples/，UI 拷到 <exe 目录>/<name>/
    function(add_bin2d_demo name)
        add_executable(${name} examples/${name}/main.cpp)
        target_link_libraries(${name} PRIVATE bin2d_backend)
        set_target_properties(${name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/examples")
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${name}>/${name}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/examples/${name}/main.html"
                "$<TARGET_FILE_DIR:${name}>/${name}/main.html"
        )
    endfunction()

    # 示例 UI 用 ../../components/xxx.bum.html 引用标准组件（相对 <exe 目录>/<name>/ 往上两级
    # = bin/），所以把源码组件同步到 bin/components/
    add_custom_target(std_components ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin/components"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/components" "${CMAKE_BINARY_DIR}/bin/components"
        COMMENT "binui: components -> bin/components"
    )

    add_bin2d_demo(animshow)
    add_bin2d_demo(fontshow)
    add_bin2d_demo(svgdemo)
    add_bin2d_demo(cubedemo)
    add_bin2d_demo(texturedemo)
    add_bin2d_demo(buttonshow)
    add_bin2d_demo(flexshow)   # 唯一开窗可缩放的示例（其余固定布局的示例都 window_resize="disable"）

    # features 额外需要组件文件（源码组件 + 两个 .bum）
    add_bin2d_demo(features)
    add_dependencies(features components_bums)
    add_custom_command(TARGET features POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/examples/features/button.bum.html"
            "${CMAKE_BINARY_DIR}/assets/components/card.bum"
            "${CMAKE_BINARY_DIR}/assets/components/dock.bum"
            "$<TARGET_FILE_DIR:features>/features/"
    )

else()
    message(STATUS "binUI: 仅实现了 Win32 演示外壳；核心库 binui 与测试仍可构建")
endif()

# =============================================
# Tests
#
# 只有四个：test_css / test_elements / test_font / test_capi。
# 一个主题一个二进制，套件内部分节；别再为一个新特性新开 test_xxx（见 CHANGELOG 的合并记录）。
# 产物与 Makefile 一致，落 bin/tests/。
# =============================================
enable_testing()

function(add_binui_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE binui)
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests")
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_binui_test(test_css tests/test_css.cpp)
add_binui_test(test_elements tests/test_elements.cpp)
add_binui_test(test_font tests/test_font.cpp)

# 示例 UI 零警告检查：examples/ 下所有 *.html、*.binui.xml 必须编译成功且无警告
# pixel_diff：像素回归的比对工具（.cpp 里用 GDI+，只在 Windows 下有用）
if(WIN32)
    add_executable(pixel_diff tools/pixel_diff.cpp)
    target_link_libraries(pixel_diff PRIVATE gdiplus gdi32 user32 ole32)

    # baseline_snapshot：迁移基线快照（三端像素指纹 + 原语流哈希 + 画质数字）
    add_executable(baseline_snapshot tools/baseline_snapshot.cpp)
    target_link_libraries(baseline_snapshot PRIVATE gdiplus gdi32 user32 ole32)
endif()

add_executable(check_examples tools/check_examples.cpp)
target_link_libraries(check_examples PRIVATE binui)
add_test(NAME check_examples COMMAND check_examples "${CMAKE_SOURCE_DIR}/examples")

# test_css / test_elements 会读写仓库内的相对路径（tests/fixtures、build-mingw/tmp），
# 工作目录必须是源码树根
set_tests_properties(test_css test_elements PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# test_elements 里的热重载用例要 examples/ 下的头（binui_paths.h / binui_hot_reload.h）
target_include_directories(test_elements PRIVATE ${CMAKE_SOURCE_DIR}/examples)

# test_font：字体服务（FreeType 光栅化 + HarfBuzz 整形）+ 输入法（IME）UTF-16 光标桥接
# 头文件与静态库通过 binui 的 PUBLIC 依赖带进来

# test_capi：C ABI 冒烟测试（不开窗口：编译 / 加载 / 节点查询 / 改文本 / 错误路径）
if(WIN32)
    add_binui_test(test_capi tests/test_capi.cpp)
    target_link_libraries(test_capi PRIVATE binui_c)
endif()

# =============================================
# Install
# =============================================
install(TARGETS binui
    ARCHIVE DESTINATION lib
)
if(WIN32)
    install(TARGETS binui_c binui_c_shared
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin
    )
endif()
install(FILES
    include/binui_format.h
    include/binui_renderer.h
    include/binui_compiler.h
    include/binui_link.h
    include/binui_asset.h
    include/binui_component.h
    include/binui_srcmap.h
    include/binui_paths.h
    include/binui_hot_reload.h
    include/binui_paint.h
    include/binui_path.h
    include/binui_c.h
    DESTINATION include/binui
)

message(STATUS "binUI v${PROJECT_VERSION}")
message(STATUS "  Core lib  -> binui")
if(WIN32)
    message(STATUS "  Demos     -> bin/examples: features / animshow / fontshow(字体与输入法) / buttonshow / flexshow(自适应布局) / svgdemo / cubedemo / texturedemo (OpenGL 3.3 borderless)")
endif()
message(STATUS "  Tests     -> bin/tests: test_css, test_elements, test_font, test_capi")
