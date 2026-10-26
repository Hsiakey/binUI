// binui_compiler.h - UI 源码 -> binUI 二进制布局编译器
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace binui {

// 编译结果：布局 + 引用的图片资源（按 imageIndex 顺序）
struct CompileResult {
    std::vector<uint8_t> layout;
    std::vector<std::string> images; // img src / image src，节点 imageIndex 指向该表
    // 编译期警告（同一份 UI 内去重）：不支持的单位（calc/em 之外的 pt/cm…）、
    // 无法识别的颜色、函数值等。空 = 没有警告。已同时打到 stderr。
    std::vector<std::string> warnings;
};

// 将 UI 源码（XML 或 HTML5）编译为 binUI（.xhtml）二进制布局。
// 自动识别格式：出现 <binui / <root 标签视为 XML，否则按 HTML5 解析。
std::vector<uint8_t> CompileBinUISource(const std::string& source);

// 同上，但额外返回引用的图片资源路径
CompileResult CompileBinUIEx(const std::string& source);

// 从文件读取并编译（扩展名 .html/.htm/.xml 均支持）
std::vector<uint8_t> CompileBinUIFile(const std::string& path);

// 同上，但额外返回引用的图片资源路径
CompileResult CompileBinUIFileEx(const std::string& path);

// XML 描述 -> binUI（节点标签映射为节点类型，见 examples/vision_monitor/main.binui.xml）
std::vector<uint8_t> CompileXML(const std::string& xml, std::vector<std::string>* outImages = nullptr);

// HTML5 语法 -> binUI（支持流式布局与 style 绝对定位）
std::vector<uint8_t> CompileHTML(const std::string& html, std::vector<std::string>* outImages = nullptr);

// 无源码时生成默认布局
std::vector<uint8_t> GenerateDefaultBinUI();

} // namespace binui
