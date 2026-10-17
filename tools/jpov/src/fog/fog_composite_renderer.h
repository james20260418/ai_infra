// JPOV FogCompositeRenderer —— froxel 雾 + 多层 WBO 透明纹理的**唯一**合成趟。
//
// 定位（Danis 2026-10-17 定）：分层按**趟**切，不按名词切。
//   · FogRenderer          ：inject / reduce / scatter —— 产出一张 froxel **场**（τ, S）；
//   · FogCompositeRenderer ：全屏一趟，把「雾场 + 0..N 个透明层」按深度交替积分。
// 两者的接缝是**数据**（FogField 纹理 + TransparentLayerSet），不是一次额外的 pass：
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
// N=1 逐项还原玻璃版旧式 `S_f + Tf·Cg + Tg·(S_op − S_f)`；N=0 退化为 `(S_op, T_op)`。
// 加性发射层（cov=0）也走同一公式：「被前方烟压暗」是 `W` 的自然结果，无需特判。
//
// ⚠️ 精确性前提（见 transparent_layer.h 契约）：各源沿视线**互不交叠**。
//    低分层的 depth 用 NEAREST 采样；深度感知上采样（联合双线性）待后续 PR。

#ifndef JPOV_SRC_FOG_FOG_COMPOSITE_RENDERER_H_
#define JPOV_SRC_FOG_FOG_COMPOSITE_RENDERER_H_

#include "tools/jpov/interface/camera.h"
#include "tools/jpov/src/fog/fog_renderer.h"
#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/transparent_layer.h"

namespace jpov {

class FogCompositeRenderer {
public:
    // 趟体：在 jpov::kFogCommonGlsl 之后拼接（公共前置提供 uNz/uBlockSide/uTilePx 与
    // SampleColumn/SampleFogBilinear）。
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
// 每槽的合成语义：0 = 遮挡（over），1 = 加性（只发射、不遮挡）。
uniform int       uLayerKind[4];
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

out vec4 oColor;   // (S.rgb, T)；配合 GL_ONE/GL_SRC_ALPHA：out = S + dst·T

// 解析第 i 个透明层 → 预乘色 c、层透射率 t、代表深度 z（视空间米）。
// 加性槽（uLayerKind=1）返回的是**发射和**而不是预乘平均色，且 t 恒为 1。
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
    z = depth_sum / accum.a;           // 代表深度（两种语义都用同一权重）
    if (uLayerKind[i] == 1) {
        // 加性：accum.rgb 就是 Σ(c·α) —— **不归一化、不乘覆盖**；不遮挡后方。
        c = accum.rgb;
        t = 1.0;
        return true;
    }
    float cov = clamp(1.0 - reveal, 0.0, 1.0);
    c = (accum.rgb / accum.a) * cov;   // 预乘：本层贡献的亮度
    t = clamp(reveal, 0.0, 1.0);
    return true;
}

// 雾累积 (τ, S)：无雾场时直接返回恒等（省掉整条 froxel 采样链）。
void FogAt(float ell, out float tau, out vec3 S) {
    if (uHasFog == 0) {
        tau = 0.0;
        S = vec3(0.0);
        return;
    }
    SampleFogBilinear(gl_FragCoord.xy, ell, tau, S);
}

void main() {
    vec2 uv = vTexCoord;
    vec2 ndc = uv * 2.0 - 1.0;
    vec3 ro = uCamPos;

    // 像素场景深度 → 沿视线距离 → froxel 连续位置 ℓ_op（不透明场景深度）。
    float dndc = texture(uSceneDepthTex, uv).r;
    vec4 pw = uInvVP * vec4(ndc, dndc * 2.0 - 1.0, 1.0);
    float z_op = length(pw.xyz / pw.w - ro);
    float ell_op = DepthToEll(z_op);

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

    // ── 雾段与层交替积分（front-to-back）──
    vec3  L = vec3(0.0);
    float W = 1.0;          // 已过的层透射率累积
    float tau_prev = 0.0;
    vec3  S_prev = vec3(0.0);
    for (int i = 0; i < n; ++i) {
        float zi = min(layer_z[i], z_op);   // 层深度不超过不透明表面
        float ell_i = DepthToEll(zi);
        float tau_i;
        vec3  S_i;
        FogAt(ell_i, tau_i, S_i);
        L += W * (S_i - S_prev);
        L += W * exp(-tau_i) * layer_c[i];
        W *= layer_t[i];
        tau_prev = tau_i;
        S_prev = S_i;
    }

    float tau_op;
    vec3  S_op;
    FogAt(ell_op, tau_op, S_op);
    L += W * (S_op - S_prev);
    float a = W * exp(-(tau_op - tau_prev));

    oColor = vec4(L, a);
}
)glsl";

    // 编译合成趟 program（归 ShaderManager 释放）。
    // Pre-condition: GL context 已激活；shader_mgr != nullptr。
    void Init(ShaderManager* shader_mgr);

    // 释放 program 与哑元纹理（可重复调用）。
    void Finalize();

    // 全屏合成：把雾场 + 透明层就地 over 进**当前绑定的** HDR FBO。
    //   field           : FogRenderer::Build 产出的 froxel 场（无效则零开销返回）。
    //   scene_depth_tex : MRT#1 场景深度（R32F，单采样）；0 = 不裁剪（按远平面处理）。
    //   layers          : 本帧透明层集合（可为空；空 = 全雾路径）。
    // Pre-conditions: Init 已调用；调用方已绑定目标 HDR FBO 且 viewport = 全屏；
    //                 scene_depth_tex 不是当前 draw FBO 的附件（否则采样↔写入反馈环）。
    // 说明：本趟自行保存/复原 blend func（属 GL_COLOR_BUFFER_BIT，glPushAttrib 兜不住）。
    void Draw(const FogField& field,
              unsigned int scene_depth_tex,
              const Camera& cam,
              const float view_proj[16],
              const FogParams& params,
              const TransparentLayerSet& layers);

private:
    ShaderManager* shader_mgr_ = nullptr;
    unsigned int prog_composite_ = 0;
    std::string fs_composite_;          // 组装后的 FS 源码（须存活到编译完成）
    unsigned int dummy_depth_tex_ = 0;  // 1×1 R32F（值 1.0）：无场景深度/空槽位时占位
};

}  // namespace jpov

#endif  // JPOV_SRC_FOG_FOG_COMPOSITE_RENDERER_H_
