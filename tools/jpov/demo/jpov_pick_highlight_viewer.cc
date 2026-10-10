// JPOV 拾取/高亮查看器（pick_highlight viewer）—— 交互式验收工具。
//
// 目的：肉眼验收**统一的拾取 + 高亮**横跨三大 3D renderer：
//   - 蒙皮（蓝人）：5 个实例，一次 instanced draw。
//   - 静态实例（连衣裙）：3 个实例，一次 instanced draw（MASK cutout）。
//   - 普通 object3d（凳子）：2 个独立 gltf 对象。
//
// 机制：
//   - 左键点击：拾取光标下的物体 → 高亮它；再次点击同一物体 → 取消高亮。
//   - 右键拖拽：轨道旋转；滚轮：缩放（复用 view_config 的 ViewConfig/ApplyInput）。
//   - 固定光照（太阳 + 天光 + 环境光），30 Hz。
//
// 高亮粒度 = **per-command（整批）**：要单独高亮某一株/某一个，就把「被高亮的那几个」
//   拆成单独的 draw command（多一次 draw call，成本对调用方可见）。见 DrawSplit 辅助。
//
// headless：`--capture <out_dir>` 渲染（无高亮 / 有高亮）两张图，供自动化核对。
//
// 运行：
//   bazel run //tools/jpov:jpov_pick_highlight_viewer
//   bazel run //tools/jpov:jpov_pick_highlight_viewer -- --capture /tmp/ph_out

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/interface/skeleton_types.h"
#include "tools/jpov/src/gltf_loader.h"
#include "tools/common/utils.h"

namespace {

constexpr int kWidth = 1280;
constexpr int kHeight = 720;

// picking_id 分配（>0 才可拾取；三大 renderer 各占一段数值区，便于阅读）。
constexpr uint32_t kIdManBase    = 101;  // 101..105
constexpr uint32_t kIdDressBase  = 201;  // 201..203
constexpr uint32_t kIdStoolBase  = 301;  // 301..302

std::string AssetPath(const std::string& assets_dir, const std::string& rel) {
    return assets_dir + "/" + rel;
}

// 一处 asset 根目录（默认仓库相对；--assets 可覆盖）。
std::string DefaultAssetsDir() {
    return jpov::GetProjectRoot() + "tools/jpov/assets/models";
}

// 求「按目标高度归一」的缩放（bounds 无效 → 1，不静默乱缩）。
float ScaleToHeight(const jpov::GltfObject& o, float target_h) {
    if (!o.bounds_valid) {
        LOG(WARNING) << "模型无 bounds，scale=1";
        return 1.0f;
    }
    const float h = o.bounds_max[1] - o.bounds_min[1];
    if (h <= 1.0e-6f) {
        LOG(WARNING) << "模型 Y 高≈0，scale=1";
        return 1.0f;
    }
    return target_h / h;
}

// 拾取/高亮查看器 App。
class PickHighlightApp : public JPOV {
public:
    using JPOV::JPOV;

    // 场景静态资源（Init 后装配一次）。
    uint32_t ground_mesh_ = 0;
    jpov::PBRMaterial ground_mat_;

    uint32_t man_mesh_ = 0;
    uint32_t man_skel_ = 0;
    jpov::PBRMaterial man_mat_;
    std::vector<jpov::SkinnedInstanceState> man_instances_;

    uint32_t dress_mesh_ = 0;
    jpov::PBRMaterial dress_mat_;
    std::vector<jpov::InstanceState> dress_instances_;

    // 凳子：每个是独立的 gltf 对象。
    struct Stool { jpov::GltfObject obj; jpov::Vec3f center; float scale; uint32_t id; };
    std::vector<Stool> stools_;

    // 交互状态
    jpov_viewer::ViewConfig view_ = jpov_viewer::DefaultView();
    std::set<uint32_t> highlighted_;   // 高亮的 picking_id 集合（per-command 高亮用）
    bool awaiting_pick_ = false;       // 上一帧发起了 pick，本帧读结果
    bool probe_pick_ = false;          // headless 自检：强制在 probe 坐标发起一次 pick
    float probe_x_ = 0.0f;
    float probe_y_ = 0.0f;

    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        cmds->camera.fbo_3d_width_  = kWidth;
        cmds->camera.fbo_3d_height_ = kHeight;

        // 读上一帧的拾取结果：命中 → 切换该 id 的高亮（再次点击取消）。
        if (awaiting_pick_) {
            const jpov::PickResult& r = last_pick();
            if (r.hit) {
                auto it = highlighted_.find(r.picking_id);
                if (it == highlighted_.end()) {
                    highlighted_.insert(r.picking_id);
                    LOG(INFO) << "highlight ON  id=" << r.picking_id;
                } else {
                    highlighted_.erase(it);
                    LOG(INFO) << "highlight OFF id=" << r.picking_id;
                }
            }
            awaiting_pick_ = false;
        }

        // 交互输入：右键拖拽轨道旋转 + 滚轮缩放。
        {
            float dx = 0.0f, dy = 0.0f, scroll = 0.0f;
            if (input.right.IsDrag()) {
                dx = input.mouse_dx;
                dy = input.mouse_dy;
            }
            if (input.scroll_delta != 0.0f) scroll = input.scroll_delta;
            jpov_viewer::ApplyInput(&view_, dx, dy, scroll,
                                    static_cast<int>(winfo.width),
                                    static_cast<int>(winfo.height));
        }

        cmds->camera.position = view_.Position();
        cmds->camera.target   = jpov_viewer::ViewConfig::Target();
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov      = 60.0f;
        cmds->camera.near     = 0.05f;
        cmds->camera.far      = 1000.0f;

        // 固定光照：太阳（斜上）+ 天光（Preetham 晴天）+ 环境光 + tone map。
        const jpov::Vec3f sun_l = {0.35f, -1.0f, -0.55f};
        cmds->sun = jpov::DirectionalLight{{sun_l}, {1.0f, 1.0f, 1.0f, 1.0f}, 3.0f};
        cmds->ambient = jpov::AmbientLight{.color = {1, 1, 1, 1}, .intensity = 0.3f};
        cmds->sky = jpov::SkyCommand{
            jpov::Vec3f(-sun_l.x(), -sun_l.y(), -sun_l.z()),
            /*turbidity*/ 2.0f, /*daylight_season*/ {1, 1, 1, 1}, /*intensity*/ 1.0f,
            /*ground_color*/ {0.05f, 0.06f, 0.08f, 1.0f},
            /*sun_radius*/ 0.02, /*sun_brightness*/ 1e3, /*sun_glow*/ 1.0};
        cmds->tone_mapping = true;

        // 高亮样式（仅当有物体被高亮时才设 → 不开高亮时整条 highlt pass 不跑，0 开销）。
        if (!highlighted_.empty()) {
            cmds->highlight_style = jpov::HighlightStyle{
                .color = {1.0f, 0.85f, 0.3f, 1.0f},   // 金黄，sRGB 屏显目标
                .outline_px = 3,                        // 屏空间恒定 3 像素宽
            };
        }

        // 地面（object3d，不可拾取）。
        cmds->DrawObject3D(ground_mesh_, ground_mat_,
                           /*center*/ {0, -0.05f, 0}, /*up*/ {0, 1, 0}, /*front*/ {0, 0, 1});

        // ── 三大 renderer 各画一批；被高亮的子批拆成单独 command（highlight=true）。──
        DrawSplitSkinned(cmds);
        DrawSplitInstanced(cmds);
        DrawSplitStools(cmds);

        // 拾取查询：仅左键点击那一帧发起（其余帧 0 开销）；probe_pick_ 供 headless 自检。
        if (probe_pick_) {
            cmds->pick.enabled  = true;
            cmds->pick.screen_x = probe_x_;
            cmds->pick.screen_y = probe_y_;
        } else if (input.left.IsClick()) {
            cmds->pick.enabled  = true;
            cmds->pick.screen_x = input.mouse_x;
            cmds->pick.screen_y = input.mouse_y;
            awaiting_pick_ = true;
        } else {
            cmds->pick.enabled = false;
        }
    }

private:
    // 蒙皮：把「被高亮的实例」与其余拆成两条命令（per-command 高亮）。
    void DrawSplitSkinned(jpov::RenderCommandList* cmds) {
        std::vector<jpov::SkinnedInstanceState> hi, lo;
        for (const auto& inst : man_instances_) {
            (highlighted_.count(inst.picking_id) ? hi : lo).push_back(inst);
        }
        if (!lo.empty()) {
            cmds->DrawMeshWithSkeleton(man_mesh_, man_skel_, man_mat_, lo, /*highlight=*/false);
        }
        if (!hi.empty()) {
            cmds->DrawMeshWithSkeleton(man_mesh_, man_skel_, man_mat_, std::move(hi), /*highlight=*/true);
        }
    }
    // 静态实例：同上。
    void DrawSplitInstanced(jpov::RenderCommandList* cmds) {
        std::vector<jpov::InstanceState> hi, lo;
        for (const auto& inst : dress_instances_) {
            (highlighted_.count(inst.picking_id) ? hi : lo).push_back(inst);
        }
        if (!lo.empty()) {
            cmds->DrawInstancedObject(dress_mesh_, dress_mat_, lo, /*highlight=*/false);
        }
        if (!hi.empty()) {
            cmds->DrawInstancedObject(dress_mesh_, dress_mat_, std::move(hi), /*highlight=*/true);
        }
    }
    // 凳子：每个是独立 object3d；高亮 = 该命令 highlight=true。
    void DrawSplitStools(jpov::RenderCommandList* cmds) {
        for (const Stool& s : stools_) {
            const bool hl = highlighted_.count(s.id) > 0;
            cmds->DrawGltfObject(s.obj, s.center, {0, 1, 0}, {0, 0, 1},
                                 /*scale=*/s.scale, /*highlight=*/hl, /*picking_id=*/s.id);
        }
    }
};

// 装配：加载三类资产并摆位。
void Install(PickHighlightApp& app, const std::string& assets) {
    const auto load = [&](const std::string& rel) {
        jpov::GltfObject o = app.LoadGltf(AssetPath(assets, rel));
        CHECK(!o.empty()) << "LoadGltf failed: " << AssetPath(assets, rel);
        return o;
    };

    app.ground_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(40.0f, 0.05f, 40.0f));
    app.ground_mat_ = jpov::PBRMaterial::SolidColor({0.75f, 0.75f, 0.75f, 1.0f});

    // —— 5 个蓝人（蒙皮实例）——
    {
        jpov::GltfObject men = load("characters/mixamo_male.glb");
        CHECK(!men.primitives.empty());
        CHECK(men.bounds_valid);
        const float h = men.bounds_max[1] - men.bounds_min[1];
        CHECK_GT(h, 1e-6f);
        const float scale = 1.8f / h;
        const float min_y = men.bounds_min[1];
        app.man_mesh_ = men.primitives[0].mesh_id;
        app.man_mat_  = men.primitives[0].material;
        // 骨架（identity pose = T-pose）。
        std::vector<jpov::SkeletonType> skels;
        CHECK(jpov::LoadGltfSkeleton(AssetPath(assets, "characters/mixamo_male.glb"), &skels));
        CHECK(!skels.empty());
        jpov::SkeletonType type = skels[0];
        type.Validate();
        app.man_skel_ = app.RegisterSkeleton(
            type, {jpov::SkeletonPose::Identity(type.bone_count())});
        for (int i = 0; i < 5; ++i) {
            jpov::SkinnedInstanceState inst;
            const float x = -4.0f + static_cast<float>(i) * 2.0f;
            inst.transform.center = {x, -scale * min_y, 0.0f};
            inst.transform.up = {0, 1, 0};
            inst.transform.front = {0, 0, 1};
            inst.transform.scale = scale;
            inst.pose_a = 0; inst.pose_b = 0; inst.ratio = 0.0f;
            inst.picking_id = kIdManBase + static_cast<uint32_t>(i);
            app.man_instances_.push_back(inst);
        }
    }

    // —— 3 条连衣裙（静态实例，MASK cutout）——
    {
        jpov::GltfObject dress = load("samples/lace_skirt/lace_skirt_cutout.glb");
        CHECK(!dress.primitives.empty());
        const float scale = dress.bounds_valid
            ? ScaleToHeight(dress, 1.4f) : 1.0f;
        const float min_y = dress.bounds_valid ? dress.bounds_min[1] : 0.0f;
        app.dress_mesh_ = dress.primitives[0].mesh_id;
        app.dress_mat_  = dress.primitives[0].material;
        for (int i = 0; i < 3; ++i) {
            jpov::InstanceState inst;
            const float x = -3.0f + static_cast<float>(i) * 3.0f;
            inst.transform.center = {x, -scale * min_y, 4.0f};
            inst.transform.up = {0, 1, 0};
            inst.transform.front = {0, 0, 1};
            inst.transform.scale = scale;
            inst.picking_id = kIdDressBase + static_cast<uint32_t>(i);
            app.dress_instances_.push_back(inst);
        }
    }

    // —— 2 个凳子（普通 object3d）——
    {
        jpov::GltfObject stool = load("scene/scene_assets/stool.glb");
        const float scale = stool.bounds_valid ? ScaleToHeight(stool, 0.9f) : 1.0f;
        const float min_y = stool.bounds_valid ? stool.bounds_min[1] : 0.0f;
        const float xs[2] = {-4.0f, 4.0f};
        for (int i = 0; i < 2; ++i) {
            PickHighlightApp::Stool s;
            s.center = {xs[i], -scale * min_y, -4.0f};
            s.scale = scale;
            s.id = kIdStoolBase + static_cast<uint32_t>(i);
            s.obj = app.LoadGltf(AssetPath(assets, "scene/scene_assets/stool.glb"));
            CHECK(!s.obj.empty());
            app.stools_.push_back(std::move(s));
        }
    }
}

JPOV::Config MakeConfig(const std::string& title, bool headless) {
    JPOV::Config cfg;
    cfg.title = title.c_str();
    cfg.width = kWidth;
    cfg.height = kHeight;
    cfg.resizable = false;
    cfg.target_fps = 30;
    cfg.headless = headless;
    return cfg;
}

jpov_viewer::ViewConfig SceneView() {
    jpov_viewer::ViewConfig v = jpov_viewer::DefaultView();
    v.phi   = 0.42;
    v.theta = 0.0;
    v.R     = 16.0;
    return v;
}

int RunCapture(const std::string& out_dir, const std::string& assets) {
    PickHighlightApp app(MakeConfig("JPOV — pick/highlight viewer (capture)", true));
    app.Init();
    Install(app, assets);

    jpov::WindowInfo winfo;
    winfo.width = kWidth;
    winfo.height = kHeight;
    jpov::InputSnapshot input{};
    app.view_ = SceneView();

    auto shoot = [&](const char* name) {
        const std::string path = out_dir + "/" + name + ".png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "capture: " << path;
    };

    shoot("scene");                                  // 无高亮
    app.highlighted_ = {kIdManBase + 2, kIdDressBase + 1, kIdStoolBase};  // 一男 + 一裙 + 一凳
    shoot("highlighted");                            // 有高亮（三大 renderer 各一）

    // 拾取自检：屏幕中心应命中物体（证明 view 的 pick 接线通）。
    app.probe_pick_ = true;
    app.probe_x_ = static_cast<float>(kWidth) * 0.5f;
    app.probe_y_ = static_cast<float>(kHeight) * 0.5f;
    app.RunOnce(input, winfo, (out_dir + "/probe.png").c_str());
    const jpov::PickResult r = app.last_pick();
    LOG(INFO) << "probe pick @center → hit=" << r.hit << " picking_id=" << r.picking_id;
    CHECK(r.hit) << "视图器拾取接线异常：屏幕中心应命中某个物体";
    app.probe_pick_ = false;

    app.Finalize();
    return 0;
}

int RunInteractive(const std::string& assets) {
    PickHighlightApp app(MakeConfig("JPOV — 拾取/高亮查看器", false));
    app.Init();
    Install(app, assets);
    app.view_ = SceneView();
    app.Run();
    app.Finalize();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_dir;
    std::string assets = DefaultAssetsDir();
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--assets") == 0 && i + 1 < argc) {
            assets = argv[i + 1];
        }
    }
    if (!out_dir.empty()) {
        return RunCapture(out_dir, assets);
    }
    return RunInteractive(assets);
}
