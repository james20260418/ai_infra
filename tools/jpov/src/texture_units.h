// JPOV 纹理单元分配表 — 全渲染器唯一的 texture unit 分配点
//
// 为什么要有这个文件（2026-09-17 重构）：
//   原先各 pass 各自硬编码 glActiveTexture(GL_TEXTURE0 + N)（材质 1..6、shadow map 7..11、
//   pose atlas 在 shadow pass 写成 7），槽位关系只存在于人脑里，极易撞槽。实际踩过的例子：
//   shadow pass 的 pose atlas 用了 7 —— 与 shadow map 首槽**同号**，仅靠
//   「shadow pass 不采样 shadow map」这一巧合才没出事。
//   现在所有单元分配集中在此，且用 static_assert 做**编译期互不重叠**校验：
//   以后谁改区间、加新纹理，重叠加立刻编译失败，而不是等人肉核对。
//
// 适用范围：**3D 渲染主链路**的跨 pass 共享单元（tile / 材质 / shadow map / pose atlas）。
//   后处理 pass（bloom / tone map / highlight）在自己的 pass 内从 unit 0 起绑自己的输入，
//   属 pass-local、不参与本表（它们不与其他 pass 同时持有纹理）。
//
// 用法：任何 glActiveTexture 都必须写成 `glActiveTexture(GL_TEXTURE0 + kTexUnitXxx)`，
//   禁止裸数字。

#ifndef JPOV_SRC_TEXTURE_UNITS_H_
#define JPOV_SRC_TEXTURE_UNITS_H_

namespace jpov {

// ==================== 3D 主链路的单元分配 ====================

// tile 光源索引纹理（object3d tile culling；DrawObject3D 内绑定）。
inline constexpr int kTexUnitTileLightIndex = 0;

// 材质纹理区间起点：kTexUnitMaterialBase + {0..5}
//   0=baseColor 1=metallic 2=roughness 3=emissive 4=AO 5=normal
inline constexpr int kTexUnitMaterialBase = 1;
inline constexpr int kMaterialTexUnitCount = 6;   // baseColor/metallic/roughness/emissive/AO/normal

// 级联 shadow map 区间起点：kTexUnitShadowMapBase + cascade_index
inline constexpr int kTexUnitShadowMapBase = 7;
inline constexpr int kMaxShadowCascades = 5;      // == ShadowConfig::kMaxCascades

// 骨骼 pose atlas（蒙皮；主 pass 与 shadow pass **同号共用**，因为两 pass 不同时活跃）。
inline constexpr int kTexUnitPoseAtlas = 12;

// 部位粗细的「绑定表」纹理（每骨 2 texel：bind 位置 + 通道下标 / bind 朝向；蒙皮用）。
//   同为「主/shadow pass 同号共用」（两 pass 不同时活跃）。
inline constexpr int kTexUnitThicknessBind = 13;

// 部位额外旋转的「通道表」纹理（每骨 1 texel：外层/内层通道号；蒙皮用）。
//   同为「主/shadow pass 同号共用」（两 pass 不同时活跃）。
inline constexpr int kTexUnitPartialRotationBind = 14;
// 部位额外旋转的「每通道常量」纹理（kNumPartialRotation texel：bind pivot + 关节骨号）。
inline constexpr int kTexUnitPartialRotationChannel = 15;

// 环境镜面（IBL specular）的天空 cubemap（见 src/env_specular.h）。
//   三份 PBR 片元（object3d / instanced / skinned）共用同一号，因为三者**不同时**着色
//   同一条命令；且它们都不占用此号。
//   ⚠️ 13/14/15 不能用：蒙皮路径的粗细绑定表 / 部位旋转表占着（同一时刻活跃）。
//   本号 > 15 与 kTexUnitGlassSceneDepth 同类：GL 3.3 只**保证** 16 个片元纹理单元
//   （0..15），实测驱动（含 llvmpipe）给 32，故 16/17 可用；若将来遇到只给 16 的
//   驱动，需连同 kTexUnitGlassSceneDepth 一起重新分配，不是单点问题。
inline constexpr int kTexUnitEnvCube = 17;

// 透明混合 pass（Object3DRenderer::DrawObject3DBlend）使用的不透明场景深度纹理
//（值 = gl_FragCoord.z）。玻璃 pass **无深度缓冲**，在 FS 里手动作深度测试时采它。
// 仅 object3d blend（kBlend）program 使用；主链路其他 program 不绑它。
inline constexpr int kTexUnitGlassSceneDepth = 16;

// 植被风的「关节表」纹理（每关节 2 texel：pivot.xyz+parent / level+phase+soft；RGBA32F，2N×1）。
//   仅静态/object3d 路径的枢轴风使用（VS 里 texelFetch）。
//   ⚠️ **故意与蒙皮专用槽 12 同号**：12..15 只被蒙皮路径（pose atlas/粗细/部位旋转）占用，
//   而蒙皮与植被风**不同时活跃**（同一条命令不会既是蒙皮又是植被风）⇒ 同号共用安全。
//   **必须留在 0..15**：GL 3.3 只保证 MAX_VERTEX_TEXTURE_IMAGE_UNITS ≥ 16，用 ≥16 在部分驱动
//   上 glActiveTexture 报 INVALID_ENUM、纹理不绑、texelFetch 读垃圾 → level 爆掉 → 整株被搬倒。
inline constexpr int kTexUnitWindJointTable = 12;

// ---- 编译期互不重叠校验（改分配表时立刻报错）----
static_assert(kTexUnitMaterialBase + kMaterialTexUnitCount <= kTexUnitShadowMapBase,
              "材质纹理单元区间与 shadow map 区间重叠");
static_assert(kTexUnitShadowMapBase + kMaxShadowCascades <= kTexUnitPoseAtlas,
              "shadow map 区间与 pose atlas 单元重叠");
static_assert(kTexUnitPoseAtlas < kTexUnitThicknessBind,
              "pose atlas 与粗细绑定表单元重叠");
static_assert(kTexUnitThicknessBind < kTexUnitPartialRotationBind,
              "粗细绑定表与部位额外旋转通道表单元重叠");
static_assert(kTexUnitPartialRotationBind < kTexUnitPartialRotationChannel,
              "部位额外旋转的两张表单元重叠");
static_assert(kTexUnitPartialRotationChannel < kTexUnitGlassSceneDepth,
              "部位额外旋转通道表与玻璃场景深度单元重叠");
static_assert(kTexUnitGlassSceneDepth < kTexUnitEnvCube,
              "玻璃场景深度与天空 cubemap 单元重叠");
// ⚠️ kTexUnitWindJointTable 故意 = kTexUnitPoseAtlas（12）：蒙皮与植被风不同时活跃，
// 同号共用，且必须留在 0..15（见该常量处注释）。故不参与下列顺序断言。

}  // namespace jpov

#endif  // JPOV_SRC_TEXTURE_UNITS_H_
