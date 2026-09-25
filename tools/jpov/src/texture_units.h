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

// ---- 编译期互不重叠校验（改分配表时立刻报错）----
static_assert(kTexUnitMaterialBase + kMaterialTexUnitCount <= kTexUnitShadowMapBase,
              "材质纹理单元区间与 shadow map 区间重叠");
static_assert(kTexUnitShadowMapBase + kMaxShadowCascades <= kTexUnitPoseAtlas,
              "shadow map 区间与 pose atlas 单元重叠");
static_assert(kTexUnitPoseAtlas < kTexUnitThicknessBind,
              "pose atlas 与粗细绑定表单元重叠");

}  // namespace jpov

#endif  // JPOV_SRC_TEXTURE_UNITS_H_
