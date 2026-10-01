// JPOV 穿衣工具 — 衣物 mesh 的 CPU 侧几何操作（就地平移 / 旋转 / 缩放 + 法线重算）
//
// 需求（2026-09-30 初版；2026-10-01 修订，Danis 定）：
//   穿衣工具面板给衣服提供 ① 平移 x/y/z；② 旋转 RX/RY/RZ（度，绕 X/Y/Z 逆时针）；
//   ③ 整体缩放。**一律直接改衣服 mesh 的顶点数据**，不靠 DrawGltfObject 的
//   center/up/front/scale 放置参数——这样保存 / 后续仿真拿到的就是已经变换好的几何。
//
//   2026-10-01 修订：**删除"绝对平移"（绝对 x/y/z 输入框）**。原设计把平移存成
//   `ClothTransform::offset` 这样的"绝对位置"状态、每次从 base 重新烘焙；Danis 认为
//   绝对位置的定义很别扭，改为——**状态就是 mesh 自身的顶点坐标**：每一步都在**当前**
//   顶点上就地施加增量变换。于是本文件不再需要"状态结构体 + 从 base 烘焙"，只有一组
//   就地操作；调用方（App）持有的 `cloth_current_` 顶点即唯一事实源。
//
// 因此本文件是"就地几何操作"的纯函数层（header-only，无 GL / UI 依赖）：
//   - TranslateMeshInPlace / RotateMeshInPlace / ScaleMeshInPlace：就地改顶点；
//   - RecomputeVertexNormals：软体仿真变形后重算法线（供显示，避免着色停留在绑定姿态）；
//   - MeshBoundsCenter：包围盒中心（调用方可用作就地旋转 / 缩放的枢轴）。
//
// 单测覆盖的边界：步长 / 系数 clamp 语义、旋转"逆时针"符号、就地变换后的期望坐标、
//   法线重算（含面积加权与退化回退）。

#ifndef JPOV_CLOTHING_CLOTHING_TRANSFORM_H_
#define JPOV_CLOTHING_CLOTHING_TRANSFORM_H_

#include <algorithm>
#include <cmath>

#include <glog/logging.h>

#include "tools/jpov/interface/mesh.h"

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

// 整体缩放的合法范围（相对初始几何的累计缩放系数）。沿用项目既有约定
// （模型编辑器 ModelPlacement 同为 [0.1, 10]），防止连续步进把缩放推成极小 / 极大。
inline constexpr float kClothScaleMin = 0.1f;
inline constexpr float kClothScaleMax = 10.0f;

// 度数 → 弧度。
inline constexpr double kClothDegToRad = 3.14159265358979323846 / 180.0;

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

// 整体缩放（累计系数）：夹到 [kClothScaleMin, kClothScaleMax]。
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

// 按轴号施加旋转：axis 0/1/2 → 绕 X/Y/Z。
// Pre-condition: axis ∈ {0,1,2}；越界是调用方 bug → 崩（不静默当无操作）。
inline jpov::Vec3f RotateClothAboutAxis(const jpov::Vec3f& v, int axis, float deg) {
    switch (axis) {
        case 0:
            return RotateClothX(v, deg);
        case 1:
            return RotateClothY(v, deg);
        case 2:
            return RotateClothZ(v, deg);
        default:
            LOG(FATAL) << "RotateClothAboutAxis: axis 必须 ∈ {0,1,2}，got " << axis;
            return v;  // 不可达（LOG(FATAL) 已终止）；为满足返回类型。
    }
}

// ==================== 就地几何操作 ====================
//
// 说明：以下"就地"操作直接改传入 mesh 的顶点属性（positions / normals / tangents），
//   uvs / indices / joint_* / flags 原样不动。调用方负责随后把新几何推上 GPU
//   （JPOV::UpdateMesh）。
//
// 枢轴（pivot）：旋转 / 缩放围绕调用方给定的 pivot——单块几何用 MeshBoundsCenter(*mesh)；
//   多块几何应传**合并包围盒中心**，保证各块绕同一枢轴转 / 缩，不相互撕开。

// 当前点的轴对齐包围盒中心（各轴 (min+max)/2）。
// Pre-condition: mesh.positions 非空。
inline jpov::Vec3f MeshBoundsCenter(const jpov::MeshData& mesh) {
    CHECK(!mesh.positions.empty()) << "MeshBoundsCenter: mesh 无顶点";
    jpov::Vec3f lo = mesh.positions.front();
    jpov::Vec3f hi = mesh.positions.front();
    for (const jpov::Vec3f& p : mesh.positions) {
        lo = jpov::Vec3f(std::min(lo.x(), p.x()), std::min(lo.y(), p.y()),
                         std::min(lo.z(), p.z()));
        hi = jpov::Vec3f(std::max(hi.x(), p.x()), std::max(hi.y(), p.y()),
                         std::max(hi.z(), p.z()));
    }
    return jpov::Vec3f((lo.x() + hi.x()) * 0.5f, (lo.y() + hi.y()) * 0.5f,
                       (lo.z() + hi.z()) * 0.5f);
}

// 就地平移：所有顶点 += delta。法线 / 切线不受平移影响。
// Pre-condition: mesh != nullptr。
inline void TranslateMeshInPlace(jpov::MeshData* mesh, const jpov::Vec3f& delta) {
    CHECK(mesh != nullptr);
    for (jpov::Vec3f& p : mesh->positions) {
        p = jpov::Vec3f(p.x() + delta.x(), p.y() + delta.y(), p.z() + delta.z());
    }
}

// 就地旋转：绕 **pivot**、绕 axis 轴（0/1/2 → X/Y/Z）逆时针转 deg 度。
// 位置与法线、切线一起转；uv / 索引不变。
// Pre-condition: mesh != nullptr；axis ∈ {0,1,2}（越界崩，见 RotateClothAboutAxis）。
inline void RotateMeshInPlace(jpov::MeshData* mesh, int axis, float deg,
                              const jpov::Vec3f& pivot) {
    CHECK(mesh != nullptr);
    const jpov::Vec3f c = pivot;
    for (jpov::Vec3f& p : mesh->positions) {
        const jpov::Vec3f rel(p.x() - c.x(), p.y() - c.y(), p.z() - c.z());
        const jpov::Vec3f r = RotateClothAboutAxis(rel, axis, deg);
        p = jpov::Vec3f(r.x() + c.x(), r.y() + c.y(), r.z() + c.z());
    }
    for (jpov::Vec3f& n : mesh->normals) {
        n = RotateClothAboutAxis(n, axis, deg);
    }
    for (jpov::Vec3f& t : mesh->tangents) {
        t = RotateClothAboutAxis(t, axis, deg);
    }
}

// 就地等比缩放：以 **pivot** 为中心放大 factor 倍。
// 位置按 factor 缩放；切线按 factor 缩放（切线不归一化，同 ApplyPlacementToMesh 的约定）；
// 法线不变（均匀缩放下单位法线等效恒等，同 ApplyPlacementToMesh）。
// Pre-condition: mesh != nullptr；factor > 0。
inline void ScaleMeshInPlace(jpov::MeshData* mesh, float factor,
                             const jpov::Vec3f& pivot) {
    CHECK(mesh != nullptr);
    CHECK_GT(factor, 0.0f) << "ScaleMeshInPlace: factor 必须 > 0，got " << factor;
    const jpov::Vec3f c = pivot;
    for (jpov::Vec3f& p : mesh->positions) {
        p = jpov::Vec3f(c.x() + (p.x() - c.x()) * factor,
                        c.y() + (p.y() - c.y()) * factor,
                        c.z() + (p.z() - c.z()) * factor);
    }
    for (jpov::Vec3f& t : mesh->tangents) {
        t = jpov::Vec3f(t.x() * factor, t.y() * factor, t.z() * factor);
    }
}

// 重算逐顶点法线（**面积加权**），供软体仿真变形后刷新着色。
//
// 做法：对每个三角形算**未归一化**的面法线 n = (b-a)×(c-a)，其模长 = 2×面积，
//   直接累加到三个顶点即为面积加权平均；最后逐顶点归一化。这是最常用的"平滑法线"
//   做法，与网格密度无关。
//
// 边界：
//   - 网格无 kNormal 标志（normals 为空）→ 不动（显示层本就不用法线）。
//   - indices 为空 → 按 non-indexed 语义（每 3 个连续顶点一个三角形）。
//   - 某顶点的所有相邻面都退化（累加后模长 ~0）→ 该顶点法线回退为 (0,1,0)，
//     避免出现零向量让着色出现 NaN。
//
// Pre-condition: mesh != nullptr；positions 非空；若 indices 非空则长度为 3 的倍数
//   且每个索引 < positions.size()（越界崩，不带坏数据进渲染）。
inline void RecomputeVertexNormals(jpov::MeshData* mesh) {
    CHECK(mesh != nullptr);
    CHECK(!mesh->positions.empty()) << "RecomputeVertexNormals: mesh 无顶点";
    if (mesh->normals.empty()) {
        return;  // 该网格不用法线（flags 无 kNormal），无需重算。
    }
    const size_t vcount = mesh->positions.size();
    CHECK_EQ(mesh->normals.size(), vcount)
        << "RecomputeVertexNormals: normals 与 positions 长度不一致";

    std::vector<jpov::Vec3f> acc(vcount, jpov::Vec3f(0.0f, 0.0f, 0.0f));

    auto add_face = [&acc, mesh](uint32_t ia, uint32_t ib, uint32_t ic) {
        CHECK_LT(ia, acc.size()) << "RecomputeVertexNormals: 索引越界 " << ia;
        CHECK_LT(ib, acc.size()) << "RecomputeVertexNormals: 索引越界 " << ib;
        CHECK_LT(ic, acc.size()) << "RecomputeVertexNormals: 索引越界 " << ic;
        const jpov::Vec3f& a = mesh->positions[ia];
        const jpov::Vec3f& b = mesh->positions[ib];
        const jpov::Vec3f& c = mesh->positions[ic];
        const jpov::Vec3f ab(b.x() - a.x(), b.y() - a.y(), b.z() - a.z());
        const jpov::Vec3f ac(c.x() - a.x(), c.y() - a.y(), c.z() - a.z());
        // 未归一化叉积 = 2×面积方向 → 面积加权。
        const jpov::Vec3f n(ab.y() * ac.z() - ab.z() * ac.y(),
                            ab.z() * ac.x() - ab.x() * ac.z(),
                            ab.x() * ac.y() - ab.y() * ac.x());
        acc[ia] = jpov::Vec3f(acc[ia].x() + n.x(), acc[ia].y() + n.y(),
                              acc[ia].z() + n.z());
        acc[ib] = jpov::Vec3f(acc[ib].x() + n.x(), acc[ib].y() + n.y(),
                              acc[ib].z() + n.z());
        acc[ic] = jpov::Vec3f(acc[ic].x() + n.x(), acc[ic].y() + n.y(),
                              acc[ic].z() + n.z());
    };

    if (mesh->indices.empty()) {
        for (size_t t = 0; t + 2 < vcount; t += 3) {
            add_face(static_cast<uint32_t>(t), static_cast<uint32_t>(t + 1),
                     static_cast<uint32_t>(t + 2));
        }
    } else {
        CHECK_EQ(mesh->indices.size() % 3, 0u)
            << "RecomputeVertexNormals: indices 长度不是 3 的倍数";
        for (size_t t = 0; t + 2 < mesh->indices.size(); t += 3) {
            add_face(mesh->indices[t], mesh->indices[t + 1], mesh->indices[t + 2]);
        }
    }

    for (size_t i = 0; i < vcount; ++i) {
        const float len = std::sqrt(acc[i].x() * acc[i].x() +
                                    acc[i].y() * acc[i].y() +
                                    acc[i].z() * acc[i].z());
        if (len > 1e-20f) {
            mesh->normals[i] = jpov::Vec3f(acc[i].x() / len, acc[i].y() / len,
                                           acc[i].z() / len);
        } else {
            mesh->normals[i] = jpov::Vec3f(0.0f, 1.0f, 0.0f);  // 退化回退
        }
    }
}

}  // namespace clothing
}  // namespace jpov

#endif  // JPOV_CLOTHING_CLOTHING_TRANSFORM_H_
