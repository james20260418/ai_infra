// JPOV 实例化拾取测试 —— 验证 InstancedObjectRenderer 拾取 + 三大 renderer 共享的
// internal-id 映射机制：
//   ① 逐实例 picking_id（同 mesh 摆 N 份，各给不同 id）；
//   ② render_internal_id 分区映射（写 FBO 的是致密 internal id，读回再查表得用户 id）；
//   ③ 空间对应：左实例 → 左 id，右实例 → 右 id（不是「只要集合对」）；
//   ④ 批内 picking_id==0 的实例 → 命中判 miss；
//   ⑤ cutout（alpha_mode=kMask）镂空片元 discard、不写 internal id（实例 + object3d 两条）。
//
// 说明：cutout 用**均匀 alpha** 贴图，避免对 UV→屏幕映射做实假设。

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"

namespace {

std::string TestDataDir() {
    const char* s = std::getenv("TEST_SRCDIR");
    if (s) {
        std::string p = s;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        return p + "__main__/tools/jpov/test";
    }
    return jpov::GetProjectRoot() + "tools/jpov/test";
}

std::string OutputDir() { return jpov::GetOutputDir() + "jpov_instanced_pick_test/"; }

// 带 UV 的四边形（局部 XY 平面，法线 +Z，CCW 朝 +Z），用于 cutout 测试
//（MeshData::MakeBox 不带 UV，MASK 材质要求 kUV）。
jpov::MeshData MakeUvQuad(float half) {
    jpov::MeshData m;
    m.flags = static_cast<jpov::MeshVertexFlags>(
        static_cast<uint8_t>(jpov::MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kNormal) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kUV));
    m.positions = {{-half, -half, 0.0f}, {half, -half, 0.0f},
                   {half, half, 0.0f}, {-half, half, 0.0f}};
    m.normals = {{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f},
                 {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}};
    m.uvs = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    m.indices = {0, 1, 2, 0, 2, 3};
    return m;
}

// 场景选择。
enum class Scene {
    kIds,              // 3 份普通实例，id 11/12/13（验逐实例 id + 空间对应）
    kZeroInBatch,      // 2 份实例，id 41 / 0（点 id=0 的那份应 miss）
    kCutoutInstanced,  // 1 份 MASK 四边形实例（DrawInstancedObject + cutout）
    kCutoutObject3d,   // 1 份 MASK 四边形（DrawObject3D + cutout）
};

class InstancedPickApp : public JPOV {
public:
    using JPOV::JPOV;

    uint32_t box_mesh_ = 0;    // 普通 box
    uint32_t quad_mesh_ = 0;   // 带 UV 四边形（cutout）
    jpov::PBRMaterial mask_mat_;
    Scene scene_ = Scene::kIds;
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
        cmds->camera.position = {0.0f, 0.0f, 8.0f};
        cmds->camera.target = {0.0f, 0.0f, 0.0f};
        cmds->camera.up = {0.0f, 1.0f, 0.0f};
        cmds->camera.near = 0.05f;
        cmds->camera.far = 1000.0f;
        cmds->sun = jpov::DirectionalLight{
            /*direction*/ {-0.3f, -1.0f, -0.4f},
            /*color*/     {1.0f, 1.0f, 1.0f, 1.0f},
            /*intensity*/ 2.5f};
        cmds->ambient = jpov::AmbientLight{.color = {1, 1, 1, 1}, .intensity = 0.4f};
        cmds->tone_mapping = true;

        switch (scene_) {
        case Scene::kIds: {
            std::vector<jpov::InstanceState> insts(3);
            insts[0].transform.center = {-2.0f, 0.0f, 0.0f};
            insts[0].picking_id = 11;
            insts[1].transform.center = {0.0f, 0.0f, 0.0f};
            insts[1].picking_id = 12;
            insts[2].transform.center = {2.0f, 0.0f, 0.0f};
            insts[2].picking_id = 13;
            cmds->DrawInstancedObject(box_mesh_,
                                      jpov::PBRMaterial::SolidColor(jpov::kColorWhite),
                                      std::move(insts));
            break;
        }
        case Scene::kZeroInBatch: {
            std::vector<jpov::InstanceState> insts(2);
            insts[0].transform.center = {-2.0f, 0.0f, 0.0f};
            insts[0].picking_id = 41;
            insts[1].transform.center = {2.0f, 0.0f, 0.0f};
            insts[1].picking_id = 0;   // 不可拾取，但仍是本批一员（占 internal id）
            cmds->DrawInstancedObject(box_mesh_,
                                      jpov::PBRMaterial::SolidColor(jpov::kColorWhite),
                                      std::move(insts));
            break;
        }
        case Scene::kCutoutInstanced: {
            std::vector<jpov::InstanceState> insts(1);
            insts[0].transform.center = {0.0f, 0.0f, 0.0f};
            insts[0].picking_id = 21;
            cmds->DrawInstancedObject(quad_mesh_, mask_mat_, std::move(insts));
            break;
        }
        case Scene::kCutoutObject3d: {
            cmds->DrawObject3D(quad_mesh_, mask_mat_,
                               /*center*/ {0.0f, 0.0f, 0.0f},
                               /*up*/     {0.0f, 1.0f, 0.0f},
                               /*front*/  {0.0f, 0.0f, 1.0f},
                               /*scale*/  1.0f,
                               /*highlight*/ false,
                               /*picking_id*/ 51);
            break;
        }
        }

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
    cfg.title = "JPOV Instanced Pick Test";
    cfg.headless = true;
    InstancedPickApp app(cfg);
    app.Init();

    app.box_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(0.8f, 0.8f, 0.8f));
    app.quad_mesh_ = app.RegisterMesh(MakeUvQuad(3.0f));

    const uint32_t tex_trans = app.RegisterTexture(
        TestDataDir() + "/object3d/instanced_pick_cutout_transparent.png");
    const uint32_t tex_opaq = app.RegisterTexture(
        TestDataDir() + "/object3d/instanced_pick_cutout_opaque.png");
    CHECK_NE(tex_trans, 0u) << "透明贴图注册失败";
    CHECK_NE(tex_opaq, 0u) << "不透明贴图注册失败";

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

    // ---- 1) 实例批：逐实例 picking_id + 空间对应 ----
    {
        app.scene_ = Scene::kIds;
        std::set<uint32_t> found;
        std::map<uint32_t, float> first_x;   // id → 首次命中的屏幕 x
        for (float x = 40.0f; x <= 1240.0f; x += 20.0f) {
            const jpov::PickResult r = run_pick(x, 360.0f);
            if (r.hit) {
                found.insert(r.picking_id);
                if (first_x.find(r.picking_id) == first_x.end()) {
                    first_x[r.picking_id] = x;
                }
            }
        }
        CHECK_EQ(found.size(), 3u)
            << "沿中线扫描应拾到 3 个不同实例 id，实际 " << found.size();
        CHECK_EQ(found.count(11) + found.count(12) + found.count(13), 3u)
            << "实例 id 集合应为 {11,12,13}";
        // 空间对应：左实例(x≈-2)→11，中→12，右→13（防「集合对但映射错位」）。
        CHECK(first_x.count(11) && first_x.count(12) && first_x.count(13));
        CHECK(first_x[11] < first_x[12] && first_x[12] < first_x[13])
            << "空间对应错位：first_x(11)=" << first_x[11]
            << " first_x(12)=" << first_x[12] << " first_x(13)=" << first_x[13]
            << "（应 11<12<13）";

        const jpov::PickResult bg = run_pick(20.0f, 20.0f);
        CHECK(!bg.hit) << "左上角背景点应未命中";
    }

    // ---- 2) 批内 picking_id==0 → miss（但仍是本批一员，占 internal id） ----
    {
        app.scene_ = Scene::kZeroInBatch;
        std::set<uint32_t> found;
        for (float x = 40.0f; x <= 1240.0f; x += 20.0f) {
            const jpov::PickResult r = run_pick(x, 360.0f);
            if (r.hit) found.insert(r.picking_id);
        }
        CHECK_EQ(found.size(), 1u) << "应只拾到 id=41（id=0 的实例不该命中），实际 "
                                   << found.size() << " 种 id";
        CHECK_EQ(found.count(41), 1u) << "应拾到 id=41";
        CHECK_EQ(found.count(0), 0u) << "picking_id=0 的实例不该被命中";
    }

    // ---- 3) cutout：镂空 discard 不写 id（实例路径）----
    {
        app.scene_ = Scene::kCutoutInstanced;
        app.mask_mat_ = jpov::PBRMaterial{};
        app.mask_mat_.base_color_tex = tex_trans;   // 整片 alpha=0
        app.mask_mat_.alpha_mode = jpov::AlphaMode::kMask;
        app.mask_mat_.alpha_cutoff = 0.5f;
        const jpov::PickResult rt = run_pick(640.0f, 360.0f);
        CHECK(!rt.hit) << "实例 cutout：整片镂空（alpha=0）应 discard，不该命中";

        app.mask_mat_.base_color_tex = tex_opaq;    // 整片 alpha=1
        const jpov::PickResult ro = run_pick(640.0f, 360.0f);
        CHECK(ro.hit) << "实例 cutout：不透明片应命中";
        CHECK_EQ(ro.picking_id, 21u) << "应命中实例 picking_id=21";
    }

    // ---- 4) cutout：镂空 discard（object3d 路径）----
    {
        app.scene_ = Scene::kCutoutObject3d;
        app.mask_mat_ = jpov::PBRMaterial{};
        app.mask_mat_.base_color_tex = tex_trans;
        app.mask_mat_.alpha_mode = jpov::AlphaMode::kMask;
        app.mask_mat_.alpha_cutoff = 0.5f;
        const jpov::PickResult rt = run_pick(640.0f, 360.0f);
        CHECK(!rt.hit) << "object3d cutout：整片镂空（alpha=0）应 discard，不该命中";

        app.mask_mat_.base_color_tex = tex_opaq;
        const jpov::PickResult ro = run_pick(640.0f, 360.0f);
        CHECK(ro.hit) << "object3d cutout：不透明片应命中";
        CHECK_EQ(ro.picking_id, 51u) << "应命中 object3d picking_id=51";
    }

    app.ReleaseTexture(tex_trans);
    app.ReleaseTexture(tex_opaq);
    app.Finalize();
    LOG(INFO) << "TEST PASSED: 实例逐实例拾取 + 空间对应 + 批内 id=0 + cutout 镂空 discard 正确";
    return 0;
}
