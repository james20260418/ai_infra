// JPOV 火柴人（skeleton mesh）gold 公共场景
//
// 以标准晴天（standard_sunny_day）为底：保留**天光天色 + 太阳 + 阴影**，
// 地面改为**浅灰纯色平板**（不用 glTF 砖/土地块），板顶面精确落在 y=0 ——
// 于是骨架空间原点（根关节）正好踩在地面上，便于肉眼核对坐标架。
// 场景中央以 **identity 位姿** 渲染一根由 `BuildBoneMeshInBoneSpace` 生成的火柴人
// （纯红 PBR 材质）。
//
// 目的（docs/jpov_retarget_design.md §4）：
//   把"骨架空间下的 T-pose 火柴人"放进标准世界里显示，作为 retarget 人工配准的**对齐参照**：
//   资产 mesh（glb）与 skeleton 都按 identity 摆放，肉眼即可判断 mesh 是否贴骨。
//
// 生成链路：SkeletonType(Mixamo23Skeleton) → BuildBoneMeshInBoneSpace(identity pose)
//   → RegisterMesh → DrawObject3D(mesh_id, kColorRed) —— 火柴人是**静态带骨 mesh**，
//   本 PR 走 object3d 直画路径（蒙皮让 mesh 与骨架重合，见 §4.5：顶点姿态==rest，
//   inverse_bind 自算即为单位阵，故直画等价于正确蒙皮）。
//
// 本头文件被 generator（写仓库 gold image）和 test（渲染 + 门禁）共用，保证两者帧一致。

#ifndef JPOV_TEST_SKELETON_JPOV_BONE_MESH_GOLD_COMMON_H_
#define JPOV_TEST_SKELETON_JPOV_BONE_MESH_GOLD_COMMON_H_

#include <cstdint>
#include <string>
#include <vector>

#include "glog/logging.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/interface/mixamo23_skeleton.h"
#include "tools/jpov/interface/skeleton_mesh.h"
#include "tools/jpov/interface/skeleton_types.h"

namespace jpov_bone_mesh_gold {

// gold image 仓库相对路径（generator 写入、test 读取比对）。
inline std::string GetGoldRelPath() {
    return "/skeleton/bone_mesh_stickman_1280x720.png";
}

// "无火柴人"基线图相对路径：同场景但不画火柴人。
// 供 test 做**火柴人可见性门禁**（两图相减，差异只可能来自火柴人）——
// 单纯"红色像素占比"会被地面/家具的暖色淹没，见 test 内注释。
inline std::string GetNoBoneRelPath() {
    return "/skeleton/bone_mesh_stickman_1280x720_nobone.png";
}

// 火柴人身高（米）。
inline constexpr float kStickmanHeight = 1.75f;
// 骨杆半宽（米）—— 骨的"大致直径" ≈ 2·radius。
inline constexpr float kBoneRadius = 0.02f;
// 地面板半厚（米）：板中心下沉该值，使**顶面精确落在 y=0**
//（骨架空间原点 = 根关节 = 脚底，正好踩在地面上）。
inline constexpr float kGroundHalfThickness = 0.05f;
// 地面板半宽（米）—— 足够大以铺满视野。
inline constexpr float kGroundHalfSize = 30.0f;

// 渲染应用：浅灰纯色地面 + 天光天色 + identity 位姿红火柴人。
class BoneMeshGoldApp : public JPOV {
public:
    using JPOV::JPOV;

    uint32_t ground_mesh_ = 0;     // 浅灰纯色地面板
    uint32_t stickman_mesh_ = 0;   // 火柴人 mesh（BuildBoneMeshInBoneSpace 产物）

    // draw_stickman=false 时输出"无火柴人"基线帧（其余完全一致），
    // 供 test 与正常帧相减做可见性门禁。
    bool draw_stickman_ = true;

    void OneIteration(int64_t, const jpov::InputSnapshot&,
                      const jpov::WindowInfo&, jpov::RenderCommandList* cmds) override {
        constexpr float kResW = 1280.0f, kResH = 720.0f;
        cmds->camera.fbo_3d_width_  = kResW;
        cmds->camera.fbo_3d_height_ = kResH;
        // 相机对准火柴人（原点附近），斜俯视角。
        cmds->camera.position = {4.0f, 2.5f, 4.0f};
        cmds->camera.target   = {0.0f, 1.0f, 0.0f};
        cmds->camera.near     = 0.05f;

        // 太阳（斜上）+ 天光（Preetham 正午晴天）+ 环境光 + HDR tone map
        // —— 与 standard_sunny_day 一致的照明基准。
        const jpov::Vec3f sun_l = {0.0f, -1.0f, -1.0f};
        cmds->sun = jpov::DirectionalLight{{sun_l}, {1, 1, 1, 1}, 3.0f};
        cmds->ambient = jpov::AmbientLight{.color = {1, 1, 1, 1}, .intensity = 0.3f};
        cmds->sky = jpov::DaySkyCommand{
            jpov::Vec3f(-sun_l.x(), -sun_l.y(), -sun_l.z()),
            /*turbidity*/ 2.0f, /*season*/ {1, 1, 1, 1}, /*intensity*/ 1.0f,
            /*ground_color*/ {0.05f, 0.06f, 0.08f, 1.0f},
            /*sun_radius*/ 0.02, /*sun_brightness*/ 1e3, /*sun_glow*/ 1.0};
        cmds->tone_mapping = true;

        // 地面：浅灰纯色板，中心下沉 kGroundHalfThickness 使顶面贴 y=0。
        if (ground_mesh_ != 0) {
            cmds->DrawObject3D(
                ground_mesh_, jpov::PBRMaterial::SolidColor({0.75f, 0.75f, 0.75f, 1.0f}),
                /*center*/ {0, -kGroundHalfThickness, 0},
                /*up*/     {0, 1, 0},
                /*front*/  {0, 0, 1},
                /*scale*/  1.0f,
                /*highlight*/ false,
                /*picking_id*/ 0);
        }

        // 火柴人：identity 位姿（center=0 / up=+Y / front=+Z / scale=1）
        // ⇒ 模型系 == 世界系，火柴人直接立在骨架空间原点（根关节在 (0,0,0)，脚踩 y=0）。
        if (draw_stickman_ && stickman_mesh_ != 0) {
            cmds->DrawObject3D(stickman_mesh_, jpov::PBRMaterial::SolidColor(jpov::kColorRed),
                               /*center*/ {0, 0, 0},
                               /*up*/     {0, 1, 0},
                               /*front*/  {0, 0, 1},
                               /*scale*/  1.0f,
                               /*highlight*/ false,
                               /*picking_id*/ 0);
        }
    }
};

// 搭建场景（须 app 已 Init()）：浅灰地面板 + 火柴人 mesh 注册。
inline void BuildScene(BoneMeshGoldApp* app) {
    // 地面：一块扁 box（局部 +Z=front, +Y=up, +X=left），用 Draw 的 center 下沉使顶面贴 y=0。
    app->ground_mesh_ = app->RegisterMesh(jpov::MeshData::MakeBox(
        /*front_half_width*/ kGroundHalfSize,
        /*up_half_width*/    kGroundHalfThickness,
        /*left_half_width*/  kGroundHalfSize));

    // 火柴人：Mixamo23 骨架 + 全恒等 pose（= 该骨架的 T-pose）→ 骨架空间带骨 mesh。
    const jpov::SkeletonType type = jpov::Mixamo23Skeleton(kStickmanHeight);
    const jpov::SkeletonPose pose = jpov::SkeletonPose::Identity(type.bone_count());
    const jpov::MeshData stickman =
        jpov::BuildBoneMeshInBoneSpace(type, pose, kBoneRadius);
    app->stickman_mesh_ = app->RegisterMesh(stickman);
    LOG(INFO) << "火柴人 mesh: " << stickman.positions.size() << " 顶点, "
              << stickman.indices.size() << " 索引";
}

}  // namespace jpov_bone_mesh_gold

#endif  // JPOV_TEST_SKELETON_JPOV_BONE_MESH_GOLD_COMMON_H_
