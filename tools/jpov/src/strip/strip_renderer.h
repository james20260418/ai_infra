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
//     uv：**u = 沿长度 s（col/15，图片从左到右）**、**v = 横跨宽度（0=right / 1=left）**；
//     四个角对应素材的左下 right0 / 左上 left0 / 右下 right15 / 右上 left15。
//
//   FS：
//     α = tint.a · vertex_alpha · texture.a · width_profile(v)   （v 是宽度方向）
//     → 与不透明场景深度手动作深度测试（WBO 目标无深度缓冲）：
//         soft_depth_m = nullopt → 硬判据（比场景远 2cm 即丢弃）；
//         有值 → clamp((scene_z − z)/值, 0, 1) 线性淡出（条带与几何相交处无硬切边）。
//     → 两个集合（StripBlend，逐**批**）：
//         kCoverage: att0 = vec4(色·α, α)·w、att1 = α（吸收介质，参与遮挡）
//         kEmissive: att2 = 色·α（发光介质，与背景相加、不被遮挡衰减）
//        不用的附件写恒等值（同粒子 FS 的写法）。
//     剖面在**片元**里逐像素求值 ⇒ 4 段几何也能有平滑的宽度方向衰减。
//
//   fresnel（M4）尚未实现 —— 不预留空字段（设了不生效比没有更糟）。
//
// ⭐ 两个独立 pass（2026-10-21 Danis 定；统一一个命令接口 StripStyle）：
//   kPolyline —— 本文件上半部分：顶点即几何，段间是弦，整批一次 draw call。
//   kSmooth   —— kSmoothVs / kSmoothFsMain：点即控制点，段间是 C1 Hermite，
//                在 FS 里按真实曲线反解参数并用 discard + fwidth 裁形。
//                控制点走 RGBA32F 数据纹理（每列 3 texel），顶点只多一个 strip id
//                ⇒ **整批仍然一次 draw call**；外扩量由 CPU 按局部曲率自动算。
//                两个 pass 的「深度测试 / 两个集合 / WBO 写入」是同一段代码（kFsEmit）。
//   两者写进**同一张 WBO 目标**（一个 pass），命令顺序 = 前后顺序，最后只 Composite 一次。
//
// 合成：Composite() 全屏解析 WBO（C = accum.rgb/accum.a、cov = 1−reveal）并 over
//   写进当前绑定的目标（HDR），随后由 horizon fog / froxel 继续。

#ifndef JPOV_SRC_STRIP_STRIP_RENDERER_H_
#define JPOV_SRC_STRIP_STRIP_RENDERER_H_

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
    // **折线 pass** 单次 draw call 最多多少条带（受静态索引缓冲尺寸约束）。
    // 超出时**分成多次 draw**（不是静默截断 —— 旧 DrawStrip3D 的 3000 顶点截断是反面教材）。
    // 平滑 pass 不分块：它整批一次上传 + 一次 draw，上限是数据纹理行数（GL_MAX_TEXTURE_SIZE）。
    static constexpr int kMaxStripsPerDraw = 1024;

    // ── 着色器分段 ──
    //
    // 折线与平滑**只在「几何 → (u,v,α)」这一步不同**；之后的深度测试 / 两个集合 /
    // WBO 写入必须逐字一致（那段抄错一处就是「画面悄悄错、gold 未必红」）。
    // 所以拼装顺序固定为：
    //   kCommonFsHead + kWboWeightGlsl + kFsEmit + (kFsMain | kSmoothFsMain)
    // 谁的着色器都一样 —— 公共段只有一份，不可能漂移。

    // 顶点着色器（折线 pass）：位置/uv 已由 CPU 展开好，这里只做 MVP。
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

    // 片元公共段：两个 pass 的 uniform / varying / 工具函数 / 输出声明。
    static constexpr const char* kCommonFsHead = R"glsl(
#version 330 core
in vec3 vUVAlpha;                 // (u, v, alpha)
uniform sampler2D uTex;
uniform int uHasTex;
uniform vec4 uTint;
uniform float uProfile[9];        // 宽度方向（v）= k/8 处的 alpha 乘子
uniform sampler2D uSceneDepth;
uniform vec2 uViewport;           // 本 pass 的像素尺寸（1× 时即 3D FBO 尺寸）
uniform float uNear;
uniform float uFar;
uniform int uBlendPath;            // 0 = 覆盖集（吸收）；1 = 加法集（发光）
uniform int uSoftDepthEnabled;     // 1 = 软深度淡出；0 = 硬判据（2cm）
uniform float uSoftDepthM;         // 软深度过渡距离（米，仅 enabled 时有效）
layout(location = 0) out vec4 FragAccum;    // 覆盖集：Σ(色·α·w, α·w)
layout(location = 1) out float FragReveal;  // 覆盖集：Π(1−α)
layout(location = 2) out vec4 FragAdd;      // 加法集：Σ(色·α)
// 窗口深度 [0,1] → 视空间深度（正，米）。
float ZView(float d) {
    float ndc = 2.0 * d - 1.0;
    return (2.0 * uNear * uFar) / ((uFar + uNear) - ndc * (uFar - uNear));
}
// 宽度方向剖面：9 knot / 8 段分段线性，在片元里求值（不是在顶点里插值）。
// 参数是**宽度方向**的 v（0 = right 边 / 1 = left 边）。
float WidthProfile(float w) {
    float x = clamp(w, 0.0, 1.0) * 8.0;
    int i = int(min(floor(x), 7.0));
    return mix(uProfile[i], uProfile[i + 1], x - float(i));
}
// 采样条带纹理并与 (color, α) 相乘（黑底素材天然透明：色×α 仍是 0）。
void ModulateTexture(vec2 uv, inout vec3 color, inout float a) {
    if (uHasTex == 1) {
        vec4 tex = texture(uTex, uv);
        color *= tex.rgb;
        a *= tex.a;
    }
}
)glsl";

    // 片元公共段（第二段）：深度测试 → 两个集合 → 写 WBO 附件。
    // 入参必须是**最终**的 color 与覆盖率 a（平滑 pass 已把裁形 AA 乘进 a）。
    // 必须拼在 kWboWeightGlsl（WeightWbo）之后。
    static constexpr const char* kFsEmit = R"glsl(
void EmitFragment(vec3 color, float a) {
    if (a <= 1.0 / 255.0) {
        discard;
    }
    // WBO 目标没有深度附件：与不透明场景深度手动作深度测试。
    float scene_z = ZView(texture(uSceneDepth, gl_FragCoord.xy / uViewport).r);
    float z = ZView(gl_FragCoord.z);
    float fade = 1.0;
    if (uSoftDepthEnabled == 1) {
        // 软深度：0 < (scene_z − z) < 过渡距离 内线性淡出，相交处不再是硬切边。
        // 衰减必须发生在写目标**之前**，否则覆盖率高估（1−Π(1−α) 与画面不一致）。
        fade = clamp((scene_z - z) / uSoftDepthM, 0.0, 1.0);
    } else if (z > scene_z + 0.02) {
        discard;
    }
    if (uBlendPath == 1) {
        // 加法集：发光介质直接与背景相加；黑底纹理 → 贡献 0 → 天然透明。
        vec3 contrib = color * a * fade;
        if (max(max(contrib.r, contrib.g), contrib.b) <= 1.0 / 255.0) {
            discard;
        }
        FragAdd = vec4(contrib, 0.0);
        FragAccum = vec4(0.0);
        FragReveal = 0.0;
    } else {
        float aa = a * fade;
        if (aa <= 1.0 / 255.0) {
            discard;
        }
        // 权重必须在 α 定稿之后算（否则 WBO 解析拿到的覆盖率与衰减不一致）。
        float w = WeightWbo(z, aa, color);
        FragAccum = vec4(color * aa, aa) * w;
        FragReveal = aa;
        FragAdd = vec4(0.0);
    }
}
)glsl";

    // 折线 pass 的 main：uv/α 直接来自顶点（段间是弦）。
    static constexpr const char* kFsMain = R"glsl(
void main() {
    vec3 color = uTint.rgb;
    float a = uTint.a * vUVAlpha.z;
    ModulateTexture(vUVAlpha.xy, color, a);
    a *= WidthProfile(vUVAlpha.y);
    EmitFragment(color, a);
}
)glsl";

    // ── 平滑 pass（kSmooth）：C1 Hermite 重建 + 裁形 ──
    //
    // 契约：left/right 是插值型控制点，曲线**严格过点**；第 i 列宽度 = |left[i]−right[i]|。
    //
    // 每个片元只处理**一个段**（它自己所在的那段）：读该段两列的控制数据（6 texel），
    // 求值一次三次 Hermite，得到真实中心线 S(t) 与半宽矢量 H(t)，然后把片元的线性参数
    // (u,v) 一阶反解到真实曲面上 → 纹理/剖面跟着曲线走；v* 出界则 discard（裁形），
    // 用 fwidth 做 1px 解析 AA。**指令数与采样点数 m 无关（O(1)）**。
    static constexpr const char* kSmoothVs = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aUVAlpha;   // (u, v, alpha)；v 已折入外扩（见 CPU 侧）
layout(location = 2) in float aStripId;  // 本批第几条带 = 数据纹理行号
uniform mat4 uViewProj;
out vec3 vUVAlpha;
out vec3 vWorld;
flat out float vStripId;
void main() {
    vUVAlpha = aUVAlpha;
    vWorld = aPos;
    vStripId = aStripId;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)glsl";

    static constexpr const char* kSmoothFsMain = R"glsl(
in vec3 vWorld;                   // 线性几何上的片元世界坐标（尚未重建）
flat in float vStripId;           // 所属条带 = 数据纹理行号
uniform float uSegCount;          // 段数 = 采样点数 − 1（m 变化不改变本 FS 成本）
uniform float uDataRows;          // 数据纹理行数（CPU 侧保证够用；这里再钳一次防越界）
// 控制点数据纹理：每列 3 texel = 12 float，按 (c.xyz, m.x)(m.yz, h.xy)(h.z, n.xyz) 打包；
// 每条带占一行，行号 = vStripId。texelFetch 越界是**未定义行为**（不是返回 0）⇒
// 行号在这里按 uDataRows 钳一次（列号由 seg 的 clamp 保证）。
uniform sampler2D uStripData;
void main() {
    // u 沿长度、v 横跨宽度；两者都是**真实参数**（CPU 已把保守外扩折进属性 v）。
    float u = vUVAlpha.x;
    float v = vUVAlpha.y;

    float segF = clamp(u, 0.0, 1.0) * uSegCount;
    int seg = int(clamp(floor(segF), 0.0, uSegCount - 1.0));
    float t = segF - float(seg);

    // 两列 × 3 texel = 6 次 texelFetch（同一条带的数据在同一行，命中同一 cache line）。
    // 这里的 3 = CPU 侧 kSmoothTexelsPerColumn（shader 里没法引用 C++ 常量，改要同步）。
    // texelFetch 越界是**未定义行为**（不是"返回 0"）：行号必须钳住。
    int row = int(clamp(vStripId, 0.0, uDataRows - 1.0));
    int tx = seg * 3;
    vec4 t0 = texelFetch(uStripData, ivec2(tx + 0, row), 0);
    vec4 t1 = texelFetch(uStripData, ivec2(tx + 1, row), 0);
    vec4 t2 = texelFetch(uStripData, ivec2(tx + 2, row), 0);
    vec4 t3 = texelFetch(uStripData, ivec2(tx + 3, row), 0);
    vec4 t4 = texelFetch(uStripData, ivec2(tx + 4, row), 0);
    vec4 t5 = texelFetch(uStripData, ivec2(tx + 5, row), 0);

    // 打包约定（与 CPU 侧 AppendSmoothStripData 逐字一致）：(c.xyz, m.x)(m.yz, h.xy)(h.z, n.xyz)
    vec3 c0 = vec3(t0.x, t0.y, t0.z);
    vec3 m0 = vec3(t0.w, t1.x, t1.y);
    vec3 h0 = vec3(t1.z, t1.w, t2.x);
    vec3 n0 = vec3(t2.y, t2.z, t2.w);
    vec3 c1 = vec3(t3.x, t3.y, t3.z);
    vec3 m1 = vec3(t3.w, t4.x, t4.y);
    vec3 h1 = vec3(t4.z, t4.w, t5.x);
    vec3 n1 = vec3(t5.y, t5.z, t5.w);

    float t2f = t * t;
    float t3f = t2f * t;
    float h00 = 2.0 * t3f - 3.0 * t2f + 1.0;
    float h10 = t3f - 2.0 * t2f + t;
    float h01 = -2.0 * t3f + 3.0 * t2f;
    float h11 = t3f - t2f;
    float g00 = 6.0 * t2f - 6.0 * t;
    float g10 = 3.0 * t2f - 4.0 * t + 1.0;
    float g01 = -6.0 * t2f + 6.0 * t;
    float g11 = 3.0 * t2f - 2.0 * t;

    vec3 S = h00 * c0 + h10 * m0 + h01 * c1 + h11 * m1;   // 真实中心线
    vec3 dS = g00 * c0 + g10 * m0 + g01 * c1 + g11 * m1;  // dS/dt
    vec3 Hh = h00 * h0 + h10 * n0 + h01 * h1 + h11 * n1;  // 真实半宽矢量
    vec3 P = S + (2.0 * v - 1.0) * Hh;                    // 真实曲面点

    // 一阶反解：Linear(u,v) = P + D ⇒ (t*,v*) ≈ (t,v) + J⁻¹D（局部正交近似）。
    vec3 D = vWorld - P;
    float dt = dot(D, dS) / max(dot(dS, dS), 1e-8);
    vec3 Pv = 2.0 * Hh;
    float dv = dot(D, Pv) / max(dot(Pv, Pv), 1e-8);
    float uStar = (float(seg) + clamp(t + dt, 0.0, 1.0)) / uSegCount;
    float vStar = v + dv;

    // 轮廓：落在真实条带之外的片元裁掉；fwidth 做 1px 解析 AA（不做硬切边的锯齿）。
    // ⚠️ 覆盖度是「到最近边的距离」，**不能两个 clamp 相乘**：那是两个边各自 0..1 的
    //    乘积，条带整宽 < 2px 时中段会被平方衰减（1px 宽只剩 0.25），掠射角提前淡出。
    float fw = max(fwidth(vStar), 1e-5);
    float cov = clamp(0.5 + min(vStar, 1.0 - vStar) / fw, 0.0, 1.0);
    if (cov <= 1.0 / 255.0) {
        discard;
    }

    float vShade = clamp(vStar, 0.0, 1.0);
    vec3 color = uTint.rgb;
    float a = uTint.a * vUVAlpha.z;
    ModulateTexture(vec2(clamp(uStar, 0.0, 1.0), vShade), color, a);
    a *= WidthProfile(vShade) * cov;
    EmitFragment(color, a);
}
)glsl";

    // 初始化：编译 draw / composite 两个 program，建 VAO + 动态 VBO + 静态 IBO。
    void Init(ShaderManager* shader_mgr);
    void Finalize();

    // 帧首：确保 1× WBO 目标存在并清成恒等值，重置「本帧有没有内容」标志。
    //
    // with_additive：本帧是否有 kEmissive 批。附件**按需申请**（不要的附件不建），
    // 所以这个值必须由调用方在 Begin 时就知道（它决定 FBO 的附件数量）。
    // Pre-condition: Init 已调用；with_additive 必须覆盖本帧所有 Draw 的需求
    //                （有 kEmissive 批就必须 true；只画覆盖批时 true 也无害）。
    void Begin(int full_w, int full_h, bool with_additive);

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
    // 1× 覆盖集（可选加法集）、无深度附件（NEAREST：1× 上插值没有意义）。
    void EnsureTarget(int w, int h, bool with_additive);

    // uniform 位置**只查一次**（Init 里），Draw 里直接用整数。
    // 为什么不每帧调 ShaderManager::GetUniform：它对每次调用都要线性扫一遍
    // programs_ 再做一次 std::string 哈希（见 shader_manager.cc:148）；
    // 本 pass 每批 15 个 uniform，1000 批 = 15000 次字符串哈希/帧，纯浪费。
    //
    // ⚠️ 前提：program 创建后不会重建（ShaderManager 按 name 缓存，无重编译路径）。
    //    若将来加 shader 热重载，必须在这里失效并重查，否则会写到旧 program 上。
    //
    // 下面是**两个 pass 共用**的逐批 uniform：字段只定义一次、名字只在
    // QueryCommonUniforms 里出现一次、上传只在 ApplyCommonUniforms 里出现一次
    //（以前 DrawSmooth 把这段连同 soft_depth 分支逐字复制过去，加字段必然会漂移）。
    struct CommonUniforms {
        int view_proj = -1;
        int tint = -1;
        int has_tex = -1;
        int tex = -1;
        int profile = -1;
        int scene_depth = -1;
        int viewport = -1;
        int near_plane = -1;
        int far_plane = -1;
        int blend_path = -1;
        int soft_enabled = -1;
        int soft_m = -1;
    };
    struct CompositeUniforms {
        int accum = -1;
        int reveal = -1;
        int add = -1;
        int additive = -1;
    };

    // 平滑 pass = 公共项 + 三个专用（段数 / 数据纹理行数 / 数据纹理采样器）。
    struct SmoothUniforms {
        CommonUniforms common;
        int seg_count = -1;
        int data_rows = -1;
        int strip_data = -1;
    };

    // 一次 Draw 的公共上下文：把 8 项挤进结构体，两个 drawer 就不用各带 7 个位置参数
    // （同类型参数一多，抄改时最容易错位）。
    struct BatchDrawContext {
        const TranslucentStripBatch* batch = nullptr;
        unsigned int gl_tex = 0;            // 0 = 纯色批（不采样纹理）
        bool want_additive = false;         // 本批走加法集
        const Camera* camera = nullptr;
        const float* view_proj = nullptr;   // column-major 16 个 float
        unsigned int scene_depth_tex = 0;   // 不透明场景深度（值 = gl_FragCoord.z）
        int full_w = 0;
        int full_h = 0;
    };

    // 公共 uniform 的**名字表**（唯一一份）：查一次位置，Draw 里直接用整数。
    void QueryCommonUniforms(unsigned int program, CommonUniforms* out /*output*/);

    // 公共 uniform 的**上传**（唯一实现：改这里两个 pass 同时生效）。
    void ApplyCommonUniforms(const CommonUniforms& uniforms, const BatchDrawContext& ctx);

    ShaderManager* shader_mgr_ = nullptr;
    unsigned int prog_draw_ = 0;
    unsigned int prog_comp_ = 0;
    CommonUniforms draw_uniforms_;
    CompositeUniforms comp_uniforms_;
    unsigned int vao_ = 0;
    unsigned int vbo_ = 0;   // 每帧重传的条带顶点
    unsigned int ibo_ = 0;   // 索引：块 j = 第 j 条带（基址 j×32），容量按需增长
    int ibo_capacity_ = 0;   // 当前 IBO 能覆盖多少条带（块数）
    WboTarget wbo_;
    bool initialized_ = false;
    bool has_content_ = false;
    bool target_with_additive_ = false;   // 当前 WBO 目标是否带加法集附件
    bool has_alpha_ = false;              // 本帧画过覆盖集
    bool has_additive_ = false;           // 本帧画过加法集
    std::vector<StripVertex> vertices_;   // 复用缓冲，避免每帧分配

    // ── 平滑 pass 的资源（kSmooth）──
    unsigned int prog_smooth_ = 0;
    SmoothUniforms smooth_uniforms_;
    unsigned int smooth_vao_ = 0;
    unsigned int smooth_vbo_ = 0;
    unsigned int smooth_data_tex_ = 0;    // RGBA32F：每列 3 texel，每条带一行
    int smooth_data_rows_ = 0;            // 当前纹理高度（按需翻倍增长）
    int smooth_max_rows_ = 0;             // GL_MAX_TEXTURE_SIZE（Init 时缓存）
    std::vector<float> smooth_vertices_;  // kSmoothFloatsPerVertex/顶点：pos(3),u,v,alpha,strip_id
    std::vector<float> smooth_data_;      // kSmoothFloatsPerColumn/列：c, m, h, n

    // 索引缓冲按需扩容（块 j = 第 j 条带，顶点基址 j×32）。幂等：够用就直接返回。
    // 为什么平滑 pass 必须能一次画完整批：它的数据纹理行号 = 全局 strip 下标，
    // 若分块就要么重传顶点、要么写一张正在被采样的纹理（两者都是坑）。
    void EnsureIboCapacity(int strips);

    // 两个 drawer：同一张 WBO 目标，各自的 program/VAO/数据通道。
    void DrawPolyline(const BatchDrawContext& ctx);
    void DrawSmooth(const BatchDrawContext& ctx);
};

}  // namespace jpov

#endif  // JPOV_SRC_STRIP_STRIP_RENDERER_H_
