// JPOV 火柴人（skeleton mesh）gold 公共场景
//
// 以标准晴天（standard_sunny_day）为底：保留**地面 + 天光天色 + 太阳 + 阴影**，
// 在场景中央以 **identity 位姿** 渲染一根由 `BuildBoneMeshInBoneSpace` 生成的火柴人
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
#include "tools/jpov/interface/mixamo23_skeleton.h"
#include "tools/jpov/interface/skeleton_mesh.h"
#include "tools/jpov/interface/skeleton_types.h"

namespace jpov_bone_mesh_gold {

// gold image 仓库相对路径（generator 写入、test 读取比对）。
inline std::string GetGoldRelPath() {
    return "/skeleton/bone_mesh_stickman_1280x720.png";
}

// “无火柴人”基线图相对路径：同场景但不画火柴人。
// 供 test 做**火柴人可见性门禁**（两图相减，差异只可能来自火柴人）——
// 单纯“红色像素占比”会被偏红的砖地面淹没，见 test 内注释。
inline std::string GetNoBoneRelPath() {
    return "/skeleton/bone_mesh_stickman_1280x720_nobone.png";
}

// 火柴人身高（米）—— 与标准晴天场景里的家具尺度协调（桌 0.71 高）。
inline constexpr float kStickmanHeight = 1.75f;
// 骨杆半宽（米）。
inline constexpr float kBoneRadius = 0.04f;

// 渲染应用：标准晴天布景 + identity 位姿红火柴人。
class BoneMeshGoldApp : public JPOV {
public:
    using JPOV::JPOV;

    struct Slot {
        jpov::GltfObject obj;
        jpov::Vec3f center, up, front;
    };
    std::vector<Slot> slots_;      // 地面 tiles
    uint32_t stickman_mesh_ = 0;   // 火柴人 mesh（BuildBoneMeshInBoneSpace 产物）

    // draw_stickman=false 时输出“无火柴人”基线帧（其余完全一致），
    // 供 test 与正常帧相减做可见性门禁。
    bool draw_stickman_ = true;

    void AddGround(jpov::GltfObject obj, jpov::Vec3f c, jpov::Vec3f u, jpov::Vec3f f) {
        slots_.push_back({std::move(obj), c, u, f});
    }

    void OneIteration(int64_t, const jpov::InputSnapshot&,
                      const jpov::WindowInfo&, jpov::RenderCommandList* cmds) override {
        constexpr float kResW = 1280.0f, kResH = 720.0f;
        cmds->camera.fbo_3d_width_  = kResW;
        cmds->camera.fbo_3d_height_ = kResH;
        // 相机对准火柴人所在区域（原点附近），与标准晴天一致的斜俯视角。
        cmds->camera.position = {4.0f, 2.5f, 4.0f};
        cmds->camera.target   = {0.0f, 1.0f, 0.0f};
        cmds->camera.near     = 0.05f;

        // 太阳（斜上）+ 天光（Preetham 正午晴天）+ 环境光 + HDR tone map
        // —— 与 standard_sunny_day 完全一致，保证地面/天色沿用同一套基准。
        const jpov::Vec3f sun_l = {0.0f, -1.0f, -1.0f};
        cmds->sun = jpov::DirectionalLight{{sun_l}, {1, 1, 1, 1}, 3.0f};
        cmds->ambient = jpov::AmbientLight{.color = {1, 1, 1, 1}, .intensity = 0.3f};
        cmds->sky = jpov::DaySkyCommand{
            jpov::Vec3f(-sun_l.x(), -sun_l.y(), -sun_l.z()),
            /*turbidity*/ 2.0f, /*season*/ {1, 1, 1, 1}, /*intensity*/ 1.0f,
            /*ground_color*/ {0.05f, 0.06f, 0.08f, 1.0f},
            /*sun_radius*/ 0.02, /*sun_brightness*/ 1e3, /*sun_glow*/ 1.0};
        cmds->tone_mapping = true;

        // 地面（DrawGltfObject → object3d 路径）。
        for (const Slot& s : slots_) {
            cmds->DrawGltfObject(s.obj, s.center, s.up, s.front);
        }

        // 火柴人：identity 位姿（center=0 / up=+Y / front=+Z / scale=1）
        // ⇒ 模型系 == 世界系，火柴人直接立在骨架空间原点（根关节在 (0,0,0)）。
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

// 搭建场景（须 app 已 Init()）：地面 tiles + 火柴人 mesh 注册。
// 地面沿用标准晴天的 5×5 砖/土 tile（与 standard_sunny_day 同款坐标）。
inline void BuildScene(BoneMeshGoldApp* app,
                       const std::string& ground_brick_gltf,
                       const std::string& ground_dirt_gltf) {
    // 地面：5×5 = 25 块 6×6 小 quad 平铺（砖/土各半，同 standard_sunny_day）。
    for (int iz = 0; iz < 5; ++iz) {
        for (int ix = 0; ix < 5; ++ix) {
            const float gx = -12.0f + ix * 6.0f;
            const float gz = -12.0f + iz * 6.0f;
            const bool brick = (gx < 0.0f);
            jpov::GltfObject g = app->LoadGltf(brick ? ground_brick_gltf
                                                     : ground_dirt_gltf);
            CHECK(!g.empty()) << "LoadGltf failed: "
                              << (brick ? ground_brick_gltf : ground_dirt_gltf);
            app->AddGround(std::move(g), {gx, -0.5f, gz}, {0, 1, 0}, {0, 0, 1});
        }
    }

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
