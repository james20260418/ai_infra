// JPOV SSR（屏幕空间反射）—— 全部 GLSL 源码
//
// 四个 pass，按执行顺序：
//   ① attr   ：把「反射体」的表面属性（视空间法线 / 粗糙度 / F0）画进属性缓冲
//   ② hiz    ：把场景深度逐级 2×2 取 min，建 4 级金字塔（外加 level0 = 场景深度本身）
//   ③ march  ：逐像素发射反射射线（Hi-Z 粗走 + 下探 + 二分细化），输出反射色（预乘权重）
//   ④ composite：把反射**加法**叠回 HDR 画面（不采样目标 ⇒ 无反馈环）
//
// 全屏三角形的 UV 一律用 gl_FragCoord / uResolution 反算，**不用 varying**：
// 仓库里踩过（见 sky_renderer.h 顶部注释）—— 无 VAO 的全屏三角形做 varying 插值时
// llvmpipe 上不可靠（vNDC 几乎不变）。
//
// 数学基准（与 Primitives3DRenderer::BuildPerspProj 同一套约定）：
//   * 视图空间向前是 -Z；「距离」d = -z_view > 0；
//   * z_ndc = (f+n)/(f-n) - 2fn/((f-n)·d)          （d=n → -1，d=f → +1）
//   * d     = 2fn / ((f+n) - z_ndc·(f-n))
//   * 屏幕空间里 1/d 沿直线**线性的**（透视正确插值），这是粗步进能精确估深度的关键。

#ifndef JPOV_SRC_SSR_SSR_SHADER_H_
#define JPOV_SRC_SSR_SSR_SHADER_H_

namespace jpov {

// ---- 全屏三角形（三个 gl_VertexID 覆盖整个 NDC；无 VAO/VBO）----
inline constexpr const char* kSsrFullscreenVs = R"glsl(
#version 330 core
void main() {
    vec2 pos;
    if (gl_VertexID == 0) { pos = vec2(-1.0, -1.0); }
    else if (gl_VertexID == 1) { pos = vec2(3.0, -1.0); }
    else { pos = vec2(-1.0, 3.0); }
    gl_Position = vec4(pos, 0.0, 1.0);
}
)glsl";

// ---- ① 表面属性 pass ----
// 与主 PBR pass **共用同一份顶点着色器**（Object3DRenderer::kMeshVs3dPBR /
// kMeshVs3dPBRFull）⇒ 顶点变换、风位移、法线/切线完全一致，不会错位。
//
// 输出两张附件（RGBA16F）：
//   0: (Nv.x, Nv.y, roughness, metallic)  —— 视空间法线只存 xy，z 按「可见面朝相机」
//      重建为 -sqrt(1-x²-y²)。可见表面的视空间法线必然 z<0，故不丢信息。
//   1: (F0.rgb, 1)                        —— 菲涅耳的基准反射率。
//      为什么不塞进附件 0：四个通道已被法线 xy / 粗糙度 / 金属度占满，
//      而金属的 F0 = base_color（彩色），压成一个标量会让彩色金属反射变灰。
inline constexpr const char* kSsrAttrFs = R"glsl(
#version 330 core
in vec3 vWorldPos;
in vec3 vWorldNormal;
in vec2 vTexCoord;
in vec3 vWorldTangent;

layout(location = 0) out vec4 FragNormalRough;
layout(location = 1) out vec4 FragF0;

uniform mat4 uView;
uniform vec3  uBaseColor;
uniform float uMetallic;
uniform float uRoughness;
uniform int   uHasNormalTex;
uniform sampler2D uNormalTex;
uniform float uNormalScale;

void main() {
    vec3 N = normalize(vWorldNormal);
    if (uHasNormalTex == 1) {
        vec3 tex_normal = texture(uNormalTex, vTexCoord).rgb * 2.0 - 1.0;
        tex_normal = normalize(tex_normal);
        tex_normal.xy *= uNormalScale;
        tex_normal = normalize(tex_normal);
        vec3 T = normalize(vWorldTangent - dot(vWorldTangent, N) * N);
        vec3 B = normalize(cross(N, T));
        N = normalize(mat3(T, B, N) * tex_normal);
    }
    // 与主 pass 同一约定：双面几何的背面法线翻转。
    if (!gl_FrontFacing) {
        N = -N;
    }
    // 世界 → 视图（view 矩阵不含缩放，取 3×3 旋转部分即可）
    vec3 Nv = normalize(mat3(uView) * N);
    // 可见表面的视空间法线**指向相机**。相机在原点、朝 -Z 看 ⇒ 表面的位置 z<0，
    // 法线指回原点 ⇒ Nv.z **> 0**。（这里最容易搞反：不是 <0。）
    // 反了说明这块几何的法线朝里，翻过来，保证下游能按 z>0 重建。
    if (Nv.z < 0.0) {
        Nv = -Nv;
    }
    vec3 F0 = mix(vec3(0.04), uBaseColor, uMetallic);
    FragNormalRough = vec4(Nv.xy, clamp(uRoughness, 0.0, 1.0), uMetallic);
    FragF0 = vec4(F0, 1.0);
}
)glsl";

// ---- ② Hi-Z 单级降采样（2×2 取 min）----
// 用 texelFetch 精确对齐 2×2 而非线性采样：min 运算必须作用在**恰好四个**源纹素上，
// 否则金字塔的保守性会被插值破坏。
// 存的是 **NDC 深度**（= gl_FragCoord.z）。标准 Z 映射是单调的 ⇒ min(ndc) 等价于
// min(线性距离)，下游线性化后比较即可，不必在建塔时做除法。
inline constexpr const char* kSsrHiZMinFs = R"glsl(
#version 330 core
layout(location = 0) out vec4 FragColor;

uniform sampler2D uSrc;
uniform vec2  uDstResolution;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy) * 2;
    float a = texelFetch(uSrc, p + ivec2(0, 0), 0).r;
    float b = texelFetch(uSrc, p + ivec2(1, 0), 0).r;
    float c = texelFetch(uSrc, p + ivec2(0, 1), 0).r;
    float d = texelFetch(uSrc, p + ivec2(1, 1), 0).r;
    FragColor = vec4(min(min(a, b), min(c, d)), 0.0, 0.0, 1.0);
}
)glsl";

// ---- ③ 射线行进 ----
// 流程（逐像素）：
//   读属性 → 不是反射体/太粗糙 → 直接写 0（这一步让"只给几个物体开 SSR"几乎免费）
//   读深度 → 重建视空间起点 P 与法线 N → R = reflect(normalize(P), N)
//   把 R 的终点投影回屏幕、裁到 [0,1]²，得到一条屏幕空间线段
//   沿线粗走（层高 2^level 像素）：光线在格子最小深度**前面** ⇒ 整格跳过；
//     在格子后面 ⇒ 下探一层重走这一格；到 level 0 且厚度合格 ⇒ 命中
//   命中后二分细化 → 取颜色；没命中 ⇒ 用大气色天空兜底
//
// 权重（写进 alpha）依次乘：粗糙度淡出 × 距离淡出 × 屏幕边缘淡出。
inline constexpr const char* kSsrMarchFs = R"glsl(
#version 330 core
layout(location = 0) out vec4 FragColor;   // rgb = 反射色（已乘权重）, a = 权重

uniform sampler2D uSceneDepth;   // 0 场景深度（gl_FragCoord.z，R32F，NEAREST）
uniform sampler2D uSceneColor;   // 1 HDR 颜色（不透明 3D 已画完）
uniform samplerCube uEnvCube;    // 2 天空 cubemap（探针：未命中 / 粗糙面的兜底）
uniform int   uHasProbe;         //   0 = 无探针（无天空），用 uProbeFallback 常量
uniform vec3  uProbeFallback;
uniform float uEnvMaxLod;        //   探针 mip 上限（粗糙度 → 模糊）
uniform mat4  uView;             //   视图矩阵：视空间反射向量 → 世界（转置乘）
uniform sampler2D uAttr0;        // 3 (Nv.xy, roughness, metallic)
uniform sampler2D uAttr1;        // 4 (F0.rgb, 1)
uniform sampler2D uHiZ0;         // 5 全分辨率深度（= uSceneDepth）
uniform sampler2D uHiZ1;         // 6
uniform sampler2D uHiZ2;         // 7
uniform sampler2D uHiZ3;         // 8
uniform sampler2D uHiZ4;         // 9

uniform mat4  uProj;
uniform vec2  uResolution;       // 本 pass（反射缓冲）分辨率
uniform float uNear;
uniform float uFar;
uniform float uTanHalfFovY;
uniform float uAspect;
uniform int   uStartLevel;       // 0 = 关闭 Hi-Z
uniform int   uMaxSteps;
uniform int   uRefineSteps;
uniform float uThicknessM;
uniform float uMaxDistanceM;
uniform float uRoughnessCutoff;
uniform int   uFadeEdges;

float DistFromNdc(float ndc) {
    return (2.0 * uNear * uFar) / ((uFar + uNear) - ndc * (uFar - uNear));
}

float NdcFromDist(float d) {
    return ((uFar + uNear) / (uFar - uNear)) - (2.0 * uNear * uFar / (uFar - uNear)) / d;
}

// 由屏幕 uv + 视距反重建视空间坐标（标准透视，无需矩阵求逆）。
vec3 ViewPos(vec2 uv, float dist) {
    float y = (uv.y * 2.0 - 1.0) * dist * uTanHalfFovY;
    float x = (uv.x * 2.0 - 1.0) * dist * uTanHalfFovY * uAspect;
    return vec3(x, y, -dist);
}

// 按层取 Hi-Z 的最小 NDC 深度。层数固定 5（0..4），用 if 链而不是动态索引：
// GLSL 330 不允许用非常量表达式索引 sampler 数组。
float SampleHiZ(vec2 uv, int level) {
    if (level <= 0) { return texture(uHiZ0, uv).r; }
    if (level == 1) { return texture(uHiZ1, uv).r; }
    if (level == 2) { return texture(uHiZ2, uv).r; }
    if (level == 3) { return texture(uHiZ3, uv).r; }
    return texture(uHiZ4, uv).r;
}

void main() {
    vec2 uv0 = gl_FragCoord.xy / uResolution;

    vec4 attr0 = texture(uAttr0, uv0);
    vec4 attr1 = texture(uAttr1, uv0);
    // 掩码：属性 pass 只画反射体，没画过的像素是清屏色（a=0）。
    if (attr1.a <= 0.0) {
        FragColor = vec4(0.0);
        return;
    }
    float roughness = attr0.z;

    float ndc0 = texture(uSceneDepth, uv0).r;
    if (ndc0 >= 0.99999) {          // 背景没有可反射的表面
        FragColor = vec4(0.0);
        return;
    }
    float dStart = DistFromNdc(ndc0);
    vec3 P = ViewPos(uv0, dStart);
    // 视空间法线：只存 xy，z 按「可见表面指向相机」重建为 **+sqrt**（z>0）。
    float nz = sqrt(max(1.0 - dot(attr0.xy, attr0.xy), 0.0));
    vec3 N = normalize(vec3(attr0.xy, nz));
    // 起点沿法线抬起一点点：掠射角下（尤其大平面）射线会和"自己这块面"的深度
    // 反复相切，产生一条条横向条纹。抬离表面是标准做法。
    P += N * (dStart * 1.0e-3);
    vec3 I = normalize(P);          // 眼 → 表面
    vec3 R = reflect(I, N);         // 反射方向（离开表面）

    vec3 F0 = attr1.rgb;
    float cosTheta = clamp(dot(N, -I), 0.0, 1.0);
    vec3 fresnel = F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);

    // ---- 探针（环境反射）：**永远有**，是 SSR 的兜底，也是粗糙面的近似 ----
    // cubemap 用世界方向索引 ⇒ 视空间反射向量要转回世界（视图旋转正交，转置即逆）。
    // mip 级按粗糙度取：越粗糙越模糊 —— 这就是"粗糙度的模糊"最便宜的做法。
    vec3 Rw = transpose(mat3(uView)) * R;
    vec3 probe = (uHasProbe != 0)
        ? textureLod(uEnvCube, Rw, clamp(roughness, 0.0, 1.0) * uEnvMaxLod).rgb
        : uProbeFallback;

    // 数值异常 / 太粗糙 ⇒ 不打射线，整块给探针。
    //   粗糙面的真实反射是一片糊，单条射线代表不了；探针的模糊 mip 反而更接近真值。
    //   这也是"SSR 打不中就退探针"这条回退链的入口。
    if (dot(R, N) <= 0.0 || roughness > uRoughnessCutoff) {
        FragColor = vec4(probe * fresnel, 1.0);
        return;
    }

    // 射线长度裁到**近平面前**：朝相机后方时屏幕里没有画面可借（见设计文档 §4.3）。
    //   注意不要在这里按 R.z>0 直接跳兜底 —— 那会在平面中间拉出一条硬分界线。
    float rayLen = uMaxDistanceM;
    if (R.z > 0.0) {
        rayLen = clamp((-uNear - P.z) / R.z, 0.0, uMaxDistanceM);
    }
    vec4 clipEnd = uProj * vec4(P + R * rayLen, 1.0);
    if (clipEnd.w <= 0.0) {
        FragColor = vec4(probe * fresnel, 1.0);
        return;
    }

    // 终点 → 屏幕线段，并裁到 [0,1]²
    vec2 uvEnd = (clipEnd.xy / clipEnd.w) * 0.5 + 0.5;
    vec2 d2 = uvEnd - uv0;
    float tMax = 1.0;
    if (d2.x > 1.0e-6) { tMax = min(tMax, (1.0 - uv0.x) / d2.x); }
    if (d2.x < -1.0e-6) { tMax = min(tMax, (0.0 - uv0.x) / d2.x); }
    if (d2.y > 1.0e-6) { tMax = min(tMax, (1.0 - uv0.y) / d2.y); }
    if (d2.y < -1.0e-6) { tMax = min(tMax, (0.0 - uv0.y) / d2.y); }
    tMax = clamp(tMax, 0.0, 1.0);

    vec2 p0 = uv0 * uResolution;
    vec2 p1 = (uv0 + d2 * tMax) * uResolution;
    vec2 seg = p1 - p0;
    float segLen = length(seg);
    if (segLen < 1.0) {             // 出屏太快：没有可信的屏幕数据
        FragColor = vec4(probe * fresnel, 1.0);
        return;
    }
    vec2 dir = seg / segLen;

    // 1/d 沿线段线性 ⇒ 只需两端距离（透视正确，零误差）
    float dEnd = max(-(P + R * (rayLen * tMax)).z, 1.0e-2);

    bool hit = false;
    float sCur = 0.0;
    float sHit = 0.0;
    int level = clamp(uStartLevel, 0, 4);
    int iter = 0;
    while (sCur < segLen && iter < uMaxSteps) {
        float stepPx = exp2(float(level));
        float sNext = sCur + stepPx;
        float t = min(sNext / segLen, 1.0);
        vec2 uvp = (p0 + dir * sNext) / uResolution;
        float dRay = 1.0 / mix(1.0 / dStart, 1.0 / dEnd, t);
        float ndcRay = NdcFromDist(dRay);
        float ndcCell = SampleHiZ(uvp, level);
        if (ndcRay > ndcCell) {
            if (level > 0) {
                level -= 1;         // 这一格可能相交 → 下探（sCur 不动，重走同一区域）
                continue;
            }
            // level 0：单像素精度，可以判真伪。
            // 厚度随距离放大：掠射角下光线与表面几乎平行，交叉点附近的深度差
            // 天然就大，固定厚度会把它们全判成"薄片"而漏掉（表现为条纹/空洞）。
            float dScene = DistFromNdc(ndcCell);
            if (dRay - dScene <= uThicknessM * (1.0 + dScene * 0.1)) {
                hit = true;
                sHit = sNext;
                break;
            }
            // 厚度不合格 = 穿过薄片，继续走
        }
        sCur = sNext;
        ++iter;
    }

    // ---- 命中 → SSR；没命中 → 探针。按置信度混，别硬切 ----
    float ssrConf = 0.0;
    vec2 hitUv = uv0;
    if (hit) {
        float lo = max(sHit - 1.0, 0.0);
        float hi = sHit;
        for (int r = 0; r < uRefineSteps; ++r) {
            float mid = 0.5 * (lo + hi);
            float t = min(mid / segLen, 1.0);
            vec2 uvp = (p0 + dir * mid) / uResolution;
            float dRay = 1.0 / mix(1.0 / dStart, 1.0 / dEnd, t);
            float ndcRay = NdcFromDist(dRay);
            float ndcScene = texture(uHiZ0, uvp).r;
            if (ndcRay > ndcScene) { hi = mid; }
            else { lo = mid; }
        }
        hitUv = (p0 + dir * hi) / uResolution;

        // 置信度：屏幕边缘不可信；粗糙度接近上限时也不可信（该交给探针）。
        float edge = min(min(hitUv.x, 1.0 - hitUv.x), min(hitUv.y, 1.0 - hitUv.y));
        float edgeFade = (uFadeEdges != 0) ? smoothstep(0.0, 0.10, edge) : 1.0;
        float roughFade = 1.0 - smoothstep(uRoughnessCutoff * 0.5, uRoughnessCutoff, roughness);
        ssrConf = clamp(edgeFade * roughFade, 0.0, 1.0);
    }
    vec3 hitColor = hit ? texture(uSceneColor, hitUv).rgb : probe;
    // 混合发生在**反射项内部**（而不是"有就加、没有就不加"）——
    // 这是消除"一半有一半没有"那条硬边的根本办法。
    vec3 refl = mix(probe, hitColor, ssrConf);
    FragColor = vec4(refl * fresnel, 1.0);
}
)glsl";

// ---- ④ 合成：加法叠回 HDR ----
// 只读反射缓冲、只写目标颜色附件 ⇒ 没有"边读边写同一张纹理"的反馈环。
// 用 glBlendFunc(GL_ONE, GL_ONE)：反射是**加性光**（打在已有画面上），
// 不是"替换"。（替换语义需要材质侧参与，见 ssr_types.h 顶部的分工说明。）
inline constexpr const char* kSsrCompositeFs = R"glsl(
#version 330 core
layout(location = 0) out vec4 FragColor;

uniform sampler2D uRefl;
uniform vec2 uResolution;

void main() {
    vec2 uv = gl_FragCoord.xy / uResolution;
    FragColor = vec4(texture(uRefl, uv).rgb, 0.0);
}
)glsl";

}  // namespace jpov

#endif  // JPOV_SRC_SSR_SSR_SHADER_H_
