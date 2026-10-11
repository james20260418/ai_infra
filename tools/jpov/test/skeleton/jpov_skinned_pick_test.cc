// JPOV 蒙皮实例拾取测试 —— 验证 SkeletonRenderer 的拾取能力
//（kSkinnedVs + JPOV_PICK 宏 + 共享 kPickIdFs；逐实例 id = kPickIdBaseSkinned + base + gl_InstanceID）。
//
// 场景：一份 mixamo_male 蒙皮网格，bind pose，**2 个实例**分列世界左右，picking_id=31/32。
// 扫描屏幕网格应恰拾到 {31,32}，且满足空间对应（左→31，右→32）。
//
// 说明：本测试只验拾取 id 的映射正确（走 mesa/llvmpipe 也确定），不做光照像素比对。

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/gltf_object.h"
#include "tools/jpov/src/gltf_loader.h"
#include "tools/common/utils.h"

namespace {

std::string OutDir() { return jpov::GetOutputDir() + "jpov_skinned_pick_test/"; }

std::string MalePath() {
    const char* s = std::getenv("TEST_SRCDIR");
    if (s) {
        std::string p = s;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        return p + "__main__/tools/jpov/assets/models/characters/mixamo_male.glb";
    }
    return jpov::GetProjectRoot() +
           "tools/jpov/assets/models/characters/mixamo_male.glb";
}

class SkinnedPickApp : public JPOV {
public:
    using JPOV::JPOV;

    uint32_t mesh_id_ = 0;
    uint32_t skel_id_ = 0;
    jpov::PBRMaterial mat_;
    bool pick_enabled = false;
    float pick_x = 0.0f;
    float pick_y = 0.0f;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count; (void)input; (void)winfo;

        cmds->camera.fbo_3d_width_ = 1280.0f;
        cmds->camera.fbo_3d_height_ = 720.0f;
        cmds->camera.position = {0.0f, 1.5f, 16.0f};
        cmds->camera.target = {0.0f, 1.5f, 0.0f};
        cmds->camera.up = {0.0f, 1.0f, 0.0f};
        cmds->camera.near = 0.05f;
        cmds->camera.far = 1000.0f;
        cmds->sun = jpov::DirectionalLight{
            /*direction*/ {0.0f, -1.0f, -1.0f},
            /*color*/     {1.0f, 1.0f, 1.0f, 1.0f},
            /*intensity*/ 3.0f};
        cmds->ambient = jpov::AmbientLight{.color = {1, 1, 1, 1}, .intensity = 0.3f};
        cmds->tone_mapping = true;

        auto make = [](float cx, uint32_t id) {
            jpov::SkinnedInstanceState inst;
            inst.transform.center = {cx, 0.0f, 0.0f};
            inst.transform.up = {0.0f, 1.0f, 0.0f};
            inst.transform.front = {0.0f, 0.0f, 1.0f};
            inst.transform.scale = 2.0f;
            inst.pose_a = 0;
            inst.pose_b = 0;
            inst.ratio = 0.0f;
            inst.picking_id = id;
            return inst;
        };
        std::vector<jpov::SkinnedInstanceState> insts{
            make(-3.0f, 31), make(3.0f, 32)};
        cmds->DrawMeshWithSkeleton(mesh_id_, skel_id_, mat_, std::move(insts));

        cmds->pick.enabled = pick_enabled;
        cmds->pick.screen_x = pick_x;
        cmds->pick.screen_y = pick_y;
    }
};

}  // namespace

int main() {
    std::system(("mkdir -p " + OutDir()).c_str());
    const std::string frame_png = OutDir() + "frame.png";

    JPOV::Config cfg;
    cfg.title = "JPOV Skinned Pick Test";
    cfg.headless = true;
    SkinnedPickApp app(cfg);
    app.Init();

    const std::string male = MalePath();
    jpov::GltfObject male_obj = app.LoadGltf(male);
    CHECK(!male_obj.empty()) << "LoadGltf failed: " << male;
    CHECK(!male_obj.primitives.empty()) << male << " 应含蒙皮 primitive";
    std::vector<jpov::SkeletonType> skels;
    CHECK(jpov::LoadGltfSkeleton(male, &skels)) << "LoadGltfSkeleton 失败";
    CHECK(!skels.empty());
    std::vector<jpov::SkeletonPose> poses{
        jpov::SkeletonPose::Identity(skels[0].bone_count())};
    app.skel_id_ = app.RegisterSkeleton(skels[0], poses);
    app.mesh_id_ = male_obj.primitives[0].mesh_id;
    app.mat_ = male_obj.primitives[0].material;

    jpov::WindowInfo winfo;
    winfo.width = 1280.0f;
    winfo.height = 720.0f;
    jpov::InputSnapshot input{};

    auto pick = [&](float x, float y) -> jpov::PickResult {
        app.pick_enabled = true;
        app.pick_x = x;
        app.pick_y = y;
        app.RunOnce(input, winfo, frame_png.c_str());
        return app.last_pick();
    };

    // 网格扫描（步长 20：蒙皮人形较细，需细网格；范围聚焦实例出现的屏幕区域）：
    // 收集命中集合 + 各 id 首次命中的屏幕 x。
    std::set<uint32_t> found;
    std::map<uint32_t, float> first_x;
    for (float y = 220.0f; y <= 480.0f; y += 20.0f) {
        for (float x = 400.0f; x <= 900.0f; x += 20.0f) {
            const jpov::PickResult r = pick(x, y);
            if (r.hit) {
                found.insert(r.picking_id);
                if (first_x.find(r.picking_id) == first_x.end()) {
                    first_x[r.picking_id] = x;
                }
            }
        }
    }
    CHECK_EQ(found.size(), 2u) << "应拾到 2 个蒙皮实例 id，实际 " << found.size();
    CHECK(found.count(31) && found.count(32)) << "id 集合应为 {31,32}";
    CHECK(first_x[31] < first_x[32])
        << "空间对应错位：first_x(31)=" << first_x[31]
        << " first_x(32)=" << first_x[32] << "（左实例应是 31）";

    const jpov::PickResult bg = pick(20.0f, 20.0f);
    CHECK(!bg.hit) << "左上角背景点应未命中";

    app.Finalize();
    LOG(INFO) << "TEST PASSED: 蒙皮多实例拾取（逐实例 id + 空间对应）正确";
    return 0;
}
