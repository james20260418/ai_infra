// JPOV StripRenderer —— 半透明条带（Strip3D）画进 1× WBO 纹理，再 over 合成进 HDR
//
// 与 particles / volume_fire **平级**的独立 effect pass（Danis 2026-10-21 定：
// 「这个 renderer 算在 effect 大类下，和 particle 并列」）。
// 设计文档：tools/jpov/docs/jpov_strip3d_translucent_design.md
//
// ⭐ 核心取舍（2026-10-21 与 Danis 讨论后定稿）：
//   条带 = **固定 16 个采样点**的双轨，形状每帧可变、顶点数不变 ⇒
//   **一批条带 = 一次 draw call**，用「共享静态索引 + 每帧重传顶点」的普通 VAO 路径，
//   **不引入 instancing**（不需要 divisor / 数据纹理 / texelFetch / 属性槽预算）。
//
//   顶点拓扑（固定，见 strip_geometry.h）：
//     16 列 ×（right, left）= 32 顶点，相邻两列 1 个 quad，共 15 quad / 90 索引。
//     uv：u 横跨宽度（right=0 → left=1），v = col/15（纹理按「16 格」设计）。
//
//   FS：
//     α = tint.a · vertex_alpha · texture.a · width_profile(u)
//     → 与不透明场景深度手动作深度测试（WBO 目标无深度缓冲）。
//       **M1 用硬判据**（z > scene_z + 2cm 即丢弃）；软深度是 M3。
//     → 写覆盖集：att0 = vec4(色·α, α)·w、att1 = α（blend 见 wbo_target）
//     剖面在**片元**里逐像素求值 ⇒ 4 段几何也能有平滑的宽度方向衰减。
//
//   本版（M1）只有覆盖集（吸收介质）。加法集（拖尾/光环）是 M2，
//   软深度 M3，fresnel M4 —— 都**不预留空字段**（设了不生效比没有更糟）。
//
// 合成：Composite() 全屏解析 WBO（C = accum.rgb/accum.a、cov = 1−reveal）并 over
//   写进当前绑定的目标（HDR），随后由 horizon fog / froxel 继续。

#ifndef JPOV_SRC_STRIP_STRIP_RENDERER_H_
#define JPOV_SRC_STRIP_STRIP_RENDERER_H_

#include <cstdint>
#include <vector>

#include "tools/jpov/interface/camera.h"
#include "tools/jpov/interface/strip_types.h"
#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/strip/strip_geometry.h"
#include "tools/jpov/src/texture_manager.h"
#include "tools/jpov/src/wbo/wbo_target.h"

namespace jpov {

class StripRenderer {
public:
    // 单次 draw call 最多多少条带（受静态索引缓冲尺寸约束）。
    // 超出时**分成多次 draw**（不是静默截断 —— 旧 DrawStrip3D 的 3000 顶点截断是反面教材）。
    static constexpr int kMaxStripsPerDraw = 1024;

    // 顶点着色器：位置/uv 已由 CPU 展开好，这里只做 MVP。
    static constexpr const char* kVs = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aUVAlpha;   // (u, v, alpha)
uniform mat4 uViewProj;
out vec3 vUVAlpha;
void main() {
    vUVAlpha = aUVAlpha;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)glsl";

    // 片元着色器头（权重函数由 wbo_shader.h 注入到 head/tail 之间）。
    static constexpr const char* kFsHead = R"glsl(
#version 330 core
in vec3 vUVAlpha;                 // (u, v, alpha)
uniform sampler2D uTex;
uniform int uHasTex;
uniform vec4 uTint;
uniform float uProfile[9];        // u = k/8 处的宽度方向 alpha 乘子
uniform sampler2D uSceneDepth;
uniform vec2 uViewport;           // 本 pass 的像素尺寸（1× 时即 3D FBO 尺寸）
uniform float uNear;
uniform float uFar;
layout(location = 0) out vec4 FragAccum;    // 覆盖集：Σ(色·α·w, α·w)
layout(location = 1) out float FragReveal;  // 覆盖集：Π(1−α)
// 窗口深度 [0,1] → 视空间深度（正，米）。
float ZView(float d) {
    float ndc = 2.0 * d - 1.0;
    return (2.0 * uNear * uFar) / ((uFar + uNear) - ndc * (uFar - uNear));
}
// 宽度方向剖面：9 knot / 8 段分段线性，在片元里求值（不是在顶点里插值）。
float WidthProfile(float u) {
    float x = clamp(u, 0.0, 1.0) * 8.0;
    int i = int(min(floor(x), 7.0));
    return mix(uProfile[i], uProfile[i + 1], x - float(i));
}
)glsl";

    static constexpr const char* kFsTail = R"glsl(
void main() {
    vec3 color = uTint.rgb;
    float a = uTint.a * vUVAlpha.z;
    if (uHasTex == 1) {
        vec4 tex = texture(uTex, vUVAlpha.xy);
        color *= tex.rgb;
        a *= tex.a;
    }
    a *= WidthProfile(vUVAlpha.x);
    if (a <= 1.0 / 255.0) {
        discard;
    }
    // WBO 目标没有深度附件：与不透明场景深度手动作深度测试。
    float scene_z = ZView(texture(uSceneDepth, gl_FragCoord.xy / uViewport).r);
    float z = ZView(gl_FragCoord.z);
    if (z > scene_z + 0.02) {
        discard;
    }
    // 权重必须在 α 定稿之后算（否则 WBO 解析拿到的覆盖率与衰减不一致）。
    float w = WeightWbo(z, a, color);
    FragAccum = vec4(color * a, a) * w;
    FragReveal = a;
}
)glsl";

    // 初始化：编译 draw / composite 两个 program，建 VAO + 动态 VBO + 静态 IBO。
    void Init(ShaderManager* shader_mgr);
    void Finalize();

    // 帧首：确保 1× WBO 目标存在并清成恒等值，重置「本帧有没有内容」标志。
    // Pre-condition: Init 已调用。
    void Begin(int full_w, int full_h);

    // 把一批条带累加画进本渲染器的 1× WBO 目标（不 clear）。
    // Pre-condition: Init 已调用；batch 满足 ValidateStripBatch；
    //                scene_depth_tex != 0（不透明场景的 MRT#1 深度纹理）。
    void Draw(const TranslucentStripBatch& batch,
              TextureManager& texture_mgr,
              const Camera& camera,
              const float view_proj[16],
              unsigned int scene_depth_tex,
              int full_w, int full_h);

    // 把本帧 WBO 结果解析并 over 合成进**当前绑定的目标**（HDR）。无内容则 no-op。
    void Composite(int full_w, int full_h);

    bool has_content() const { return has_content_; }

private:
    // 1× 覆盖集、无加法集、无深度附件（NEAREST：1× 上插值没有意义）。
    void EnsureTarget(int w, int h);

    ShaderManager* shader_mgr_ = nullptr;
    unsigned int prog_draw_ = 0;
    unsigned int prog_comp_ = 0;
    unsigned int vao_ = 0;
    unsigned int vbo_ = 0;   // 每帧重传的条带顶点
    unsigned int ibo_ = 0;   // 静态索引：kMaxStripsPerDraw 个固定块
    WboTarget wbo_;
    bool initialized_ = false;
    bool has_content_ = false;
    std::vector<StripVertex> vertices_;   // 复用缓冲，避免每帧分配
};

}  // namespace jpov

#endif  // JPOV_SRC_STRIP_STRIP_RENDERER_H_
