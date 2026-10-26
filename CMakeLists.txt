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
    src/binui_charts.cpp
)
target_include_directories(binui PUBLIC ${CMAKE_SOURCE_DIR}/include PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(binui PUBLIC freetype harfbuzz)

# =============================================
# Win32 演示外壳 + 示例
# =============================================
if(WIN32)
    add_library(app_shell STATIC
        src/platform/app_win32.cpp
    )
    target_include_directories(app_shell PUBLIC ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(app_shell PUBLIC binui user32 gdi32 gdiplus ole32)

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

    add_binui_demo(vision_monitor examples/vision_monitor main.binui.xml)

    # OpenGL 渲染后端（WGL + GL 3.3 core）：无边框窗口 + 字形图集 + 顶点四边形
    add_library(gl_shell STATIC src/backends/binui_gl-gdi.cpp)
    target_include_directories(gl_shell PUBLIC ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(gl_shell PUBLIC binui opengl32 gdiplus dwmapi)

    # animshow：动画实验室，OpenGL 后端
    add_executable(animshow examples/animshow/main.cpp)
    target_link_libraries(animshow PRIVATE gl_shell)
    add_custom_command(TARGET animshow POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:animshow>/animshow"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/examples/animshow/main.html"
            "$<TARGET_FILE_DIR:animshow>/animshow/main.html"
    )

    # svgdemo：<svg> 声明式矢量 + <canvas> 命令式绘制面
    add_executable(svgdemo examples/svgdemo/main.cpp)
    target_link_libraries(svgdemo PRIVATE gl_shell)
    add_custom_command(TARGET svgdemo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:svgdemo>/svgdemo"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/examples/svgdemo/main.html"
            "$<TARGET_FILE_DIR:svgdemo>/svgdemo/main.html"
    )

    # cssshow：CSS 伪类（:hover/:active/:focus/:checked）+ 伪元素（::before/::after）
    add_executable(cssshow examples/cssshow/main.cpp)
    target_link_libraries(cssshow PRIVATE gl_shell)
    add_custom_command(TARGET cssshow POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:cssshow>/cssshow"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/examples/cssshow/main.html"
            "$<TARGET_FILE_DIR:cssshow>/cssshow/main.html"
    )

    # singlewindow：单窗口模式（透明 PNG 立绘 + z-index 分层 + 运行时换图 + 等比自适应）
    add_executable(singlewindow examples/singlewindow/main.cpp)
    target_link_libraries(singlewindow PRIVATE gl_shell)
    add_custom_command(TARGET singlewindow POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:singlewindow>/singlewindow"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/examples/singlewindow/main.html"
            "$<TARGET_FILE_DIR:singlewindow>/singlewindow/main.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/examples/assets/noela.png"
            "$<TARGET_FILE_DIR:singlewindow>/singlewindow/NOELA_N001_1227x1227.png"
    )

    # multiwindow：独立窗口模式（多进程·多窗口：stage/hero/dialog，立绘为透明分层窗口）
    add_executable(multiwindow examples/multiwindow/main.cpp)
    target_link_libraries(multiwindow PRIVATE gl_shell)
    add_custom_command(TARGET multiwindow POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:multiwindow>/multiwindow"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/examples/multiwindow/stage.html"
            "$<TARGET_FILE_DIR:multiwindow>/multiwindow/stage.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/examples/multiwindow/hero.html"
            "$<TARGET_FILE_DIR:multiwindow>/multiwindow/hero.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/examples/multiwindow/dialog.html"
            "$<TARGET_FILE_DIR:multiwindow>/multiwindow/dialog.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/examples/assets/noela.png"
            "$<TARGET_FILE_DIR:multiwindow>/multiwindow/NOELA_N001_1227x1227.png"
    )

    # galgamechat：Galgame 式 Agent 前端原型（逐字打字机 + 上滚 backlog + 输入回合 + 后端钩子）
    add_executable(galgamechat examples/galgamechat/main.cpp)
    target_link_libraries(galgamechat PRIVATE gl_shell)
    add_custom_command(TARGET galgamechat POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:galgamechat>/galgamechat"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/examples/galgamechat/main.html"
            "$<TARGET_FILE_DIR:galgamechat>/galgamechat/main.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/examples/assets/noela.png"
            "$<TARGET_FILE_DIR:galgamechat>/galgamechat/NOELA_N001_1227x1227.png"
    )

    # =============================================
    # 可选视频编解码模块（<video> 原生支持）
    # OFF（默认）：binui_video 为桩，视频节点仅绘制底色，核心库零依赖；
    # ON：FFmpeg 解码 + WASAPI 音频（需 FFMPEG_ROOT 指向含 include/、lib/ 的开发目录）
    # =============================================
    option(BINUI_ENABLE_VIDEO "Enable FFmpeg video codec module (<video> playback)" OFF)
    if(BINUI_ENABLE_VIDEO)
        set(FFMPEG_ROOT "" CACHE PATH "FFmpeg dev prefix containing include/ and lib/")
        if(NOT FFMPEG_ROOT)
            message(FATAL_ERROR "BINUI_ENABLE_VIDEO=ON requires FFMPEG_ROOT (FFmpeg dev dir with include/ and lib/)")
        endif()
        add_library(binui_video STATIC src/binui_video_ffmpeg.cpp)
        target_include_directories(binui_video PUBLIC "${FFMPEG_ROOT}/include")
        target_link_directories(binui_video PUBLIC "${FFMPEG_ROOT}/lib")
        target_link_libraries(binui_video PUBLIC binui
            avformat avcodec swscale swresample avutil ole32)
    else()
        add_library(binui_video STATIC src/binui_video_stub.cpp)
        target_link_libraries(binui_video PUBLIC binui)
    endif()

    # videodemo：<video> 播放器（GL 后端；控制条/交互始终可用，
    # 实际解码与声音取决于 BINUI_ENABLE_VIDEO）
    add_executable(videodemo examples/videodemo/main.cpp)
    target_link_libraries(videodemo PRIVATE gl_shell binui_video)
    add_custom_command(TARGET videodemo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:videodemo>/videodemo"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/examples/videodemo/main.html"
            "$<TARGET_FILE_DIR:videodemo>/videodemo/main.html"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/examples/videodemo/media"
            "$<TARGET_FILE_DIR:videodemo>/videodemo/media"
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
install(FILES
    include/binui_format.h
    include/binui_renderer.h
    include/binui_compiler.h
    DESTINATION include/binui
)

message(STATUS "binUI v${PROJECT_VERSION}")
message(STATUS "  Core lib  -> binui")
if(WIN32)
    message(STATUS "  Demos     -> vision_monitor (GDI)")
    message(STATUS "  GL demos  -> animshow, svgdemo, cssshow, videodemo, singlewindow, multiwindow, galgamechat (OpenGL 3.3 borderless)")
endif()
message(STATUS "  Tests     -> test_binui, test_renderer, test_compiler")
