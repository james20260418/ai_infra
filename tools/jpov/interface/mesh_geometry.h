// JPOV mesh_geometry — 顶点法线 / 切线空间的 CPU 重算（GL-free，header-only）
//
// 用途：动力学仿真（如 ARAP 布料贴合）每帧改动了顶点【位置】后，法线/切线这两类
//   「位置的派生量」立即失效。必须用【变形后的位置】重算，否则渲染会用「旧形状的
//   法线」去照亮「新形状的几何」——表现为阴影方向错乱、法线贴图打转。法线贴图本身
//   无需处理：它存的是切线空间下的【相对】方向，只要 TBN 的 N/T 跟着几何走即可。
//
// ═══════════ 约定（Danis 2026-09-22 定稿，实测依据见 PR 描述）═══════════
//
//   1. **自推、不焊接**：邻域取自【顶点索引】的拓扑邻接（三角形共享顶点），
//      **绝不按位置合并顶点**。理由：顶点分裂是数据的一部分——
//        · 分裂来源 A：UV 缝（贴图需要，几何上连续）
//        · 分裂来源 B：硬边 / split normal（几何上要求保留棱）
//      glTF/OBJ 格式**不区分**这两者（只给顶点元组）。焊接一刀切会把 B 的棱磨圆
//      （反例：直棱柱三条棱交会处一个位置三个顶点，法线分别是上/左/右面；焊接后
//      三者被平均 → 硬边消失）。因此渲染侧的法线推导**按索引**做，与所有标准
//      渲染器一致。
//      （对照：物理仿真侧**需要**按位置焊接，否则 UV 缝两侧顶点会被约束力拉开成
//      裂缝。两边拓扑不同是正常的——物理拓扑 ≠ 渲染拓扑。）
//
//   2. **面积加权**：累加【未归一化】的面法线（|cross| 正比于三角形面积），最后
//      整体归一化。等价于经典面积加权顶点法线。
//
//   3. **切线由 UV 推导**：T = ∂P/∂u，用【变形后的位置】+【不变的 UV】解 2×2 线性
//      方程组。公式与 obj_loader.cc::ComputeTangents / gltf_loader.cc 逐字一致
//      （per-face 单位化后累加再归一化），保证与既有 JPOV 管线同源、不产生第二套约定。
//      UV 不随几何变形改变，故 det(UV)（每个三角形的 UV 雅可比行列式）理论上是
//      不变量；本文件按「每帧重算」实现以保证 load 与每帧走同一函数（同源优先于
//      省一点计算）。
//
//   4. **B（副切线）不存**：shader 内 B = cross(N, T)，故本文件不产出副切线。
//
// 量纲：位置 = 模型局部坐标（无量纲单位）；UV = 无量纲（[0,1] 或平铺 >1）。
//
// 退化处理：det(UV) ≈ 0 的三角形（零面积 UV / 退化）跳过其切线贡献（不除零），
//   该三角形的顶点仍可由其余有效三角形获得切线；若某顶点所有面都退化，
//   其切线为 (0,0,0)（与既有 loader 行为一致）。

#ifndef JPOV_INTERFACE_MESH_GEOMETRY_H_
#define JPOV_INTERFACE_MESH_GEOMETRY_H_

#include <cmath>
#include <cstdint>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/interface/mesh.h"

namespace jpov {

// 切线推导的退化阈值（与 obj_loader.cc::ComputeTangents 保持一致）：
//   · UV 雅可比行列式 |det| < 1e-8 → 该三角形在 UV 空间退化，跳过切线
//   · 面切线长度 < 1e-8      → 该三角形几何退化（零面积），跳过切线
inline constexpr float kTangentDegenerateEps = 1e-8f;

// 用当前顶点位置重算【面积加权顶点法线】（按索引累加，不焊接）。
//
// 算法：对每个三角形求未归一化面法线 cross(p1-p0, p2-p0)（模长 = 2·面积），
// 累加到三个顶点的累加器；最后逐顶点归一化。
//
// 注意：累加的是【未归一化】叉积 ⇒ 面积加权；若要改角度加权（对翻转面更鲁棒），
// 需另外实现，本函数不做。
//
// Pre-condition:  mesh->positions 非空；indices 非空且为 3 的倍数，索引均在界内。
// Post-condition: mesh->normals 长度 == positions.size()，逐顶点单位向量。
//                 源索引中未出现在任何三角形里的顶点，其法线保持 (0,0,0)。
//
// 不修改 flags（应由调用方确保已声明 kNormal）。
void RecomputeVertexNormals(MeshData* mesh);

// 用当前顶点位置 + 不变的 UV 重算【逐顶点切线】（按索引累加，不焊接）。
//
// 算法（每个三角形）：
//   e1 = p1 - p0,  e2 = p2 - p0
//   dUV1 = uv1 - uv0,  dUV2 = uv2 - uv0
//   det = dUV1.x * dUV2.y - dUV2.x * dUV1.y        （UV 雅可比行列式）
//   t   = (dUV2.y * e1 - dUV1.y * e2) / det        （= ∂P/∂u）
// 逐条单位化后累加到三个顶点，最后逐顶点归一化。
//
// Pre-condition:  mesh->positions 非空；indices 非空且为 3 的倍数、索引在界内；
//                 mesh->uvs 长度 == positions.size()。
// Post-condition: mesh->tangents 长度 == positions.size()。
//                 返回 true 表示至少产出一条非零切线；false 表示全部退化
//                 （此时 tangents 全为 (0,0,0)）。
bool RecomputeVertexTangents(MeshData* mesh);

// 一步到位：用当前 positions 重算法线；若声明了 kTangent 则连同切线一起重算。
//
// 调用场景：load 时（第 0 帧）+ 每帧变形后，两次都走本函数，保证「load 时补的」
// 与「每帧重算的」是同一份代码、同一套约定（同源）。
//
// Pre-condition:  flags 已声明 kNormal（本函数填 normals）；
//                 若声明 kTangent，则必须同时声明 kUV 且 uvs 非空
//                 （MeshData 不变式：kTangent ⇒ kNormal + kUV）。
// Post-condition: normals 已按当前 positions 重算；
//                 声明 kTangent 时 tangents 亦已重算（全退化时记 WARNING 并保持全零，
//                 由调用方决定是否摘掉材质的法线贴图——本函数不擅自改 flags）。
void RecomputeTangentSpace(MeshData* mesh);

// ───────────────────────────── 实现 ─────────────────────────────

inline void RecomputeVertexNormals(MeshData* mesh) {
    CHECK_NOTNULL(mesh);
    const size_t vcount = mesh->positions.size();
    CHECK_GT(vcount, 0u) << "RecomputeVertexNormals: positions 不能为空";
    CHECK(!mesh->indices.empty()) << "RecomputeVertexNormals: indices 不能为空";
    CHECK_EQ(mesh->indices.size() % 3, 0u)
        << "RecomputeVertexNormals: indices 必须是 3 的倍数（triangle list）";

    std::vector<Vec3f> accum(vcount, Vec3f(0.0f, 0.0f, 0.0f));
    const size_t num_tris = mesh->indices.size() / 3;
    for (size_t t = 0; t < num_tris; ++t) {
        const uint32_t i0 = mesh->indices[t * 3 + 0];
        const uint32_t i1 = mesh->indices[t * 3 + 1];
        const uint32_t i2 = mesh->indices[t * 3 + 2];
        CHECK_LT(i0, vcount);
        CHECK_LT(i1, vcount);
        CHECK_LT(i2, vcount);

        const Vec3f& p0 = mesh->positions[i0];
        const Vec3f& p1 = mesh->positions[i1];
        const Vec3f& p2 = mesh->positions[i2];
        // 未归一化叉积：模长 = 2·面积 ⇒ 累加即面积加权。
        const Vec3f face_n = (p1 - p0).Cross(p2 - p0);
        accum[i0] += face_n;
        accum[i1] += face_n;
        accum[i2] += face_n;
    }

    mesh->normals.resize(vcount);
    for (size_t i = 0; i < vcount; ++i) {
        const float len = accum[i].Norm();
        if (len < 1e-12f) {
            mesh->normals[i] = Vec3f(0.0f, 0.0f, 1.0f);  // 孤立/退化顶点：给一个确定值
        } else {
            mesh->normals[i] = accum[i] * (1.0f / len);
        }
    }
}

inline bool RecomputeVertexTangents(MeshData* mesh) {
    CHECK_NOTNULL(mesh);
    const size_t vcount = mesh->positions.size();
    CHECK_GT(vcount, 0u) << "RecomputeVertexTangents: positions 不能为空";
    CHECK(!mesh->indices.empty()) << "RecomputeVertexTangents: indices 不能为空";
    CHECK_EQ(mesh->indices.size() % 3, 0u)
        << "RecomputeVertexTangents: indices 必须是 3 的倍数（triangle list）";
    CHECK_EQ(mesh->uvs.size(), vcount)
        << "RecomputeVertexTangents: uvs 长度必须与 positions 一致";

    std::vector<Vec3f> accum(vcount, Vec3f(0.0f, 0.0f, 0.0f));
    const size_t num_tris = mesh->indices.size() / 3;
    for (size_t t = 0; t < num_tris; ++t) {
        const uint32_t i0 = mesh->indices[t * 3 + 0];
        const uint32_t i1 = mesh->indices[t * 3 + 1];
        const uint32_t i2 = mesh->indices[t * 3 + 2];
        CHECK_LT(i0, vcount);
        CHECK_LT(i1, vcount);
        CHECK_LT(i2, vcount);

        const Vec3f& p0 = mesh->positions[i0];
        const Vec3f& p1 = mesh->positions[i1];
        const Vec3f& p2 = mesh->positions[i2];
        const Vec2f& uv0 = mesh->uvs[i0];
        const Vec2f& uv1 = mesh->uvs[i1];
        const Vec2f& uv2 = mesh->uvs[i2];

        const Vec3f edge1 = p1 - p0;
        const Vec3f edge2 = p2 - p0;
        const float duv1x = uv1.x() - uv0.x();
        const float duv1y = uv1.y() - uv0.y();
        const float duv2x = uv2.x() - uv0.x();
        const float duv2y = uv2.y() - uv0.y();

        // UV 雅可比行列式过小（退化 UV）→ 跳过本三角形，不除零。
        const float det = duv1x * duv2y - duv2x * duv1y;
        if (std::fabs(det) < kTangentDegenerateEps) {
            continue;
        }
        const float r = 1.0f / det;
        // t = (dUV2.y * e1 - dUV1.y * e2) / det，即 ∂P/∂u。
        const Vec3f tang = (edge1 * duv2y - edge2 * duv1y) * r;
        const float len = tang.Norm();
        if (len < kTangentDegenerateEps) {
            continue;
        }
        const Vec3f tang_unit = tang * (1.0f / len);
        accum[i0] += tang_unit;
        accum[i1] += tang_unit;
        accum[i2] += tang_unit;
    }

    mesh->tangents.resize(vcount);
    bool any_valid = false;
    for (size_t i = 0; i < vcount; ++i) {
        const float len = accum[i].Norm();
        if (len < kTangentDegenerateEps) {
            mesh->tangents[i] = Vec3f(0.0f, 0.0f, 0.0f);
        } else {
            mesh->tangents[i] = accum[i] * (1.0f / len);
            any_valid = true;
        }
    }
    return any_valid;
}

inline void RecomputeTangentSpace(MeshData* mesh) {
    CHECK_NOTNULL(mesh);
    CHECK(MeshHasFlag(mesh->flags, MeshVertexFlags::kNormal))
        << "RecomputeTangentSpace: flags 必须声明 kNormal";
    RecomputeVertexNormals(mesh);

    if (!MeshHasFlag(mesh->flags, MeshVertexFlags::kTangent)) {
        return;  // 未声明切线：只算法线，不碰 tangents（保持空，满足 MeshData 不变式）。
    }
    CHECK(MeshHasFlag(mesh->flags, MeshVertexFlags::kUV))
        << "RecomputeTangentSpace: 声明 kTangent 时 flags 必须同时声明 kUV";
    CHECK_EQ(mesh->uvs.size(), mesh->positions.size())
        << "RecomputeTangentSpace: 声明 kTangent 时 uvs 必须与 positions 等长";

    const bool any_valid = RecomputeVertexTangents(mesh);
    if (!any_valid) {
        // 全部三角形切线退化：tangents 已置全零。不擅自摘 flags（调用方决定是否
        // 退化为「无法线贴图」渲染）；此处显式告警，便于定位退化资产。
        LOG(WARNING) << "RecomputeTangentSpace: 切线全部退化（无有效 UV 三角形），"
                        "tangents 全零；若材质带法线贴图将无法构建 TBN";
    }
}

}  // namespace jpov

#endif  // JPOV_INTERFACE_MESH_GEOMETRY_H_
