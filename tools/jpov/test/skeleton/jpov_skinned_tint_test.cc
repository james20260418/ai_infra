// JPOV 蒙皮路径的逐实例 base tint 像素校验（GPU）。
//
// 用 1 根骨的极简蒙皮网格（box，全部顶点绑到 joint0，weight=1）+ Identity pose，
// 画 3 个实例，各给不同调色板：
//   x=-3 红 / x=0 绿 / x=3 红但 enabled=false（白）
// 纯 ambient ⇒ 像素颜色直接反映「实例 state 的 BaseTint 有没有进 shader」。
//
// 覆盖链路：SkinnedInstanceState::base_tint → loc12 misc（byte1 开关 + RGB888）
//           → VS flat varying（颜色/开关/id）→ FS 乘法。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/common/utils.h"
#include "tools/jpov/include/jpov/jpov.h"

namespace {

constexpr int kResW = 1280;
constexpr int kResH = 720;

jpov::MeshData MakeSkinnedTintedBox() {
    jpov::MeshData m = jpov::MeshData::MakeBox(0.6f, 0.6f, 0.6f);
    m.flags = static_cast<jpov::MeshVertexFlags>(
        static_cast<uint8_t>(m.flags) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kJoints) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kBaseTint));
    m.base_tint.assign(m.positions.size(), 1);
    m.joint_indices.assign(m.positions.size(), std::array<int32_t, 4>{0, 0, 0, 0});
    m.joint_weights.assign(m.positions.size(), std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f});
    m.Validate();
    return m;
}

jpov::SkeletonType MakeOneBoneSkeleton() {
    jpov::SkeletonType sk;
    jpov::SkeletonJoint root;
    root.parent = jpov::kSkeletonNoParent;
    root.rest_offset = {0.0f, 0.0f, 0.0f};
    root.name = "root";
    sk.joints.push_back(root);
    sk.Validate();
    return sk;
}

jpov::SkinnedInstanceState MakeInstance(float x, const jpov::Color& c0,
                                        bool enabled) {
    jpov::SkinnedInstanceState inst;
    inst.transform.center = {x, 0.0f, 0.0f};
    inst.transform.up = {0.0f, 1.0f, 0.0f};
    inst.transform.front = {0.0f, 0.0f, 1.0f};
    inst.transform.scale = 2.0f;
    inst.pose_a = 0;
    inst.pose_b = 0;
    inst.ratio = 0.0f;
    inst.base_tint.enabled = enabled;
    inst.base_tint.color[0] = c0;
    inst.base_tint.color[1] = {0.0f, 0.0f, 1.0f, 1.0f};
    return inst;
}

class SkinnedTintApp : public JPOV {
public:
    using JPOV::JPOV;

    uint32_t mesh_id = 0;
    uint32_t skel_id = 0;

    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;

        cmds->camera.fbo_3d_width_ = static_cast<float>(kResW);
        cmds->camera.fbo_3d_height_ = static_cast<float>(kResH);
        cmds->camera.position = {0.0f, 0.0f, 8.0f};
        cmds->camera.target = {0.0f, 0.0f, 0.0f};
        cmds->camera.up = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov = 60.0f;
        cmds->camera.near = 0.05f;
        cmds->camera.far = 100.0f;
        cmds->ambient = jpov::AmbientLight{.color = {1.0f, 1.0f, 1.0f, 1.0f},
                                           .intensity = 1.0f};
        cmds->tone_mapping = false;

        const jpov::Color kRed{1.0f, 0.0f, 0.0f, 1.0f};
        const jpov::Color kGreen{0.0f, 1.0f, 0.0f, 1.0f};
        std::vector<jpov::SkinnedInstanceState> insts = {
            MakeInstance(-3.0f, kRed, /*enabled*/ true),
            MakeInstance(0.0f, kGreen, /*enabled*/ true),
            MakeInstance(3.0f, kRed, /*enabled*/ false),
        };
        cmds->DrawMeshWithSkeleton(
            mesh_id, skel_id, jpov::PBRMaterial::SolidColor(jpov::kColorWhite),
            std::move(insts));
    }
};

int WorldXToPixel(float world_x) {
    const double tan_half = 0.5773502691896257;
    const double half_h_world = 8.0 * tan_half;
    const double ndc_x = static_cast<double>(world_x) / (half_h_world * (1280.0 / 720.0));
    return static_cast<int>(640.0 + ndc_x * 640.0 + 0.5);
}

struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;
};

Rgb SamplePixel(const unsigned char* rgb, int w, int x, int y) {
    const unsigned char* p = rgb + (static_cast<size_t>(y) * w + x) * 3;
    return Rgb{static_cast<int>(p[0]), static_cast<int>(p[1]), static_cast<int>(p[2])};
}

}  // namespace

int main() {
    const std::string outdir = jpov::GetOutputDir() + "jpov_skinned_tint_test/";
    std::system(("mkdir -p " + outdir).c_str());
    const std::string png = outdir + "frame.png";

    JPOV::Config cfg;
    cfg.title = "JPOV Skinned Tint Test";
    cfg.headless = true;
    SkinnedTintApp app(cfg);
    app.Init();

    app.mesh_id = app.RegisterMesh(MakeSkinnedTintedBox());
    const jpov::SkeletonType sk = MakeOneBoneSkeleton();
    std::vector<jpov::SkeletonPose> poses = {
        jpov::SkeletonPose::Identity(sk.bone_count())};
    app.skel_id = app.RegisterSkeleton(sk, poses);
    CHECK_NE(app.skel_id, 0u);

    jpov::WindowInfo winfo;
    winfo.width = static_cast<float>(kResW);
    winfo.height = static_cast<float>(kResH);
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, png.c_str());
    app.Finalize();

    int w = 0;
    int h = 0;
    int comps = 0;
    unsigned char* rgb = stbi_load(png.c_str(), &w, &h, &comps, 3);
    CHECK(rgb != nullptr) << "读回渲染图失败: " << png;
    CHECK_EQ(w, kResW);
    CHECK_EQ(h, kResH);

    const int y = kResH / 2;
    const Rgb red = SamplePixel(rgb, w, WorldXToPixel(-3.0f), y);
    const Rgb green = SamplePixel(rgb, w, WorldXToPixel(0.0f), y);
    const Rgb white = SamplePixel(rgb, w, WorldXToPixel(3.0f), y);
    stbi_image_free(rgb);

    LOG(INFO) << "red inst = (" << red.r << "," << red.g << "," << red.b << ")";
    LOG(INFO) << "green inst = (" << green.r << "," << green.g << "," << green.b << ")";
    LOG(INFO) << "disabled inst = (" << white.r << "," << white.g << "," << white.b << ")";

    bool ok = true;
    if (!(red.r > 2 * red.g + 10 && red.r > 2 * red.b + 10)) {
        LOG(ERROR) << "红色实例未被逐实例染色";
        ok = false;
    }
    if (!(green.g > 2 * green.r + 10 && green.g > 2 * green.b + 10)) {
        LOG(ERROR) << "绿色实例未被逐实例染色";
        ok = false;
    }
    const int mx = std::max(std::max(white.r, white.g), white.b);
    const int mn = std::min(std::min(white.r, white.g), white.b);
    if (mx < 40 || mx - mn > 6) {
        LOG(ERROR) << "enabled=false 的实例不该被染色";
        ok = false;
    }

    if (!ok) {
        LOG(ERROR) << "TEST FAILED: skinned tint（图: " << png << "）";
        return 1;
    }
    LOG(INFO) << "TEST PASSED: 蒙皮路径逐实例 tint（红/绿/禁用白）";
    return 0;
}
