// JPOV skeleton gold 公共场景（M1：蒙皮 T-rest 蓝人 + A/B object3d 对照）
//
// generator(写仓库 gold PNG) 与 test(渲一帧与 gold 逐像素 diff) 共用同一场景，
// 保证两者帧完全一致 —— M1 验证：左=真蒙皮(bind pose)渲, 右=object3d 直画同 mesh+材质。
// 若蒙皮链路正确(bind 下 M_i=I), 两人渲染应几乎相同 → 像素级门禁。
#ifndef JPOV_TEST_SKELETON_JPOV_SKELETON_GOLD_COMMON_H_
#define JPOV_TEST_SKELETON_JPOV_SKELETON_GOLD_COMMON_H_

#include <cstdint>
#include <string>
#include <vector>

#include "glog/logging.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/gltf_object.h"
#include "tools/jpov/src/gltf_loader.h"
#include "tools/jpov/src/skeleton/skinning_bind_pose.h"

namespace jpov_skeleton_gold {

inline std::string GetGoldRelPath() { return "/skeleton/human_skeleton_gold_t_rest_1280x720.png"; }

// test/generator 输出帧格式常量（与 RunOnce winfo 一致：640x360）。
constexpr int kOutW = 640;
constexpr int kOutH = 360;

// 蒙皮渲染场景 app：把「同一画布」画成两人(左 skinned, 右 object3d) + sunny-day base。
class SkeletonGoldApp : public JPOV {
public:
    using JPOV::JPOV;

    struct Slot { jpov::GltfObject obj; jpov::Vec3f center, up, front; };
    std::vector<Slot> slots_;   // 地面 tiles + 人物

    uint32_t mesh_id_ = 0;   // 蓝人 mesh(mixamo_male) 已注册
    uint32_t skel_id_ = 0;   // 蓝人骨架 SkeletonManager id
    jpov::PBRMaterial mat_;  // 蓝人材质(baseColor 贴图等)

    void AddSlot(jpov::GltfObject obj, jpov::Vec3f c, jpov::Vec3f u, jpov::Vec3f f) {
        slots_.push_back({std::move(obj), c, u, f});
    }

    void OneIteration(int64_t, const jpov::InputSnapshot&,
                      const jpov::WindowInfo&, jpov::RenderCommandList* cmds) override {
        constexpr float kResW = 1280.0f, kResH = 720.0f;
        cmds->camera.fbo_3d_width_  = kResW;
        cmds->camera.fbo_3d_height_ = kResH;
        const jpov::Vec3f center{0.0f, 1.0f, 0.0f};
        cmds->camera.position = {4.0f, 2.0f, 4.0f};
        cmds->camera.target   = center;
        cmds->camera.near     = 0.05f;

        // sunny-day：太阳(斜上) + 天光(Preetham) + 环境光 + tone_mapping。
        const jpov::Vec3f sun_l = {0.0f, -1.0f, -1.0f};
        cmds->sun = jpov::DirectionalLight{{sun_l}, {1,1,1,1}, 3.0f};
        cmds->ambient = jpov::AmbientLight{.color = {1,1,1,1}, .intensity = 0.3f};
        cmds->sky = jpov::DaySkyCommand{
            jpov::Vec3f(-sun_l.x(), -sun_l.y(), -sun_l.z()),
            2.0f, {1,1,1,1}, 1.0f, {0.05f,0.06f,0.08f,1}, 0.02, 1e3, 1.0};
        cmds->tone_mapping = true;

        // 地面（DrawGltfObject → object3d；让照明/tile 设置跑）+ 右 object3d 人。
        for (const Slot& s : slots_) cmds->DrawGltfObject(s.obj, s.center, s.up, s.front);
        // 右：object3d 直画同 mesh+材质（rest T-pose, 非蒙皮 —— 对照基准）
        if (mesh_id_ != 0)
            cmds->DrawObject3D(mesh_id_, mat_,
                               /*center*/ {2.0f,0,0}, /*up*/ {0,0,-1}, /*front*/ {0,1,0},
                               /*scale*/ 4.0f, /*highlight*/ false, /*picking_id*/ 0);
        // 左：真蒙皮（bind pose, pose_a==pose_b=0）
        if (mesh_id_ != 0 && skel_id_ != 0) {
            jpov::SkinnedInstanceState inst;
            inst.center = {0,0,0}; inst.up = {0,0,-1}; inst.front = {0,1,0};
            inst.scale = 4.0f; inst.pose_a = 0; inst.pose_b = 0; inst.ratio = 0.0f;
            std::vector<jpov::SkinnedInstanceState> instances{inst};
            cmds->DrawMeshWithSkeleton(mesh_id_, skel_id_, mat_, std::move(instances));
        }
    }
};

// 搭建整个场景（须 app 已 Init()）：地面 tiles + 蒙皮人(A/B 共用 mixamo_male mesh/材质/skeleton)。
inline void BuildScene(SkeletonGoldApp* app, const std::string& scene_assets_dir,
                       const std::string& male_glb) {
    const auto load = [app](const std::string& p) {
        jpov::GltfObject o = app->LoadGltf(p);
        CHECK(!o.empty()) << "LoadGltf failed: " << p;
        return o;
    };
    // 地面 5x5 tiles
    for (int iz = 0; iz < 5; ++iz) {
        for (int ix = 0; ix < 5; ++ix) {
            const float gx = -12.0f + ix * 6.0f, gz = -12.0f + iz * 6.0f;
            const bool brick = (gx < 0.0f);
            auto ground = load(scene_assets_dir + (brick ? "ground/ground_brick.gltf"
                                                         : "ground/ground_dirt.gltf"));
            app->AddSlot(std::move(ground), {gx, -0.5f, gz}, {0,1,0}, {0,0,1});
        }
    }
    // 蓝人(蒙皮 rest mesh + 材质) + 骨架(bind pose)
    jpov::GltfObject male = load(male_glb);
    CHECK(!male.primitives.empty()) << male_glb << " 应含蒙皮 primitive";
    std::vector<jpov::SkeletonType> skels;
    CHECK(jpov::LoadGltfSkeleton(male_glb, &skels));
    CHECK(!skels.empty()) << "LoadGltfSkeleton 失败";
    std::vector<jpov::SkeletonPose> poses;
    poses.push_back(jpov::MakeMixamoBindPose());
    app->skel_id_ = app->RegisterSkeleton(skels[0], poses);
    app->mesh_id_ = male.primitives[0].mesh_id;
    app->mat_ = male.primitives[0].material;
}

}  // namespace jpov_skeleton_gold

#endif  // JPOV_TEST_SKELETON_JPOV_SKELETON_GOLD_COMMON_H_
