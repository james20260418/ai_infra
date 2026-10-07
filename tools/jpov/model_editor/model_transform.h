// JPOV 模型编辑器 — target 的平移 / 旋转 / 缩放（就地改顶点，纯函数 / GL-free）
//
// 用途（2026-10-06 Danis）：模型编辑器左上角沿用穿衣工具的「步进式」变换——平移 /
//   旋转 / 缩放各给一个步长，点 < / > 按钮按步长作用一次。与穿衣工具不同，本工具
//   **没有软体仿真器**，变换直接**烘进 target 的 CPU 顶点**（每次点击改一次几何，
//   再把新顶点推上 GPU）。
//
// 约定：
//   - 平移：顶点点位整体加 delta。
//   - 旋转：绕**给定枢轴**、绕世界 X/Y/Z 轴**逆时针**转 deg 度（右手法则，从轴正向
//     朝原点看为逆时针）；位置与**法线、切线**一起转（方向量不因平移改变）。
//   - 缩放：绕枢轴**均匀**缩放 factor 倍；位置缩放，法线方向不因均匀缩放改变，
//     切线按 factor 缩放（遵守「切线不归一化」的项目约定）。
//   - 以上三者都不改变属性 flags、index、UV、骨权（只动几何/方向量）。
//
// Pre-condition（不满足即 LOG(FATAL)）：mesh != nullptr 且 Validate() 通过；
//   axis ∈ {0,1,2}；factor > 0。

#ifndef JPOV_MODEL_EDITOR_MODEL_TRANSFORM_H_
#define JPOV_MODEL_EDITOR_MODEL_TRANSFORM_H_

#include <cmath>

#include <glog/logging.h>

#include "tools/jpov/interface/mesh.h"

namespace jpov {
namespace model_editor {

// 轴对齐包围盒（合并各 primitive 用）。valid == false 表示无可用顶点。
struct MeshBounds {
    jpov::Vec3f min{0.0f, 0.0f, 0.0f};
    jpov::Vec3f max{0.0f, 0.0f, 0.0f};
    bool valid = false;
};

// 取单个 mesh 的轴对齐包围盒。
// Pre-condition: mesh.Validate() 通过（positions 非空）。
inline MeshBounds ComputeMeshBounds(const jpov::MeshData& mesh) {
    mesh.Validate();
    MeshBounds b;
    b.min = mesh.positions[0];
    b.max = mesh.positions[0];
    for (const jpov::Vec3f& p : mesh.positions) {
        b.min = jpov::Vec3f(std::min(b.min.x(), p.x()),
                            std::min(b.min.y(), p.y()),
                            std::min(b.min.z(), p.z()));
        b.max = jpov::Vec3f(std::max(b.max.x(), p.x()),
                            std::max(b.max.y(), p.y()),
                            std::max(b.max.z(), p.z()));
    }
    b.valid = true;
    return b;
}

// 平移：所有顶点点位加 delta（法线 / 切线不受平移影响）。
inline void TranslateMesh(jpov::MeshData* mesh /*inout*/, const jpov::Vec3f& delta) {
    CHECK(mesh != nullptr);
    mesh->Validate();
    for (jpov::Vec3f& p : mesh->positions) {
        p = jpov::Vec3f(p.x() + delta.x(), p.y() + delta.y(), p.z() + delta.z());
    }
}

// 绕世界轴 axis（0=X,1=Y,2=Z）逆时针旋转 deg 度。
// 位置绕 pivot 旋转；法线 / 切线只转向量（不平移）。
// Pre-condition: 0 <= axis < 3。
inline void RotateMesh(jpov::MeshData* mesh /*inout*/, int axis, float deg,
                       const jpov::Vec3f& pivot) {
    CHECK(mesh != nullptr);
    CHECK_GE(axis, 0);
    CHECK_LT(axis, 3);
    mesh->Validate();

    constexpr float kPi = 3.14159265358979323846f;
    const float rad = deg * kPi / 180.0f;
    const float c = std::cos(rad);
    const float s = std::sin(rad);

    const auto rotate = [axis, c, s](const jpov::Vec3f& v) -> jpov::Vec3f {
        const float x = v.x();
        const float y = v.y();
        const float z = v.z();
        switch (axis) {
            case 0:  // 绕 X：y,z 平面
                return jpov::Vec3f(x, y * c - z * s, y * s + z * c);
            case 1:  // 绕 Y：z,x 平面
                return jpov::Vec3f(x * c + z * s, y, -x * s + z * c);
            default:  // 绕 Z：x,y 平面
                return jpov::Vec3f(x * c - y * s, x * s + y * c, z);
        }
    };

    for (jpov::Vec3f& p : mesh->positions) {
        const jpov::Vec3f rel(p.x() - pivot.x(), p.y() - pivot.y(),
                              p.z() - pivot.z());
        const jpov::Vec3f r = rotate(rel);
        p = jpov::Vec3f(r.x() + pivot.x(), r.y() + pivot.y(), r.z() + pivot.z());
    }
    for (jpov::Vec3f& n : mesh->normals) {
        n = rotate(n);
    }
    for (jpov::Vec3f& t : mesh->tangents) {
        t = rotate(t);
    }
}

// 绕 pivot 均匀缩放 factor 倍。位置缩放；法线方向不变；切线乘 factor。
// Pre-condition: factor > 0。
inline void ScaleMesh(jpov::MeshData* mesh /*inout*/, float factor,
                      const jpov::Vec3f& pivot) {
    CHECK(mesh != nullptr);
    CHECK_GT(factor, 0.0f);
    mesh->Validate();
    for (jpov::Vec3f& p : mesh->positions) {
        p = jpov::Vec3f(pivot.x() + (p.x() - pivot.x()) * factor,
                        pivot.y() + (p.y() - pivot.y()) * factor,
                        pivot.z() + (p.z() - pivot.z()) * factor);
    }
    for (jpov::Vec3f& t : mesh->tangents) {
        t = jpov::Vec3f(t.x() * factor, t.y() * factor, t.z() * factor);
    }
}

}  // namespace model_editor
}  // namespace jpov

#endif  // JPOV_MODEL_EDITOR_MODEL_TRANSFORM_H_
