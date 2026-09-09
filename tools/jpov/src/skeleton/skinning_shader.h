// JPOV skeleton skinning — 蒙皮顶点/片元着色器 + 渲染入口（M1 静态退化为先调通）
//
// 目的：把「rest mesh 顶点 (带 JOINTS_0/WEIGHTS_0) 经 SkeletonManager 的 pose atlas
// 肤色矩阵蒙皮，再走与 object3d PBR 相同的光照」落成可渲染路径。
//
// M1（本阶段）目标：**静态退化门** —— 用 bind pose（肤矩阵=I）走真实蒙皮 VS，
// 输出应与「直画非蒙皮该 rest mesh」几乎一致（diff≈0）。证明「顶点→atlas 肤矩阵→Σw·M·v」
// 这条链路本身对，之后才是动画（两 pose 插值）。
//
// 设计：
//   - VS 与 object3d PBR 完整版共用同一套 uniform( uMVP / uModel )与输出 varying：
//     vWorldPos / vWorldNormal / vTexCoord / vWorldTangent —— 因此 **FS 直接复用
//     kMeshFs3dPBR（同光照），不重写片元**。
//   - 顶点蒙皮发生在 mesh 局部空间：pos = Σ w_i·(M_i·aPos)，M_i = atlas 该骨最终肤矩阵。
//     normal / tangent 用同一套肤矩阵 mat3(M_i) 蒙皮（绑定时肤矩阵=I，各 M_i 单位）。
//   - 蒙皮后再做模型变换 uModel(center/up/front/scale)：世界法线/切线沿用 object3d 的
//     mat3(transpose(inverse(uModel))) 约定 —— 这样在 bind pose 下，本 VS 的输出与
//     object3d PBR 完整版**逐位一致**（M_i=I 时 Σ w·aPos = aPos、法线/切线同理），
//     保证 M1 diff≈0。
//   - 每骨 4×4 矩阵在 pose atlas 为【行主序 texel】（见 SkeletonManager PutMat4Row：
//     bone 占 4 连续 texel，texel t 存矩阵第 t 行），故从 texel 还原成 GLSL mat4
//     （列主序存储）需按「行元素 → mat 列位置」重排（见 LoadBoneMatrix）。
//
// M1 只做「单 pose 静态」（可退化成同 pose 或仅一个实例），不铺 instancing ——
// instancing 与两 pose 插值交给后续。每步独立可测、崩溃面小。

#ifndef JPOV_SRC_SKELETON_SKINNING_SHADER_H_
#define JPOV_SRC_SKELETON_SKINNING_SHADER_H_

namespace jpov {

// ==================== 蒙皮顶点着色器 ====================
// 输出与 object3d kMeshVs3dPBRFull 完全一致（vWorldPos/vWorldNormal/vTexCoord/vWorldTangent），
// 仅把「顶点经 uModel」改为「顶点先经 atlas 肤矩阵蒙皮、再经 uModel」。
// 因此 FS 端直接复用 kMeshFs3dPBR（同光照），保证 M1 sunny-day 效果与 object3d 一致。
inline constexpr const char* kSkinnedVs = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in ivec4 aJoint;
layout(location = 4) in vec4 aWeight;
layout(location = 5) in vec3 aTangent;

uniform mat4 uMVP;        // proj*view*model（Object3DRenderer/DrawObject3D 同约定；model 由调用方乘进）
uniform mat4 uModel;      // 局部→世界（center/up/front/scale）
uniform sampler2D uPoseAtlas;  // RGBA32F 骨骼动画纹理（pose atlas）
uniform int   uBoneCount;      // 该骨架骨数
uniform int   uPoseRow;        // 本实例 pose 在 atlas 的行（y）
uniform int   uPoseCol;        // 本实例 pose 在 atlas 的列起点（x / (bone*4)）

out vec3 vWorldPos;
out vec3 vWorldNormal;
out vec2 vTexCoord;
out vec3 vWorldTangent;

// 从 atlas 取第 bone 的 4×4 行主序矩阵，转成 GLSL mat4（列主序）。
// x0 = uPoseCol*uBoneCount*4 + bone*4；每个 texel=矩阵一行（row-major），行序 r=t+0..
mat4 LoadBoneMatrix(int bone) {
    int x0 = uPoseCol * uBoneCount * 4 + bone * 4;
    float m00 = texelFetch(uPoseAtlas, ivec2(x0 + 0, uPoseRow), 0).r;
    float m01 = texelFetch(uPoseAtlas, ivec2(x0 + 0, uPoseRow), 0).g;
    float m02 = texelFetch(uPoseAtlas, ivec2(x0 + 0, uPoseRow), 0).b;
    float m03 = texelFetch(uPoseAtlas, ivec2(x0 + 0, uPoseRow), 0).a;
    float m10 = texelFetch(uPoseAtlas, ivec2(x0 + 1, uPoseRow), 0).r;
    float m11 = texelFetch(uPoseAtlas, ivec2(x0 + 1, uPoseRow), 0).g;
    float m12 = texelFetch(uPoseAtlas, ivec2(x0 + 1, uPoseRow), 0).b;
    float m13 = texelFetch(uPoseAtlas, ivec2(x0 + 1, uPoseRow), 0).a;
    float m20 = texelFetch(uPoseAtlas, ivec2(x0 + 2, uPoseRow), 0).r;
    float m21 = texelFetch(uPoseAtlas, ivec2(x0 + 2, uPoseRow), 0).g;
    float m22 = texelFetch(uPoseAtlas, ivec2(x0 + 2, uPoseRow), 0).b;
    float m23 = texelFetch(uPoseAtlas, ivec2(x0 + 2, uPoseRow), 0).a;
    float m30 = texelFetch(uPoseAtlas, ivec2(x0 + 3, uPoseRow), 0).r;
    float m31 = texelFetch(uPoseAtlas, ivec2(x0 + 3, uPoseRow), 0).g;
    float m32 = texelFetch(uPoseAtlas, ivec2(x0 + 3, uPoseRow), 0).b;
    float m33 = texelFetch(uPoseAtlas, ivec2(x0 + 3, uPoseRow), 0).a;
    // texel[t] 存矩阵【第 t 行】：m_{row,col}。GLSL mat4 以列主序存储/构造 →
    // 我们把「行主序元素」填到对应列/行位置：
    //   col0 = (m00, m10, m20, m30)
    //   col1 = (m01, m11, m21, m31) ...
    return mat4(
        m00, m10, m20, m30,   // column 0
        m01, m11, m21, m31,   // column 1
        m02, m12, m22, m32,   // column 2
        m03, m13, m23, m33);  // column 3
}

void main() {
    // 4-bone 蒙皮：mesh 局部空间内 pos/normal/tangent = Σ w_i · M_i · (顶点)。
    vec3 sp = vec3(0.0);
    vec3 sn = vec3(0.0);
    vec3 st = vec3(0.0);
    for (int i = 0; i < 4; ++i) {
        float w = aWeight[i];
        if (w <= 0.0) continue;
        mat4 m = LoadBoneMatrix(aJoint[i]);
        sp += w * (m * vec4(aPos, 1.0)).xyz;
        sn += w * vec3(mat3(m) * aNormal);
        st += w * vec3(mat3(m) * aTangent);
    }

    // 世界坐标/法线：uModel=model（center/up/front/scale），统一 DrawObject3D 约定。
    vec4 world = uModel * vec4(sp, 1.0);
    vWorldPos = world.xyz;
    vWorldNormal = normalize(mat3(transpose(inverse(uModel))) * sn);
    vWorldTangent = normalize(mat3(transpose(inverse(uModel))) * st);
    vTexCoord = aTexCoord;
    // ⚠️ uMVP = mvp*model（proj*view*model, Object3DRenderer/DrawObject3D 同约定）, 故直接乘 sp。
    gl_Position = uMVP * vec4(sp, 1.0);
}
)glsl";

// FS 复用 object3d PBR 片元（同光照）。本模块不重复定义；绘制时直接以
// {kSkinnedVs, kMeshFs3dPBR} 注册 program（见 object3d_renderer.h 导出）。

// ==================== 蒙皮阴影顶点着色器 ====================
// 阴影 pass 专用：在网格局部空间蒙皮(同 kSkinnedVs)后, 用光空间 MVP 裁剪 + 输出线性深度
// （与 object3d 的 kShadowVs 语义一致：uShadowMVP=光VP*model 用于 gl_Position 近远裁剪；
//   uShadowDepthMVP=DepthVP*model 输出的 vShadowDepth 为相对主视锥中心的线性深度, w=1）。
// 输入/输出与 kShadowVs 完全对齐（vShadowDepth），故 FS 复用 kShadowFs。
inline constexpr const char* kSkinnedShadowVs = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in ivec4 aJoint;
layout(location = 4) in vec4 aWeight;
layout(location = 5) in vec3 aTangent;
uniform mat4 uShadowMVP;        // 光空间 裁剪(含 model)
uniform mat4 uShadowDepthMVP;   // 光空间 线性深度(含 model)
uniform sampler2D uPoseAtlas;
uniform int   uBoneCount;
uniform int   uPoseRow;
uniform int   uPoseCol;
out float vShadowDepth;

mat4 LoadBoneMatrix(int bone) {
    int x0 = uPoseCol * uBoneCount * 4 + bone * 4;
    float m00 = texelFetch(uPoseAtlas, ivec2(x0 + 0, uPoseRow), 0).r;
    float m01 = texelFetch(uPoseAtlas, ivec2(x0 + 0, uPoseRow), 0).g;
    float m02 = texelFetch(uPoseAtlas, ivec2(x0 + 0, uPoseRow), 0).b;
    float m03 = texelFetch(uPoseAtlas, ivec2(x0 + 0, uPoseRow), 0).a;
    float m10 = texelFetch(uPoseAtlas, ivec2(x0 + 1, uPoseRow), 0).r;
    float m11 = texelFetch(uPoseAtlas, ivec2(x0 + 1, uPoseRow), 0).g;
    float m12 = texelFetch(uPoseAtlas, ivec2(x0 + 1, uPoseRow), 0).b;
    float m13 = texelFetch(uPoseAtlas, ivec2(x0 + 1, uPoseRow), 0).a;
    float m20 = texelFetch(uPoseAtlas, ivec2(x0 + 2, uPoseRow), 0).r;
    float m21 = texelFetch(uPoseAtlas, ivec2(x0 + 2, uPoseRow), 0).g;
    float m22 = texelFetch(uPoseAtlas, ivec2(x0 + 2, uPoseRow), 0).b;
    float m23 = texelFetch(uPoseAtlas, ivec2(x0 + 2, uPoseRow), 0).a;
    float m30 = texelFetch(uPoseAtlas, ivec2(x0 + 3, uPoseRow), 0).r;
    float m31 = texelFetch(uPoseAtlas, ivec2(x0 + 3, uPoseRow), 0).g;
    float m32 = texelFetch(uPoseAtlas, ivec2(x0 + 3, uPoseRow), 0).b;
    float m33 = texelFetch(uPoseAtlas, ivec2(x0 + 3, uPoseRow), 0).a;
    return mat4(m00,m10,m20,m30, m01,m11,m21,m31, m02,m12,m22,m32, m03,m13,m23,m33);
}

void main() {
    vec3 sp = vec3(0.0);
    for (int i = 0; i < 4; ++i) {
        float w = aWeight[i];
        if (w <= 0.0) continue;
        mat4 m = LoadBoneMatrix(aJoint[i]);
        sp += w * (m * vec4(aPos, 1.0)).xyz;
    }
    // 光空间裁剪(uvShadowMVP 含 model): 蒙皮后直接乘光 VP*model。
    vec4 clip = uShadowMVP * vec4(sp, 1.0);
    gl_Position = clip;
    vec4 dpos = uShadowDepthMVP * vec4(sp, 1.0);
    vShadowDepth = dpos.z / dpos.w;
}
)glsl";

}  // namespace jpov

#endif  // JPOV_SRC_SKELETON_SKINNING_SHADER_H_
