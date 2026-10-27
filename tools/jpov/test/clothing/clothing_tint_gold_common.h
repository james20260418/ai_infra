// 顶点 tint gold 公共场景（generator 写 gold / test 比对共用）。
//
// 资产：`lace_skirt/tinted_sample.glb`（穿衣工具保存：带 _JPOV_RELAX + _JPOV_TINT，
// 顶点 id 0/1/2 = 不换色 / 通道1 / 通道2）。
// 三个场景共用固定相机/ambient（无太阳，避免 llvmpipe 光照抖动）：
//   · kObject3d ：1 件裙子，object3d 路径（material.base_tint 红/绿）
//   · kInstanced：2 件裙子（静态实例路径，**逐实例**调色板：左红/绿，右蓝/黄）
//   · kSkinned  ：2 件裙子（蒙皮路径，Identity pose；逐实例调色板：左红/绿，右蓝/黄）
// 目的：把「顶点 tint 三条路径 + 逐实例颜色」锁成 gold；并配合 tint-on/off 行为门禁。
#ifndef JPOV_TEST_CLOTHING_CLOTHING_TINT_GOLD_COMMON_H_
#define JPOV_TEST_CLOTHING_CLOTHING_TINT_GOLD_COMMON_H_

#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/common/utils.h"
#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/src/gltf_loader.h"
#include "tools/jpov/test/test_utils.h"

namespace jpov_cloth_tint_gold {

constexpr int kW = 640;
constexpr int kH = 360;

enum class Mode {
    kObject3d,
    kInstanced,
    kSkinned,
};

inline const char* ModeName(Mode m) {
    switch (m) {
        case Mode::kObject3d:
            return "object3d";
        case Mode::kInstanced:
            return "instanced";
        case Mode::kSkinned:
            return "skinned";
    }
    return "?";
}

inline std::string GoldRelPath(Mode m) {
    return std::string("/clothing/cloth_tint_") + ModeName(m) + "_640x360.png";
}

inline std::string ClothPath() {
    return jpov::GetModelsDir() +
           "/clothing/lace_skirt/lace_skirt_tinted_sample.glb";
}

class TintGoldApp : public JPOV {
public:
    using JPOV::JPOV;

    Mode mode = Mode::kObject3d;
    bool tint_on = true;
    uint32_t mesh_id = 0;
    uint32_t skel_id = 0;
    jpov::PBRMaterial material;

    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;

        cmds->camera.fbo_3d_width_ = static_cast<float>(kW);
        cmds->camera.fbo_3d_height_ = static_cast<float>(kH);
        cmds->camera.position = {0.0f, 0.93f, 3.4f};
        cmds->camera.target = {0.0f, 0.93f, 0.0f};
        cmds->camera.up = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov = 45.0f;
        cmds->camera.near = 0.05f;
        cmds->camera.far = 100.0f;
        cmds->ambient = jpov::AmbientLight{.color = {1.0f, 1.0f, 1.0f, 1.0f},
                                           .intensity = 1.0f};
        cmds->tone_mapping = false;

        const jpov::Color kRed{1.0f, 0.0f, 0.0f, 1.0f};
        const jpov::Color kGreen{0.0f, 1.0f, 0.0f, 1.0f};
        const jpov::Color kBlue{0.0f, 0.0f, 1.0f, 1.0f};
        const jpov::Color kYellow{1.0f, 1.0f, 0.0f, 1.0f};

        if (mode == Mode::kObject3d) {
            jpov::PBRMaterial m = material;
            m.base_tint.enabled = tint_on;
            m.base_tint.color[0] = kRed;
            m.base_tint.color[1] = kGreen;
            cmds->DrawObject3D(mesh_id, m,
                               /*center*/ {0.0f, 0.0f, 0.0f},
                               /*up*/ {0.0f, 1.0f, 0.0f},
                               /*front*/ {0.0f, 0.0f, 1.0f},
                               /*scale*/ 2.0f);
            return;
        }

        auto instance_tint = [&](bool right) {
            jpov::BaseTint t;
            t.enabled = tint_on;
            t.color[0] = right ? kBlue : kRed;
            t.color[1] = right ? kYellow : kGreen;
            return t;
        };

        if (mode == Mode::kInstanced) {
            std::vector<jpov::InstanceState> insts(2);
            for (int k = 0; k < 2; ++k) {
                insts[k].transform.center = {(k == 0 ? -0.5f : 0.5f), 0.0f, 0.0f};
                insts[k].transform.up = {0.0f, 1.0f, 0.0f};
                insts[k].transform.front = {0.0f, 0.0f, 1.0f};
                insts[k].transform.scale = 2.0f;
                insts[k].base_tint = instance_tint(/*right*/ k == 1);
            }
            cmds->DrawInstancedObject(mesh_id, material, std::move(insts));
            return;
        }

        CHECK(mode == Mode::kSkinned);
        std::vector<jpov::SkinnedInstanceState> insts(2);
        for (int k = 0; k < 2; ++k) {
            insts[k].transform.center = {(k == 0 ? -0.5f : 0.5f), 0.0f, 0.0f};
            insts[k].transform.up = {0.0f, 1.0f, 0.0f};
            insts[k].transform.front = {0.0f, 0.0f, 1.0f};
            insts[k].transform.scale = 2.0f;
            insts[k].pose_a = 0;
            insts[k].pose_b = 0;
            insts[k].ratio = 0.0f;
            insts[k].base_tint = instance_tint(/*right*/ k == 1);
        }
        cmds->DrawMeshWithSkeleton(mesh_id, skel_id, material, std::move(insts));
    }
};

// 渲一帧（mode + tint on/off）到 out_png。同参数 ⇒ 同帧（generator/test 共用）。
inline bool RenderTintGold(Mode mode, bool tint_on, const std::string& out_png) {
    JPOV::Config cfg;
    cfg.width = kW;
    cfg.height = kH;
    cfg.headless = true;
    cfg.fonts = {
        {"tools/jpov/fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"tools/jpov/fonts/DejaVuSans.ttf", 0, jpov::kFontBuiltinLatin},
    };
    TintGoldApp app(cfg);
    app.Init();

    const std::string path = ClothPath();
    jpov::GltfObject obj = app.LoadGltf(path);
    CHECK(!obj.empty()) << "加载 tinted 裙子失败: " << path;
    CHECK(!obj.primitives.empty());
    app.mesh_id = obj.primitives[0].mesh_id;
    app.material = obj.primitives[0].material;
    app.mode = mode;
    app.tint_on = tint_on;

    if (mode == Mode::kSkinned) {
        std::vector<jpov::SkeletonType> skels;
        CHECK(jpov::LoadGltfSkeleton(path, &skels)) << "读骨架失败: " << path;
        CHECK(!skels.empty()) << "tinted 裙子应自带 skin";
        std::vector<jpov::SkeletonPose> poses = {
            jpov::SkeletonPose::Identity(skels[0].bone_count())};
        app.skel_id = app.RegisterSkeleton(skels[0], poses);
        CHECK_NE(app.skel_id, 0u);
    }

    jpov::WindowInfo winfo;
    winfo.width = static_cast<float>(kW);
    winfo.height = static_cast<float>(kH);
    app.RunOnce(jpov::InputSnapshot(), winfo, out_png.c_str());
    app.Finalize();
    LOG(INFO) << "tint gold 渲图: " << out_png << "（mode=" << ModeName(mode)
              << ", tint=" << (tint_on ? "on" : "off") << "）";
    return true;
}

}  // namespace jpov_cloth_tint_gold

#endif  // JPOV_TEST_CLOTHING_CLOTHING_TINT_GOLD_COMMON_H_
