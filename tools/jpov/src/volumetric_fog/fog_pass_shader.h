// JPOV 局部体积雾 — 全屏 fog pass 的 GLSL（vertex + fragment）
//
// 见 docs/jpov_volumetric_fog_design.md §3.4 / §6 / §7。一次全屏三角形；每像素查本 tile
// 的 ≤K 个雾体（uint16 index → 属性纹理 texelFetch），闭式求 τ，局部累加 τ/S，
// 末尾就地合成 L = L_scene·exp(−τ) + S 写回 HDR。
//
// 数学与 src/volumetric_fog/fog_volume.h 的 CPU 参考实现逐式对应（同一闭式）。
// 属性纹理布局与 src/volumetric_fog/fog_common.h 逐一对应。

#ifndef JPOV_SRC_VOLUMETRIC_FOG_FOG_PASS_SHADER_H_
#define JPOV_SRC_VOLUMETRIC_FOG_FOG_PASS_SHADER_H_

namespace jpov {
namespace volumetric_fog {

// 全屏三角形 vertex（复用 tone map 的 gl_VertexID 覆盖 NDC 模式）。
inline constexpr const char* kFogPassVs = R"glsl(
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

inline constexpr const char* kFogPassFs = R"glsl(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D  uHdrTex;      // 场景 HDR 颜色（RGBA16F）
uniform sampler2D  uDepthTex;    // 场景深度（R32F，gl_FragCoord.z ∈ [0,1]）
uniform usampler2D uTileTex;     // tile 索引表（R16UI；宽=grid_w*K，高=grid_h）
uniform sampler2D  uAttrTex;     // 雾体属性（RGBA32F；宽=5，高=雾体数）
uniform mat4  uInvVP;            // 相机 逆(Proj*View)
uniform vec3  uCamPos;           // 相机世界位置
uniform int   uTileSize;         // tile 边长（像素）
uniform int   uK;                // 每 tile 雾体上限（= kMaxFogPerTile）

const int   kAttrTexels   = 5;
const float kShapeSphere  = 0.0;
const float kShapeCylinder= 1.0;
const float kDome         = 0.0;
const float kSharp        = 1.0;
const float kUniform      = 2.0;
const float kDomeAxial    = 3.0;
const uint  kSentinel     = 0xFFFFu;

// ∫_lo^hi (c0 + c1 t + ... + c6 t^6) dt
float IntegratePoly(float c[7], float lo, float hi) {
    float il = 0.0, ih = 0.0, pl = 1.0, ph = 1.0;
    for (int k = 0; k < 7; ++k) {
        pl *= lo;
        ph *= hi;
        il += c[k] * pl / float(k + 1);
        ih += c[k] * ph / float(k + 1);
    }
    return ih - il;
}

// 球雾 τ（闭式）。d 单位向量。
float SphereTau(vec3 o, vec3 d, vec3 c, float r, float sigma0, float profile,
                float tmin, float tmax) {
    vec3 m = o - c;
    float b = dot(m, d);
    float mm = dot(m, m);
    float disc = b * b - (mm - r * r);
    if (disc <= 0.0) return 0.0;
    float L = sqrt(disc);
    float lo = max(-b - L, tmin);
    float hi = min(-b + L, tmax);
    if (hi <= lo) return 0.0;
    float tc = -b;
    float a = 1.0 / (r * r);
    float beta = 1.0 - a * (mm - b * b);
    float q0 = beta - a * tc * tc;
    float q1 = 2.0 * a * tc;
    float q2 = -a;
    float cp[7];
    for (int i = 0; i < 7; ++i) cp[i] = 0.0;
    if (profile < 0.5) {                       // kDome
        cp[0] = q0; cp[1] = q1; cp[2] = q2;
    } else {                                   // kSharp = q^2
        cp[0] = q0 * q0;
        cp[1] = 2.0 * q0 * q1;
        cp[2] = q1 * q1 + 2.0 * q0 * q2;
        cp[3] = 2.0 * q1 * q2;
        cp[4] = q2 * q2;
    }
    return sigma0 * IntegratePoly(cp, lo, hi);
}

// 圆柱雾 τ（径向 × 轴向分离式闭式）。d 单位向量。
float CylinderTau(vec3 o, vec3 d, vec3 base, vec3 axisIn, float r, float hgt,
                  float sigma0, float radialProfile, float axialProfile,
                  float tmin, float tmax) {
    vec3 axis = normalize(axisIn);
    vec3 u0 = o - base;
    float d_ax = dot(d, axis);
    float y0 = dot(u0, axis);
    float u_d = dot(u0, d);
    float uu = dot(u0, u0);
    float A = 1.0 - d_ax * d_ax;
    float B = 2.0 * (u_d - y0 * d_ax);
    float C = uu - y0 * y0;
    float R2 = r * r;

    float rad_lo = -1e30, rad_hi = 1e30;
    if (A > 1e-9) {
        float disc = B * B - 4.0 * A * (C - R2);
        if (disc < 0.0) return 0.0;
        float s = sqrt(disc);
        float r1 = (-B - s) / (2.0 * A);
        float r2 = (-B + s) / (2.0 * A);
        rad_lo = min(r1, r2);
        rad_hi = max(r1, r2);
    } else {
        if (abs(B) < 1e-12) {
            if (C > R2) return 0.0;
        } else {
            float root = (R2 - C) / B;
            if (B > 0.0) rad_hi = root; else rad_lo = root;
        }
    }

    float z_lo = -1e30, z_hi = 1e30;
    if (abs(d_ax) > 1e-9) {
        float ta = (0.0 - y0) / d_ax;
        float tb = (hgt - y0) / d_ax;
        z_lo = min(ta, tb);
        z_hi = max(ta, tb);
    } else if (y0 < 0.0 || y0 > hgt) {
        return 0.0;
    }

    float lo = max(max(rad_lo, z_lo), tmin);
    float hi = min(min(rad_hi, z_hi), tmax);
    if (hi <= lo) return 0.0;

    // f_r = 1 - rho^2/R2 = (1 - C/R2) - (B/R2) t - (A/R2) t^2
    float fr0 = 1.0 - C / R2;
    float fr1 = -B / R2;
    float fr2 = -A / R2;
    float c[7];
    for (int i = 0; i < 7; ++i) c[i] = 0.0;
    if (radialProfile < 0.5) {                 // kDome
        c[0] = fr0; c[1] = fr1; c[2] = fr2;
    } else {                                   // kSharp = f_r^2
        c[0] = fr0 * fr0;
        c[1] = 2.0 * fr0 * fr1;
        c[2] = fr1 * fr1 + 2.0 * fr0 * fr2;
        c[3] = 2.0 * fr1 * fr2;
        c[4] = fr2 * fr2;
    }

    float dens[7];
    for (int i = 0; i < 7; ++i) dens[i] = 0.0;
    if (axialProfile > 1.5) {                  // kDomeAxial: f_y = 1 - (y/h)^2
        float h2 = hgt * hgt;
        float fy0 = 1.0 - (y0 * y0) / h2;
        float fy1 = -2.0 * y0 * d_ax / h2;
        float fy2 = -(d_ax * d_ax) / h2;
        dens[0] = c[0] * fy0;
        dens[1] = c[1] * fy0 + c[0] * fy1;
        dens[2] = c[2] * fy0 + c[1] * fy1 + c[0] * fy2;
        dens[3] = c[3] * fy0 + c[2] * fy1 + c[1] * fy2;
        dens[4] = c[4] * fy0 + c[3] * fy1 + c[2] * fy2;
        dens[5] = c[5] * fy0 + c[4] * fy1 + c[3] * fy2;
        dens[6] = c[6] * fy0 + c[5] * fy1 + c[4] * fy2;
    } else {                                   // kUniform
        for (int i = 0; i < 7; ++i) dens[i] = c[i];
    }
    return sigma0 * IntegratePoly(dens, lo, hi);
}

void main() {
    vec2 uv = vTexCoord;
    vec2 ndc = uv * 2.0 - 1.0;

    // 视线：由逆 VP 取近/远两点定方向。
    vec4 pn = uInvVP * vec4(ndc, -1.0, 1.0);
    vec4 pf = uInvVP * vec4(ndc,  1.0, 1.0);
    vec3 o = uCamPos;
    vec3 d = normalize(pf.xyz / pf.w - pn.xyz / pn.w);

    // 场景深度 → 世界交点 → 沿视线的距离 t_depth（弦末端裁剪用）。
    float ndc_z = texture(uDepthTex, uv).r;
    vec4 pw = uInvVP * vec4(ndc, ndc_z * 2.0 - 1.0, 1.0);
    float t_depth = length(pw.xyz / pw.w - o);

    // 查找本像素所属 tile 的雾体列表。
    ivec2 fc = ivec2(gl_FragCoord.xy);
    int tx = fc.x / uTileSize;
    int ty = fc.y / uTileSize;

    float tau = 0.0;
    vec3 sct = vec3(0.0);   // Σ S_i
    for (int i = 0; i < 16; ++i) {
        if (i >= uK) break;
        uint idx = texelFetch(uTileTex, ivec2(tx * uK + i, ty), 0).r;
        if (idx == kSentinel) break;

        vec4 a0 = texelFetch(uAttrTex, ivec2(0, int(idx)), 0);  // pos.xyz, radius
        vec4 a1 = texelFetch(uAttrTex, ivec2(1, int(idx)), 0);  // axis.xyz, height
        vec4 a2 = texelFetch(uAttrTex, ivec2(2, int(idx)), 0);  // color.rgb, sigma0
        vec4 a3 = texelFetch(uAttrTex, ivec2(3, int(idx)), 0);  // L_in.rgb, shape
        vec4 a4 = texelFetch(uAttrTex, ivec2(4, int(idx)), 0);  // profiles

        float ti;
        if (a3.w < 0.5) {
            ti = SphereTau(o, d, a0.xyz, a0.w, a2.w, a4.x, 0.0, t_depth);
        } else {
            ti = CylinderTau(o, d, a0.xyz, a1.xyz, a0.w, a1.w, a2.w,
                             a4.x, a4.y, 0.0, t_depth);
        }
        if (ti > 0.0) {
            float Ti = exp(-ti);
            tau += ti;
            sct += a2.rgb * a3.rgb * (1.0 - Ti);
        }
        if (tau > 12.0) break;   // τ 饱和早退（T=e^-12≈6e-6，后续雾体不可见）
    }

    vec3 scene = texture(uHdrTex, uv).rgb;
    float T = exp(-tau);
    FragColor = vec4(scene * T + sct, 1.0);
}
)glsl";

}  // namespace volumetric_fog
}  // namespace jpov

#endif  // JPOV_SRC_VOLUMETRIC_FOG_FOG_PASS_SHADER_H_
