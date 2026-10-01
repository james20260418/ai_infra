// JPOV 远景仰角雾（空气透视，horizon fog）—— GLSL 着色器
//
// 全屏三角形 VS（gl_VertexID，覆盖 NDC）+ 解析式 FS。
//
// FS 由相机逆 VP 反推每像素的视线方向 d 与到可见面的距离 dist：
//   band = 1 − smoothstep(elev_inner, elev_outer, |d.y|)   // 仰角带（水平带内=1）
//   ramp = smoothstep(start_distance, full_distance, dist)  // 距离
//   τ    = density · band · ramp
//   α    = 1 − exp(−τ)
// 输出 **(雾色, α)**，交由固定管线 alpha 混合（GL_SRC_ALPHA, ONE_MINUS_SRC_ALPHA）
// **就地**合成到当前帧缓冲：
//   L_out = fog_color·α + L_scene·(1 − α) = L_scene·e^{−τ} + fog_color·(1 − e^{−τ})
// 因为混合用的是帧缓冲里**已有**的颜色，本 pass 不需要采样场景颜色、也不需要自己的 FBO。
//
// 关键：沿视线 d 不变 ⇒ `d.y` 整条射线是常量 ⇒ 仰角条件无需求交，只是一个乘子。
//
// 收敛色默认取「该像素方向的**大气色天空**」（不含日月盘/光晕，由 sky 单独一趟渲到纹理）：
// 收敛色必须用**天空辐射亮度**，才能让远景真正融进天边；若用 CPU 推导的天光「颜色」
//（环境光量级，比天空辐射暗很多）会把远景压暗成一条脏带。
//
// 对**所有**像素都跑：天空像素的收敛色 = 它自己的天空色 ⇒ 几乎不变；地平线以下的
// 「地色背景」也会被朝地平线天空色拉（这正是想要的空气透视）。

#ifndef JPOV_SRC_HORIZON_FOG_HORIZON_FOG_SHADER_H_
#define JPOV_SRC_HORIZON_FOG_HORIZON_FOG_SHADER_H_

namespace jpov {

// 全屏三角形 VS（与 tonemap/bloom 同款 gl_VertexID 覆盖 NDC，无 VAO/VBO）。
inline constexpr const char* kHorizonFogVs = R"glsl(
#version 330 core
out vec2 vTexCoord;
void main() {
    vec2 pos;
    if (gl_VertexID == 0) pos = vec2(-1.0, -1.0);
    else if (gl_VertexID == 1) pos = vec2( 3.0, -1.0);
    else pos = vec2(-1.0,  3.0);
    vTexCoord = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
}
)glsl";

inline constexpr const char* kHorizonFogFs = R"glsl(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D uSceneDepthTex;  // MRT#1 场景深度（R32F；gl_FragCoord.z，1.0=背景/远平面）
uniform sampler2D uSkyTex;         // 大气色天空（=该像素方向的天空色，不含日月盘/光晕）
uniform mat4  uInvVP;              // 相机 逆(Proj*View)
uniform vec3  uCamPos;             // 相机世界位置
uniform float uStart;              // 起雾距离（米）
uniform float uFull;               // 满雾距离（米）
uniform float uElevSinInner;       // sin(仰角带内边界)；|d.y| ≤ 它 → 满
uniform float uElevSinOuter;       // sin(仰角带外边界)；|d.y| ≥ 它 → 无
uniform float uDensity;            // σ_max（带内、满距离处的 τ）
uniform vec3  uFogColor;           // 无天空 / uUseSkyColor=0 时的收敛色（线性 HDR）
uniform int   uUseSkyColor;        // 1=收敛色取 uSkyTex（该方向天空色）；0=用 uFogColor
void main() {
    // 由逆 VP 反推视线方向 d（近/远平面上同屏幕点的两点差）与到该像素可见面的距离。
    vec2 ndc = vTexCoord * 2.0 - 1.0;
    vec4 pn = uInvVP * vec4(ndc, -1.0, 1.0);
    vec4 pf = uInvVP * vec4(ndc,  1.0, 1.0);
    vec3 d = normalize(pf.xyz / pf.w - pn.xyz / pn.w);
    float ndc_z = texture(uSceneDepthTex, vTexCoord).r;
    vec4 pw = uInvVP * vec4(ndc, ndc_z * 2.0 - 1.0, 1.0);
    float dist = length(pw.xyz / pw.w - uCamPos);

    float band = 1.0 - smoothstep(uElevSinInner, uElevSinOuter, abs(d.y));
    float ramp = smoothstep(uStart, uFull, dist);
    float tau = uDensity * band * ramp;
    float alpha = 1.0 - exp(-tau);
    // 收敛色 = 该像素方向的天空大气色（与天边无缝）；无从取值时退回常量雾色。
    vec3 fog_col = (uUseSkyColor != 0) ? texture(uSkyTex, vTexCoord).rgb : uFogColor;
    // 就地 alpha 混合：out = fog·α + scene·(1−α)（scene 已在目标帧缓冲中）。
    FragColor = vec4(fog_col, alpha);
}
)glsl";

}  // namespace jpov

#endif  // JPOV_SRC_HORIZON_FOG_HORIZON_FOG_SHADER_H_
