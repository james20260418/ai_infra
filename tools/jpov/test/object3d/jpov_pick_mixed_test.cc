// JPOV 混合拾取测试 —— 验证三大 renderer（object3d / 蒙皮 / 静态实例）**同一帧**共存时，
// 共享 pick FBO 上的 **render_internal_id 分区**（base 分段）能正确消歧：
//   同一帧里三类各摆一个、各带不同 picking_id；网格扫描应拾到 {1,2,3} 全部三个 id，
//   且满足空间对应（左/中/右）。
//
// 这是 internal-id 方案最核心的风险点：三条渲染路径写进同一张 FBO 的 id 若分区/查表错，
// 会命中「别的 renderer 的表」而返回错误 id（或 miss）。

#include <algorithm>
#include <array>
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

std::string OutputDir() { return jpov::GetOutputDir() + "jpov_pick_mixed_test/"; }

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

constexpr uint32_t kIdObject3D = 1;
constexpr uint32_t kIdInstanced = 2;
constexpr uint32_t kIdSkinned = 3;

class MixedPickApp : public JPOV {
public:
    using JPOV::JPOV;

    uint32_t box_mesh_ = 0;
    uint32_t skinned_mesh_ = 0;
    uint32_t skel_id_ = 0;
    jpov::PBRMaterial skinned_mat_;
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
        cmds->camera.position = {0.0f, 1.0f, 10.0f};
        cmds->camera.target = {0.0f, 1.0f, 0.0f};
        cmds->camera.up = {0.0f, 1.0f, 0.0f};
        cmds->camera.near = 0.05f;
        cmds->camera.far = 1000.0f;
        cmds->sun = jpov::DirectionalLight{
            /*direction*/ {0.0f, -1.0f, -1.0f},
            /*color*/     {1.0f, 1.0f, 1.0f, 1.0f},
            /*intensity*/ 3.0f};
        cmds->ambient = jpov::AmbientLight{.color = {1, 1, 1, 1}, .intensity = 0.3f};
        cmds->tone_mapping = true;

        // 左：object3d box，id=1。
        cmds->DrawObject3D(box_mesh_, jpov::PBRMaterial::SolidColor(jpov::kColorWhite),
                           /*center*/ {-3.0f, 1.0f, 0.0f},
                           /*up*/     {0.0f, 1.0f, 0.0f},
                           /*front*/  {0.0f, 0.0f, 1.0f},
                           /*scale*/  1.0f,
                           /*highlight*/ false,
                           /*picking_id*/ kIdObject3D);

        // 中：静态实例 box，id=2。
        std::vector<jpov::InstanceState> insts(1);
        insts[0].transform.center = {0.0f, 1.0f, 0.0f};
        insts[0].picking_id = kIdInstanced;
        cmds->DrawInstancedObject(box_mesh_,
                                  jpov::PBRMaterial::SolidColor(jpov::kColorWhite),
                                  std::move(insts));

        // 右：蒙皮实例（mixamo_male），id=3。放大 + 相机拉近，保证剪影足够大可被网格扫到。
        jpov::SkinnedInstanceState inst;
        inst.transform.center = {3.0f, 0.0f, 0.0f};
        inst.transform.up = {0.0f, 1.0f, 0.0f};
        inst.transform.front = {0.0f, 0.0f, 1.0f};
        inst.transform.scale = 2.5f;
        inst.pose_a = 0;
        inst.pose_b = 0;
        inst.ratio = 0.0f;
        inst.picking_id = kIdSkinned;
        std::vector<jpov::SkinnedInstanceState> sinsts{inst};
        cmds->DrawMeshWithSkeleton(skinned_mesh_, skel_id_, skinned_mat_,
                                   std::move(sinsts));

        cmds->pick.enabled = pick_enabled;
        cmds->pick.screen_x = pick_x;
        cmds->pick.screen_y = pick_y;
    }
};

}  // namespace

int main() {
    std::system(("mkdir -p " + OutputDir()).c_str());
    const std::string frame_png = OutputDir() + "frame.png";

    JPOV::Config cfg;
    cfg.title = "JPOV Pick Mixed Test";
    cfg.headless = true;
    MixedPickApp app(cfg);
    app.Init();

    app.box_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(0.8f, 0.8f, 0.8f));

    const std::string male = MalePath();
    jpov::GltfObject male_obj = app.LoadGltf(male);
    CHECK(!male_obj.empty()) << "LoadGltf failed: " << male;
    CHECK(!male_obj.primitives.empty());
    std::vector<jpov::SkeletonType> skels;
    CHECK(jpov::LoadGltfSkeleton(male, &skels)) << "LoadGltfSkeleton 失败";
    CHECK(!skels.empty());
    std::vector<jpov::SkeletonPose> poses{
        jpov::SkeletonPose::Identity(skels[0].bone_count())};
    app.skel_id_ = app.RegisterSkeleton(skels[0], poses);
    app.skinned_mesh_ = male_obj.primitives[0].mesh_id;
    app.skinned_mat_ = male_obj.primitives[0].material;

    jpov::WindowInfo winfo;
    winfo.width = 1280.0f;
    winfo.height = 720.0f;
    jpov::InputSnapshot input{};

    auto run_pick = [&](float x, float y) -> jpov::PickResult {
        app.pick_enabled = true;
        app.pick_x = x;
        app.pick_y = y;
        app.RunOnce(input, winfo, frame_png.c_str());
        return app.last_pick();
    };

    // 网格扫描：收集命中集合 + 各 id 的命中包围盒（用于空间对应判定）。
    std::set<uint32_t> found;
    std::map<uint32_t, std::array<float, 4>> bbox;  // minx,maxx,miny,maxy
    for (float y = 80.0f; y <= 640.0f; y += 40.0f) {
        for (float x = 60.0f; x <= 1220.0f; x += 40.0f) {
            const jpov::PickResult r = run_pick(x, y);
            if (!r.hit) continue;
            found.insert(r.picking_id);
            auto it = bbox.find(r.picking_id);
            if (it == bbox.end()) {
                bbox[r.picking_id] = {x, x, y, y};
            } else {
                it->second[0] = std::min(it->second[0], x);
                it->second[1] = std::max(it->second[1], x);
                it->second[2] = std::min(it->second[2], y);
                it->second[3] = std::max(it->second[3], y);
            }
        }
    }
    for (const auto& kv : bbox) {
        LOG(INFO) << "  id=" << kv.first << " bbox x[" << kv.second[0] << ","
                  << kv.second[1] << "] y[" << kv.second[2] << "," << kv.second[3] << "]";
    }

    CHECK_EQ(found.size(), 3u)
        << "同一帧三类 renderer 应拾到 3 个不同 id（分区消歧），实际 " << found.size();
    CHECK(found.count(kIdObject3D) && found.count(kIdInstanced) && found.count(kIdSkinned))
        << "id 集合应为 {object3d=1, instanced=2, skinned=3}（跨 renderer 分区/查表错误）";
    // 空间对应：左(object3d) < 中(instanced) < 右(skinned)，用命中包围盒 min-x 判定。
    CHECK(bbox[kIdObject3D][0] < bbox[kIdInstanced][0] &&
          bbox[kIdInstanced][0] < bbox[kIdSkinned][0])
        << "三类空间对应错位：minx o3d=" << bbox[kIdObject3D][0]
        << " inst=" << bbox[kIdInstanced][0] << " skinned=" << bbox[kIdSkinned][0];

    const jpov::PickResult bg = run_pick(20.0f, 20.0f);
    CHECK(!bg.hit) << "左上角背景点应未命中";

    app.Finalize();
    LOG(INFO) << "TEST PASSED: 三大 renderer 同帧拾取分区消歧 + 空间对应正确";
    return 0;
}
