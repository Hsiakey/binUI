#version 450
// binui shape.frag —— 圆角矩形的解析抗锯齿（v0.35.0）
//
// 与 binGPU 的 shape.frag、bin2d 的 shape_coverage 逐字同一套式子：
//     q   = abs(p) - half + r
//     d   = length(max(q,0)) + min(max(q.x,q.y),0) - r
//     cov = clamp(0.5 - d, 0, 1)                  实心
//     cov = clamp(0.5 - |d + t/2| + t/2, 0, 1)    环带（厚度 t）
//
// 轴对齐 1:1 时距离场 1-Lipschitz、|grad d| ≡ 1，所以 0.5 - d 是**精确**覆盖率，
// 不需要 fwidth（导数在 2×2 quad 上是分块常数，小半径的弧上会退化成方块）。
//
// 顶点色是**预乘**的（binui 的惯例），所以这里是 vcol * cov，四个通道一起乘。

noperspective layout(location = 0) in vec2 vLocal;
flat layout(location = 1) in vec2 vExt;
flat layout(location = 2) in vec2 vRT;
noperspective layout(location = 3) in vec4 vcol;

layout(location = 0) out vec4 frag;

void main() {
    float r = max(vRT.x, 0.0);
    float th = vRT.y;
    vec2 q = abs(vLocal) - vExt + vec2(r);
    float d = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;
    float cov;
    if (th > 0.0) {
        float ht = th * 0.5;
        cov = 0.5 - abs(d + ht) + ht;
    } else {
        cov = 0.5 - d;
    }
    frag = vcol * clamp(cov, 0.0, 1.0);
}
