// JPOV Gold Image Generator — 骨架蒙皮 T-rest(blue 人) 单 pose 静态验证
//
// 验证骨架蒙皮渲染的基础链路（M1）：rest 蓝人(mixamo_male.glb) 用 bind pose(cover)
// 经真蒙皮 VS 渲出 T-pose，叠加在 standard_sunny_day 的 base(地面 + 天光 + 照明)上。
//
// base（Danis 调试指引）：先渲出不带人物的 sunny-day 底（地面 tiles + 天光 Preetham +
// sun + ambient + tone_mapping），确认它本就不黑；再叠蒙皮人。
// 这里直接给 generator 加 base + 人，二者同帧；base 有 object3d → 照明/tile-light
// 设置会跑（否则只有 skinned_mesh 时 object3d 为空，光照段被跳过）。
//
// 数据流：
//   JPOV::LoadGltf(mixamo_male.glb)  → GltfObject{primitives[0].mesh_id + material(带 baseColor 贴图)}
//   JPOV::LoadGltfSkeleton(...)       → SkeletonType(23 骨树 + inverseBind, 无 pose)
//   jpov::MakeMixamoBindPose()        → bind pose(cover)
//   RegisterSkeleton(type, {bindPose}) → skeleton_id
//   OneIteration: 画base(地面 tiles via DrawGltfObject) + DrawMeshWithSkeleton(mesh_id, skeleton_id, material, 静态实例)
#include <cstdint>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/gltf_object.h"
#include "tools/jpov/src/gltf_loader.h"
#include "tools/jpov/src/skeleton/skinning_bind_pose.h"
#include "tools/jpov/test/test_utils.h"

namespace {

std::string AssetPath(const std::string& rel) {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        return p + "__main__/tools/jpov/test/object3d/scene_assets/" + rel;
    }
    return jpov::GetProjectRoot() +
        "tools/jpov/test/object3d/scene_assets/" + rel;
}

}  // namespace

class SkeletonGoldGenerator : public JPOV {
public:
    using JPOV::JPOV;

    struct Slot {
        jpov::GltfObject obj;
        jpov::Vec3f center, up, front;
    };
    std::vector<Slot> slots_;

    void AddSlot(jpov::GltfObject obj, jpov::Vec3f cen,
                 jpov::Vec3f up, jpov::Vec3f front) {
        slots_.push_back({std::move(obj), cen, up, front});
    }

    void SetSkinned(jpov::GltfObject obj, uint32_t mesh_id,
                    uint32_t skel_id, jpov::PBRMaterial mat) {
        gltf_ = std::move(obj);
        mesh_id_ = mesh_id;
        skel_id_ = skel_id;
        material_ = std::move(mat);
    }

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count; (void)input; (void)winfo;

        const float kResW = 1280.0f;
        const float kResH = 720.0f;
        cmds->camera.fbo_3d_width_  = kResW;
        cmds->camera.fbo_3d_height_ = kResH;

        // 相机：正对站立人形（同 standard_sunny_day，中心高 ~2.5，人在 y~1 站立）。
        const jpov::Vec3f scene_center = {0.0f, 1.0f, 0.0f};
        cmds->camera.position = {4.0f, 2.0f, 4.0f};
        cmds->camera.target   = scene_center;
        cmds->camera.near     = 0.05f;

        // ── 太阳平行光 ──
        const jpov::Vec3f sun_light_dir = {0.0f, -1.0f, -1.0f};
        cmds->sun = jpov::DirectionalLight{
            {sun_light_dir}, {1.0f, 1.0f, 1.0f, 1.0f}, 3.0f};
        // ── 环境光 ──
        cmds->ambient = jpov::AmbientLight{
            .color = {1.0f, 1.0f, 1.0f, 1.0f}, .intensity = 0.3f};
        // ── 天光背景(Preetham) ──
        cmds->sky = jpov::DaySkyCommand{
            jpov::Vec3f(-sun_light_dir.x(), -sun_light_dir.y(), -sun_light_dir.z()),
            2.0f, {1.0f, 1.0f, 1.0f, 1.0f}, 1.0f,
            {0.05f, 0.06f, 0.08f, 1.0f}, 0.02, 1e3, 1.0,
        };
        cmds->tone_mapping = true;

        // ---- base：地面 tiles(DrawGltfObject) ----
        for (const Slot& s : slots_) {
            cmds->DrawGltfObject(s.obj, s.center, s.up, s.front);
        }

        // ---- 蒙皮人: 单实例单 pose(静态 T-rest) ----
        if (mesh_id_ != 0 && skel_id_ != 0) {
            jpov::SkinnedInstanceState inst;
            inst.center = {0.0f, 0.0f, 0.0f};
            inst.up     = {0.0f, 0.0f, -1.0f};
            inst.front  = {0.0f, 1.0f, 0.0f};
            inst.scale  = 4.0f;
            inst.pose_a = 0; inst.pose_b = 0; inst.ratio = 0.0f;
            std::vector<jpov::SkinnedInstanceState> instances{inst};
            cmds->DrawMeshWithSkeleton(mesh_id_, skel_id_, material_,
                                       std::move(instances));
        }

        // ---- A/B 对照：Object3D(非蒙皮) 静态人物 在 x=+2 ----
        // 用同一 mesh_id + 材质, 同一 up/front/scale, 只是走 DrawObject3D(忽略 joints,
        // 画原始 bind 顶点)。颜色应和蒙皮人一致 → 若不一致, 就是蒙皮 shader 的 diff;
        // 若一致, 排除 load/材质问题。
        if (mesh_id_ != 0) {
            cmds->DrawObject3D(mesh_id_, material_,
                               /*center*/ {2.0f, 0.0f, 0.0f},
                               /*up*/     {0.0f, 0.0f, -1.0f},
                               /*front*/  {0.0f, 1.0f, 0.0f},
                               /*scale*/  4.0f,
                               /*highlight*/ false,
                               /*picking_id*/ 0);
        }
    }

private:
    jpov::GltfObject gltf_;
    uint32_t mesh_id_ = 0, skel_id_ = 0;
    jpov::PBRMaterial material_;
};

int main() {
    const std::string outpath =
        jpov::GetTestDataDir() + "/skeleton/human_skeleton_gold_t_rest_1280x720.png";
    const std::string male_glb =
        jpov::GetProjectRoot() +
        "tools/jpov/test/object3d/mixamo_male/mixamo_male.glb";

    JPOV::Config cfg;
    cfg.title = "JPOV Skeleton Gold Generator (T-rest)";
    cfg.headless = true;
    SkeletonGoldGenerator app(cfg);
    app.Init();

    // base: 地面 5×5 tiles
    for (int iz = 0; iz < 5; ++iz) {
        for (int ix = 0; ix < 5; ++ix) {
            const float gx = -12.0f + ix * 6.0f;
            const float gz = -12.0f + iz * 6.0f;
            const bool brick = (gx < 0.0f);
            jpov::GltfObject g = app.LoadGltf(AssetPath(
                brick ? "ground/ground_brick.gltf" : "ground/ground_dirt.gltf"));
            CHECK(!g.empty()) << "LoadGltf ground failed";
            app.AddSlot(std::move(g), {gx, -0.5f, gz}, {0, 1, 0}, {0, 0, 1});
        }
    }

    // 蒙皮人
    jpov::GltfObject gltf = app.LoadGltf(male_glb);
    CHECK(!gltf.primitives.empty()) << "mixamo_male.glb 应含蒙皮 primitive";
    std::vector<jpov::SkeletonType> skels;
    CHECK(jpov::LoadGltfSkeleton(male_glb, &skels));
    CHECK(!skels.empty());
    std::vector<jpov::SkeletonPose> poses;
    poses.push_back(jpov::MakeMixamoBindPose());
    const uint32_t skel_id = app.RegisterSkeleton(skels[0], poses);
    const uint32_t mesh_id = gltf.primitives[0].mesh_id;
    jpov::PBRMaterial material = gltf.primitives[0].material;
    app.SetSkinned(std::move(gltf), mesh_id, skel_id, std::move(material));

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "骨架蒙皮 T-rest gold image generated: " << outpath;
    return 0;
}
