// JPOV — CSM 阴影的 GLSL 共享件：**单级联单次采样**（无 PCF）。
//
// 谁在用：fire_fog（雾体阴影）与 strip（条带消费天光时的太阳项）。两边是同一套数学、
// 同一套 uniform、同一套深度偏置，只差「要不要算」的那个开关 uniform —— 所以只有一份实现。
// object3d 的主光照走 PCF（`pcfShadow`，12 tap + 黄金角），与这里不是同一档，不复用。
//
// 为什么不复制两遍：这组函数是**采样语义**（UV 推导 / 深度比较方向 / 偏置 / 级联选择 /
// 末尾 fade）的唯一出处；复制两遍就会出现「一边改了偏置、另一边没改」的漂移，而这种漂移
// 在画面上只表现为「某处阴影稍微飘一点」，很难在 review 里看出来。
//
// 用法：把本片段拼进 FS 源码（`std::string(a) + kCsmSingleTapGlsl + b`），
// 调用点为 `CsmShadow(world_pos, distance_to_camera, enabled)`；
// 每个 renderer 自己把对应的开关 uniform（`uHasSun` / `uHasCsm`）传进来。

#ifndef JPOV_SRC_CSM_SHADOW_GLSL_H_
#define JPOV_SRC_CSM_SHADOW_GLSL_H_

namespace jpov {

// Pre-condition: 调用方已声明 `uSunDir` 之类的自身 uniform（本片段不负责它们）。
inline constexpr const char* kCsmSingleTapGlsl = R"glsl(
uniform int   uCascadeCount;              // 级联段数 [1,5]
uniform float uCascadeRanges[5];          // 各级联 far 距离（严格递增；末项 = 总阴影距离）
uniform sampler2D uShadowMap[5];
uniform mat4  uShadowVP[5];               // light view-proj（世界 → 阴影裁剪空间）
uniform mat4  uShadowDepthVP[5];          // 世界 → 深度比较空间（与 shadow pass 同源）
uniform float uShadowTexelWorld[5];       // 每个纹素覆盖的世界长度（米）
uniform float uShadowBiasCascade[5];      // 按级联覆写的偏置（米；override=0 时不生效）
uniform int   uShadowBiasOverride;        // 0 = 用 texel 尺寸推偏置，1 = 用上面的数组
uniform float uShadowFadeStart;           // 级联外淡出区间（米）：start 内完全生效
uniform float uShadowFadeEnd;             //   end 外完全不受影（避免硬边）

// 单级联单次采样。返回 1 = 受照、0 = 在影子里（不在该级联覆盖内也算受照）。
// 深度比较用**相对主视锥中心的线性深度**（米），两端同源（见 object3d 的说明）。
float CsmTap(sampler2D smap, mat4 vp, mat4 dvp, float texel_w,
             float bias_val, int bias_override, vec3 wp) {
    vec4 lsp = vp * vec4(wp, 1.0);
    vec3 ndc = lsp.xyz / lsp.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        return 1.0;
    }
    vec4 dpos = dvp * vec4(wp, 1.0);
    float cur = dpos.z / dpos.w;
    float tw = (bias_override != 0) ? bias_val : texel_w;
    // 偏置固定按 tanθ=1 取保守值：雾没有表面法线，薄片（条带）的法线又常与深度轴近乎正交，
    // 用 N·L 推导只会低估偏置 → 自阴影 acne。宁可少一点悬浮（peter-panning）。
    cur -= max(0.01, 1.5 * tw);
    return (cur <= texture(smap, uv).r) ? 1.0 : 0.0;
}

// 按片元到相机的距离选主级联，末尾按 fade 淡出（1 = 完全受照）。
// GLSL 330 禁止动态索引 sampler ⇒ 五级按常量索引展开（加级联要同步改这里和 kMaxCascades）。
float CsmShadow(vec3 wp, float dist, int enabled) {
    if (enabled == 0 || uCascadeCount <= 0) {
        return 1.0;
    }
    int c = uCascadeCount - 1;
    for (int i = 0; i < 5; ++i) {
        if (i < uCascadeCount && dist <= uCascadeRanges[i]) {
            c = i;
            break;
        }
    }
    float s;
    if (c == 0) {
        s = CsmTap(uShadowMap[0], uShadowVP[0], uShadowDepthVP[0], uShadowTexelWorld[0],
                   uShadowBiasCascade[0], uShadowBiasOverride, wp);
    } else if (c == 1) {
        s = CsmTap(uShadowMap[1], uShadowVP[1], uShadowDepthVP[1], uShadowTexelWorld[1],
                   uShadowBiasCascade[1], uShadowBiasOverride, wp);
    } else if (c == 2) {
        s = CsmTap(uShadowMap[2], uShadowVP[2], uShadowDepthVP[2], uShadowTexelWorld[2],
                   uShadowBiasCascade[2], uShadowBiasOverride, wp);
    } else if (c == 3) {
        s = CsmTap(uShadowMap[3], uShadowVP[3], uShadowDepthVP[3], uShadowTexelWorld[3],
                   uShadowBiasCascade[3], uShadowBiasOverride, wp);
    } else {
        s = CsmTap(uShadowMap[4], uShadowVP[4], uShadowDepthVP[4], uShadowTexelWorld[4],
                   uShadowBiasCascade[4], uShadowBiasOverride, wp);
    }
    float fade = 1.0 - clamp((dist - uShadowFadeStart)
                             / max(uShadowFadeEnd - uShadowFadeStart, 1e-5), 0.0, 1.0);
    return mix(1.0, s, fade);
}
)glsl";

}  // namespace jpov

#endif  // JPOV_SRC_CSM_SHADOW_GLSL_H_
