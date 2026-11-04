// JPOV Skeleton — 程序化「蓝人模特」网格（GL-free）。
//
// 为什么需要它（2026-11-04 Danis）：动捕视频验收时，火柴人（bone rods）只能看出
// 「骨架有没有摆对」，看不出体态/重心/朝向，表达力不够；而现成的 Tripo glb
// (`mixamo_male.glb`) 的 rest 网格本身是**蹲姿**、骨长也只有 1.0m 级，拿它当
// 对比模特会把「重定向错」和「资产 rest 错」混在一起。
//
// 本文件生成一个**与 Mixamo23 骨架同源**的蓝人模特：
//   - 顶点在骨架空间（root 在原点、Y-up、模型正面 +Z）；
//   - 每根骨 = 一个胶囊（圆柱 + 两端球），刚性权重 100% 给该骨；
//   - 关节处球体互相重叠，弯曲时不会露出明显断口（低模人台的常用做法）；
//   - 不读文件、不碰 GL，输出可直接 RegisterMesh + SkeletonManager 蒙皮。
//
// 边界：这是**验收用模特**，不是角色资产。刚体权重在肘/膝弯曲角度很大时会看到
// 关节处的体积损失；需要更高质量时换真正的蒙皮角色，而不是在本文件里继续加几何。

#ifndef JPOV_INTERFACE_SKELETON_MANNEQUIN_H_
#define JPOV_INTERFACE_SKELETON_MANNEQUIN_H_

#include <cmath>
#include <string>
#include <vector>

#include <glog/logging.h>
#include "geom/common/quaternion.h"
#include "geom/common/vec.h"
#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/interface/skeleton_types.h"

namespace jpov {

inline constexpr int kMannequinSides = 10;
inline constexpr int kMannequinSphereStacks = 6;
inline constexpr int kMannequinSphereSlices = 10;

// 一根骨的视觉半径（米，按 1.75m 骨架给出；最终 × radius_scale）。
inline float MannequinBoneRadius(const std::string& name) {
    if (name.find("Hips") != std::string::npos) {
        return 0.105f;
    }
    if (name.find("Spine") != std::string::npos) {
        return 0.10f;
    }
    if (name.find("Neck") != std::string::npos) {
        return 0.045f;
    }
    if (name.find("Head") != std::string::npos) {
        return 0.095f;
    }
    if (name.find("Shoulder") != std::string::npos) {
        return 0.055f;
    }
    if (name.find("ForeArm") != std::string::npos) {
        return 0.042f;
    }
    if (name.find("Arm") != std::string::npos) {
        return 0.052f;
    }
    if (name.find("Hand") != std::string::npos) {
        return 0.034f;
    }
    if (name.find("UpLeg") != std::string::npos) {
        return 0.075f;
    }
    if (name.find("Leg") != std::string::npos) {
        return 0.060f;
    }
    if (name.find("Toe") != std::string::npos) {
        return 0.034f;
    }
    if (name.find("Foot") != std::string::npos) {
        return 0.050f;
    }
    return 0.040f;
}

namespace mannequin_internal {

inline void AddVertex(MeshData* mesh, const Vec3f& position, const Vec3f& normal,
                      int joint) {
    CHECK(mesh != nullptr);
    CHECK_GE(joint, 0);
    mesh->positions.push_back(position);
    mesh->normals.push_back(normal.Norm() > 1e-8f ? normal.Unit() : Vec3f{0, 1, 0});
    mesh->joint_indices.push_back(std::array<int32_t, 4>{joint, 0, 0, 0});
    mesh->joint_weights.push_back(std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f});
}

inline Vec3f AnyPerpendicular(const Vec3f& axis) {
    Vec3f candidate = std::abs(axis.y()) < 0.9f ? Vec3f{0, 1, 0} : Vec3f{1, 0, 0};
    Vec3f u = Vec3f{axis.y() * candidate.z() - axis.z() * candidate.y(),
                    axis.z() * candidate.x() - axis.x() * candidate.z(),
                    axis.x() * candidate.y() - axis.y() * candidate.x()};
    CHECK_GT(u.Norm(), 1e-6f) << "AnyPerpendicular: 轴退化";
    return u.Unit();
}

// 圆柱（无端盖；两端由球体盖住）。
inline void AddCylinder(MeshData* mesh, const Vec3f& start, const Vec3f& end,
                        float radius, int joint) {
    const Vec3f axis = end - start;
    const float length = axis.Norm();
    if (length < 1e-5f || radius <= 0.0f) {
        return;
    }
    const Vec3f w = axis * (1.0f / length);
    const Vec3f u = AnyPerpendicular(w);
    const Vec3f v = Vec3f{w.y() * u.z() - w.z() * u.y(),
                          w.z() * u.x() - w.x() * u.z(),
                          w.x() * u.y() - w.y() * u.x()};
    const uint32_t base = static_cast<uint32_t>(mesh->positions.size());
    for (int i = 0; i < kMannequinSides; ++i) {
        const float a = 2.0f * 3.14159265358979323846f * static_cast<float>(i) /
                        static_cast<float>(kMannequinSides);
        const Vec3f radial = u * std::cos(a) + v * std::sin(a);
        AddVertex(mesh, start + radial * radius, radial, joint);
        AddVertex(mesh, end + radial * radius, radial, joint);
    }
    for (int i = 0; i < kMannequinSides; ++i) {
        const uint32_t i0 = base + static_cast<uint32_t>(i) * 2;
        const uint32_t i1 = base + static_cast<uint32_t>((i + 1) % kMannequinSides) * 2;
        const uint32_t i2 = i0 + 1;
        const uint32_t i3 = i1 + 1;
        mesh->indices.push_back(i0);
        mesh->indices.push_back(i1);
        mesh->indices.push_back(i3);
        mesh->indices.push_back(i0);
        mesh->indices.push_back(i3);
        mesh->indices.push_back(i2);
    }
}

inline void AddSphere(MeshData* mesh, const Vec3f& center, float radius, int joint) {
    if (radius <= 0.0f) {
        return;
    }
    const uint32_t base = static_cast<uint32_t>(mesh->positions.size());
    for (int stack = 0; stack <= kMannequinSphereStacks; ++stack) {
        const float phi = 3.14159265358979323846f * static_cast<float>(stack) /
                          static_cast<float>(kMannequinSphereStacks);
        const float y = std::cos(phi);
        const float r = std::sin(phi);
        for (int slice = 0; slice < kMannequinSphereSlices; ++slice) {
            const float theta = 2.0f * 3.14159265358979323846f *
                                static_cast<float>(slice) /
                                static_cast<float>(kMannequinSphereSlices);
            const Vec3f n{r * std::cos(theta), y, r * std::sin(theta)};
            AddVertex(mesh, center + n * radius, n, joint);
        }
    }
    const int ring = kMannequinSphereSlices;
    for (int stack = 0; stack < kMannequinSphereStacks; ++stack) {
        for (int slice = 0; slice < ring; ++slice) {
            const uint32_t a = base + static_cast<uint32_t>(stack * ring + slice);
            const uint32_t b = base + static_cast<uint32_t>(stack * ring + (slice + 1) % ring);
            const uint32_t c = base + static_cast<uint32_t>((stack + 1) * ring + slice);
            const uint32_t d = base + static_cast<uint32_t>((stack + 1) * ring + (slice + 1) % ring);
            mesh->indices.push_back(a);
            mesh->indices.push_back(b);
            mesh->indices.push_back(d);
            mesh->indices.push_back(a);
            mesh->indices.push_back(d);
            mesh->indices.push_back(c);
        }
    }
}

}  // namespace mannequin_internal

// 造一个与 skeleton 同源的蓝人模特（顶点在骨架空间，T-pose）。
//
// Pre-condition: skeleton.Validate() 通过；radius_scale > 0（1.0 = 1.75m 成人比例）。
// 返回 MeshData：flags = kPosition|kNormal|kJoints，可直接 RegisterMesh。
inline MeshData BuildMannequinMeshInBoneSpace(const SkeletonType& skeleton,
                                              float radius_scale = 1.0f) {
    skeleton.Validate();
    CHECK_GT(radius_scale, 0.0f)
        << "BuildMannequinMeshInBoneSpace: radius_scale 必须 >0";

    // rest 世界朝向 + 位置（与 skeleton_mesh.h 同一条复合规则）。
    const size_t n = skeleton.joints.size();
    std::vector<geom::Quaternion<float>> world_rot(n, geom::Quaternion<float>::Identity());
    std::vector<Vec3f> world_pos(n, Vec3f{});
    for (size_t j = 0; j < n; ++j) {
        const geom::Quaternion<float> bind = skeleton.bind_rotation.empty()
                                                 ? geom::Quaternion<float>::Identity()
                                                 : skeleton.bind_rotation[j];
        const int p = skeleton.joints[j].parent;
        world_rot[j] = (p == kSkeletonNoParent ? bind
                                               : world_rot[static_cast<size_t>(p)] * bind)
                           .Normalized();
        if (p == kSkeletonNoParent) {
            world_pos[j] = skeleton.joints[j].rest_offset;
        } else {
            world_pos[j] = world_pos[static_cast<size_t>(p)] +
                           geom::RotateVector(world_rot[static_cast<size_t>(p)],
                                              skeleton.joints[j].rest_offset);
        }
    }

    MeshData mesh;
    mesh.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(MeshVertexFlags::kNormal) |
        static_cast<uint8_t>(MeshVertexFlags::kJoints));

    // 关节球：每个关节一个，权重给**该关节自己**。球体互相重叠，负责盖住弯折时的接缝。
    //   ⚠️ 段与球的权重归属必须按拓扑：一段 j→child 由 **j** 控制（LBS 里绕 j 转），
    //   所以段权重给 j，而不是 child。早期版本按 child 权重生成，肘/膝会像断开的木偶
    //   （2026-11-04 实拍：大腿和小腿之间出现明显缺口）。
    for (size_t j = 0; j < n; ++j) {
        if (skeleton.joints[j].name.empty()) {
            continue;  // 包装 Root 不画
        }
        const float radius = MannequinBoneRadius(skeleton.joints[j].name) * radius_scale;
        mannequin_internal::AddSphere(&mesh, world_pos[j], radius, static_cast<int>(j));
    }

    // 段：每个非叶关节 → 每个子关节，权重给父关节（= 该段绕父关节转）。
    for (size_t j = 0; j < n; ++j) {
        if (skeleton.joints[j].name.empty()) {
            continue;  // 包装 Root 不画
        }
        const float radius = MannequinBoneRadius(skeleton.joints[j].name) * radius_scale;
        for (size_t c = 0; c < n; ++c) {
            if (skeleton.joints[c].parent != static_cast<int>(j)) {
                continue;
            }
            mannequin_internal::AddCylinder(&mesh, world_pos[j], world_pos[c],
                                            radius, static_cast<int>(j));
        }
    }

    // 头：Head 骨段很短，单独给一个更大的球（人台辨识度主要靠头）。
    for (size_t j = 0; j < n; ++j) {
        if (skeleton.joints[j].name.find("Head") != std::string::npos) {
            const Vec3f head_dir = geom::RotateVector(
                world_rot[j], Vec3f{0.0f, 1.0f, 0.0f});
            mannequin_internal::AddSphere(
                &mesh, world_pos[j] + head_dir * (0.070f * radius_scale),
                0.100f * radius_scale, static_cast<int>(j));
            break;
        }
    }

    mesh.Validate();
    return mesh;
}

}  // namespace jpov

#endif  // JPOV_INTERFACE_SKELETON_MANNEQUIN_H_
