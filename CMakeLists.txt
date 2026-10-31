# CMakeLists.txt - binUI 跨平台描述式 UI 核心
# 核心库 binui：XML/HTML5 -> binUI 编译 + binUI 解析/命中测试/事件（纯 C++，无平台依赖）
# 字体渲染：FreeType + HarfBuzz（third_party 源码，静态链接）
cmake_minimum_required(VERSION 3.16)
project(binui
    VERSION 0.2.0
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
add_library(binui STATIC
    src/compiler/binui_compiler.cpp
    src/renderer/binui_renderer.cpp
    src/font/binui_font_renderer.cpp
    src/common/binui_link.cpp
    src/binui_charts.cpp
)
target_include_directories(binui PUBLIC ${CMAKE_SOURCE_DIR}/include PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(binui PUBLIC freetype harfbuzz)

# UI 源码 -> 二进制布局（.bui / .bum）的命令行编译器；构建期编组件、CI 里也能单独用
add_executable(binui_compile tools/binui_compile.cpp)
target_link_libraries(binui_compile PRIVATE binui)

# =============================================
# Win32 演示外壳 + 示例
# =============================================
if(WIN32)
    add_library(app_shell STATIC
        src/platform/app_win32.cpp
    )
    target_include_directories(app_shell PUBLIC ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(app_shell PUBLIC binui user32 gdi32 gdiplus ole32)

    # OpenGL 渲染后端（WGL + GL 3.3 core）：无边框窗口 + 字形图集 + 顶点四边形
    add_library(gl_shell STATIC src/backends/binui_gl-gdi.cpp)
    target_include_directories(gl_shell PUBLIC ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(gl_shell PUBLIC binui opengl32 gdiplus dwmapi)

    # C ABI：给 Python / Go / Rust 等宿主语言的唯一入口（include/binui_c.h）。
    # 静态库给 C/C++/Rust 直接链，动态库给 ctypes / cgo（免编译，随包分发）。
    add_library(binui_c STATIC src/binui_c.cpp)
    target_link_libraries(binui_c PUBLIC binui gl_shell)

    add_library(binui_c_shared SHARED src/binui_c.cpp)
    target_compile_definitions(binui_c_shared PRIVATE BINUI_C_BUILD_DLL)
    target_link_libraries(binui_c_shared PRIVATE binui gl_shell)
    set_target_properties(binui_c_shared PROPERTIES OUTPUT_NAME binui_c)

    function(add_binui_demo name src_dir ui_file)
        add_executable(${name} ${src_dir}/main.cpp)
        target_link_libraries(${name} PRIVATE app_shell binui)
        if(MSVC)
            set_target_properties(${name} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
        endif()
        # 构建后把 UI 源码复制到可执行文件旁的子目录，演示程序无需路径参数即可运行
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${name}>/${name}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/${src_dir}/${ui_file}"
                "$<TARGET_FILE_DIR:${name}>/${name}/${ui_file}"
        )
    endfunction()

    # 6 个示例 = 6 组：功能 / 动画 / 字体 / 2D 渲染 / 3D 渲染 / 纹理
    # .bum 由 binui_compile 在构建期从 .bum.xml 生成，不进源码树
    set(BINUI_COMPONENT_BUMS "")
    foreach(cname card dock)
        set(bum_out "${CMAKE_BINARY_DIR}/assets/components/${cname}.bum")
        add_custom_command(
            OUTPUT "${bum_out}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/assets/components"
            COMMAND $<TARGET_FILE:binui_compile>
                    "${CMAKE_SOURCE_DIR}/examples/features/${cname}.bum.xml" -o "${bum_out}"
            DEPENDS binui_compile "${CMAKE_SOURCE_DIR}/examples/features/${cname}.bum.xml"
            COMMENT "binui: ${cname}.bum.xml -> ${cname}.bum"
            VERBATIM
        )
        list(APPEND BINUI_COMPONENT_BUMS "${bum_out}")
    endforeach()
    add_custom_target(components_bums ALL DEPENDS ${BINUI_COMPONENT_BUMS})

    # 通用 demo 目标：链接 gl_shell，构建后把 UI 文件拷到 <exe>/<name>/
    function(add_gl_demo name)
        add_executable(${name} examples/${name}/main.cpp)
        target_link_libraries(${name} PRIVATE gl_shell)
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${name}>/${name}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/examples/${name}/main.html"
                "$<TARGET_FILE_DIR:${name}>/${name}/main.html"
        )
    endfunction()

    add_gl_demo(animshow)
    add_gl_demo(fontshow)
    add_gl_demo(svgdemo)
    add_gl_demo(cubedemo)
    add_gl_demo(texturedemo)

    # features 额外需要组件文件（源码组件 + 两个 .bum）
    add_gl_demo(features)
    add_dependencies(features components_bums)
    add_custom_command(TARGET features POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/examples/features/button.bum.xml"
            "${CMAKE_BINARY_DIR}/assets/components/card.bum"
            "${CMAKE_BINARY_DIR}/assets/components/dock.bum"
            "$<TARGET_FILE_DIR:features>/features/"
    )

else()
    message(STATUS "binUI: 仅实现了 Win32 演示外壳；核心库 binui 与测试仍可构建")
endif()

# =============================================
# Tests
# =============================================
enable_testing()

function(add_binui_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE binui)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_binui_test(test_binui tests/test_binui.cpp)
add_binui_test(test_renderer tests/test_renderer.cpp)
add_binui_test(test_compiler tests/test_compiler.cpp)
add_binui_test(test_anim tests/test_anim.cpp)

# 示例 UI 零警告检查：examples/ 下所有 *.html、*.binui.xml 必须编译成功且无警告
add_executable(check_examples tools/check_examples.cpp)
target_link_libraries(check_examples PRIVATE binui)
add_test(NAME check_examples COMMAND check_examples "${CMAKE_SOURCE_DIR}/examples")

# test_renderer_real / test_compiler 需要访问源码树下的 examples/
set_tests_properties(test_compiler PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_binui_test(test_renderer_real tests/test_renderer_real.cpp)
set_tests_properties(test_renderer_real PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# test_font：字体服务（FreeType 光栅化 + HarfBuzz 整形）与渲染质量参数
add_binui_test(test_font tests/test_font.cpp)

# test_capi：C ABI 冒烟测试（不开窗口：编译 / 加载 / 节点查询 / 改文本 / 错误路径）
if(WIN32)
    add_binui_test(test_capi tests/test_capi.cpp)
    target_link_libraries(test_capi PRIVATE binui_c)
endif()

# test_hot_reload：路径解析 + 热重载（需要 examples/ 下的 header，工作目录须为源码树根）
add_binui_test(test_hot_reload tests/test_hot_reload.cpp)
target_include_directories(test_hot_reload PRIVATE ${CMAKE_SOURCE_DIR}/examples)
set_tests_properties(test_hot_reload PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

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
    include/binui_paths.h
    include/binui_hot_reload.h
    include/binui_paint.h
    include/binui_c.h
    DESTINATION include/binui
)

message(STATUS "binUI v${PROJECT_VERSION}")
message(STATUS "  Core lib  -> binui")
if(WIN32)
    message(STATUS "  Demos     -> features / animshow / fontshow / svgdemo / cubedemo / texturedemo (OpenGL 3.3 borderless)")
endif()
message(STATUS "  Tests     -> test_binui, test_renderer, test_compiler")
