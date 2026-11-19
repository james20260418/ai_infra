// JPOV FogCompositeRenderer —— froxel 雾 + 多层 WBO 透明纹理的**唯一**合成趟。
//
// 定位（Danis 2026-10-17 定）：分层按**趟**切，不按名词切。
//   · FogRenderer          ：inject / reduce / scatter —— 产出一张 froxel **场**（τ, S）；
//   · FogCompositeRenderer ：全屏一趟，把「雾场 + 0..N 个透明层」按深度交替积分。
// 两者的接缝是**数据**（FogField + TransparentLayerSet），不是一次额外的 pass：
// 合成本来就跑在调用方的 HDR FBO 上，拆类只是把「谁拥有合成逻辑」分清楚。
//
// 这就是「froxel 不认层是谁生产的」这个契约的实现：
//   输入 = (froxel 场, 不透明场景深度, 至多 4 个已解算透明层)
//   输出 = 就地 over 进 HDR：out.rgb + dst·out.a（GL_ONE / GL_SRC_ALPHA）
//
// ── 多段积分（N 层 ⇒ N+1 段雾）─────────────────────────────────────────────
// 设层按代表深度升序 z_1..z_N，T_i = 该层透射率、C_i = 该层预乘色；
// 雾累积 (τ(z), S(z)) 来自 froxel；z_op = 不透明场景深度。front-to-back：
//
//   L = 0; W = 1; S_prev = 0; τ_prev = 0
//   for i in 1..N:
//       L += W·(S(z_i) − S_prev)          // 雾段 (z_{i-1} → z_i)
//       L += W·exp(−τ(z_i))·C_i           // 层自身（被**前方雾**衰减）
//       W *= T_i                          // 层遮挡后方
//       S_prev = S(z_i); τ_prev = τ(z_i)
//   L += W·(S(z_op) − S_prev)             // 最后一段雾
//   a  = W·exp(−(τ(z_op) − τ_prev))       // 背景总透过率
//
// N=0 退化为 `(S_op, T_op)` —— 与「全雾路径」逐位相同（gold diff=0）；
// 加性发射层（cov=0）也走同一公式：「被前方烟压暗」是 `W` 的自然结果，无需特判。
//
// ⚠️ 精确性前提（见 transparent_layer.h 契约）：各源沿视线**互不交叠**。
//
// ── GL 状态机契约（谁保存 / 谁复原）────────────────────────────────────────────
// Draw() 自行保存并复原（不依赖调用方）：
//   · GL_BLEND_SRC/DST_RGB 与 GL_BLEND_SRC/DST_ALPHA（本趟按 rgb/alpha 分开保存与复原：
//     `GL_ONE/GL_SRC_ALPHA` 或逐通道透射的两次 draw；这些值属 GL_COLOR_BUFFER_BIT，
//     **不在** glPushAttrib(GL_ENABLE_BIT) 里）；
//   · GL_VIEWPORT（本趟按雾场构建尺寸设全屏）；
//   · 本趟用到的纹理单元（0..2+3·4）的 GL_TEXTURE_2D 绑定与活动单元。
// 不保存、由调用方 glPushAttrib(GL_ENABLE_BIT) 兜底：blend/depth/cull 的 enable 位、
// 当前 program（本趟结束置 0）、VAO。
// Draw() **不触碰**当前 FBO 的 draw buffer（由调用方设置）。

#ifndef JPOV_SRC_FOG_FOG_COMPOSITE_RENDERER_H_
#define JPOV_SRC_FOG_FOG_COMPOSITE_RENDERER_H_

#include <string>

#include "tools/jpov/interface/camera.h"
#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/src/fog/fog_renderer.h"
#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/transparent_layer.h"
#include "tools/jpov/src/water/water_medium_math.h"   // WaterMediumSample（POD，GL-free）

namespace jpov {

class FogCompositeRenderer {
public:
    // 趟体：在 jpov::kFogCommonGlsl 之后拼接（公共前置提供 uNz/uBlockSide/uTilePx
    // 与 vTexCoord）。
    static constexpr const char* kFsComposite = R"glsl(

uniform sampler2D uScatter;
uniform int       uHasFog;   // 0 = 本帧无雾场 → 跳过 froxel 采样（τ=0, S=0，恒等）
uniform sampler2D uSceneDepthTex;
uniform mat4  uInvVP;
uniform vec3  uCamPos;
uniform float uZNear;
uniform float uZFar;
uniform float uZScale;     // = uNz / ln(uZFar/uZNear)（CPU 预算）
uniform int   uGridCols;   // Nxy 单元列数
uniform int   uGridRows;   // Nxy 单元行数

out vec4 oColor;   // 语义随 uSpectralOutput：0=(S.rgb, T.r)、1=(T.rgb, ·)、2=(L.rgb, ·)

// 输出选择（C++ 侧按帧决定，见 fog_composite_renderer.cc 的 Draw）：
//   0 = 旧路径：标量 a（雾 gold 逐字节不变），混合 GL_ONE / GL_SRC_ALPHA；
//   1 = 只输出**逐通道透射率** T（alpha 写 1），混合 GL_ZERO / GL_SRC_COLOR ⇒ dst *= T；
//   2 = 只输出**注入亮度** L（alpha 写 0），混合 GL_ONE / GL_ONE       ⇒ dst += L。
//   ⚠️ alpha 的写法不是随意：混合因子对 RGB 与 alpha 同时生效，靠"输出 alpha 1/0"
//      才能让 dst 的 alpha 在两次 draw 后保持不变（Windows 侧没有 glBlendFuncSeparate）。
// 1+2 是「逐通道透射」的必需形式：固定管线的混合只有一个标量 alpha 或一个颜色因子，
// 无法在一次 draw 里同时做 `dst·T(逐通道) + L`。水体存在时由 C++ 发两次 draw。
uniform int uSpectralOutput;

// 读某 tile 的 scatter 列在「位置 ℓ（= Nz·ln(z/near)/ln(far/near)）」处的 (τ, S)。
// texel t 的自然位置 = t+1（覆盖 [z_near, z_{t+1}））；ℓ<1 与 identity 插值。
void SampleColumn(ivec2 tile_xy, float ell, out float tau, out vec3 S) {
    int t_hi = int(floor(ell));
    float fr = ell - float(t_hi);
    t_hi = clamp(t_hi, 0, uNz - 1);
    int t_lo = t_hi - 1;
    vec4 vhi = texelFetch(uScatter, ivec2(tile_xy.x + (t_hi % uBlockSide),
                                          tile_xy.y + (t_hi / uBlockSide)), 0);
    vec4 vlo = (t_lo >= 0)
        ? texelFetch(uScatter, ivec2(tile_xy.x + (t_lo % uBlockSide),
                                     tile_xy.y + (t_lo / uBlockSide)), 0)
        : vec4(0.0);   // identity：τ=0, S=0 ⇒ T=1, S=0
    tau = mix(vlo.x, vhi.x, fr);
    S   = mix(vlo.yzw, vhi.yzw, fr);
}

// 屏幕像素位置 px 处的雾累积 (τ, S)：4 邻 tile 双线性（按 tile 中心对齐）。
void SampleFogBilinear(vec2 px, float ell, out float tau, out vec3 S) {
    float cu = px.x / float(uTilePx) - 0.5;
    float cv = px.y / float(uTilePx) - 0.5;
    int tx0 = int(floor(cu));
    int ty0 = int(floor(cv));
    float fu = cu - float(tx0);
    float fv = cv - float(ty0);
    tau = 0.0;
    S = vec3(0.0);
    for (int dy = 0; dy < 2; ++dy) {
        int ty = clamp(ty0 + dy, 0, uGridRows - 1);
        float wy = (dy == 0) ? (1.0 - fv) : fv;
        for (int dx = 0; dx < 2; ++dx) {
            int tx = clamp(tx0 + dx, 0, uGridCols - 1);
            float wx = (dx == 0) ? (1.0 - fu) : fu;
            float w = wx * wy;
            float ctau;
            vec3  cS;
            SampleColumn(ivec2(tx * uBlockSide, ty * uBlockSide), ell, ctau, cS);
            tau += w * ctau;
            S   += w * cS;
        }
    }
}

// 视距 z（米）→ froxel 连续位置 ℓ（clamp 到雾区间）。
float DepthToEll(float z) {
    z = clamp(z, uZNear, uZFar);
    return uZScale * log(z / uZNear);
}

// ── 透明层（WBO 解算结果；uLayerCount 个有效槽位，其余不采）──
// ⚠️ GLSL 330 不允许**采样器数组**动态索引 ⇒ 4 槽显式展开 + if 链取用。
uniform int       uLayerCount;
uniform sampler2D uLayer0Accum;
uniform sampler2D uLayer0Reveal;
uniform sampler2D uLayer0Depth;
uniform sampler2D uLayer1Accum;
uniform sampler2D uLayer1Reveal;
uniform sampler2D uLayer1Depth;
uniform sampler2D uLayer2Accum;
uniform sampler2D uLayer2Reveal;
uniform sampler2D uLayer2Depth;
uniform sampler2D uLayer3Accum;
uniform sampler2D uLayer3Reveal;
uniform sampler2D uLayer3Depth;

// 深度 1.0 的语义 = 无穷远（= jpov::kDepthInfinityM），**不是 camera.far**。
// 相机 far 只负责裁剪远处的模型；雾/介质要的是「背景到底有多远」。
uniform float uDepthInfinityM;
// 水凝胶的"无穷远"射线参数：**真 +inf**（解析解在 len→∞ 时收敛）。
// 放在这里而不是介质片段里：没有介质时这个 uniform 也必须在，否则 main 会编译失败。
uniform float uWaterInfiniteZ;
// 水皮层在层集合里的下标（-1 = 没有 / 相机在水下）。
// 约定（2026-11-19 Danis 定）：**相机在水上时，水皮一定包住水体** ⇒
//   水皮就是水面 ⇒ 它之前的介质前缀恒为空（只有空气）。所以水体层不乘 exp(−tau_i)，
//   也不贡献 S_i；它之后的水柱整段由 W=层透射率 衰减。
uniform int uWaterLayerIndex;

// 解析第 i 个透明层 → 预乘色 c、层透射率 t、代表深度 z（视空间米）。
// 返回 false = 该像素上此层为空（accum.a≈0），不产生深度边界。
bool ResolveLayer(int i, vec2 uv, out vec3 c, out float t, out float z) {
    vec4 accum = vec4(0.0);
    float reveal = 1.0;
    float depth_sum = 0.0;
    if (i == 0) {
        accum = texture(uLayer0Accum, uv);
        reveal = texture(uLayer0Reveal, uv).r;
        depth_sum = texture(uLayer0Depth, uv).r;
    } else if (i == 1) {
        accum = texture(uLayer1Accum, uv);
        reveal = texture(uLayer1Reveal, uv).r;
        depth_sum = texture(uLayer1Depth, uv).r;
    } else if (i == 2) {
        accum = texture(uLayer2Accum, uv);
        reveal = texture(uLayer2Reveal, uv).r;
        depth_sum = texture(uLayer2Depth, uv).r;
    } else {
        accum = texture(uLayer3Accum, uv);
        reveal = texture(uLayer3Reveal, uv).r;
        depth_sum = texture(uLayer3Depth, uv).r;
    }
    if (accum.a <= 1e-8) {
        c = vec3(0.0);
        t = 1.0;
        z = 0.0;
        return false;
    }
    float cov = clamp(1.0 - reveal, 0.0, 1.0);
    c = (accum.rgb / accum.a) * cov;   // 预乘：本层贡献的亮度
    t = clamp(reveal, 0.0, 1.0);
    z = depth_sum / accum.a;           // 代表深度
    return true;
}

// 场的累积 (τ, S)：froxel 雾场 + 额外介质提供者 MediumAt（水体的解析项）。
// 两者都是「相机 → z」的前缀累积量，所以直接相加；froxel 的 τ 是标量（σ 灰），
// 广播到三个通道 —— 水体逐通道由 MediumAt 自己负责。
// z_fog 与 z_med 分开：雾是有限体积（到 z_op 为止），水凝胶是半空间解析解，
// 背景像素要积到**无穷远**才是"水体铺到地平线"。见 camera.h 的深度约定。
void FieldAt(vec3 ro, vec3 rd, float z_fog, float z_med, out vec3 tau, out vec3 S) {
    vec3 fog_tau = vec3(0.0);
    vec3 fog_S = vec3(0.0);
    if (uHasFog != 0) {
        float ct;
        vec3 cS;
        SampleFogBilinear(gl_FragCoord.xy, DepthToEll(z_fog), ct, cS);
        fog_tau = vec3(ct);
        fog_S = cS;
    }
    vec3 med_tau;
    vec3 med_S;
    MediumAt(ro, rd, z_med, med_tau, med_S);   // 无水时恒等（kDefaultMediumGlsl）
    tau = fog_tau + med_tau;
    S = fog_S + med_S;
}

void main() {
    vec2 uv = vTexCoord;
    vec2 ndc = uv * 2.0 - 1.0;
    vec3 ro = uCamPos;

    // 像素场景深度 → 沿视线距离 → froxel 连续位置 ℓ_op（不透明场景深度）。
    float dndc = texture(uSceneDepthTex, uv).r;
    vec4 pw = uInvVP * vec4(ndc, dndc * 2.0 - 1.0, 1.0);
    vec3 p_scene = pw.xyz / pw.w;
    float z_op = length(p_scene - ro);
    // 背景像素（深度 = 1.0）的语义是**无穷远**：
    //   雾积到 uDepthInfinityM（有限，够大）；水凝胶积到 +inf（解析解在 len→∞ 收敛）。
    //   两者都不会发散；camera.far 不参与这里（见 camera.h 的深度约定）。
    float z_med = z_op;
    if (dndc >= 0.999999) {
        z_op = uDepthInfinityM;      // 雾：有限但足够远
        z_med = uWaterInfiniteZ;     // 水体：真无穷（解析解收敛）
    }
    vec3  rd = normalize(p_scene - ro);   // 水体/介质提供者需要单位射线

    // ── 收集有效层 → 按代表深度升序（近→远）插入排序 ──
    vec3  layer_c[4];
    float layer_t[4];
    float layer_z[4];
    int n = 0;
    for (int i = 0; i < 4; ++i) {
        if (i >= uLayerCount) {
            break;
        }
        vec3 c;
        float t;
        float z;
        if (!ResolveLayer(i, uv, c, t, z)) {
            continue;
        }
        int j = n;
        while (j > 0 && layer_z[j - 1] > z) {
            layer_c[j] = layer_c[j - 1];
            layer_t[j] = layer_t[j - 1];
            layer_z[j] = layer_z[j - 1];
            --j;
        }
        layer_c[j] = c;
        layer_t[j] = t;
        layer_z[j] = z;
        ++n;
    }

    // ── 雾段与层交替积分（front-to-back，逐通道 τ/W）──
    //    逐通道是水体（σ 跨通道差 20+ 倍）的硬性要求；froxel 的标量雾在 FieldAt 里广播。
    vec3  L = vec3(0.0);
    vec3  W = vec3(1.0);          // 已过的层透射率累积（逐通道）
    vec3  tau_prev = vec3(0.0);
    vec3  S_prev = vec3(0.0);
    for (int i = 0; i < n; ++i) {
        bool is_water = (i == uWaterLayerIndex);
        // 水体层：前缀恒空（水皮 = 水面，其上方只有空气）。
        float zi = is_water ? 0.0 : min(layer_z[i], z_op);
        vec3  tau_i;
        vec3  S_i;
        FieldAt(ro, rd, zi, zi, tau_i, S_i);
        L += W * (S_i - S_prev);
        L += W * exp(-tau_i) * layer_c[i];   // 水体层 tau_i = 0 ⇒ 不衰减
        W *= vec3(layer_t[i]);
        tau_prev = tau_i;
        S_prev = S_i;
    }

    vec3 tau_op;
    vec3 S_op;
    FieldAt(ro, rd, z_op, z_med, tau_op, S_op);
    L += W * (S_op - S_prev);
    vec3 T = W * exp(-tau_op);   // 背景透射率（逐通道）：必须是**全路径**衰减

    if (uSpectralOutput == 0) {
        oColor = vec4(L, T.r);   // 旧路径：标量 a（无水时三通道相等 ⇒ 与改前逐字节相同）
    } else if (uSpectralOutput == 1) {
        oColor = vec4(T, 1.0);   // 乘 T（blend: ZERO / SRC_COLOR）
    } else {
        // 加 L（blend: ONE / ONE）。**alpha 必须写 0**：blend 对 alpha 也用同一组因子
        //（GL_ONE/GL_ONE）⇒ 写 1 会把 dst 的 alpha 累加成 2.0 之类的垃圾值。
        oColor = vec4(L, 0.0);
    }
}
)glsl";

    FogCompositeRenderer() = default;
    ~FogCompositeRenderer();
    FogCompositeRenderer(const FogCompositeRenderer&) = delete;
    FogCompositeRenderer& operator=(const FogCompositeRenderer&) = delete;

    // 编译合成趟 program + 建 1×1 深度哑元。要求 GL context 已激活。
    //
    // medium_glsl：可选的「介质提供者」GLSL 片段，必须定义
    //     void MediumAt(vec3 ro, vec3 rd, float z, out vec3 tau, out vec3 S)
    //   语义：z 处（沿视线的欧氏距离）的**前缀累积** (τ, S)。传 nullptr = 无额外介质
    //   （用 kDefaultMediumGlsl 恒等）。水体实现见 src/water/water_medium_shader.h。
    //   片段被拼在 kFogCommonGlsl 之后、kFsComposite 之前。
    // Pre-condition: shader_mgr != nullptr
    void Init(ShaderManager* shader_mgr, const char* medium_glsl = nullptr);

    // 释放 Init 申请的资源（与 Init 对称，可重复调用）。
    void Finalize();

    // 全屏合成：把雾场 + 透明层就地 over 进**当前绑定的** HDR FBO。
    //   field           : FogRenderer::Build 产出的 froxel 场（无效 ⇒ 只合成层）。
    //   scene_depth_tex : MRT#1 场景深度（R32F，单采样）；0 = 不裁剪（按远平面处理）。
    //   view_proj       : Proj*View（列主序 16 float）。
    //   params          : 取 z_near / z_far / nz / tile_px（须与构建场时同一份）。
    //   layers          : 本帧透明层集合（可为空；空 = 全雾路径）。
    //   medium          : 可选的全局均质介质（水体）。非空且 Init 时提供了 MediumAt 片段时：
    //                     ① 场 = froxel + 介质（前缀累加，逐通道）；
    //                     ② 输出改为**逐通道透射**：发两次 draw（dst *= T，再 dst += L），
    //                        因为固定管线没法在一次 draw 里同时做 `dst·T(rgb) + L`。
    // Pre-conditions: Init 已调用；调用方已绑定目标 HDR FBO；
    //                 scene_depth_tex 不是当前 draw FBO 的附件（否则采样↔写入反馈环）。
    void Draw(const FogField& field,
              unsigned int scene_depth_tex,
              const Camera& cam,
              const float view_proj[16],
              const FogParams& params,
              const TransparentLayerSet& layers,
              const water::WaterMediumSample* medium = nullptr,
              // 水皮层下标（-1 = 无 / 相机在水下）
              int water_layer_index = -1);

private:
    ShaderManager* shader_mgr_ = nullptr;
    unsigned int prog_composite_ = 0;
    std::string fs_composite_;          // 组装后的 FS 源码（须存活到编译完成）
    unsigned int dummy_depth_tex_ = 0;  // 1×1 R32F（值 1.0）：无场景深度/空槽位时占位
};

}  // namespace jpov

#endif  // JPOV_SRC_FOG_FOG_COMPOSITE_RENDERER_H_
