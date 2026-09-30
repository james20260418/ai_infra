// JPOV 穿衣工具 — 衣物变换（平移 / 旋转 / 缩放）状态与烘焙（纯函数 / 无 GL）
//
// 需求（2026-09-30 Danis 定）：穿衣工具面板要给衣服提供
//   ① 平移：x / y / z 三个**绝对位置**输入框 + 各自的**步长**输入框 + 步进按钮（"<" ">"）；
//   ② 旋转：只有**步长**输入框 + 步进按钮（"<" ">"），角度单位度，语义 = 让当前模型绕
//      X / Y / Z 轴**逆时针**转多少度；
//   ③ 整体缩放：步进式（"+" 乘系数变大 / "−" 除以系数变小），系数本身也用输入框调。
//
// 关键约定（Danis 明确强调）：**一律直接修改模型的 mesh 数据来实现衣服的调节**，
//   不靠 DrawGltfObject 的 center / up / front / scale 放置参数。旋转尤其如此
//   （"我所有的旋转衣物的操作都是直接 apply 到 mesh 上"）。这样编辑后保存 / 后续
//   仿真统一化时，拿到的就是已经变换好的几何，不必再套一遍放置变换。
//
// 因此本文件是"变换状态 → 烘进 CPU 顶点"的纯函数层：
//   - 状态 = ClothTransform（绝对平移 + 欧拉角 + 整体缩放）；
//   - 烘焙 = 由欧拉角构造旋转基 (up, front)，复用既有 ApplyPlacementToMesh
//     （v' = center + R·(scale·v)，法线只转不平移、切线按 scale 缩放）把几何写死在顶点上。
//
// 本文件与 GL / UI 解耦（header-only 纯函数），便于单测覆盖的边界：
//   - 步长 / 系数的 clamp 语义（Danis 指定的三档上限）；
//   - 欧拉角 → 旋转基的方向与"逆时针"符号；
//   - 烘焙后顶点的期望坐标。

#ifndef JPOV_CLOTHING_CLOTHING_TRANSFORM_H_
#define JPOV_CLOTHING_CLOTHING_TRANSFORM_H_

#include <algorithm>
#include <cmath>

#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/interface/mesh_transform.h"

namespace jpov {
namespace clothing {

// ==================== 合法范围（Danis 2026-09-30 指定） ====================

// 平移步长绝对值上限（米）。用户完成输入后 clamp 到 [-kTransStepAbsMax, +kTransStepAbsMax]。
inline constexpr float kTransStepAbsMax = 5.0f;
// 旋转步长绝对值上限（度）。clamp 到 [-kRotStepAbsMax, +kRotStepAbsMax]。
inline constexpr float kRotStepAbsMax = 90.0f;
// 缩放步进系数范围（无量纲）。clamp 到 [kScaleStepMin, kScaleStepMax]：
// "+" 乘此系数、"−" 除以此系数，故 ≥ 1 保证 "+" 变大、"−" 变小。
inline constexpr float kScaleStepMin = 1.0f;
inline constexpr float kScaleStepMax = 2.0f;

// 整体缩放的合法范围。沿用项目既有约定（模型编辑器 ModelPlacement 同为 [0.1, 10]），
// 防止连续步进把缩放推成极小 / 极大。
inline constexpr float kClothScaleMin = 0.1f;
inline constexpr float kClothScaleMax = 10.0f;

// 度数 → 弧度。
inline constexpr double kClothDegToRad = 3.14159265358979323846 / 180.0;

// ==================== 变换状态 ====================

// 衣服的调节状态（状态外置：由 App 持有，面板控件写回，本层只读）。
//
// 默认全为"恒等"：绝对位置 0、旋转 0 度、缩放 1.0 —— 与"衣服按资产原始坐标显示"一致。
struct ClothTransform {
    // 绝对位置（米）。平移没有范围限制（Danis 只要求步长有上限），故不做 clamp。
    jpov::Vec3f offset{0.0f, 0.0f, 0.0f};
    // 绕世界 X / Y / Z 轴的逆时针旋转角（度）。旋转角本身不 clamp / 不 wrap，
    // 由烘焙时按三角函数取值（周期 360°，故数值大小不影响结果）。
    jpov::Vec3f rotation_deg{0.0f, 0.0f, 0.0f};
    // 整体缩放系数（无量纲），范围 [kClothScaleMin, kClothScaleMax]。
    float scale = 1.0f;
};

// ==================== clamp（用户完成输入后调用） ====================

// 平移步长：绝对值 ≤ kTransStepAbsMax。
inline float ClampTransStep(float v) {
    return std::clamp(v, -kTransStepAbsMax, kTransStepAbsMax);
}

// 旋转步长：绝对值 ≤ kRotStepAbsMax。
inline float ClampRotStep(float v) {
    return std::clamp(v, -kRotStepAbsMax, kRotStepAbsMax);
}

// 缩放步进系数：夹到 [kScaleStepMin, kScaleStepMax]。
inline float ClampScaleStep(float v) {
    return std::clamp(v, kScaleStepMin, kScaleStepMax);
}

// 整体缩放：夹到 [kClothScaleMin, kClothScaleMax]。
inline float ClampClothScale(float v) {
    return std::clamp(v, kClothScaleMin, kClothScaleMax);
}

// ==================== 旋转（右手系，"逆时针"约定） ====================

// 绕 +X 轴逆时针旋转 deg 度。右手系下 +Y → +Z：
//   Rx = [ 1   0    0  ]
//        [ 0  cos -sin ]
//        [ 0  sin  cos ]
// 从 +X 轴看向原点时，+Y 转向 +Z 即逆时针（Danis 的"逆时针"语义）。
inline jpov::Vec3f RotateClothX(const jpov::Vec3f& v, float deg) {
    const float a = static_cast<float>(deg * kClothDegToRad);
    const float c = std::cos(a);
    const float s = std::sin(a);
    return jpov::Vec3f(v.x(), c * v.y() - s * v.z(), s * v.y() + c * v.z());
}

// 绕 +Y 轴逆时针旋转 deg 度。右手系下 +Z → +X：
//   Ry = [ cos  0  sin ]
//        [  0   1   0  ]
//        [-sin  0  cos ]
inline jpov::Vec3f RotateClothY(const jpov::Vec3f& v, float deg) {
    const float a = static_cast<float>(deg * kClothDegToRad);
    const float c = std::cos(a);
    const float s = std::sin(a);
    return jpov::Vec3f(c * v.x() + s * v.z(), v.y(), -s * v.x() + c * v.z());
}

// 绕 +Z 轴逆时针旋转 deg 度。右手系下 +X → +Y：
//   Rz = [ cos -sin  0 ]
//        [ sin  cos  0 ]
//        [  0    0   1 ]
inline jpov::Vec3f RotateClothZ(const jpov::Vec3f& v, float deg) {
    const float a = static_cast<float>(deg * kClothDegToRad);
    const float c = std::cos(a);
    const float s = std::sin(a);
    return jpov::Vec3f(c * v.x() - s * v.y(), s * v.x() + c * v.y(), v.z());
}

// 依次施加 X → Y → Z 旋转（即总旋转 R = Rz·Ry·Rx）。
// 说明：三个角都绕**世界轴**，先转 X 再转 Y 最后转 Z。该顺序是 UI 步进式
//   "先点哪个轴先转哪个"的自然读法；不同顺序会给出不同的欧拉表示，但都是合法旋转。
inline jpov::Vec3f RotateClothEuler(const jpov::Vec3f& v,
                                    const jpov::Vec3f& rot_deg) {
    const jpov::Vec3f after_x = RotateClothX(v, rot_deg.x());
    const jpov::Vec3f after_y = RotateClothY(after_x, rot_deg.y());
    return RotateClothZ(after_y, rot_deg.z());
}

// 欧拉角（度）→ ApplyPlacementToMesh 需要的旋转基 (up, front)。
//   up    = R·(0,1,0)   // 模型局部 +Y 的世界方向
//   front = R·(0,0,1)   // 模型局部 +Z 的世界方向
// R 为正交旋转，故 up / front 单位且正交，MakePlacementBasis 可无损还原 R。
//
// Pre-condition: up != nullptr 且 front != nullptr。
inline void EulerDegToUpFront(const jpov::Vec3f& rot_deg,
                              jpov::Vec3f* up /*output*/,
                              jpov::Vec3f* front /*output*/) {
    CHECK(up != nullptr);
    CHECK(front != nullptr);
    *up = RotateClothEuler(jpov::Vec3f(0.0f, 1.0f, 0.0f), rot_deg);
    *front = RotateClothEuler(jpov::Vec3f(0.0f, 0.0f, 1.0f), rot_deg);
}

// ==================== 烘焙 ====================

// 把变换状态烘进一份 CPU mesh（返回新 mesh，不改入参）。
//
// 复用既有 ApplyPlacementToMesh：v' = offset + R·(scale·v)，法线 = R·n，
// 切线 = scale·(R·t)；uvs / indices / 骨骼 / flags 原样保留。
//
// Pre-condition: base.Validate() 通过；transform.scale ∈ [kClothScaleMin, kClothScaleMax]。
inline jpov::MeshData BakeClothMesh(const jpov::MeshData& base,
                                    const ClothTransform& transform) {
    jpov::Vec3f up;
    jpov::Vec3f front;
    EulerDegToUpFront(transform.rotation_deg, &up, &front);
    return jpov::ApplyPlacementToMesh(base, transform.offset, up, front,
                                      transform.scale);
}

}  // namespace clothing
}  // namespace jpov

#endif  // JPOV_CLOTHING_CLOTHING_TRANSFORM_H_
