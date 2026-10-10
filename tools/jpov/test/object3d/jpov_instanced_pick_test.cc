// JPOV 实例化拾取测试 —— 验证 InstancedObjectRenderer 的拾取能力：
//   ① 逐实例 picking_id（同 mesh 摆 N 份，各给不同 id）；
//   ② render_internal_id 分区映射（写 FBO 的是致密 internal id，读回再查表得用户 id）；
//   ③ cutout（alpha_mode=kMask）镂空片元 discard、不写 internal id。
//
// 覆盖：
//   1. 静态实例批（普通 box，无贴图）：3 份实例，picking_id = 11/12/13。沿屏幕中线扫描，
//      命中 id 集合应恰为 {11,12,13}；背景点未命中。
//   2. cutout 实例（带 UV 的四边形 + MASK 材质）：
//      - baseColor 贴图整片 alpha≈0 → 全片 discard → 拾取**未命中**（镂空不填 id）；
//      - 换成 alpha=1 的贴图 → 命中该实例 id（21）。
//
// 说明：用**均匀 alpha** 的贴图，避免对 UV→屏幕映射做实假设（任何 UV 采到的
//   alpha 都一样）。llvmpipe 下仅验拾取 id，不做光照像素比对。

#include <cstdio>
#include <cstdlib>
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

class InstancedPickApp : public JPOV {
public:
    using JPOV::JPOV;

    uint32_t box_mesh_ = 0;    // 普通 box（实例 id 场景）
    uint32_t quad_mesh_ = 0;   // 带 UV 四边形（cutout 场景）
    jpov::PBRMaterial mask_mat_;
    bool cutout_scene_ = false;
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

        if (!cutout_scene_) {
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
        } else {
            std::vector<jpov::InstanceState> insts(1);
            insts[0].transform.center = {0.0f, 0.0f, 0.0f};
            insts[0].picking_id = 21;
            cmds->DrawInstancedObject(quad_mesh_, mask_mat_, std::move(insts));
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

    // ---- 1) 实例批：逐实例 picking_id ----
    {
        app.cutout_scene_ = false;
        std::set<uint32_t> found;
        int hits = 0;
        for (float x = 40.0f; x <= 1240.0f; x += 20.0f) {
            const jpov::PickResult r = run_pick(x, 360.0f);
            if (r.hit) {
                found.insert(r.picking_id);
                ++hits;
            }
        }
        LOG(INFO) << "instanced scan y=360: hits=" << hits
                  << " distinct ids=" << found.size();
        CHECK_EQ(found.size(), 3u)
            << "沿中线扫描应拾到 3 个不同实例 id，实际 " << found.size();
        CHECK(found.count(11) && found.count(12) && found.count(13))
            << "实例 id 集合应为 {11,12,13}（逐实例 id 映射失败）";

        const jpov::PickResult bg = run_pick(20.0f, 20.0f);
        CHECK(!bg.hit) << "左上角背景点应未命中";
        LOG(INFO) << "background corner → hit=" << bg.hit;
    }

    // ---- 2) cutout：镂空 discard 不写 internal id ----
    {
        app.cutout_scene_ = true;
        const uint32_t tex_trans = app.RegisterTexture(
            TestDataDir() + "/object3d/instanced_pick_cutout_transparent.png");
        const uint32_t tex_opaq = app.RegisterTexture(
            TestDataDir() + "/object3d/instanced_pick_cutout_opaque.png");
        CHECK_NE(tex_trans, 0u) << "透明贴图注册失败";
        CHECK_NE(tex_opaq, 0u) << "不透明贴图注册失败";

        // 透明贴图：整片 alpha=0 → 全片被 discard → 拾取未命中。
        app.mask_mat_ = jpov::PBRMaterial{};
        app.mask_mat_.base_color_tex = tex_trans;
        app.mask_mat_.alpha_mode = jpov::AlphaMode::kMask;
        app.mask_mat_.alpha_cutoff = 0.5f;
        const jpov::PickResult rt = run_pick(640.0f, 360.0f);
        LOG(INFO) << "cutout(transparent) center → hit=" << rt.hit;
        CHECK(!rt.hit)
            << "整片镂空（alpha=0）应被 discard，不该写入 internal id";

        // 不透明贴图：命中该实例 picking_id=21。
        app.mask_mat_.base_color_tex = tex_opaq;
        const jpov::PickResult ro = run_pick(640.0f, 360.0f);
        LOG(INFO) << "cutout(opaque) center → hit=" << ro.hit
                  << " id=" << ro.picking_id;
        CHECK(ro.hit) << "不透明片应命中";
        CHECK_EQ(ro.picking_id, 21u) << "应命中该实例的 picking_id=21";

        app.ReleaseTexture(tex_trans);
        app.ReleaseTexture(tex_opaq);
    }

    app.Finalize();
    LOG(INFO) << "TEST PASSED: 实例化逐实例拾取 + cutout 镂空 discard 正确";
    return 0;
}
