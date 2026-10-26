// JPOV 环境镜面（IBL specular）—— 共享 GLSL 片段 + uniform 上传
//
// 为什么单独成一个模块：
//   三个静态/蒙皮 PBR 片元（Object3DRenderer / InstancedObjectRenderer /
//   SkeletonRenderer::kMeshFs3dPBR）是**各自独立的拷贝**（见 instanced 头文件的
//   同步约定）。若把「环境镜面」这段分别抄三遍，一改公式就必然分叉，而分叉的症状
//   （某个路径的金属块偏暗/偏亮）出图极难发现 —— 这正是植被风当年改成拼接的原因。
//   故：GLSL 只有这一份，三份 FS 用 std::string 拼接把它嵌进去；
//       uniform 上传也只有这一份。
//
// 用法（三份 FS 都是）：
//   inline static const std::string kMeshFs3dPBR =
//       std::string(R"glsl(...头部...)glsl") + kEnvSpecularGlsl + R"glsl(...尾部...)glsl";
// 并在最终着色处：
//   vec3 env_spec = EnvSpecularTerm(N, V, roughness, metallic, base_color);
//
// 语义与参数见 interface/render_command.h 的 EnvSpecular。

#ifndef JPOV_SRC_ENV_SPECULAR_H_
#define JPOV_SRC_ENV_SPECULAR_H_

#include "tools/jpov/src/shader_manager.h"

namespace jpov {

// 三份 PBR 片元共用的环境镜面 GLSL（uniform + 分裂求和 + 求值函数）。
// 依赖调用方 FS 里已有的 PI 之外的**无任何本地符号**：本片段自包含。
inline constexpr const char* kEnvSpecularGlsl = R"glsl(
// ---- 环境镜面（IBL specular）---- 见 interface/render_command.h 的 EnvSpecular ----
uniform samplerCube uEnvCube;    // 128² 天空 cubemap（Renderer 每帧由 SkyCommand 渲）
uniform int   uEnvEnabled;       // 0 = 关闭（无 env_specular）：直接返回 0
uniform float uEnvSkyScale;      // 天光消费量 [0,1]
uniform float uEnvMaxLod;        // cubemap 最大 mip 级（= log2(边长)）

// 分裂求和（split-sum）第二项的解析拟合（Karis，"Physically Based Shading on
// Mobile"）：免掉一张预积分的环境 BRDF LUT 纹理。
//   spec_env = prefiltered(R, roughness) * (F0 * AB.x + AB.y)
vec2 EnvBrdfApprox(float roughness, float NoV) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

// N = 世界空间法线（已含法线贴图扰动）、V = 片元指向相机。
vec3 EnvSpecularTerm(vec3 N, vec3 V, float roughness, float metallic, vec3 base_color) {
    if (uEnvEnabled == 0) {
        return vec3(0.0);
    }
    // 粗糙度下限 0.02：完全镜面（0）时 mip 级恒为 0，得到逐纹素硬反射；
    // 128² 的天空 cubemap 在这个精度下会露出可见的立方体接缝。
    float r = clamp(roughness, 0.02, 1.0);
    float NoV = clamp(dot(N, V), 1.0e-4, 1.0);
    vec3 R = reflect(-V, N);
    // 粗糙度 → mip 级的映射是**近似**（真实做法要按 GGX 做重要性采样的预滤波）。
    // 天空是极低频的渐变，两级 mip 的差别肉眼不可见，故不做 PMREM。
    vec3 prefiltered = textureLod(uEnvCube, R, r * uEnvMaxLod).rgb;
    vec2 AB = EnvBrdfApprox(r, NoV);
    vec3 F0 = mix(vec3(0.04), base_color, metallic);
    return prefiltered * (F0 * AB.x + AB.y) * uEnvSkyScale;
}
)glsl";

// 上传环境镜面的 uniform + 绑定 cubemap。
//
// 参数：
//   shader_mgr    : 共享 shader 缓存（取 uniform location）。
//   prog          : 目标 PBR program（三份 FS 任一份）。
//   env_cube_tex  : 天空 cubemap 纹理；enabled=false 时可为 0（仍会关掉开关）。
//   max_lod       : cubemap 最大 mip 级（= log2(边长)），粗糙度→mip 映射用。
//   sky_scale     : 天光消费量 [0,1]。
//   enabled       : 本帧是否启用。**每帧都必须调用一次**：uniform 是 program 状态，
//                   上一帧开着而本帧没上传，就会继续用旧的（甚至已销毁的）cubemap。
//
// Pre-condition: enabled=true 时 env_cube_tex ≠ 0，且 sky_scale ∈ [0,1]。
void UploadEnvSpecular(ShaderManager& shader_mgr, unsigned int prog,
                       unsigned int env_cube_tex, float max_lod, float sky_scale,
                       bool enabled);

}  // namespace jpov

#endif  // JPOV_SRC_ENV_SPECULAR_H_
