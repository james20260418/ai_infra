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
//   FS（逐片元）：
//     point = PointColor(u)                     // 逐点半线性采样 RGBA（rgb 可 HDR，a = 纵向不透明度）
//     color = point.rgb · texture.rgb           // 有纹理时
//     α     = point.a · texture.a · width_profile(v)
//     → 与不透明场景深度手动作深度测试（WBO 目标无深度缓冲）：
//         soft_depth_m = nullopt → 硬判据（比场景远 2cm 即丢弃）；
//         有值 → clamp((scene_z − z)/值, 0, 1) 线性淡出（条带与几何相交处无硬切边）。
//     → 两个集合（StripBlend，逐**批**）：
//         kCoverage: att0 = vec4(色·α, α)·w、att1 = α（吸收介质，参与遮挡）
//         kEmissive: att2 = 色·α（发光介质，与背景相加、不被遮挡衰减）
//        不用的附件写恒等值（同粒子 FS 的写法）。
//
//   ⭐ 逐点颜色 + 宽度剖面都在**同一张 per-instance 数据纹理**里（一行 = 一条带，
//      行宽 25 texel = 16 列颜色 + 9 个剖面 knot）：
//        · 两段都用硬件双线性 ⇒ 颜色列间 / 剖面 knot 间都是**精确分段线性**；
//        · 都在片元里按真实参数求值 ⇒ 4 段几何也能有平滑的颜色/宽度过渡；
//        · 不需要任何逐实例顶点属性（几何 5 float/顶点，平滑 pass 6 float/顶点）。
//      也没有 batch 级 tint / 逐条带 tint / 加法增益 —— 颜色与 α 只有**一个**来源：
//      `Strip16::point_color`（见 interface/strip_types.h 的数据分层说明）。
//
//   fresnel（M4，2026-10-22）：逐片元 Schlick 边反，参数见 interface/strip_types.h:StripFresnel。
//     法线来源**与 pass 绑定**（不可单独选）：
//       折线 pass ⇒ dFdx/dFdy（折面法线，逐三角形恒定）；
//       平滑 pass ⇒ dS × H（重建曲面的解析法线，无折面）。
//     「平滑几何 + 折面法线」这个组合不提供 —— 边反会变成与几何无关的亮带。
//     开关关闭时片段逐字节等于旧画面。
//
// ⭐ 两个独立 pass（2026-10-21 Danis 定；统一一个命令接口 StripInterpolation）：
//   kPolyline —— 本文件上半部分：顶点即几何，段间是弦，整批一次 draw call。
//   kSmooth   —— kSmoothVs / kSmoothFsMain：点即控制点，段间是 C1 Hermite，
//                在 FS 里按真实曲线反解参数并用 discard + fwidth 裁形。
//                控制点走 RGBA32F 数据纹理（每列 4 texel：c/m/h/n + 逐点 rgba），
//                顶点只多一个 strip id ⇒ **整批仍然一次 draw call**；
//                外扩量由 CPU 按局部曲率自动算。
//                两个 pass 的「深度测试 / 两个集合 / WBO 写入」是同一段代码（kFsEmit）。
//                每个 pass 有**两个 program**（天光关/开）：主函数共用，
//                只差 ApplySkyLight 的定义（见 kSkyLightOffFs / kSkyLightFs）。
//   两者写进**同一张 WBO 目标**（一个 pass），命令顺序 = 前后顺序，最后只 Composite 一次。
//
// 合成：Composite() 全屏解析 WBO（C = accum.rgb/accum.a、cov = 1−reveal）并 over
//   写进当前绑定的目标（HDR），随后由 horizon fog / froxel 继续。

#ifndef JPOV_SRC_STRIP_STRIP_RENDERER_H_
#define JPOV_SRC_STRIP_STRIP_RENDERER_H_

#include <vector>

#include "tools/jpov/interface/camera.h"
#include "tools/jpov/interface/render_command.h"   // DirectionalLight / ShadowConfig / CascadeFBO（CSM 输入）
#include "tools/jpov/interface/strip_types.h"
#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/strip/strip_geometry.h"
#include "tools/jpov/src/texture_manager.h"
#include "tools/jpov/src/wbo/wbo_target.h"

namespace jpov {

// CSM（级联阴影）输入：与 object3d / fire_fog **同一套资源**，由 Renderer 填。
//
// 为什么是「外部填」而不是渲染器自己找：阴影 pass 属于主链路（Renderer 持有
// shadow_fbos_ / 各级联矩阵），条带只是一个 effect pass，不该反过来持有它。
//
// 语义：只有 `std::optional<DirectionalLight> sun` 有值、且 fbos 非空时才**可能**有阴影；
// 具体某批要不要吃阴影由 `TranslucentStripBatch::sky_light` 决定（开关绑定在一起，
// 没有独立的阴影开关）。
struct StripCsmShadow {
    std::optional<DirectionalLight> sun;   // 无 ⇒ 太阳项不衰减（= 完全受照）
    ShadowConfig cfg{};                    // 级联范围 / 淡出 / 偏置
    std::vector<CascadeFBO> fbos;          // 长度 = cfg.cascade_count（仅 sun 时有效）
    float vp[ShadowConfig::kMaxCascades][16] = {};
    float depth_vp[ShadowConfig::kMaxCascades][16] = {};
    float texel_world[ShadowConfig::kMaxCascades] = {};

    // 可用 = 有太阳 + 有级联纹理。不可用时调用方传 nullptr 也等价。
    bool IsValid() const { return sun.has_value() && !fbos.empty(); }
};

class StripRenderer {
public:
    // **折线 pass** 单次 draw call 最多多少条带（受静态索引缓冲尺寸约束）。
    // 超出时**分成多次 draw**（不是静默截断 —— 旧 DrawStrip3D 的 3000 顶点截断是反面教材）。
    // 平滑 pass 不分块：它整批一次上传 + 一次 draw，上限是数据纹理行数（GL_MAX_TEXTURE_SIZE）。
    static constexpr int kMaxStripsPerDraw = 1024;

    // ── 着色器分段 ──
    //
    // 折线与平滑**只在「几何 → (u,v)」这一步不同**；之后的深度测试 / 两个集合 /
    // WBO 写入必须逐字一致（那段抄错一处就是「画面悄悄错、gold 未必红」）。
    // 所以拼装顺序固定为：
    //   kCommonFsHead + kWboWeightGlsl + kFsEmit + (kFsMain | kSmoothFsMain)
    // 谁的着色器都一样 —— 公共段只有一份，不可能漂移。

    // 顶点着色器（折线 pass）：位置/uv 已由 CPU 展开好，这里只做 MVP。
    // vWorld 只为 fresnel 服务（导数法线必须拿到**世界空间**位置）。
    //
    // vStripId 是**免费**的：索引缓冲按「块 j = 顶点基址 j×32」静态生成，
    // 于是 `gl_VertexID / 32` 就是条带下标（索引绘制里 gl_VertexID = 索引值本身）。
    // 32 = kStripVerticesPerStrip，GLSL 引用不到 C++ 常量，改一处必须同步另一处。
    static constexpr const char* kVs = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;        // (u = 沿长度, v = 横跨宽度)
uniform mat4 uViewProj;
out vec2 vUV;
out vec3 vWorld;
flat out float vStripId;
void main() {
    vUV = aUV;
    vWorld = aPos;
    vStripId = float(gl_VertexID / 32);
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)glsl";

    // 片元公共段：两个 pass 的 uniform / varying / 工具函数 / 输出声明。
    static constexpr const char* kCommonFsHead = R"glsl(
#version 330 core
in vec2 vUV;                      // (u 沿长度, v 横跨宽度)
in vec3 vWorld;                   // 世界坐标（fresnel 求法线/视线用）
flat in float vStripId;           // 本片元所属条带（= per-instance 数据的行号）
uniform sampler2D uTex;
// per-instance 数据纹理：一行 = 一条带，行宽 25 texel：
//   [0..15]  逐点 RGBA（u 方向 16 列，硬件双线性 ⇒ 列间平滑）
//   [16..24] 宽度剖面 9 个 knot（v = k/8，硬件双线性 ⇒ **精确分段线性**）
uniform sampler2D uAppearance;
uniform float uAppearanceRows;    // 该纹理**实际高度**（容量，不一定等于本批条带数）
uniform sampler2D uSceneDepth;
uniform vec2 uViewport;           // 本 pass 的像素尺寸（1× 时即 3D FBO 尺寸）
uniform float uNear;
uniform float uFar;
uniform int uHasTex;
uniform int uBlendPath;            // 0 = 覆盖集（吸收）；1 = 加法集（发光）
uniform int uSoftDepthEnabled;     // 1 = 软深度淡出；0 = 硬判据（2cm）
uniform float uSoftDepthM;         // 软深度过渡距离（米，仅 enabled 时有效）
uniform float uSoftDepthInv;       // = 1/uSoftDepthM：逐片元把除法换成乘法（uniform 除法
                                   //   编译器未必会提倒数的倒数，这里显式给）
uniform vec3 uCamPos;              // 世界空间相机位置（fresnel 的 V）
uniform int uFresnelEnabled;       // 1 = 开（否则这一段全部跳过）
uniform vec3 uFresnelRimColor;
uniform float uFresnelF0;
uniform float uFresnelPower;
uniform int uFresnelPow5;          // 1 = power 恰好是 5 ⇒ 走五次乘法特化（见 FresnelFactor）
uniform float uFresnelStrength;
uniform float uFresnelGrazeAlpha;
layout(location = 0) out vec4 FragAccum;    // 覆盖集：Σ(色·α·w, α·w)
layout(location = 1) out float FragReveal;  // 覆盖集：Π(1−α)
layout(location = 2) out vec4 FragAdd;      // 加法集：Σ(色·α)
// 窗口深度 [0,1] → 视空间深度（正，米）。
float ZView(float d) {
    float ndc = 2.0 * d - 1.0;
    return (2.0 * uNear * uFar) / ((uFar + uNear) - ndc * (uFar - uNear));
}
// 本片元所属条带的纹理行坐标（texel **中心** ⇒ 竖直方向权重恰好落在本行，
// 不会渗到相邻条带）。行号按纹理**实际高度**钳一次：texelFetch/取样越界是 UB。
float AppearanceRow() {
    return (clamp(vStripId, 0.0, uAppearanceRows - 1.0) + 0.5) /
           max(uAppearanceRows, 1.0);
}
// 逐点 RGBA：16 列 × 每列 1 texel，硬件双线性 ⇒ 列间平滑过渡。
// 采样坐标刻意落在 texel 正中心（u = col/15 → tx = (col+0.5)/25），
// 于是列上的颜色/alpha 是精确的、不掺邻列；两个 texel 之间才走插值。
// 25 / 15 分别对应 CPU 侧的 kAppearanceTexels / kStripQuads（GLSL 引用不到 C++ 常量）。
vec4 PointColor(float u) {
    float tx = (clamp(u, 0.0, 1.0) * 15.0 + 0.5) / 25.0;
    // textureLod(...,0)：这张纹理没有 mipmap，隐式导数只为算 LOD —— 纯浪费；
    // 而且平滑 pass 的 u* 在段边界被 clamp 过，导数本身是脏的。
    return textureLod(uAppearance, vec2(tx, AppearanceRow()), 0.0);
}
// 宽度方向剖面：9 knot / 8 段，在片元里求值（不是在顶点里插值）。
// 参数是**宽度方向**的 v（0 = right 边 / 1 = left 边）。
// 9 个 knot 等距 ⇒ 双线性采样得到的就是分段线性函数（knot 上是精确值）。
float WidthProfile(float w) {
    float tx = (16.0 + clamp(w, 0.0, 1.0) * 8.0 + 0.5) / 25.0;
    return textureLod(uAppearance, vec2(tx, AppearanceRow()), 0.0).r;
}
// 采样条带纹理并与 (color, α) 相乘（黑底素材天然透明：色×α 仍是 0）。
void ModulateTexture(vec2 uv, inout vec3 color, inout float a) {
    if (uHasTex == 1) {
        vec4 tex = texture(uTex, uv);
        color *= tex.rgb;
        a *= tex.a;
    }
}
// Schlick 菲涅耳：F = f0 + (1−f0)·(1−|N·V|)^power，再乘 strength 并 clamp。
// 用 |N·V| 而不是 max(N·V,0)：条带是**单层片**，两面都要有反光（法线朝向无意义）。
// N 长度退化（叉积≈ 0，比如投影退化/极细条带）时当作正对视线（只剩 f0），
// 不返回 NaN —— NaN 进 accum 会污染整帧后处理。
float FresnelFactor(vec3 N) {
    // V 用逆平方根手算：normalize() 会先 length() 再逐分量除；这里省一次 sqrt，
    // 同时给「相机与片元重合」√(0) 的情况一个确定性回退（NaN 进 accum 会污染整帧）。
    vec3 to_cam = uCamPos - vWorld;
    float d2 = dot(to_cam, to_cam);
    vec3 V = (d2 > 1e-12) ? to_cam * inversesqrt(d2) : vec3(0.0, 0.0, 1.0);
    float n2 = dot(N, N);
    // d 必须 clamp 到 [0,1]：浮点误差会让 |N·V| 冒到 1+eps，pow(负数, k) = NaN。
    float d = (n2 > 1e-12) ? clamp(abs(dot(N, V) * inversesqrt(n2)), 0.0, 1.0) : 1.0;
    float edge = 1.0 - d;
    // 通用 pow 在驱动里是 exp2(y·log2(x))：两个特殊函数 + 若干指令。
    // power == 5（Schlick 的默认指数，也是绝大多数取值）时用五次乘法替掉它。
    // 分支条件是 **uniform**（整批同一个值）⇒ 一个 draw 内一致，没有 divergence。
    // 两条路的浮点结果不逐位相同（pow 的实现精度本来就低于连乘），实测差异只到 LSB。
    float falloff;
    if (uFresnelPow5 == 1) {
        falloff = edge * edge * edge * edge * edge;
    } else {
        falloff = pow(edge, uFresnelPower);
    }
    float f = uFresnelF0 + (1.0 - uFresnelF0) * falloff;
    return clamp(f * uFresnelStrength, 0.0, 1.0);
}
// 把菲涅耳作用到（尚未定稿的）(color, a) 上：
//   颜色 → 掠射处趋近 rim_color（反射色）
//   覆盖度 → 掠射处趋近 graze_alpha（边变实，中心仍然看得穿）
//
// ⚠️ 刻意**不乘任何 α 乘子**（逐点 alpha / 纹理 alpha / 剖面 alpha 都不乘）：
//   反射是独立旋钮 —— 薄膜 a=0.12 也该有实心亮边（这正是 fresnel 存在的理由）。
//   2026-10-22 重构把「不透明度」并进了逐点颜色的 alpha 通道，两者不再可分：
//   若乘上去，0.12 的玻璃壳亮边也只剩 0.12（等于没有边反），所以这里选择不乘。
//   代价（已知限制）：开了 fresnel 的条带不要在长度方向做淡出，否则淡出段的边反会保留。
// 调用位置固定：ModulateTexture 之后、WidthProfile / 裁形 AA 之前（它们是最后一层）。
void ApplyFresnel(vec3 N, inout vec3 color, inout float a) {
    if (uFresnelEnabled == 0) {
        return;
    }
    float f = FresnelFactor(N);
    color = mix(color, uFresnelRimColor, f);
    a = mix(a, uFresnelGrazeAlpha, f);
}
)glsl";

    // 片元公共段（第二段）：深度测试 → 两个集合 → 写 WBO 附件。
    // 入参必须是**最终**的 color 与覆盖率 a（平滑 pass 已把裁形 AA 乘进 a）。
    // 必须拼在 kWboWeightGlsl（WeightWbo）之后。
    static constexpr const char* kFsEmit = R"glsl(
void EmitFragment(vec3 color, float a) {
    // 早退门槛：放在读场景深度**之前**（每个片元最便宜的一道筛子），
    // 但判据必须按「**注入量**」而不是覆盖率 —— 两个集合注入的东西不同：
    //   覆盖集：注入量 = 色·(α·fade)，fade ≤ 1 ⇒ α ≤ 1/255 的片元下面怎么都会被丢，
    //           所以这里直接用 α 判（与旧行为逐位相同，且不必先算 fade）。
    //   加法集：颜色可以是 HDR（> 1），这时「α 极小但色极亮」的片元注入量可以 ≫ 1/255，
    //           只按 α 判会把发光拖尾/射流的淡边整条丢掉（实测射流尾部少 100~290 px、
    //           最大 9/255）。这里乘上颜色，fade 留到下面按精确 contrib 再判一次。
    if (uBlendPath == 1) {
        if (max(max(color.r, color.g), color.b) * a <= 1.0 / 255.0) {
            discard;
        }
    } else if (a <= 1.0 / 255.0) {
        discard;
    }
    // WBO 目标没有深度附件：与不透明场景深度手动作深度测试。
    float scene_z = ZView(texture(uSceneDepth, gl_FragCoord.xy / uViewport).r);
    float z = ZView(gl_FragCoord.z);
    float fade = 1.0;
    if (uSoftDepthEnabled == 1) {
        // 软深度：0 < (scene_z − z) < 过渡距离 内线性淡出，相交处不再是硬切边。
        // 衰减必须发生在写目标**之前**，否则覆盖率高估（1−Π(1−α) 与画面不一致）。
        fade = clamp((scene_z - z) * uSoftDepthInv, 0.0, 1.0);
    } else if (z > scene_z + 0.02) {
        discard;
    }
    if (uBlendPath == 1) {
        // 加法集：发光介质直接与背景相加；黑底纹理 → 贡献 0 → 天然透明。
        // 亮度完全由 color（逐点 HDR × 纹理）与 α 决定 —— 没有额外增益旋钮。
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

    // 折线 pass 的 main：uv 直接来自顶点（段间是弦）。
    // 法线只能来自屏幕空间导数（= 所在三角形平面法线，逐折面恒定 ——
    // 16 列折线上会看到一圈圈亮带，要光滑请切 kSmooth）。
    static constexpr const char* kFsMain = R"glsl(
void main() {
    vec4 point = PointColor(vUV.x);
    vec3 color = point.rgb;
    float a = point.a;
    ModulateTexture(vUV, color, a);
    // 法线：折线 pass 只能拿屏幕空间导数（= 所在三角形平面法线，逐折面恒定）。
    // 天光与 fresnel 共用同一个 N；两者都关时它是死代码，编译器会丢掉。
    vec3 N = cross(dFdx(vWorld), dFdy(vWorld));
    ApplySkyLight(N, color);
    ApplyFresnel(N, color, a);
    a *= WidthProfile(vUV.y);
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
layout(location = 1) in vec2 aUV;        // (u, v)；v 已折入外扩（见 CPU 侧）
layout(location = 2) in float aStripId;  // 本批第几条带 = 数据纹理行号
uniform mat4 uViewProj;
out vec2 vUV;
out vec3 vWorld;
flat out float vStripId;
void main() {
    vUV = aUV;
    vWorld = aPos;
    vStripId = aStripId;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)glsl";

    static constexpr const char* kSmoothFsMain = R"glsl(
uniform float uSegCount;          // 段数 = 采样点数 − 1（m 变化不改变本 FS 成本）
uniform float uDataRows;          // 数据纹理行数（CPU 侧保证够用；这里再钳一次防越界）
// 控制点数据纹理：每列 4 texel = 16 float，按
//   (c.xyz, m.x)(m.yz, h.xy)(h.z, n.xyz)(rgba) 打包；
// 每条带占一行，行号 = vStripId。texelFetch 越界是**未定义行为**（不是返回 0）⇒
// 行号在这里按 uDataRows 钳一次（列号由 seg 的 clamp 保证）。
uniform sampler2D uStripData;
void main() {
    // u 沿长度、v 横跨宽度；两者都是**真实参数**（CPU 已把保守外扩折进属性 v）。
    float u = vUV.x;
    float v = vUV.y;

    float segF = clamp(u, 0.0, 1.0) * uSegCount;
    int seg = int(clamp(floor(segF), 0.0, uSegCount - 1.0));
    float t = segF - float(seg);

    // 两列 × 4 texel = 8 次 texelFetch（同一条带的数据在同一行，命中同一 cache line）。
    // 这里的 4 = CPU 侧 kSmoothTexelsPerColumn（shader 里没法引用 C++ 常量，改要同步）。
    // texelFetch 越界是**未定义行为**（不是"返回 0"）：行号必须钳住。
    int row = int(clamp(vStripId, 0.0, uDataRows - 1.0));
    int tx = seg * 4;
    vec4 t0 = texelFetch(uStripData, ivec2(tx + 0, row), 0);
    vec4 t1 = texelFetch(uStripData, ivec2(tx + 1, row), 0);
    vec4 t2 = texelFetch(uStripData, ivec2(tx + 2, row), 0);
    vec4 t3 = texelFetch(uStripData, ivec2(tx + 3, row), 0);
    vec4 t4 = texelFetch(uStripData, ivec2(tx + 4, row), 0);
    vec4 t5 = texelFetch(uStripData, ivec2(tx + 5, row), 0);
    vec4 t6 = texelFetch(uStripData, ivec2(tx + 6, row), 0);
    vec4 t7 = texelFetch(uStripData, ivec2(tx + 7, row), 0);

    // 打包约定（与 CPU 侧 AppendSmoothStripData 逐字一致）：(c.xyz, m.x)(m.yz, h.xy)(h.z, n.xyz)(rgba)
    vec3 c0 = vec3(t0.x, t0.y, t0.z);
    vec3 m0 = vec3(t0.w, t1.x, t1.y);
    vec3 h0 = vec3(t1.z, t1.w, t2.x);
    vec3 n0 = vec3(t2.y, t2.z, t2.w);
    vec4 color0 = t3;   // 第 seg 列的逐点 rgba
    vec3 c1 = vec3(t4.x, t4.y, t4.z);
    vec3 m1 = vec3(t4.w, t5.x, t5.y);
    vec3 h1 = vec3(t5.z, t5.w, t6.x);
    vec3 n1 = vec3(t6.y, t6.z, t6.w);
    vec4 color1 = t7;   // 第 seg+1 列的逐点 rgba

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
    // 段内真实参数：u* 与逐点 RGBA 必须用**同一个** t*（否则颜色会顺着重建偏差漂）。
    float tStar = clamp(t + dt, 0.0, 1.0);
    float uStar = (float(seg) + tStar) / uSegCount;
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
    // 逐点 RGBA 来自**同一行**的控制点数据（每列最后一个 texel），按段内 t* 线性插值：
    // 与旧的「去 per-instance 纹理采样」数学等价（那张纹理列间也是硬件双线性）——
    // 换掉的是**一次跨纹理的过滤采样**（少一次 bilinear + LOD），代价是多 2 次同行 texelFetch。
    vec4 point = mix(color0, color1, tStar);
    vec3 color = point.rgb;
    float a = point.a;
    ModulateTexture(vec2(clamp(uStar, 0.0, 1.0), vShade), color, a);
    // 解析法线：曲面 P(t,w) = S(t) + w·H(t) ⇒ N = ∂P/∂t × ∂P/∂w = dS × H。
    // 这是**无折面**的真实法线（折线 pass 只能拿导数法线，会随 15 段出亮带），
    // 也是平滑 pass 的附带好处：同一份数据已经在手上了，不用额外顶点属性。
    // 天光与 fresnel 共用这个 N（两者都关时它是死代码）。
    vec3 N = cross(dS, Hh);
    ApplySkyLight(N, color);
    ApplyFresnel(N, color, a);
    a *= WidthProfile(vShade) * cov;
    EmitFragment(color, a);
}
)glsl";

    // ── 天光消费（sky_light）：**两套定义**，主函数只有一份 ──
    //
    // 需求：开启光照消费**不影响原本 pass 的效率**，用开关控制。
    // 做法是**两个 program**（折线/平滑各一对），主函数共用，差异只有下面这个函数：
    //   kSkyLightOffFs → 空实现 ⇒ 编译器把调用点整个消掉，指令数与改动前完全一致；
    //   kSkyLightFs    → 真正的天光注入。
    // 不用 uniform 分支的原因：那会在**每条带每个片元**上都多一次分支判断 + 常驻寄存器，
    // 而这套东西只对少数批（玻璃/薄膜）有意义；用 program 切分 = 不开的人真的零成本。
    //
    // 公式（与雾的合成同构，见 interface/strip_types.h:StripSkyLight）：
    //   skylight(N) = tricolor_lerp(N.y)·ambient_scale + sun_color·sun_intensity·|N·L|
    //   color      += injection · skylight(N)
    // 注入量随后会被 α 乘（EmitFragment），所以越不透明补得越多 —— 与雾的 L_fog·α 同一件事。
    static constexpr const char* kSkyLightOffFs = R"glsl(
// 不开天光：空实现。GLSL 编译器会把调用点内联成空 ⇒ 零额外指令。
void ApplySkyLight(vec3 N, inout vec3 color) {
}
)glsl";

    static constexpr const char* kSkyLightFs = R"glsl(
uniform vec3 uSkyZenith;      // 天顶色（法线朝上）
uniform vec3 uSkyHorizon;     // 天际线色（法线水平）
uniform vec3 uSkyGround;      // 地反射色（法线朝下）
uniform float uSkyAmbientScale;
uniform vec3 uSunDir;         // **指向太阳**的单位向量（CPU 侧已归一）
uniform vec3 uSunColor;
uniform float uSunIntensity;
uniform float uSkyInjection;

// 阴影开关：**与天光同一个开关** —— 有 sky_light 就采样 CSM 调制太阳项，没有独立开关。
// 天光项（ambient）不乘阴影：环境光不该被直射光的影子遮住。
// 条带自身仍不参与阴影生成（不写 shadow map，那需要给半透明片单开一次 shadow pass）。
// 采样是**单级联单次采样、无 PCF**（实现见 csm_shadow_glsl.h，与 fire_fog 共用）。
uniform int uHasCsm;   // 0 = 不做阴影（无太阳 / 无级联）

// 天光辐亮度（线性 HDR）。N 可以非单位（导数叉积/曲面叉积都有长度）：
// 先归一拿方向；长度≈ 0（退化片元）时当作「朝上」，不出 NaN。
vec3 SkyLightRadiance(vec3 N) {
    float n2 = dot(N, N);
    vec3 n = (n2 > 1e-12) ? N * inversesqrt(n2) : vec3(0.0, 1.0, 0.0);
    // N.y 分段线性：上→天顶、水平→天际线、下→地（与 AmbientLight::tricolor 同一约定，
    // 天际线是中继）。下半球有地色兜底，不会死黑。
    vec3 ambient = (n.y >= 0.0) ? mix(uSkyHorizon, uSkyZenith, n.y)
                                : mix(uSkyHorizon, uSkyGround, -n.y);
    vec3 sky = ambient * uSkyAmbientScale;
    // 太阳项只在真的给了强度时才算：否则每个片元白付一次 shadow map 采样 + 两次 mat4×vec4
    // （纯天光 / 入夜就是这条路径）。条件全是 uniform ⇒ 全屏一致，不会有 warp 分歧。
    // 条带是**单层片**，两面都受光 ⇒ 取 |N·L|（与 fresnel 用 |N·V| 同一理由）。
    if (uSunIntensity > 0.0) {
        float shadow = CsmShadow(vWorld, length(uCamPos - vWorld), uHasCsm);
        sky += uSunColor * (abs(dot(n, uSunDir)) * uSunIntensity * shadow);
    }
    return sky;
}

void ApplySkyLight(vec3 N, inout vec3 color) {
    color += uSkyInjection * SkyLightRadiance(N);
}
)glsl";

    // 初始化：编译 4 个 drawing program（折线/平滑 × 天光关/开）+ 合成 program，
    // 建 VAO + 动态 VBO + 静态 IBO。
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
    // csm：CSM 资源（框见上）。**只有 sky_light 的批会真的采样**；没有太阳/级联时传
    //      nullptr（不是"省略"—— 这个参数故意必填，避免「以为传了其实没传」）。
    void Draw(const TranslucentStripBatch& batch,
              TextureManager& texture_mgr,
              const Camera& camera,
              const float view_proj[16],
              unsigned int scene_depth_tex,
              const StripCsmShadow* csm,
              int full_w, int full_h);

    // 把本帧 WBO 结果解析并 over 合成进**当前绑定的目标**（HDR）。无内容则 no-op。
    void Composite(int full_w, int full_h);

    bool has_content() const { return has_content_; }

private:
    // per-instance 数据纹理的行宽（texel）：16 列逐点 RGBA + 9 个剖面 knot。
    // ⚠️ 必须与 kCommonFsHead 里 PointColor / WidthProfile 的 `15.0`/`25.0`/`16.0`
    //    字面量同步（shader 引用不到 C++ 常量）。
    static constexpr int kAppearanceTexels = kStripSampleCount + kStripProfileKnots;

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
        int tex = -1;
        int has_tex = -1;
        int appearance = -1;
        int appearance_rows = -1;
        int scene_depth = -1;
        int viewport = -1;
        int near_plane = -1;
        int far_plane = -1;
        int blend_path = -1;
        int soft_enabled = -1;
        int soft_m = -1;
        int soft_inv = -1;
        int cam_pos = -1;
        int fresnel_enabled = -1;
        int fresnel_rim_color = -1;
        int fresnel_f0 = -1;
        int fresnel_power = -1;
        int fresnel_pow5 = -1;
        int fresnel_strength = -1;
        int fresnel_graze_alpha = -1;
        // 天光（只有 lit program 有这些 uniform；unlit program 里 GetUniform 返回 -1，
        // glUniform 对 -1 是 no-op，所以不分支也不会出错）。
        int sky_zenith = -1;
        int sky_horizon = -1;
        int sky_ground = -1;
        int sky_ambient_scale = -1;
        int sun_dir = -1;
        int sun_color = -1;
        int sun_intensity = -1;
        int sky_injection = -1;
        // CSM（同样只在 lit program 里存在；unlit 全是 -1 ⇒ glUniform no-op）。
        // 数组型 uniform 的**逐元素**位置在 Init 查好（数组 uniform 的首元素位置不保证
        // 连续可用，object3d 也是逐元素查 + 逐元素 glUniform1i）。
        int has_csm = -1;
        int cascade_count = -1;
        int cascade_ranges = -1;              // float[5]：按基名上传整数组
        int shadow_map[5] = {-1, -1, -1, -1, -1};
        int shadow_vp[5] = {-1, -1, -1, -1, -1};
        int shadow_depth_vp[5] = {-1, -1, -1, -1, -1};
        int shadow_texel_world = -1;          // float[5]
        int shadow_bias_cascade = -1;         // float[5]
        int shadow_bias_override = -1;
        int shadow_fade_start = -1;
        int shadow_fade_end = -1;
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

    // 一次 Draw 的公共上下文：把批级参数挤进结构体，两个 drawer 就不用各带 7 个位置参数
    // （同类型参数一多，抄改时最容易错位）。
    struct BatchDrawContext {
        const TranslucentStripBatch* batch = nullptr;
        unsigned int gl_tex = 0;            // 0 = 纯色批（不采样纹理）
        bool want_additive = false;         // 本批走加法集
        bool want_light = false;            // 本批消费天光（选 lit program）
        const StripCsmShadow* csm = nullptr; // CSM 资源（want_light 时才可能用到）
        const Camera* camera = nullptr;
        const float* view_proj = nullptr;   // column-major 16 个 float
        unsigned int scene_depth_tex = 0;   // 不透明场景深度（值 = gl_FragCoord.z）
        int full_w = 0;
        int full_h = 0;
    };

    // 公共 uniform 的**名字表**（唯一一份）：查一次位置，Draw 里直接用整数。
    void QueryCommonUniforms(unsigned int program, CommonUniforms* out /*output*/);

    // 平滑 pass 特有 uniform（段数 / 数据行数 / 控制点数据纹理）：折线与 lit 变体各查一次，
    // 但**名字表只有这一份**（以前在 Init 里抄了两遍三段）。
    void QuerySmoothUniforms(unsigned int program, SmoothUniforms* out /*output*/);

    // 公共 uniform 的**上传**（唯一实现：改这里两个 pass 同时生效）。
    void ApplyCommonUniforms(const CommonUniforms& uniforms, const BatchDrawContext& ctx);

    ShaderManager* shader_mgr_ = nullptr;
    unsigned int prog_draw_ = 0;
    unsigned int prog_comp_ = 0;
    // 「开天光」的两个变体：不在原 program 里加 uniform 分支，
    // 而是单独编译 —— 不开天光的批在 GPU 上零额外指令（见 kSkyLightOffFs）。
    unsigned int prog_draw_lit_ = 0;
    unsigned int prog_smooth_lit_ = 0;
    CommonUniforms draw_uniforms_;
    CommonUniforms draw_lit_uniforms_;
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
    SmoothUniforms smooth_lit_uniforms_;
    unsigned int smooth_vao_ = 0;
    unsigned int smooth_vbo_ = 0;
    // per-instance 数据纹理（两个 pass 共用）：宽 kAppearanceTexels，高 = 条带数。
    unsigned int appearance_tex_ = 0;
    int appearance_capacity_ = 0;         // 当前纹理高度（条带数上限）
    std::vector<float> appearance_data_;  // 复用缓冲：每行 kAppearanceTexels × 4 float
    unsigned int smooth_data_tex_ = 0;    // RGBA32F：每列 4 texel，每条带一行
    int smooth_data_rows_ = 0;            // 当前纹理高度（按需翻倍增长）
    int max_texture_rows_ = 0;            // GL_MAX_TEXTURE_SIZE（Init 时缓存）
    std::vector<float> smooth_vertices_;  // kSmoothFloatsPerVertex/顶点：pos(3),u,v,strip_id
    std::vector<float> smooth_data_;      // kSmoothFloatsPerColumn/列：c, m, h, n, rgba

    // 索引缓冲按需扩容（块 j = 第 j 条带，顶点基址 j×32）。幂等：够用就直接返回。
    // 为什么平滑 pass 必须能一次画完整批：它的数据纹理行号 = 全局 strip 下标，
    // 若分块就要么重传顶点、要么写一张正在被采样的纹理（两者都是坑）。
    void EnsureIboCapacity(int strips);

    // 逐条带的颜色/剖面 gather 到复用缓冲，并上传为纹理（整批一次）。
    // 两个 pass 都必须在 ApplyCommonUniforms 之前调用（它负责绑定纹理）。
    void UploadAppearance(const std::vector<Strip16>& strips);

    // 两个 drawer：同一张 WBO 目标，各自的 program/VAO/数据通道。
    void DrawPolyline(const BatchDrawContext& ctx);
    void DrawSmooth(const BatchDrawContext& ctx);
};

}  // namespace jpov

#endif  // JPOV_SRC_STRIP_STRIP_RENDERER_H_
