#version 450
// 与 GL 后端片元着色器一致：纹理 × 顶点色，输出预乘 alpha（混合 ONE, ONE_MINUS_SRC_ALPHA）。
// 注：顶点色的字节序在 Vulkan 侧由 VkVertexInputAttributeDescription 的格式决定，
// vk_shot 的三色卡用例专门校验它与 GDI 一致。
layout(location = 0) in vec4 vcol;
layout(location = 1) in vec2 vuv;

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) out vec4 frag;

void main() {
    vec4 t = texture(tex, vuv);
    frag = vec4(t.rgb * vcol.rgb, t.a * vcol.a);
}
