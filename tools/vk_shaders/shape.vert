#version 450
// binui shape.vert —— 解析圆角（v0.35.0）
//
// 与 binui 的 quad.vert 同一套坐标约定（NDC y 向下、viewSize 是 push constant），
// 只是顶点换成 36 字节的形状顶点（与 binGPU 的 BgpuShapeVertex 同口径）：
//   location 0：vec2  位置（目标像素）
//   location 1：vec2  局部坐标（相对盒子中心，顶点处 = ±half）
//   location 2：vec2  盒子半尺寸
//   location 3：float 圆角半径（浮点）
//   location 4：float 环带厚度（0 = 实心，浮点）
//   location 5：vec4  预乘色（binui 的顶点色一律预乘）
//
// vLocal 用 noperspective：w 恒 1 时数学上是空操作，但那一遍除法会让线性属性多舍入
// 一次；vExt / vRT 对四个顶点是常量，用 flat。

layout(push_constant) uniform PC { vec2 viewSize; } pc;

layout(location = 0) in vec2 pos;
layout(location = 1) in vec2 local;
layout(location = 2) in vec2 ext;
layout(location = 3) in float rad;
layout(location = 4) in float thick;
layout(location = 5) in vec4 col;

noperspective layout(location = 0) out vec2 vLocal;
flat layout(location = 1) out vec2 vExt;
flat layout(location = 2) out vec2 vRT;
noperspective layout(location = 3) out vec4 vcol;

void main() {
    gl_Position = vec4(pos.x / pc.viewSize.x * 2.0 - 1.0,
                       pos.y / pc.viewSize.y * 2.0 - 1.0, 0.0, 1.0);
    vLocal = local;
    vExt = ext;
    vRT = vec2(rad, thick);
    vcol = col;
}
