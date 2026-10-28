#include "binui_compiler.h"
#include "binui_renderer.h"
#include "binui_format.h"
#include <cstdio>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>

static int failed = 0;

#define TEST(name, expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s (%s)\n", name, #expr); \
        failed++; \
    } else { \
        printf("PASS: %s\n", name); \
    } \
} while(0)

using namespace binui;

static bool ParseInto(BinUIRenderer& r, const std::vector<uint8_t>& binui) {
    return !binui.empty() && r.loadLayout(binui.data(), binui.size());
}

int main() {
    printf("=== test_compiler: XML/HTML5 -> binUI -> parse roundtrip ===\n\n");

    // 1. XML -> binUI
    {
        const char* xml =
            "<binui width=\"480\" height=\"800\" bg=\"#121212\">\n"
            "  <label x=\"16\" y=\"46\" w=\"300\" h=\"36\" color=\"#FFFFFF\" size=\"20\">Material Demo</label>\n"
            "  <button x=\"32\" y=\"276\" w=\"416\" h=\"48\" color=\"#BB86FC\" size=\"14\">CLICK ME</button>\n"
            "  <input x=\"32\" y=\"348\" w=\"416\" h=\"48\" color=\"#1E1E1E\" size=\"14\">Enter text...</input>\n"
            "</binui>\n";

        BinUIRenderer r;
        auto binui = CompileXML(xml);
        TEST("CompileXML produces data", !binui.empty());
        TEST("CompileXML parses", ParseInto(r, binui));
        const auto& nodes = r.getNodes();
        TEST("XML node count == 4", nodes.size() == 4);
        if (nodes.size() == 4) {
            TEST("XML root id 0", nodes[0].id == 0 && nodes[0].type == BINUI_ROOT);
            TEST("XML root size 480x800", nodes[0].width == 480 && nodes[0].height == 800);
            TEST("XML label text", nodes[1].text == "Material Demo");
            TEST("XML button interactive", nodes[2].type == BINUI_BUTTON && (nodes[2].flags & BINUI_INTERACTIVE));
            TEST("XML button text", nodes[2].text == "CLICK ME");
            TEST("XML input text", nodes[3].type == BINUI_INPUT && nodes[3].text == "Enter text...");
        }
    }

    // 2. HTML5 -> binUI
    {
        const char* html =
            "<html width=\"480\" height=\"800\" bg=\"#121212\">\n"
            "<body>\n"
            "  <h3 style=\"color:#FFFFFF\">HTML5 Demo</h3>\n"
            "  <button style=\"background:#BB86FC; height:48\">CLICK ME</button>\n"
            "  <input type=\"text\" value=\"Enter text...\">\n"
            "</body>\n"
            "</html>\n";

        BinUIRenderer r;
        auto binui = CompileHTML(html);
        TEST("CompileHTML produces data", !binui.empty());
        TEST("CompileHTML parses", ParseInto(r, binui));
        const auto& nodes = r.getNodes();
        TEST("HTML node count == 4", nodes.size() == 4);
        if (nodes.size() == 4) {
            TEST("HTML root id 0", nodes[0].id == 0 && nodes[0].type == BINUI_ROOT);
            TEST("HTML h3 label text", nodes[1].type == BINUI_LABEL && nodes[1].text == "HTML5 Demo");
            TEST("HTML button text", nodes[2].type == BINUI_BUTTON && nodes[2].text == "CLICK ME");
            TEST("HTML input value", nodes[3].type == BINUI_INPUT && nodes[3].text == "Enter text...");
        }
    }

    // 3. 自动识别格式（CompileBinUISource）
    {
        BinUIRenderer r;
        TEST("Auto-detect XML", ParseInto(r, CompileBinUISource(
            "<binui width=\"100\" height=\"100\"><label>hi</label></binui>")));
        TEST("Auto-detect HTML", ParseInto(r, CompileBinUISource(
            "<html width=\"100\" height=\"100\"><body><p>hi</p></body></html>")));
        TEST("Auto-detect HTML doctype", ParseInto(r, CompileBinUISource(
            "<!DOCTYPE html><html width=\"100\" height=\"100\"><body><p>hi</p></body></html>")));
    }

    // 4. 实际示例文件
    {
        BinUIRenderer r3;
        auto vision = CompileBinUIFile("examples/vision_monitor/main.binui.xml");
        TEST("Compile examples/vision_monitor/main.binui.xml", ParseInto(r3, vision));
        TEST("vision_monitor node count == 21", !vision.empty() && r3.getNodes().size() == 21);
    }

    // 4.5 真实父子层级
    {
        // XML 嵌套：card 包含 label，button 是 card 的兄弟
        const char* xml =
            "<binui width=\"200\" height=\"300\">"
            "<card x=\"0\" y=\"0\" w=\"100\" h=\"100\">"
            "<label x=\"5\" y=\"5\" w=\"50\" h=\"20\">child</label>"
            "</card>"
            "<button x=\"0\" y=\"200\" w=\"100\" h=\"30\">sibling</button>"
            "</binui>";
        BinUIRenderer r;
        auto ufl = CompileXML(xml);
        TEST("hier XML parses", ParseInto(r, ufl));
        const auto& n = r.getNodes();
        TEST("hier XML node count == 4", n.size() == 4);
        if (n.size() == 4) {
            TEST("XML root parentId == 0xFFFF",
                 n[0].type == BINUI_ROOT && n[0].parentId == 0xFFFF);
            TEST("XML card parentId == root", n[1].parentId == n[0].id);
            TEST("XML label parentId == card",
                 n[2].type == BINUI_LABEL && n[2].parentId == n[1].id);
            TEST("XML button parentId == root",
                 n[3].type == BINUI_BUTTON && n[3].parentId == n[0].id);
        }

        // HTML 嵌套：body 被跳过，div 的子节点应挂到 html 根
        const char* html =
            "<html width=\"200\" height=\"300\">"
            "<body>"
            "<div x=\"0\" y=\"0\" w=\"100\" h=\"100\">"
            "<span x=\"5\" y=\"5\">inner</span>"
            "</div>"
            "</body>"
            "</html>";
        BinUIRenderer r2;
        auto ufl2 = CompileHTML(html);
        TEST("hier HTML parses", ParseInto(r2, ufl2));
        const auto& m = r2.getNodes();
        TEST("hier HTML node count == 3", m.size() == 3);
        if (m.size() == 3) {
            TEST("HTML root parentId == 0xFFFF",
                 m[0].type == BINUI_ROOT && m[0].parentId == 0xFFFF);
            TEST("HTML div parentId == root", m[1].parentId == m[0].id);
            TEST("HTML span parentId == div", m[2].parentId == m[1].id);
        }
    }

    // 4.7 margin / padding
    {
        // XML padding 存储
        BinUIRenderer r;
        auto ufl = CompileXML(
            "<binui width=\"200\" height=\"300\">"
            "<label x=\"0\" y=\"0\" w=\"100\" h=\"30\" padding=\"8\">pad</label>"
            "</binui>");
        TEST("pad XML parses", ParseInto(r, ufl));
        const auto& n = r.getNodes();
        TEST("XML padding stored", n.size() == 2 && n[1].padding == 8);

        // HTML：margin 影响流式布局，padding 存入节点
        BinUIRenderer r2;
        auto ufl2 = CompileHTML(
            "<html width=\"200\" height=\"400\">"
            "<body>"
            "<div style=\"height:20\">a</div>"
            "<div style=\"height:20; margin:10\">b</div>"
            "<div style=\"height:20; padding:5\">c</div>"
            "</body>"
            "</html>");
        TEST("margin HTML parses", ParseInto(r2, ufl2));
        const auto& m = r2.getNodes();
        if (m.size() != 4) {
            TEST("margin HTML node count == 4", false);
        } else {
            TEST("flow a y==0", m[1].y == 0 && m[1].height == 20);
            TEST("margin b y==30", m[2].y == 30);
            TEST("margin b x==10", m[2].x == 10);
            TEST("margin b w==180", m[2].width == 180);
            TEST("flow c y==60", m[3].y == 60);
            TEST("padding c == 5", m[3].padding == 5);
        }
    }

    // 4.8 父级相对布局：容器的流式子元素排布在容器内部，容器自动扩展容纳
    {
        BinUIRenderer r;
        auto ufl = CompileHTML(
            "<html width=\"200\" height=\"400\">"
            "<body>"
            "<div style=\"background:#333; height:100\">"
            "<span style=\"height:20\">x</span>"
            "<span style=\"height:20\">y</span>"
            "</div>"
            "</body>"
            "</html>");
        TEST("container layout parses", ParseInto(r, ufl));
        const auto& n = r.getNodes();
        if (n.size() != 4) {
            TEST("container node count == 4", false);
        } else {
            // root(0), div(1), span1(2), span2(3) —— 注意 span 是 label 类型
            TEST("div is LIST at y==0", n[1].type == BINUI_LIST && n[1].y == 0 && n[1].height == 100);
            TEST("span1 inside div y==0", n[2].type == BINUI_LABEL && n[2].parentId == n[1].id && n[2].y == n[1].y);
            TEST("span2 stacked inside div y==20", n[3].y == n[1].y + 20);
        }

        // 容器自动扩展：显式高度小于流式内容时撑大
        BinUIRenderer r2;
        auto ufl2 = CompileHTML(
            "<html width=\"200\" height=\"400\">"
            "<body>"
            "<div style=\"height:10\">"
            "<span style=\"height:20\">a</span>"
            "</div>"
            "</body>"
            "</html>");
        TEST("grow layout parses", ParseInto(r2, ufl2));
        const auto& m = r2.getNodes();
        TEST("container auto-grow to fit child",
             m.size() == 3 && m[1].height >= 20);
    }

    // 4.9 实体解码
    {
        // XML：文本中的命名/数值实体
        BinUIRenderer r;
        auto ufl = CompileXML(
            "<binui width=\"200\" height=\"300\">"
            "<label x=\"0\" y=\"0\" w=\"100\" h=\"30\">Tom &amp; Jerry &#65;&#x42; a&nbsp;b</label>"
            "</binui>");
        TEST("entity XML parses", ParseInto(r, ufl));
        const auto& n = r.getNodes();
        TEST("XML text entities decoded",
             n.size() == 2 && n[1].text == "Tom & Jerry AB a\xC2\xA0" "b");

        // HTML：文本与 input value 属性中的实体
        BinUIRenderer r2;
        auto ufl2 = CompileHTML(
            "<html width=\"200\" height=\"300\">"
            "<body>"
            "<p>a &lt; b &amp;&amp; c &gt; d</p>"
            "<input type=\"text\" value=\"x &quot;y&quot;\">"
            "</body>"
            "</html>");
        TEST("entity HTML parses", ParseInto(r2, ufl2));
        const auto& m = r2.getNodes();
        bool textOk = false, attrOk = false;
        for (const auto& nd : m) {
            if (nd.type == BINUI_LABEL && nd.text == "a < b && c > d") textOk = true;
            if (nd.type == BINUI_INPUT && nd.text == "x \"y\"") attrOk = true;
        }
        TEST("HTML text entities decoded", textOk);
        TEST("HTML attr entity decoded", attrOk);

        // 未知实体保留原样
        BinUIRenderer r3;
        auto ufl3 = CompileHTML(
            "<html width=\"100\" height=\"100\"><body><p>&unknown; ok</p></body></html>");
        TEST("unknown entity parses", ParseInto(r3, ufl3));
        bool unkOk = false;
        for (const auto& nd : r3.getNodes())
            if (nd.type == BINUI_LABEL && nd.text == "&unknown; ok") unkOk = true;
        TEST("unknown entity kept as-is", unkOk);
    }

    // 4.10 图片资源引用
    {
        auto res = CompileBinUIEx(
            "<html width=\"200\" height=\"300\">"
            "<body>"
            "<img src=\"a.png\" style=\"position:absolute; left:0; top:0; width:20; height:20\">"
            "<img src=\"b.png\" style=\"position:absolute; left:30; top:0; width:20; height:20\">"
            "<div>x</div>"
            "</body>"
            "</html>");
        TEST("img resources collected",
             res.images.size() == 2 && res.images[0] == "a.png" && res.images[1] == "b.png");

        BinUIRenderer r;
        TEST("img layout parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        if (r.getNodes().size() < 3) {
            TEST("img node count", false);
        } else {
            TEST("img[0] imageIndex==0",
                 r.getNodes()[1].type == BINUI_IMAGE && r.getNodes()[1].imageIndex == 0);
            TEST("img[1] imageIndex==1",
                 r.getNodes()[2].type == BINUI_IMAGE && r.getNodes()[2].imageIndex == 1);
        }

        // XML 资源引用
        auto res2 = CompileBinUIEx(
            "<binui width=\"100\" height=\"100\">"
            "<image x=\"0\" y=\"0\" w=\"20\" h=\"20\" src=\"icon.png\"/>"
            "</binui>");
        TEST("XML img resource",
             res2.images.size() == 1 && res2.images[0] == "icon.png");

        // 渲染器：imageIndex -> 资源路径 映射
        BinUIRenderer r3;
        std::unordered_map<std::string, std::vector<uint8_t>> resources;
        resources["a.png"] = { 1, 2, 3, 4 };
        r3.setImageOrder({ "a.png" });
        r3.loadResources(resources);
        TEST("getImage returns loaded image",
             r3.getImage(0) != nullptr && r3.getImage(0)->data.size() == 4);
        TEST("getImage out-of-range null", r3.getImage(5) == nullptr);
        TEST("getImage missing path null", r3.getImage(99) == nullptr);
    }

    // 4.11 百分比尺寸（width/height/left/top: N%，相对父容器内容盒）
    {
        // HTML：width/height % 相对父容器内容盒
        BinUIRenderer r;
        auto ufl = CompileHTML(
            "<html width=\"200\" height=\"100\">"
            "<body>"
            "<div style=\"width:50%; height:50%\">x</div>"
            "</body>"
            "</html>");
        TEST("pct HTML parses", ParseInto(r, ufl));
        const auto& n = r.getNodes();
        TEST("pct width 50% of 200", n.size() == 2 && std::fabs(n[1].width - 100) < 0.01f);
        TEST("pct height 50% of 100", std::fabs(n[1].height - 50) < 0.01f);

        // 流式 + % 高度：后续子元素按新高度下移
        BinUIRenderer r2;
        auto ufl2 = CompileHTML(
            "<html width=\"200\" height=\"200\">"
            "<body>"
            "<div style=\"height:25%\">a</div>"
            "<div style=\"height:25%\">b</div>"
            "</body>"
            "</html>");
        TEST("pct flow parses", ParseInto(r2, ufl2));
        const auto& m = r2.getNodes();
        TEST("pct flow stacking",
             m.size() == 3 && std::fabs(m[1].height - 50) < 0.01f
             && std::fabs(m[2].y - 50) < 0.01f && std::fabs(m[2].height - 50) < 0.01f);

        // XML：x/y/w/h %
        BinUIRenderer r3;
        auto ufl3 = CompileXML(
            "<binui width=\"200\" height=\"200\">"
            "<card x=\"10%\" y=\"10%\" w=\"50%\" h=\"50%\">c</card>"
            "</binui>");
        TEST("pct XML parses", ParseInto(r3, ufl3));
        const auto& k = r3.getNodes();
        TEST("pct XML geometry",
             k.size() == 2 && std::fabs(k[1].x - 20) < 0.01f
             && std::fabs(k[1].y - 20) < 0.01f
             && std::fabs(k[1].width - 100) < 0.01f && std::fabs(k[1].height - 100) < 0.01f);

        // 根 width/height:100% -> 默认窗口尺寸 1280x720
        BinUIRenderer r4;
        auto ufl4 = CompileHTML("<html style=\"width:100%; height:100%\"><body>x</body></html>");
        TEST("pct root parses", ParseInto(r4, ufl4));
        TEST("pct root default size",
             !r4.getNodes().empty()
             && std::fabs(r4.getNodes()[0].width - 1280) < 0.01f
             && std::fabs(r4.getNodes()[0].height - 720) < 0.01f);

        // 缩放规格段：编译产物应带 resize spec，手写格式不带
        TEST("compiled layout has resize spec", r.hasResizeSpec());
        TEST("default layout has no resize spec",
             [] {
                 BinUIRenderer rr;
                 auto d = GenerateDefaultBinUI();
                 rr.loadLayout(d.data(), d.size());
                 return !rr.hasResizeSpec();
             }());
    }

    // 4b. min/max 尺寸约束：编译期即生效（初始布局），HTML 与 XML 均支持
    {
        // HTML：min-width 撑开自动宽节点（容器小于最小值时钳制）
        BinUIRenderer r1;
        auto u1 = CompileHTML(
            "<html width=\"100\" height=\"100\"><body>"
            "<button style=\"min-width:200\">x</button>"
            "</body></html>");
        TEST("html min-width parses", ParseInto(r1, u1));
        TEST("html min-width initial", std::fabs(r1.getNodes()[1].width - 200) < 0.01f);

        // HTML：max-width 钳制百分比宽度
        BinUIRenderer r2;
        auto u2 = CompileHTML(
            "<html width=\"300\" height=\"100\"><body>"
            "<button style=\"width:100%; max-width:120\">x</button>"
            "</body></html>");
        TEST("html max-width parses", ParseInto(r2, u2));
        TEST("html max-width initial", std::fabs(r2.getNodes()[1].width - 120) < 0.01f);

        // HTML：min/max-height 影响流式堆叠
        BinUIRenderer r3;
        auto u3 = CompileHTML(
            "<html width=\"200\" height=\"200\"><body>"
            "<div style=\"min-height:70\">a</div>"
            "<div style=\"height:100; max-height:40\">b</div>"
            "</body></html>");
        TEST("html height constraints parse", ParseInto(r3, u3));
        const auto& k = r3.getNodes();
        TEST("html min-height initial", std::fabs(k[1].height - 70) < 0.01f);
        TEST("html max-height initial", std::fabs(k[2].height - 40) < 0.01f);
        TEST("html flow uses clamped heights", std::fabs(k[2].y - 70) < 0.01f);

        // HTML：% 约束
        BinUIRenderer r4;
        auto u4 = CompileHTML(
            "<html width=\"400\" height=\"200\"><body>"
            "<button style=\"width:60; min-width:50%\">x</button>"
            "</body></html>");
        TEST("html pct constraint parses", ParseInto(r4, u4));
        TEST("html pct min-width initial",
             std::fabs(r4.getNodes()[1].width - 200) < 0.01f);

        // HTML：flex 子项 max-width
        BinUIRenderer r5;
        auto u5 = CompileHTML(
            "<html width=\"600\" height=\"100\"><body>"
            "<div style=\"display:flex; height:50\">"
            "<button style=\"flex:1; max-width:100\">A</button>"
            "<button style=\"flex:1\">B</button>"
            "</div></body></html>");
        TEST("html flex constraint parses", ParseInto(r5, u5));
        const auto& f = r5.getNodes();
        TEST("html flex max-width initial", std::fabs(f[2].width - 100) < 0.01f);
        TEST("html flex sibling initial", std::fabs(f[3].width - 300) < 0.01f);

        // XML：属性形式
        BinUIRenderer r6;
        auto u6 = CompileXML(
            "<binui width=\"300\" height=\"100\">"
            "<button x=\"0\" y=\"0\" w=\"200\" max-width=\"90\">x</button>"
            "</binui>");
        TEST("xml constraint parses", ParseInto(r6, u6));
        TEST("xml max-width initial", std::fabs(r6.getNodes()[1].width - 90) < 0.01f);

        // XML：min-height
        BinUIRenderer r7;
        auto u7 = CompileXML(
            "<binui width=\"300\" height=\"100\">"
            "<card x=\"0\" y=\"0\" w=\"100\" h=\"10\" min-height=\"60\">c</card>"
            "</binui>");
        TEST("xml min-height parses", ParseInto(r7, u7));
        TEST("xml min-height initial", std::fabs(r7.getNodes()[1].height - 60) < 0.01f);
    }

    // 5. 无效输入安全失败
    {
        TEST("Empty source -> empty output", CompileBinUISource("").empty());
        TEST("Random bytes -> no crash", CompileBinUISource("hello world this is not markup").size() < (1u << 20));
    }

    // 6. 运行时图片 API：updateImage / updateImageRaw / imageGeneration
    {
        const char* src =
            "<binui width=\"320\" height=\"240\">"
            "<image x=\"8\" y=\"8\" w=\"64\" h=\"64\" src=\"a.png\"></image>"
            "</binui>";
        auto binui = CompileBinUISource(src);
        BinUIRenderer r;
        TEST("image layout parses", ParseInto(r, binui));
        r.setImageOrder({ "a.png" });
        TEST("image initial generation 0", r.imageGeneration(0) == 0);
        TEST("getImage null before load", r.getImage(0) == nullptr);

        const uint8_t png[] = { 0x89, 0x50, 0x4E, 0x47 }; // 无效 PNG 字节，仅测 API 通路
        TEST("updateImage replaces data", r.updateImage(0, png, sizeof png));
        const ImageResource* img = r.getImage(0);
        TEST("updateImage generation bumped", r.imageGeneration(0) == 1 && img && !img->data.empty());
        TEST("updateImage not raw", img && !img->rawPixels);
        TEST("updateImage data preserved", img && img->data[0] == 0x89 && img->data.size() == 4);

        uint8_t rgba[4 * 2 * 2] = { 255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,0,255 };
        TEST("updateImageRaw replaces pixels", r.updateImageRaw(0, 2, 2, rgba));
        const ImageResource* raw = r.getImage(0);
        TEST("updateImageRaw generation bumped", r.imageGeneration(0) == 2 && raw && raw->rawPixels);
        TEST("updateImageRaw dims stored", raw && raw->width == 2 && raw->height == 2 && raw->channels == 4);
        TEST("updateImageRaw pixels copied", raw && raw->data.size() == 16 && raw->data[0] == 255 && raw->data[15] == 255);
    }

    // 7. <app_window> 作根元素：XML 自动合成 ROOT 节点
    {
        auto u = CompileBinUISource(
            "<app_window width=\"800\" height=\"600\" bg=\"#202030\" title=\"Win\">"
            "<label x=\"10\" y=\"10\" w=\"120\" h=\"24\">hi</label>"
            "</app_window>");
        BinUIRenderer r;
        TEST("app_window-root parses", ParseInto(r, u));
        TEST("app_window-root has synthesized ROOT id 0",
             !r.getNodes().empty() && r.getNodes()[0].id == 0 &&
             r.getNodes()[0].type == BINUI_ROOT);
        TEST("app_window-root root size from window",
             std::fabs(r.getNodes()[0].width - 800) < 0.01f &&
             std::fabs(r.getNodes()[0].height - 600) < 0.01f);
        TEST("app_window-root root bg from window", r.getNodes()[0].color == 0xFF202030u);
        TEST("app_window-root children parented to root",
             r.getNodes().size() > 1 && r.getNodes()[1].parentId == 0);
        TEST("app_window-root window desc present", r.windowDesc().present &&
             r.windowDesc().width == 800 && r.windowDesc().height == 600 &&
             r.windowDesc().title == "Win");
    }

    // 8. <app_window> 包裹层 + HTML 正文：仍按 HTML 解析
    {
        auto res = CompileBinUIEx(
            "<!DOCTYPE html><app_window width=\"500\" height=\"400\" bg=\"#101020\" title=\"H\">"
            "<div id=\"box\" x=\"10\" y=\"10\" width=\"200\" height=\"50\">hello</div>"
            "</app_window>");
        BinUIRenderer r;
        TEST("app_window+html doc stays HTML",
             !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        uint16_t box = r.findNodeByName("box");
        TEST("app_window+html has div child", box != 0);
        TEST("app_window+html window desc present", r.windowDesc().present &&
             r.windowDesc().title == "H");
    }

    // 9. <video> 编译（HTML）：BINUI_VIDEO 节点 + 伪图片槽 + 视频描述段
    {
        const char* html =
            "<html><body style=\"width:640px;height:480px;background:#111111\">"
            "<video id=\"vid\" src=\"media/sample.mp4\" controls autoplay loop "
            "volume=\"0.8\" x=\"20\" y=\"20\" width=\"600\" height=\"400\"></video>"
            "</body></html>";
        auto res = CompileBinUIEx(html);
        BinUIRenderer r;
        TEST("video layout parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        uint16_t vid = r.findNodeByName("vid");
        TEST("video node found by id", vid != 0);
        bool hasVideoType = false;
        for (const auto& n : r.getNodes())
            if (n.id == vid) { hasVideoType = (n.type == BINUI_VIDEO); break; }
        TEST("video node type BINUI_VIDEO", hasVideoType);
        TEST("one video in descriptor table", r.getVideos().size() == 1);
        const BinUIRenderer::VideoState* v = r.findVideo(vid);
        TEST("findVideo returns state", v != nullptr);
        TEST("video src compiled", v && v->src == "media/sample.mp4");
        TEST("video flags autoplay|loop|controls",
             v && (v->flags & VID_AUTOPLAY) && (v->flags & VID_LOOP) && (v->flags & VID_CONTROLS));
        TEST("video muted flag absent", v && !(v->flags & VID_MUTED));
        TEST("video volume parsed", v && v->volume > 0.79f && v->volume < 0.81f);
        TEST("video pseudo image slot allocated", res.images.size() >= 1 &&
             res.images[0] == "media/sample.mp4" && v && v->imageIndex == 0);
        // loadLayout 内部首帧 stepAnimations 会触发 autoplay，故只断言时钟/时长未动
        TEST("video initial runtime state", v && v->position == 0.0 && v->duration == 0.0 && !v->ended);
    }

    // 10. 无 <video> 布局：视频表为空
    {
        auto res = CompileBinUIEx("<html><body style=\"width:320px;height:240px\"><p>plain</p></body></html>");
        BinUIRenderer r;
        r.loadLayout(res.layout.data(), res.layout.size());
        TEST("no-video layout yields empty table", r.getVideos().empty());
        TEST("no-video findVideo null", r.findVideo(0) == nullptr);
    }

    // 11. <video> 编译（XML）与默认值
    {
        const char* xml =
            "<binui width=\"480\" height=\"360\" bg=\"#101010\">"
            "<video x=\"10\" y=\"10\" w=\"460\" h=\"300\" src=\"a.mp4\" muted></video>"
            "</binui>";
        auto res = CompileBinUIEx(xml);
        BinUIRenderer r;
        TEST("xml video layout parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        const BinUIRenderer::VideoState* v = !r.getVideos().empty() ? &r.getVideos()[0] : nullptr;
        TEST("xml video desc exists", v != nullptr);
        TEST("xml video muted only", v && (v->flags & VID_MUTED) &&
             !(v->flags & VID_AUTOPLAY) && !(v->flags & VID_CONTROLS));
        TEST("xml video default volume", v && v->volume == 1.0f);
    }

    // 12. z-index 层叠（v10）：解析 + 绘制顺序 + 命中测试按层顶优先
    {
        const char* html =
            "<html><body style=\"width:200px;height:200px;background:#000000\">"
            "<button id=\"low\" style=\"position:absolute;left:0;top:0;width:100px;"
            "height:100px;z-index:1\">A</button>"
            "<button id=\"high\" style=\"position:absolute;left:0;top:0;width:100px;"
            "height:100px;z-index:9\">B</button>"
            "</body></html>";
        auto res = CompileBinUIEx(html);
        BinUIRenderer r;
        TEST("zindex layout parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        uint16_t low = r.findNodeByName("low");
        uint16_t high = r.findNodeByName("high");
        TEST("zindex nodes found", low != 0 && high != 0);

        const auto& nodes = r.getNodes();
        auto zOf = [&](uint16_t id) -> int {
            for (const auto& n : nodes) if (n.id == id) return n.zIndex;
            return -1000;
        };
        TEST("zindex parsed low=1", zOf(low) == 1);
        TEST("zindex parsed high=9", zOf(high) == 9);
        TEST("zindex default 0", zOf(0) == 0); // 根未指定

        // 绘制顺序：z 高者靠后（更靠上）
        auto order = r.drawOrder();
        auto posOf = [&](uint16_t id) -> int {
            for (size_t k = 0; k < order.size(); k++)
                if (nodes[order[k]].id == id) return (int)k;
            return -1;
        };
        TEST("zindex draw order high after low", posOf(high) > posOf(low) && posOf(high) >= 0);

        // 命中测试：重叠区域内层顶（z 高）胜出
        TEST("zindex hitTest topmost", r.hitTest(50, 50) == (int)high);
    }

    // 13. z-index 未指定时：同层 absolute 按 DOM 顺序，后声明者在上
    {
        const char* html =
            "<html><body style=\"width:200px;height:200px\">"
            "<button id=\"first\" style=\"position:absolute;left:0;top:0;width:100px;height:100px\">1</button>"
            "<button id=\"second\" style=\"position:absolute;left:0;top:0;width:100px;height:100px\">2</button>"
            "</body></html>";
        auto res = CompileBinUIEx(html);
        BinUIRenderer r;
        TEST("zindex-default parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        uint16_t first = r.findNodeByName("first");
        uint16_t second = r.findNodeByName("second");
        TEST("zindex-default dom-order hit", r.hitTest(50, 50) == (int)second && first != second);
    }

    // 14. 长度单位：px / 无单位 / % / em / rem / vw / vh
    {
        auto res = CompileBinUIEx(
            "<html><body style=\"width:1280px;height:720px\">"
            "<div id=\"emu\"  style=\"left:0;top:0;width:10em;height:20vh;font-size:14px\"></div>"
            "<div id=\"remu\" style=\"left:0;top:0;width:2rem;height:50%\"></div>"
            "<div id=\"vwu\"  style=\"left:0;top:0;width:25vw;height:10px\"></div>"
            "</body></html>");
        BinUIRenderer r;
        TEST("units: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        auto boxOf = [&](const char* name) -> const BinUIRenderer::NodeInfo* {
            uint16_t nid = r.findNodeByName(name);
            for (const auto& nd : r.getNodes()) if (nd.id == nid) return &nd;
            return nullptr;
        };
        const auto* emu = boxOf("emu");
        // 10em × 14px = 140；20vh = 0.2 × 720 = 144
        TEST("units: em resolves against font-size (10em -> 140)",
             emu && std::fabs(emu->width - 140.0f) < 0.5f);
        TEST("units: vh resolves against viewport (20vh -> 144)",
             emu && std::fabs(emu->height - 144.0f) < 0.5f);
        const auto* remu = boxOf("remu");
        // rem 基准 = 根字号 14 -> 2rem = 28
        TEST("units: rem resolves against root font (2rem -> 28)",
             remu && std::fabs(remu->width - 28.0f) < 0.5f);
        const auto* vwu = boxOf("vwu");
        // 25vw = 0.25 × 1280 = 320
        TEST("units: vw resolves against viewport (25vw -> 320)",
             vwu && std::fabs(vwu->width - 320.0f) < 0.5f);
        TEST("units: valid input produces no warnings", res.warnings.empty());
    }

    // 15. 不支持的单位 / calc()：必须报警告，不能静默当 px
    {
        auto res = CompileBinUIEx(
            "<html><body>"
            "<div id=\"bad\" style=\"left:0;top:0;width:12pt;height:30\"></div>"
            "<div id=\"fn\"  style=\"left:0;top:40;width:calc(50% - 10px);height:30\"></div>"
            "</body></html>");
        bool sawPt = false, sawCalc = false;
        for (const auto& w : res.warnings) {
            if (w.find("pt") != std::string::npos) sawPt = true;
            if (w.find("calc") != std::string::npos) sawCalc = true;
        }
        TEST("units: pt warns instead of silent px", sawPt);
        TEST("units: calc() warns instead of silent 0", sawCalc);
        BinUIRenderer r;
        TEST("units: bad-unit UI still parses",
             !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
    }

    // 16. 颜色：命名色 / transparent / !important / rgb() 百分比 / 现代 slash / hsl / #AARRGGBB
    {
        auto res = CompileBinUIEx(
            "<html><body>"
            "<div id=\"c1\" style=\"left:0;top:0;width:10px;height:10px;background:red\"></div>"
            "<div id=\"c2\" style=\"left:0;top:0;width:10px;height:10px;background:transparent\"></div>"
            "<div id=\"c3\" style=\"left:0;top:0;width:10px;height:10px;background:#00ff00 !important\"></div>"
            "<div id=\"c4\" style=\"left:0;top:0;width:10px;height:10px;background:rgb(100%, 0%, 0%)\"></div>"
            "<div id=\"c5\" style=\"left:0;top:0;width:10px;height:10px;background:rgb(0 0 255 / 50%)\"></div>"
            "<div id=\"c6\" style=\"left:0;top:0;width:10px;height:10px;background:hsl(120, 100%, 25%)\"></div>"
            "<div id=\"c7\" style=\"left:0;top:0;width:10px;height:10px;background:#80112233\"></div>"
            "</body></html>");
        BinUIRenderer r;
        TEST("colors: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        auto colorOf = [&](const char* name) -> uint32_t {
            uint16_t nid = r.findNodeByName(name);
            for (const auto& nd : r.getNodes()) if (nd.id == nid) return nd.color;
            return 0xDEADBEEFu;
        };
        TEST("colors: named 'red' -> FFFF0000", colorOf("c1") == 0xFFFF0000u);
        TEST("colors: 'transparent' -> alpha 0", colorOf("c2") == 0x00000000u);
        TEST("colors: '!important' stripped (no alpha loss)", colorOf("c3") == 0xFF00FF00u);
        TEST("colors: rgb() percentage channels", colorOf("c4") == 0xFFFF0000u);
        TEST("colors: modern 'rgb(r g b / a)'", colorOf("c5") == 0x800000FFu);
        TEST("colors: hsl(120,100%,25%) -> green", colorOf("c6") == 0xFF008000u);
        TEST("colors: #AARRGGBB kept as binUI convention", colorOf("c7") == 0x80112233u);

        bool sawArgbNote = false;
        for (const auto& w : res.warnings)
            if (w.find("AARRGGBB") != std::string::npos) sawArgbNote = true;
        TEST("colors: 8-digit hex warns about AARRGGBB vs CSS RRGGBBAA", sawArgbNote);

        // 无法识别的颜色：保留默认色 + 警告
        auto bad = CompileBinUIEx(
            "<html><body><div id=\"cb\" style=\"left:0;top:0;width:10px;height:10px;background:notacolor\"></div>"
            "</body></html>");
        bool sawBad = false;
        for (const auto& w : bad.warnings)
            if (w.find("notacolor") != std::string::npos) sawBad = true;
        TEST("colors: unknown color warns", sawBad);
    }

    // 17. XML 侧同样吃 em/rem/vw/vh 与命名色
    {
        auto res = CompileBinUIEx(
            "<binui width=\"800\" height=\"600\">"
            "<label id=\"xl\" x=\"10\" y=\"10\" w=\"10em\" size=\"20\" color=\"tomato\">t</label>"
            "</binui>");
        BinUIRenderer r;
        TEST("xml-units: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        uint16_t xl = r.findNodeByName("xl");
        for (const auto& nd : r.getNodes()) {
            if (nd.id != xl) continue;
            TEST("xml-units: 10em with size=20 -> 200", std::fabs(nd.width - 200.0f) < 0.5f);
            TEST("xml-colors: named 'tomato' -> FFFF6347", nd.color == 0xFFFF6347u);
        }
    }

    // 18. flex 主轴分配：百分比刚性项占位 / 溢出不出负偏移 / space-around / column 分高度
    {
        auto res = CompileBinUIEx(
            "<html><body style=\"width:1264px;height:200px\">"
            "<div id=\"row\" style=\"display:flex;height:32px;gap:8px;justify-content:space-between\">"
            "<span id=\"pa\" style=\"width:50%;height:20px\">A</span>"
            "<span id=\"fb\" style=\"flex:1;height:20px\">B</span>"
            "</div></body></html>");
        BinUIRenderer r;
        TEST("flex: mixed parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        auto nodeOf = [&](const char* name) -> const BinUIRenderer::NodeInfo* {
            uint16_t nid = r.findNodeByName(name);
            for (const auto& nd : r.getNodes()) if (nd.id == nid) return &nd;
            return nullptr;
        };
        const auto* pa = nodeOf("pa");
        const auto* fb = nodeOf("fb");
        // 回归：百分比宽必须先占位（以前没进 fixedSum，弹性项重叠并溢出容器）
        TEST("flex: percent child keeps 50% (632)", pa && std::fabs(pa->width - 632.0f) < 0.5f);
        TEST("flex: flexible child gets remainder (624)", fb && std::fabs(fb->width - 624.0f) < 0.5f);
        TEST("flex: no overlap (B starts after A + gap)",
             pa && fb && std::fabs(fb->x - (pa->x + pa->width + 8.0f)) < 0.5f);
        TEST("flex: row exactly fills container", fb && std::fabs(fb->x + fb->width - 1264.0f) < 0.5f);
    }
    {
        // 回归：溢出时 space-between 不得算出负间距（曾把第二项推到第一项左边）
        auto res = CompileBinUIEx(
            "<html><body style=\"width:300px;height:100px\">"
            "<div style=\"display:flex;height:40px;justify-content:space-between\">"
            "<div id=\"o1\" style=\"width:200px;height:20px\"></div>"
            "<div id=\"o2\" style=\"width:200px;height:20px\"></div>"
            "</div></body></html>");
        BinUIRenderer r;
        TEST("flex overflow parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        uint16_t o2 = r.findNodeByName("o2");
        for (const auto& nd : r.getNodes()) {
            if (nd.id != o2) continue;
            TEST("flex overflow: no negative spacing (o2.x == 200)", std::fabs(nd.x - 200.0f) < 0.5f);
        }
    }
    {
        // space-around：首尾各半个间隔（CSS 语义），之前首尾给的是整份
        auto res = CompileBinUIEx(
            "<html><body style=\"width:600px;height:100px\">"
            "<div style=\"display:flex;height:40px;justify-content:space-around\">"
            "<div id=\"s1\" style=\"width:100px;height:20px\"></div>"
            "<div id=\"s2\" style=\"width:100px;height:20px\"></div>"
            "</div></body></html>");
        BinUIRenderer r;
        TEST("flex space-around parses",
             !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        float x1 = -1, x2 = -1;
        for (const auto& nd : r.getNodes()) {
            if (nd.id == r.findNodeByName("s1")) x1 = nd.x;
            if (nd.id == r.findNodeByName("s2")) x2 = nd.x;
        }
        TEST("flex space-around: half gap at edges (100 / 400)",
             std::fabs(x1 - 100.0f) < 0.5f && std::fabs(x2 - 400.0f) < 0.5f);
    }
    {
        // column：flex:1 沿主轴（高度）分配，而不是错作用在宽度上
        auto res = CompileBinUIEx(
            "<html><body style=\"width:300px;height:400px\">"
            "<div style=\"display:flex;flex-direction:column;height:400px;gap:10\">"
            "<div id=\"c1\" style=\"flex:1;width:50px\"></div>"
            "<div id=\"c2\" style=\"flex:1\"></div>"   // 未声明宽 -> 交叉轴拉伸
            "</div></body></html>");
        BinUIRenderer r;
        TEST("flex column parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        const BinUIRenderer::NodeInfo* c1 = nullptr;
        const BinUIRenderer::NodeInfo* c2 = nullptr;
        for (const auto& nd : r.getNodes()) {
            if (nd.id == r.findNodeByName("c1")) c1 = &nd;
            if (nd.id == r.findNodeByName("c2")) c2 = &nd;
        }
        TEST("flex column: main axis = height ((400-10)/2 = 195)",
             c1 && std::fabs(c1->height - 195.0f) < 0.5f);
        TEST("flex column: second below first + gap",
             c1 && c2 && std::fabs(c2->y - (c1->y + c1->height + 10.0f)) < 0.5f);
        TEST("flex column: declared cross size kept (50)", c1 && std::fabs(c1->width - 50.0f) < 0.5f);
        TEST("flex column: undeclared cross size stretches (300)", c2 && std::fabs(c2->width - 300.0f) < 0.5f);
    }

    // 19. 命名实体表 / <br> 真实换行 / input 默认标签 / LIST 文本字号
    {
        auto res = CompileBinUIEx(
            "<html><body><p id=\"e\">A&mdash;B&hellip;C&pi;&rarr;&hearts;&frac12;&epsilon;"
            "&unknownent;</p></body></html>");
        BinUIRenderer r;
        TEST("entities: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        std::string got;
        for (const auto& nd : r.getNodes())
            if (nd.id == r.findNodeByName("e")) got = nd.text;
        // — … π → ♥ ½ ε + 未知实体原样保留
        const std::string want = "A\xE2\x80\x94" "B\xE2\x80\xA6" "C\xCF\x80" "\xE2\x86\x92"
                                 "\xE2\x99\xA5" "\xC2\xBD" "\xCE\xB5" "&unknownent;";
        TEST("entities: named table decodes (mdash/hellip/pi/rarr/hearts/frac12/epsilon)",
             got == want);
    }
    {
        auto res = CompileBinUIEx(
            "<html><body><p id=\"mb\">line1<br>line2<br>line3</p></body></html>");
        BinUIRenderer r;
        TEST("br: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        TEST("br: node count has no placeholder node", r.getNodes().size() == 2);
        const BinUIRenderer::NodeInfo* mb = nullptr;
        for (const auto& nd : r.getNodes())
            if (nd.id == r.findNodeByName("mb")) mb = &nd;
        TEST("br: becomes '\\n' in text", mb && mb->text == "line1\nline2\nline3");
        TEST("br: node grows to fit lines (h > 20*2)", mb && mb->height > 40.0f);
    }
    {
        auto res = CompileBinUIEx(
            "<html><body>"
            "<input id=\"s\" type=\"submit\"><input id=\"r\" type=\"reset\">"
            "<input id=\"b\" type=\"button\"><input id=\"t\" type=\"text\">"
            "</body></html>");
        BinUIRenderer r;
        TEST("input default: parses",
             !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        auto nodeOf = [&](const char* name) -> const BinUIRenderer::NodeInfo* {
            uint16_t nid = r.findNodeByName(name);
            for (const auto& nd : r.getNodes()) if (nd.id == nid) return &nd;
            return nullptr;
        };
        const auto* s = nodeOf("s");
        const auto* rr = nodeOf("r");
        TEST("input default: submit -> BUTTON 'Submit'",
             s && s->type == BINUI_BUTTON && s->text == "Submit");
        TEST("input default: reset -> BUTTON 'Reset'",
             rr && rr->type == BINUI_BUTTON && rr->text == "Reset");
        TEST("input default: text stays INPUT", nodeOf("t") && nodeOf("t")->type == BINUI_INPUT);
    }
    {
        auto res = CompileBinUIEx(
            "<html><body><div id=\"lf\" style=\"height:60px;font-size:20px\">hello</div>"
            "<div id=\"ld\" style=\"height:60px\">list text</div></body></html>");
        BinUIRenderer r;
        TEST("list font: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        for (const auto& nd : r.getNodes()) {
            if (nd.id == r.findNodeByName("lf")) TEST("list font: node.fontSize == 20", nd.fontSize == 20);
            if (nd.id == r.findNodeByName("ld")) TEST("list font: default 14", nd.fontSize == 14);
        }
    }

    // 20. text-align（flags 空闲位，不升格式版本）/ 静态 opacity / <li> 符号 / 不支持属性警告
    {
        auto res = CompileBinUIEx(
            "<html><body>"
            "<p id=\"al\" style=\"height:20px\">l</p>"
            "<p id=\"ac\" style=\"height:20px;text-align:center\">c</p>"
            "<p id=\"ar\" style=\"height:20px;text-align:right\">r</p>"
            "</body></html>");
        BinUIRenderer r;
        TEST("align: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        auto flagsOf = [&](const char* name) -> int {
            uint16_t nid = r.findNodeByName(name);
            for (const auto& nd : r.getNodes()) if (nd.id == nid) return nd.flags;
            return -1;
        };
        TEST("align: default is left (no bits)",
             flagsOf("al") >= 0 && !(flagsOf("al") & BINUI_TEXT_CENTER) && !(flagsOf("al") & BINUI_TEXT_RIGHT));
        TEST("align: center sets flag", flagsOf("ac") & BINUI_TEXT_CENTER);
        TEST("align: right sets flag", flagsOf("ar") & BINUI_TEXT_RIGHT);
    }
    {
        auto res = CompileBinUIEx(
            "<html><body>"
            "<div id=\"op\" style=\"height:20px;background:#FF0000;opacity:0.5\"></div>"
            "<div id=\"op0\" style=\"height:20px;background:#00FF00;opacity:0\"></div>"
            "</body></html>");
        BinUIRenderer r;
        TEST("opacity: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        auto colorOf = [&](const char* name) -> uint32_t {
            uint16_t nid = r.findNodeByName(name);
            for (const auto& nd : r.getNodes()) if (nd.id == nid) return nd.color;
            return 0xDEADBEEFu;
        };
        TEST("opacity: folded into alpha (0.5 -> 0x80)", colorOf("op") == 0x80FF0000u);
        TEST("opacity: zero -> fully transparent", colorOf("op0") == 0x0000FF00u);
    }
    {
        auto res = CompileBinUIEx(
            "<html><body><ul><li id=\"u1\">one</li></ul>"
            "<ol><li id=\"o1\">first</li><li id=\"o2\">second</li></ol></body></html>");
        BinUIRenderer r;
        TEST("li: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        auto textOf = [&](const char* name) -> std::string {
            uint16_t nid = r.findNodeByName(name);
            for (const auto& nd : r.getNodes()) if (nd.id == nid) return nd.text;
            return {};
        };
        TEST("li: ul gets bullet", textOf("u1") == "\xE2\x80\xA2 one");
        TEST("li: ol gets numbering", textOf("o1") == "1. first" && textOf("o2") == "2. second");
    }
    {
        // v13 起 border 与 font-weight 已实现：不再告警，而是落到扩展绘制段
        auto res = CompileBinUIEx(
            "<html><body><div id=\"d1\" style=\"height:20px;border:1px solid red\">a</div>"
            "<div id=\"d2\" style=\"height:20px;border:2px solid blue\">b</div>"
            "<p id=\"pw\" style=\"font-weight:bold\">c</p></body></html>");
        int borderWarns = 0, weightWarns = 0;
        for (const auto& w : res.warnings) {
            if (w.find("border") != std::string::npos) borderWarns++;
            if (w.find("font-weight") != std::string::npos) weightWarns++;
        }
        TEST("css v13: border 不再告警", borderWarns == 0);
        TEST("css v13: font-weight 不再告警", weightWarns == 0);
        BinUIRenderer r;
        r.loadLayout(res.layout.data(), res.layout.size());
        const BinUIPaintNode* p1 = r.paintOf(r.findNodeByName("d1"));
        const BinUIPaintNode* p2 = r.paintOf(r.findNodeByName("d2"));
        TEST("css v13: 两个节点的边框宽度各自生效",
             p1 && p2 && p1->borderW[0] == 1 && p2->borderW[0] == 2 &&
             p1->borderColor[0] == 0xFFFF0000u && p2->borderColor[0] == 0xFF0000FFu);
        const BinUIPaintNode* pw = r.paintOf(r.findNodeByName("pw"));
        TEST("css v13: font-weight:bold -> 700",
             pw && (pw->flags & PAINT_WEIGHT) && pw->weight == 7);
    }

    // 21. <style> 普通 CSS 规则 + 级联（内联 > #id > .class > tag > 同权重后写胜）
    {
        auto res = CompileBinUIEx(
            "<html><head><style>"
            ".btn { background: #ff0000; width: 120px; height: 40px; border-radius: 8px; }"
            "</style></head><body><button id=\"b\" class=\"btn\">Go</button></body></html>");
        BinUIRenderer r;
        TEST("css: class rule parses",
             !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        const BinUIRenderer::NodeInfo* b = nullptr;
        for (const auto& nd : r.getNodes()) if (nd.id == r.findNodeByName("b")) b = &nd;
        TEST("css: class rule applies (width/height/bg/radius)",
             b && std::fabs(b->width - 120.0f) < 0.5f && std::fabs(b->height - 40.0f) < 0.5f &&
                 b->color == 0xFFFF0000u && b->radius == 8);
    }
    {
        auto res = CompileBinUIEx(
            "<html><head><style>"
            "div   { background: #111111; }"
            ".card { background: #222222; }"
            "#top  { background: #333333; }"
            "</style></head><body>"
            "<div id=\"a\" class=\"card\" style=\"height:20px\">x</div>"
            "<div id=\"top\" class=\"card\" style=\"height:20px\">y</div>"
            "<div id=\"c\" class=\"card\" style=\"height:20px\">z</div>"
            "<div id=\"t\" style=\"height:20px\">w</div>"
            "<div id=\"inl\" class=\"card\" style=\"height:20px;background:#444444\">v</div>"
            "</body></html>");
        BinUIRenderer r;
        TEST("cascade: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        auto colorOf = [&](const char* name) -> uint32_t {
            uint16_t nid = r.findNodeByName(name);
            for (const auto& nd : r.getNodes()) if (nd.id == nid) return nd.color;
            return 0xDEADBEEFu;
        };
        TEST("cascade: #id beats .class", colorOf("top") == 0xFF333333u);
        TEST("cascade: .class beats tag", colorOf("a") == 0xFF222222u && colorOf("c") == 0xFF222222u);
        TEST("cascade: tag applies when nothing else", colorOf("t") == 0xFF111111u);
        TEST("cascade: inline beats everything", colorOf("inl") == 0xFF444444u);
    }
    {
        auto res = CompileBinUIEx(
            "<html><head><style>"
            ".dup { background: #111111; }"
            ".dup { background: #222222; }"
            "h1, .grp { color: #0000ff; font-size: 20px; }"
            "p.note { display: none; }"
            "</style></head><body>"
            "<div id=\"d\" class=\"dup\" style=\"height:20px\"></div>"
            "<p id=\"g\" class=\"grp\" style=\"height:24px\">group</p>"
            "<p id=\"n\" class=\"note\">hidden</p>"
            "</body></html>");
        BinUIRenderer r;
        TEST("css2: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        uint32_t dupColor = 0xDEADBEEFu;
        for (const auto& nd : r.getNodes()) if (nd.id == r.findNodeByName("d")) dupColor = nd.color;
        TEST("css2: same specificity -> later wins", dupColor == 0xFF222222u);
        const BinUIRenderer::NodeInfo* g = nullptr;
        for (const auto& nd : r.getNodes()) if (nd.id == r.findNodeByName("g")) g = &nd;
        TEST("css2: selector group applies to .class", g && g->fontSize == 20);
        TEST("css2: tag.class with display:none removes node", r.findNodeByName("n") == 0);
    }
    {
        auto res = CompileBinUIEx(
            "<html><head><style>"
            "div .x { color: red; }"                       // v13 起支持：不该再告警
            ".y:visited { color: blue; }"
            "p::first-line { color: green; }"
            ".a.b { color: green; }"                       // v13 起支持：不该再告警
            "@media (max-width: 600px) { .z { color: green; } }"
            "</style></head><body><div style=\"height:20px\"><span class=\"a b x\">x</span></div>"
            "</body></html>");
        int descendant = 0, pseudo = 0, pelem = 0, compound = 0, media = 0;
        for (const auto& w : res.warnings) {
            if (w.find("div .x") != std::string::npos) descendant++;
            if (w.find(":visited") != std::string::npos) pseudo++;
            if (w.find("::first-line") != std::string::npos) pelem++;
            if (w.find(".a.b") != std::string::npos) compound++;
            if (w.find("@media") != std::string::npos) media++;
        }
        TEST("css warn: 后代组合子不再告警（v13）", descendant == 0);
        TEST("css warn: unsupported pseudo class", pseudo == 1);
        TEST("css warn: unsupported pseudo element", pelem == 1);
        TEST("css warn: 复合类不再告警（v13）", compound == 0);
        TEST("css warn: @media", media == 1);
    }
    {
        // 动画规则仍由动画解析器处理，不能被普通 CSS 解析吃掉
        auto res = CompileBinUIEx(
            "<html><head><style>"
            "@keyframes pulse { 0% { opacity: 0.2; } 100% { opacity: 1; } }"
            ".pulse { animation: pulse 1s infinite; width: 60px; height: 60px; }"
            "</style></head><body><div id=\"p\" class=\"pulse\"></div></body></html>");
        BinUIRenderer r;
        TEST("css+anim: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        const BinUIRenderer::NodeInfo* p = nullptr;
        for (const auto& nd : r.getNodes()) if (nd.id == r.findNodeByName("p")) p = &nd;
        TEST("css+anim: static props from class still apply", p && std::fabs(p->width - 60.0f) < 0.5f);
    }

    {
        // ---- v11 运行时伪类：:hover / :active / :focus / :checked ----
        auto res = CompileBinUIEx(
            "<html><head><style>"
            ".hov { background: #202020; width: 100px; height: 40px; }"
            ".hov:hover { background: #00ff00; }"
            ".hov:active { background: #ff0000; }"
            ".foc:focus { background: #0000ee; }"
            ".foc:checked { background: #ff00ff; }"
            ".big:hover { transform: scale(1.5); }"
            "</style></head><body>"
            "<div id=\"h\" class=\"hov\"></div>"
            "<button id=\"b\" class=\"foc\" style=\"width:60px;height:24px;background:#111111\">x</button>"
            "<div id=\"s\" class=\"big\" style=\"width:40px;height:20px\"></div>"
            "</body></html>");
        BinUIRenderer r;
        TEST("v11: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        const uint16_t hId = r.findNodeByName("h");
        const auto* hs = r.stateStyle(hId, BINUI_ST_HOVER);
        TEST("v11: :hover style compiled",
             hs && (hs->fields & BINUI_STF_COLOR) && hs->color == 0xFF00FF00u);

        r.setHover(10, 10);
        TEST("v11: hover applies author color", r.drawColor(hId) == 0xFF00FF00u);
        r.press(10, 10);
        TEST("v11: :active outranks :hover", r.drawColor(hId) == 0xFFFF0000u);
        r.release(10, 10);
        TEST("v11: release falls back to hover", r.drawColor(hId) == 0xFF00FF00u);
        r.clearHover();
        TEST("v11: leave restores base color", r.drawColor(hId) == 0xFF202020u);

        const uint16_t bId = r.findNodeByName("b");
        r.setNodeChecked(bId, true);
        TEST("v11: :checked applies", r.drawColor(bId) == 0xFFFF00FFu);
        r.setKeyboardFocus(bId);
        TEST("v11: :focus outranks :checked", r.drawColor(bId) == 0xFF0000EEu);
        r.setKeyboardFocus(-1);
        r.setNodeChecked(bId, false);
        TEST("v11: states cleared", r.drawColor(bId) == 0xFF111111u);

        const uint16_t sId = r.findNodeByName("s");
        const auto* ss = r.stateStyle(sId, BINUI_ST_HOVER);
        TEST("v11: transform scale compiled",
             ss && (ss->fields & BINUI_STF_SCALE) && std::fabs(ss->scale - 1.5f) < 0.001f);
    }
    {
        // ---- 结构伪类：兄弟序号在编译期求值 ----
        auto res = CompileBinUIEx(
            "<html><head><style>"
            "li:first-child { color: #ff0000; }"
            "li:last-child { color: #00ff00; }"
            "li:nth-child(even) { color: #0000ff; }"
            "p:first-of-type { font-size: 30px; }"
            "div:not(.skip) { background: #123456; }"
            "</style></head><body>"
            "<ul><li id=\"l1\">a</li><li id=\"l2\">b</li><li id=\"l3\">c</li></ul>"
            "<p id=\"p1\">x</p><p id=\"p2\">y</p>"
            "<div id=\"d1\"></div><div id=\"d2\" class=\"skip\"></div>"
            "</body></html>");
        BinUIRenderer r;
        TEST("struct: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));
        auto colorOf = [&](const char* name) -> uint32_t {
            const uint16_t id = r.findNodeByName(name);
            for (const auto& nd : r.getNodes()) if (nd.id == id) return nd.color;
            return 0xDEADBEEFu;
        };
        auto fontSizeOf = [&](const char* name) -> int {
            const uint16_t id = r.findNodeByName(name);
            for (const auto& nd : r.getNodes()) if (nd.id == id) return nd.fontSize;
            return -1;
        };
        TEST("struct: :first-child", colorOf("l1") == 0xFFFF0000u);
        TEST("struct: :nth-child(even)", colorOf("l2") == 0xFF0000FFu);
        TEST("struct: :last-child", colorOf("l3") == 0xFF00FF00u);
        TEST("struct: :first-of-type", fontSizeOf("p1") == 30 && fontSizeOf("p2") != 30);
        TEST("struct: :not(.skip) matches", colorOf("d1") == 0xFF123456u);
        TEST("struct: :not(.skip) skips", colorOf("d2") != 0xFF123456u);
    }
    {
        // ---- 伪元素 ::before / ::after ----
        auto res = CompileBinUIEx(
            "<html><head><style>"
            ".bullet::before { content: \"\\25B8 \"; }"
            ".sep::after { content: \"\"; width: 120px; height: 2px; background: #333333; }"
            ".tag::after { content: attr(data-tag); }"
            ".nocon::after { color: #ff0000; }"
            "</style></head><body>"
            "<ul><li id=\"b\" class=\"bullet\">item</li></ul>"
            "<div id=\"s1\" class=\"sep\"></div>"
            "<span id=\"t\" class=\"tag\" data-tag=\"AB\">tag</span>"
            "<span id=\"nc\" class=\"nocon\">nc</span>"
            "</body></html>");
        BinUIRenderer r;
        TEST("pseudo: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));

        std::string bText;
        const uint16_t bId = r.findNodeByName("b");
        for (const auto& nd : r.getNodes()) if (nd.id == bId) bText = nd.text;
        // inline 模式：内容并入宿主文本，且抑制 <li> 自动项目符号
        TEST("pseudo: inline ::before prepends text",
             bText.size() > 3 && bText.rfind("\xE2\x96\xB8 ", 0) == 0);
        TEST("pseudo: no auto bullet when ::before present",
             bText.find("\xE2\x80\xA2") == std::string::npos);

        // box 模式：合成一个子节点（分隔条）
        const uint16_t s1Id = r.findNodeByName("s1");
        const BinUIRenderer::NodeInfo* sep = nullptr;
        for (const auto& nd : r.getNodes())
            if (nd.parentId == s1Id && nd.id != 0 && std::fabs(nd.height - 2.0f) < 0.01f) sep = &nd;
        TEST("pseudo: box ::after creates child node", sep != nullptr);
        TEST("pseudo: box node keeps width/color",
             sep && std::fabs(sep->width - 120.0f) < 0.5f && sep->color == 0xFF333333u);

        std::string tText;
        const uint16_t tId = r.findNodeByName("t");
        for (const auto& nd : r.getNodes()) if (nd.id == tId) tText = nd.text;
        TEST("pseudo: content attr() appends host attribute",
             tText.size() >= 5 && tText.compare(tText.size() - 2, 2, "AB") == 0);

        int noContentWarn = 0;
        for (const auto& w : res.warnings)
            if (w.find("缺少 content") != std::string::npos) noContentWarn++;
        TEST("pseudo: ::after without content warns", noContentWarn == 1);
    }

    {
        // ---- 组件内联：<use src="x.bum.xml"> ----
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / "binui_test_cmp";
        fs::create_directories(dir);
        auto writeFile = [&](const std::string& name, const std::string& text) {
            std::ofstream f(dir / name, std::ios::binary);
            f << text;
        };

        // 组件：内联参数 + 带默认值的参数
        writeFile("btn.bum.xml",
            "<div style=\"height:40\">"
            "<button style=\"position:absolute; left:0; top:0; width:{{w}}; height:40;"
            " background:{{bg=#101010}}\">{{label}}</button></div>");
        // 组合组件：内部再引用 btn.bum.xml
        writeFile("card.bum.xml",
            "<div style=\"height:80\">"
            "<span style=\"height:20\">{{heading}}</span>"
            "<use src=\"btn.bum.xml\" x=\"0\" y=\"24\" w=\"100\" label=\"卡片按钮\"/>"
            "</div>");
        writeFile("host.html",
            "<html><body>"
            "<use src=\"btn.bum.xml\" x=\"10\" y=\"10\" w=\"120\" label=\"确定\" bg=\"#FF0000\"/>"
            "<use src=\"btn.bum.xml\" x=\"10\" y=\"60\" w=\"120\" label=\"默认底色\"/>"
            "<use src=\"card.bum.xml\" x=\"10\" y=\"110\" w=\"200\" heading=\"卡片标题\"/>"
            "<use x=\"10\" y=\"200\"/>"
            "<use src=\"missing.bum.xml\" x=\"10\" y=\"240\"/>"
            "</body></html>");

        auto res = CompileBinUIFileEx((dir / "host.html").generic_string());
        BinUIRenderer r;
        TEST("component: parses", !res.layout.empty() && r.loadLayout(res.layout.data(), res.layout.size()));

        auto findByText = [&](const std::string& text) -> const BinUIRenderer::NodeInfo* {
            for (const auto& n : r.getNodes()) if (n.text == text) return &n;
            return nullptr;
        };
        const auto* okBtn = findByText("确定");
        TEST("component: param substituted into text", okBtn != nullptr);
        TEST("component: param substituted into style",
             okBtn && std::fabs(okBtn->width - 120.0f) < 0.5f && okBtn->color == 0xFFFF0000u);
        const auto* dfltBtn = findByText("默认底色");
        TEST("component: default value used when param omitted",
             dfltBtn && dfltBtn->color == 0xFF101010u);
        const auto* cardBtn = findByText("卡片按钮");
        TEST("component: nested <use> expanded", cardBtn != nullptr);
        TEST("component: nested heading substituted", findByText("卡片标题") != nullptr);

        int noSrc = 0, missingFile = 0;
        for (const auto& w : res.warnings) {
            if (w.find("缺少 src") != std::string::npos) noSrc++;
            if (w.find("读不到组件文件") != std::string::npos) missingFile++;
        }
        TEST("component: missing src warns", noSrc == 1);
        TEST("component: missing file warns", missingFile == 1);

        // ---- v12：二进制组件 .bum（编译期由链接器离线合并）----
        writeFile("pack.bum.xml",
            "<div style=\"height:30\"><span style=\"height:20; color:#00FF00\">打包组件</span></div>");
        const auto packRes = CompileBinUIFileEx((dir / "pack.bum.xml").generic_string());
        TEST("component: binary build produces layout", !packRes.layout.empty());
        {
            std::ofstream f(dir / "pack.bum", std::ios::binary);
            f.write(reinterpret_cast<const char*>(packRes.layout.data()),
                    static_cast<std::streamsize>(packRes.layout.size()));
        }
        writeFile("binhost.html",
            "<html><body>"
            "<use src=\"pack.bum\" x=\"10\" y=\"10\" w=\"140\"/>"
            "<use src=\"pack.bum\" external x=\"10\" y=\"60\" w=\"140\"/>"
            "</body></html>");
        const auto bres = CompileBinUIFileEx((dir / "binhost.html").generic_string());
        BinUIRenderer rb;
        TEST("component: binary host parses",
             !bres.layout.empty() && rb.loadLayout(bres.layout.data(), bres.layout.size()));
        int packed = 0;
        const BinUIRenderer::NodeInfo* packedSpan = nullptr;
        for (const auto& n : rb.getNodes())
            if (n.text == "打包组件") { packed++; packedSpan = &n; }
        TEST("component: binary .bum inlined at compile time", packed == 1);
        const BinUIRenderer::NodeInfo* packedWrap = nullptr;
        if (packedSpan)
            for (const auto& n : rb.getNodes())
                if (n.id == packedSpan->parentId) packedWrap = &n;
        TEST("component: <use w> 覆盖二进制组件根宽",
             packedWrap && std::fabs(packedWrap->width - 140.0f) < 0.5f);
        TEST("component: binary 组件根坐标落在锚点处",
             packedWrap && std::fabs(packedWrap->y - 10.0f) < 0.5f);

        // ---- v12：外挂组件（external）-> 组件引用段 + 运行期挂载 ----
        TEST("component: external <use> 记录一个挂载点", rb.componentRefs().size() == 1);
        TEST("component: 挂载点路径已相对宿主改写",
             !rb.componentRefs().empty() && rb.componentRefs()[0].src.find("pack.bum") != std::string::npos);
        if (!rb.componentRefs().empty()) {
            const uint16_t anchor = rb.componentRefs()[0].anchorId;
            std::vector<uint8_t> packBytes = packRes.layout;
            TEST("component: 运行期挂载成功",
                 rb.mountComponent(packBytes.data(), packBytes.size(), anchor));
            int after = 0;
            for (const auto& n : rb.getNodes())
                if (n.text == "打包组件") after++;
            TEST("component: 挂载后节点数增加", after == packed + 1);
            TEST("component: 已挂载标记幂等", rb.isComponentMounted(anchor));
        }

        // ---- 二进制与源码并存：二进制优先；源码更新则改用源码并告警 ----
        {
            std::ofstream f(dir / "pack.bum.xml", std::ios::binary);
            f << "<div style=\"height:30\"><span style=\"height:20\">源码更新版</span></div>";
        }
        std::filesystem::last_write_time(
            dir / "pack.bum.xml",
            std::filesystem::last_write_time(dir / "pack.bum") + std::chrono::seconds(5));
        const auto nres = CompileBinUIFileEx((dir / "binhost.html").generic_string());
        bool staleWarn = false;
        for (const auto& w : nres.warnings)
            if (w.find("源码比二进制新") != std::string::npos) staleWarn = true;
        TEST("component: 源码比二进制新时告警", staleWarn);
        BinUIRenderer rn;
        rn.loadLayout(nres.layout.data(), nres.layout.size());
        bool hasNewText = false;
        for (const auto& n : rn.getNodes())
            if (n.text == "源码更新版") hasNewText = true;
        TEST("component: 源码更新版生效（改用源码内联）", hasNewText);

        // ---- v12：资源段（布局自描述：imageIndex -> 图片路径）----
        writeFile("img.html",
            "<html><body><img src=\"noela.png\" style=\"width:40; height:40\"/></body></html>");
        const auto ires = CompileBinUIFileEx((dir / "img.html").generic_string());
        BinUIRenderer ri;
        ri.loadLayout(ires.layout.data(), ires.layout.size());
        TEST("v12: 资源段随布局写出",
             !ires.images.empty() && ri.imagePaths().size() == ires.images.size() &&
             ri.imagePaths()[0] == ires.images[0]);

        // 参数值按纯文本注入：传进来的标签不能变成真节点
        writeFile("inject.bum.xml", "<div style=\"height:20\"><span>{{body}}</span></div>");
        writeFile("inject.html",
            "<html><body><use src=\"inject.bum.xml\" x=\"0\" y=\"0\" w=\"300\""
            " body=\"a &lt;use src=&quot;btn.bum.xml&quot;&gt; b\"/></body></html>");
        auto res2 = CompileBinUIFileEx((dir / "inject.html").generic_string());
        BinUIRenderer r2;
        TEST("component: inject parses", !res2.layout.empty() && r2.loadLayout(res2.layout.data(), res2.layout.size()));
        int textWithMarkup = 0;
        for (const auto& n : r2.getNodes())
            if (n.text.find("<use") != std::string::npos) textWithMarkup = 1;
        TEST("component: param value injected as text, not markup", textWithMarkup == 1);
        TEST("component: injection adds no extra nodes", r2.getNodes().size() == 3);
    }

    // ---- v13：扩展绘制段（文字色/边框/渐变/字重/行高/装饰）+ 底色与文字色分家 ----
    {
        const char* html =
            "<html><body style=\"background:#FFFFFF; color:#333333\">\n"
            "  <div class=\"card\" id=\"card\">\n"
            "    <span class=\"title\" id=\"title\">标题</span>\n"
            "    <span id=\"plain\">继承父色</span>\n"
            "  </div>\n"
            "  <a class=\"link\" id=\"link\">链接</a>\n"
            "  <button class=\"btn\" id=\"btn\">确定</button>\n"
            "  <span class=\"badge\" id=\"badge\">徽标</span>\n"
            "  <div class=\"grad\" id=\"grad\"></div>\n"
            "  <div class=\"clip\" id=\"clip\"></div>\n"
            "  <div class=\"scroll\" id=\"scroll\"></div>\n"
            "</body></html>\n"
            "<style>\n"
            "  .card { padding:8px; background:#FFFFFF; border:1px solid #DDDDDD;"
            "          border-bottom:2px solid #FF0000; border-radius:8px; }\n"
            "  .title { font-size:18px; font-weight:700; line-height:160%; color:#111111; }\n"
            "  .link { font-size:14px; text-decoration:underline; color:#0055FF; }\n"
            "  .btn  { width:100px; height:36px; background:#8B5CF6; color:#101010; }\n"
            "  .badge { background:#FFEEDD; color:#884400; border-radius:4px; }\n"
            "  .grad { width:200px; height:60px; background:linear-gradient(to right, #202020, #404040); }\n"
            "  .clip { width:200px; height:40px; overflow:hidden; }\n"
            "  .scroll { width:200px; height:40px; overflow:auto; }\n"
            "</style>\n";
        auto res = CompileBinUIEx(html);
        BinUIRenderer r;
        TEST("v13: 布局可解析", ParseInto(r, res.layout));
        TEST("v13: 版本号 >= 13", BINUI_VERSION >= 13);

        auto byId = [&](const char* name) -> uint16_t { return r.findNodeByName(name); };
        auto nodeOf = [&](uint16_t id) -> const BinUIRenderer::NodeInfo* {
            for (const auto& n : r.getNodes()) if (n.id == id) return &n;
            return nullptr;
        };

        // 容器默认透明（v13 起不再默认 #222222），底色只来自作者声明
        const uint16_t cardId = byId("card");
        const auto* card = nodeOf(cardId);
        TEST("v13: 卡片找到", card != nullptr);
        if (card) {
            TEST("v13: 卡片底色 = 声明的白色", card->color == 0xFFFFFFFFu);
            const BinUIPaintNode* p = r.paintOf(cardId);
            TEST("v13: 卡片有边框绘制段", p != nullptr && (p->flags & PAINT_BORDER));
            if (p && (p->flags & PAINT_BORDER)) {
                TEST("v13: 边框四边宽 1/1/2/1",
                     p->borderW[0] == 1 && p->borderW[2] == 2 &&
                     p->borderW[1] == 1 && p->borderW[3] == 1);
                TEST("v13: 边框颜色按边生效（上灰下红）",
                     p->borderColor[0] == 0xFFDDDDDDu && p->borderColor[2] == 0xFFFF0000u);
            }
        }

        // 文字色与底色分家：LABEL 有底色时文字色走 textColor，不再吃掉背景
        const uint16_t titleId = byId("title");
        const auto* title = nodeOf(titleId);
        const BinUIPaintNode* tp = r.paintOf(titleId);
        // LABEL 没有底色时沿用旧约定（node.color 存文字色），textColorOf 两条路径都通
        TEST("v13: 标题文字色取到 #111111", r.textColorOf(titleId) == 0xFF111111u);
        // 有底色的 LABEL：底色与文字色必须分家
        const uint16_t badgeId = byId("badge");
        const auto* badge = nodeOf(badgeId);
        const BinUIPaintNode* bd = r.paintOf(badgeId);
        TEST("v13: 有底色的 LABEL 底色正确", badge && badge->color == 0xFFFFEEDDu);
        TEST("v13: 有底色的 LABEL 文字色走 textColor",
             bd && (bd->flags & PAINT_TEXT_COLOR) && bd->textColor == 0xFF884400u);
        (void)title;
        TEST("v13: 字重 700", tp && (tp->flags & PAINT_WEIGHT) && tp->weight == 7);
        TEST("v13: 行高 160%", tp && (tp->flags & PAINT_LINE_HEIGHT) && tp->lineHeight == 160);
        TEST("v13: 字号 18", title && title->fontSize == 18);

        // CSS color 继承：没写颜色的子元素取祖先的 color
        TEST("v13: 文字色可继承", r.textColorOf(byId("plain")) == 0xFF333333u);

        // 下划线
        const BinUIPaintNode* lp = r.paintOf(byId("link"));
        TEST("v13: text-decoration:underline", lp && (lp->flags & PAINT_UNDERLINE) &&
             lp->textColor == 0xFF0055FFu);

        // 按钮文字色（以前恒白，无法指定）
        const BinUIPaintNode* bp = r.paintOf(byId("btn"));
        TEST("v13: 按钮文字色可指定", bp && (bp->flags & PAINT_TEXT_COLOR) &&
             bp->textColor == 0xFF101010u);

        // 渐变
        const BinUIPaintNode* gp = r.paintOf(byId("grad"));
        TEST("v13: 渐变段写出", gp && (gp->flags & PAINT_GRADIENT) &&
             gp->gradFrom == 0xFF202020u && gp->gradTo == 0xFF404040u &&
             gp->gradDir == BINUI_GRAD_H);
        {
            bool colorWarn = false;
            for (const auto& w : res.warnings)
                if (w.find("无法识别的颜色") != std::string::npos) colorWarn = true;
            TEST("v13: 渐变不被当成颜色解析（无假警告）", !colorWarn);
        }

        // overflow：hidden = 只裁剪（不可滚），auto = 可滚
        const uint16_t clipId = byId("clip"), scrollId = byId("scroll");
        TEST("v13: overflow:hidden 仍是裁剪容器", r.isScrollable(clipId));
        TEST("v13: overflow:hidden 不可滚动", !r.isUserScrollable(clipId));
        TEST("v13: overflow:auto 可滚动", r.isUserScrollable(scrollId));
        TEST("v13: overflow:hidden 滚轮无效",
             r.scrollAt(20.0f, nodeOf(clipId) ? nodeOf(clipId)->y + 5.0f : 0.0f, -30.0f) < 0);
    }

    // ---- v13：选择器组合子 / 复合类 / 属性选择器 ----
    {
        const char* html =
            "<html><body>\n"
            "  <div class=\"card\">\n"
            "    <span class=\"title\">A</span>\n"
            "    <span class=\"body\">B</span>\n"
            "  </div>\n"
            "  <div class=\"box\"><span class=\"deep\">C</span></div>\n"
            "  <button class=\"btn primary\">D</button>\n"
            "  <button class=\"btn\">E</button>\n"
            "  <input type=\"text\" value=\"F\">\n"
            "  <input type=\"button\" value=\"G\">\n"
            "  <ul class=\"adj\"><li>H</li><li>I</li></ul>\n"
            "  <ul class=\"gen\"><li>J</li><li>K</li><li>L</li></ul>\n"
            "</body></html>\n"
            "<style>\n"
            "  .card > .title   { color:#AA0000; }\n"          // 子代
            "  .card .body     { color:#00AA00; }\n"           // 后代
            "  .box .deep      { color:#0000AA; }\n"           // 后代（跨层）
            "  .btn.primary    { color:#00AAAA; }\n"           // 复合类
            "  input[type=text]{ color:#AA00AA; }\n"           // 属性选择器
            "  .adj li + li    { color:#AAAA00; }\n"           // 相邻兄弟
            "  .gen li ~ li    { color:#00AA00; }\n"           // 后续兄弟
            "  .body:not(.x)  { background:#111111; }\n"       // :not
            "</style>\n";
        auto res = CompileBinUIEx(html);
        BinUIRenderer r;
        TEST("sel: 布局可解析", ParseInto(r, res.layout));

        auto textColor = [&](const std::string& text) -> uint32_t {
            for (const auto& n : r.getNodes()) {
                // <li> 会自动加 "• " / "1. " 前缀，这里按后缀匹配
                if (n.text.size() < text.size()) continue;
                if (n.text.compare(n.text.size() - text.size(), text.size(), text) != 0) continue;
                if (const BinUIPaintNode* p = r.paintOf(n.id))
                    if (p->flags & PAINT_TEXT_COLOR) return p->textColor;
                return n.color;   // LABEL 旧约定
            }
            return 0;
        };
        TEST("sel: 子代组合子 .card > .title", textColor("A") == 0xFFAA0000u);
        TEST("sel: 后代组合子 .card .body", textColor("B") == 0xFF00AA00u);
        TEST("sel: 后代组合子跨层 .box .deep", textColor("C") == 0xFF0000AAu);
        TEST("sel: 复合类 .btn.primary", textColor("D") == 0xFF00AAAAu);
        TEST("sel: 复合类未命中时回落", textColor("E") != 0xFF00AAAAu);
        TEST("sel: 属性选择器 input[type=text]", textColor("F") == 0xFFAA00AAu);
        TEST("sel: 属性选择器不误伤 input[type=button]", textColor("G") != 0xFFAA00AAu);
        TEST("sel: 相邻兄弟 li + li", textColor("I") == 0xFFAAAA00u);
        TEST("sel: 相邻兄弟不命中首项", textColor("H") != 0xFFAAAA00u);
        TEST("sel: 后续兄弟 li ~ li", textColor("K") == 0xFF00AA00u);
        TEST("sel: 后续兄弟同样命中第三项", textColor("L") == 0xFF00AA00u);
        TEST("sel: 后续兄弟不命中首项", textColor("J") != 0xFF00AA00u);
        TEST("sel: :not() 内层复合类", textColor("B") == 0xFF00AA00u);
    }
    // ---- v14：UA 默认样式 / 行内语义 / 表单属性 ----
    {
        const char* html =
            "<app_window width=\"800\" height=\"600\" bg=\"#FFFFFF\"><html><body>\n"
            "  <b id=\"b1\">粗体</b>\n"
            "  <i id=\"i1\">斜体</i>\n"
            "  <u id=\"u1\">下划线</u>\n"
            "  <s id=\"s1\">删除</s>\n"
            "  <mark id=\"m1\">高亮</mark>\n"
            "  <h2 id=\"h2\">标题</h2>\n"
            "  <hr id=\"hr1\">\n"
            "  <pre id=\"pre1\">a\nb\nc</pre>\n"
            "  <input id=\"ph\" type=\"text\" placeholder=\"请输入\">\n"
            "  <input id=\"val\" type=\"text\" value=\"真实值\">\n"
            "  <input id=\"cb\" type=\"checkbox\" checked>\n"
            "  <input id=\"hid\" type=\"hidden\" value=\"x\">\n"
            "  <button id=\"btn\" disabled>禁用</button>\n"
            "  <input id=\"ro\" type=\"text\" readonly value=\"只读\">\n"
            "  <a id=\"lnk\" href=\"https://example.com\">链接</a>\n"
            "  <img id=\"im\" alt=\"替代文字\">\n"
            "</body></html></app_window>\n";
        auto res = CompileBinUIEx(html);
        BinUIRenderer r;
        TEST("v14: 布局可解析", ParseInto(r, res.layout));
        TEST("v14: 版本号 14", BINUI_VERSION == 14);

        auto byId = [&](const char* name) -> uint16_t { return r.findNodeByName(name); };
        auto nodeOf = [&](uint16_t id) -> const BinUIRenderer::NodeInfo* {
            for (const auto& n : r.getNodes()) if (n.id == id) return &n;
            return nullptr;
        };
        auto paintOf = [&](const char* name) -> const BinUIPaintNode* {
            return r.paintOf(byId(name));
        };

        // <b> 加粗（以前只是普通文字）
        const BinUIPaintNode* pb = paintOf("b1");
        TEST("v14: <b> 走 700 字重", pb && (pb->flags & PAINT_WEIGHT) && pb->weight == 7);
        // <i> 斜体
        const BinUIPaintNode* pi = paintOf("i1");
        TEST("v14: <i> 斜体标记", pi && (pi->textFlags & TEXT_ITALIC));
        // <h2> 默认加粗
        const BinUIPaintNode* ph2 = paintOf("h2");
        TEST("v14: <h2> 默认加粗", ph2 && (ph2->flags & PAINT_WEIGHT) && ph2->weight == 7);
        // <u>/<s> 是行内 LABEL（以前掉到 LIST 变块级），带装饰线
        const auto* u1 = nodeOf(byId("u1"));
        const auto* s1 = nodeOf(byId("s1"));
        TEST("v14: <u> 是 LABEL 不是块级容器", u1 && u1->type == BINUI_LABEL);
        TEST("v14: <u> 带下划线", paintOf("u1") && (paintOf("u1")->flags & PAINT_UNDERLINE));
        TEST("v14: <s> 是 LABEL", s1 && s1->type == BINUI_LABEL);
        TEST("v14: <s> 带删除线", paintOf("s1") && (paintOf("s1")->flags & PAINT_STRIKE));
        // <mark> 黄底深字
        const auto* m1 = nodeOf(byId("m1"));
        TEST("v14: <mark> 有黄底", m1 && ((m1->color >> 24) & 0xFF) && m1->color != 0xFFFFFFFFu);
        TEST("v14: <mark> 深色文字", paintOf("m1") &&
             (paintOf("m1")->flags & PAINT_TEXT_COLOR) && paintOf("m1")->textColor == 0xFF1A1A1Au);
        // <hr> 有高度和底色（v13 容器透明化后曾经完全不可见）
        const auto* hr = nodeOf(byId("hr1"));
        TEST("v14: <hr> 有可见高度", hr && hr->height >= 1.0f && hr->height <= 4.0f);
        TEST("v14: <hr> 有不透明底色", hr && ((hr->color >> 24) & 0xFF) == 0xFF);
        // <pre> 保留换行
        const auto* pre = nodeOf(byId("pre1"));
        TEST("v14: <pre> 保留原始换行", pre && pre->text.find('\n') != std::string::npos);

        // placeholder 不再是真实值
        const uint16_t phId = byId("ph");
        const auto* ph = nodeOf(phId);
        TEST("v14: placeholder 文本仍可显示", ph && ph->text == "请输入");
        TEST("v14: placeholder 有占位标记", r.isPlaceholder(phId));
        TEST("v14: placeholder 的真实值为空", r.inputValue(phId).empty());
        // value 相反
        const uint16_t valId = byId("val");
        TEST("v14: value 是真实值", !r.isPlaceholder(valId) && r.inputValue(valId) == "真实值");

        // checkbox：18x18 方块 + 初始勾选播种
        const uint16_t cbId = byId("cb");
        const auto* cb = nodeOf(cbId);
        TEST("v14: checkbox 是小方块", cb && cb->width <= 20.0f && cb->height <= 20.0f);
        TEST("v14: checkbox checked 播种进渲染器", r.isNodeChecked(cbId));
        // hidden 不生成节点
        TEST("v14: input type=hidden 不生成节点", byId("hid") == 0);
        // disabled / readonly
        TEST("v14: button disabled", r.isDisabled(byId("btn")));
        TEST("v14: input readonly", r.isReadonly(byId("ro")));
        TEST("v14: readonly 不是 disabled", !r.isDisabled(byId("ro")));
        // <a href> 可点
        const uint16_t lnkId = byId("lnk");
        const auto* lnk = nodeOf(lnkId);
        TEST("v14: <a href> 可命中", lnk &&
             r.hitTest(lnk->x + lnk->width * 0.5f, lnk->y + lnk->height * 0.5f) == (int)lnkId);
        // img alt 兜底
        const auto* im = nodeOf(byId("im"));
        TEST("v14: img alt 写进文本（资源缺失时显示）", im && im->text == "替代文字");

        // 默认文字色随根底色自适应：白底 → 深字
        const auto* bold1 = nodeOf(byId("b1"));
        TEST("v14: 白底默认深色文字", bold1 && bold1->color == 0xFF1A1A1Au);

        // 深底 → 白字（现有暗色主题观感不变）
        auto darkRes = CompileBinUIEx(
            "<app_window width=\"200\" height=\"100\" bg=\"#0E0E13\"><html><body>"
            "<span id=\"t\">x</span></body></html></app_window>");
        BinUIRenderer rd;
        TEST("v14: 深底布局可解析", ParseInto(rd, darkRes.layout));
        const uint16_t tid = rd.findNodeByName("t");
        const BinUIRenderer::NodeInfo* tn = nullptr;
        for (const auto& n : rd.getNodes()) if (n.id == tid) tn = &n;
        TEST("v14: 深底默认白色文字", tn && tn->color == 0xFFFFFFFFu);
    }

    printf("\n=== Results: %d tests failed ===\n", failed);
    return failed > 0 ? 1 : 0;
}
