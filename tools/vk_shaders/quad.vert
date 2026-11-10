#version 450
// binUI 四边形批次：顶点是像素坐标（左上原点）。
// 注意：Vulkan 的 NDC **y 向下**（-1 = 顶部），与 GL 相反，所以这里不能照抄 GL 的
// "1 - y/h*2"，而是直接 y/h*2-1 —— 否则整幅画面上下颠倒（vkcolor 用例抓到过）。
layout(location = 0) in vec2 pos;
layout(location = 1) in vec4 col;
layout(location = 2) in vec2 uv;

layout(push_constant) uniform Push { vec2 viewSize; } pc;

layout(location = 0) out vec4 vcol;
layout(location = 1) out vec2 vuv;

void main() {
    gl_Position = vec4(pos.x / pc.viewSize.x * 2.0 - 1.0,
                       pos.y / pc.viewSize.y * 2.0 - 1.0, 0.0, 1.0);
    vcol = col;
    vuv = uv;
}
