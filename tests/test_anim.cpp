// test_anim.cpp - v7 动画引擎测试：@keyframes 关键帧动画 + transition 过渡 + 运行时样式 API
#include "binui_compiler.h"
#include "binui_renderer.h"
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>

using namespace binui;

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); g_fail++; } } while (0)

// 轮询直到条件满足或超时
template <typename F>
static bool WaitFor(F f, int timeoutMs) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

int main() {
    const char* html = R"HTML(<!DOCTYPE html>
<html width="400" height="300" bg="#101014">
<head><style>
@keyframes fadeUp {
    from { opacity: 0; transform: translateY(20px); }
    to   { opacity: 1; transform: translateY(0); }
}
@keyframes pulse {
    0%, 100% { transform: scale(1); }
    50%      { transform: scale(1.08); }
}
button { transition: background-color 0.3s ease, transform 0.2s ease; }
</style></head>
<body>
    <div animation="fadeUp 0.5s ease 0.15s 1 normal both"
         style="position:absolute; left:20; top:40; width:200; height:80; background:#16161D; border-radius:10">Card</div>
    <button animation="pulse 1s linear 0s infinite"
            style="position:absolute; left:20; top:140; width:120; height:44; background:#8B5CF6; border-radius:8">Btn</button>
    <button style="position:absolute; left:160; top:140; width:120; height:44; background:#03DAC6; border-radius:8; transition:opacity 0.4s ease">Btn2</button>
</body>
</html>)HTML";

    auto res = CompileBinUIEx(html);
    CHECK(!res.layout.empty(), "compile");
    BinUIRenderer r;
    CHECK(r.loadLayout(res.layout.data(), res.layout.size()), "loadLayout");

    int cardId = -1, btnId = -1, btn2Id = -1;
    for (const auto& n : r.getNodes()) {
        if (n.type == BINUI_LIST) cardId = n.id;
        else if (n.type == BINUI_BUTTON && btnId < 0) btnId = n.id;
        else if (n.type == BINUI_BUTTON) btn2Id = n.id;
    }
    CHECK(cardId >= 0 && btnId >= 0 && btn2Id >= 0, "ids");

    // 关键帧：加载时刻应处于 delay 阶段（backwards fill → opacity 0，translateY 20）
    CHECK(r.drawOpacity((uint16_t)cardId) < 0.05f, "keyframe initial opacity");
    CHECK(r.drawOffsetY((uint16_t)cardId) > 19.0f, "keyframe initial translateY");

    // 动画推进：opacity 最终达到 1 并保持（fill forwards）
    CHECK(WaitFor([&] { r.stepAnimations(); return r.drawOpacity((uint16_t)cardId) > 0.95f; }, 2000),
          "keyframe reaches end");
    CHECK(WaitFor([&] { r.stepAnimations(); return r.drawOffsetY((uint16_t)cardId) < 1.0f; }, 2000),
          "keyframe translate returns to 0");
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    r.stepAnimations();
    CHECK(r.drawOpacity((uint16_t)cardId) > 0.95f, "keyframe fill forwards holds");

    // 无限循环动画（pulse）：scale 应周期变化
    bool sawNot1 = false;
    for (int i = 0; i < 12; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        r.stepAnimations();
        float s = r.drawScale((uint16_t)btnId);
        if (s > 1.02f) sawNot1 = true;
    }
    CHECK(sawNot1, "infinite keyframe animates scale");

    // hover 过渡：进入按钮 → 背景色/缩放平滑过渡到高亮
    uint32_t c0 = r.drawColor((uint16_t)btnId);
    r.setHover(70, 170); // 命中 btn
    CHECK(WaitFor([&] { r.stepAnimations(); return r.drawScale((uint16_t)btnId) > 1.03f; }, 1000),
          "hover scale transitions in");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    r.stepAnimations();
    uint32_t c1 = r.drawColor((uint16_t)btnId);
    CHECK(c1 != c0, "hover color transitions in");

    // 离开 → 恢复
    auto nearColor = [](uint32_t a, uint32_t b, int eps) {
        int ra = (int)((a >> 16) & 0xFF), ga = (int)((a >> 8) & 0xFF), ba = (int)(a & 0xFF);
        int rb = (int)((b >> 16) & 0xFF), gb = (int)((b >> 8) & 0xFF), bb = (int)(b & 0xFF);
        return std::abs(ra - rb) <= eps && std::abs(ga - gb) <= eps && std::abs(ba - bb) <= eps;
    };
    r.setHover(300, 170);
    CHECK(WaitFor([&] { r.stepAnimations(); return r.drawScale((uint16_t)btnId) < 1.01f; }, 1000),
          "hover scale transitions out");
    CHECK(WaitFor([&] { r.stepAnimations(); return nearColor(r.drawColor((uint16_t)btnId), c0, 4); }, 1000),
          "hover color restores");

    // 运行时样式 API：setStyleFloat 触发 opacity 过渡
    r.setStyleFloat((uint16_t)btn2Id, ANIM_OPACITY, 0.5f);
    CHECK(WaitFor([&] { r.stepAnimations(); return r.drawOpacity((uint16_t)btn2Id) < 0.9f; }, 2000),
          "style opacity transition starts");
    CHECK(WaitFor([&] { r.stepAnimations(); float o = r.drawOpacity((uint16_t)btn2Id); return o > 0.42f && o < 0.58f; }, 2000),
          "style opacity transition settles");

    if (g_fail == 0) std::printf("=== Results: all animation tests passed ===\n");
    else std::printf("=== Results: %d FAILED ===\n", g_fail);
    return g_fail;
}
