// JPOV 穿衣工具 — 面板参数 clamp 与 mesh 法线重算（纯函数 / 无 GL）
//
// 2026-10-01 改版（Danis）：平移 / 旋转 / 缩放**不再是**作用在 CPU 顶点上的“烘焙”变换，
//   而是**即时作用于仿真器内部状态**（见 soft_mesh_simulator 的 ApplyTranslation /
//   ApplyRotation / ApplyScaling）——因为面板要在**仿真进行中**也能改变这件衣服的位置 /
//   朝向 / 大小，且不中断仿真。故本文件只剩两块与 mesh 无关/有关的小工具：
//
//   - 面板输入值的 **clamp**：步长 / 缩放系数 / 累计缩放的合法范围（Danis 指定）。
//   - RecomputeVertexNormals：软体仿真 / 即时变换会改顶点位置，**法线必须重算**，
//     否则着色停留在旧姿态（形状动了光不动）。显示层每帧调用。

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

// ==================== 法线重算 ====================

// 重算逐顶点法线（**面积加权**），供软体仿真 / 即时变换后刷新着色。
//
// 做法：对每个三角形算**未归一化**的面法线 n = (b-a)×(c-a)，其模长 = 2×面积，
//   直接累加到三个顶点即为面积加权平均；最后逐顶点归一化。这是最常用的“平滑法线”
//   做法，与网格密度无关。
//
// 边界：
//   - 网格无 kNormal 标志（normals 为空）→ 不动（显示层本就不用法线）。
//   - indices 为空 → 按 non-indexed 语义（每 3 个连续顶点一个三角形）。
//   - 某顶点的所有相邻面都退化（累加后模长 ~0）→ 该顶点法线回退为 (0,1,0)，
//     避免出现零向量让着色出现 NaN。
//
// Pre-condition（不满足即 LOG(FATAL)）：mesh != nullptr；positions 非空；若 indices
//   非空则长度为 3 的倍数且每个索引 < positions.size()（越界崩，不带坏数据进渲染）。
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
