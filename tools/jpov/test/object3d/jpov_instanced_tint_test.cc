// JPOV 静态实例路径的逐实例 base tint 像素校验（GPU）。
//
// 场景（纯 ambient、无太阳/点光）：同一 tinted mesh 画多个实例，各实例给不同调色板：
//   x=-3  id1 + 红   → 红
//   x=-1  id1 + 绿   → 绿
//   x= 1  id1 + 蓝   → 蓝
//   x= 3  id1 + 红但 enabled=false → 白（开关保护）
//   x= 5  id2 + color1=黄 → 黄（通道 2 用 color1）
//
// 覆盖链路：InstanceState::base_tint → loc12/13/14 per-instance attr → VS flat varying
//           → FS 乘法（id 来自顶点 loc6.z）。

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

// 盒子中心世界 x（相机 (0,0,8)、fov 60）。
constexpr float kBoxX[5] = {-3.0f, -1.0f, 1.0f, 3.0f, 5.0f};

jpov::MeshData MakeTintedBox(uint8_t tint_id) {
    jpov::MeshData m = jpov::MeshData::MakeBox(0.6f, 0.6f, 0.6f);
    m.flags = static_cast<jpov::MeshVertexFlags>(
        static_cast<uint8_t>(m.flags) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kBaseTint));
    m.base_tint.assign(m.positions.size(), tint_id);
    m.Validate();
    return m;
}

jpov::InstanceState MakeInstance(float x, const jpov::Color& c0, const jpov::Color& c1,
                                 bool enabled) {
    jpov::InstanceState inst;
    inst.transform.center = {x, 0.0f, 0.0f};
    inst.transform.up = {0.0f, 1.0f, 0.0f};
    inst.transform.front = {0.0f, 0.0f, 1.0f};
    inst.transform.scale = 1.0f;
    inst.base_tint.enabled = enabled;
    inst.base_tint.color[0] = c0;
    inst.base_tint.color[1] = c1;
    return inst;
}

class InstancedTintApp : public JPOV {
public:
    using JPOV::JPOV;

    uint32_t mesh_id1 = 0;
    uint32_t mesh_id2 = 0;

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
        const jpov::Color kBlue{0.0f, 0.0f, 1.0f, 1.0f};
        const jpov::Color kYellow{1.0f, 1.0f, 0.0f, 1.0f};
        const jpov::Color kWhite{1.0f, 1.0f, 1.0f, 1.0f};

        const jpov::PBRMaterial mat = jpov::PBRMaterial::SolidColor(kWhite);
        std::vector<jpov::InstanceState> batch1 = {
            MakeInstance(kBoxX[0], kRed, kWhite, /*enabled*/ true),
            MakeInstance(kBoxX[1], kGreen, kWhite, /*enabled*/ true),
            MakeInstance(kBoxX[2], kBlue, kWhite, /*enabled*/ true),
            MakeInstance(kBoxX[3], kRed, kWhite, /*enabled*/ false),
        };
        cmds->DrawInstancedObject(mesh_id1, mat, std::move(batch1));

        // 通道 2：id2 的 mesh，调色板 color1 = 黄（color0 无关）。
        std::vector<jpov::InstanceState> batch2 = {
            MakeInstance(kBoxX[4], kRed, kYellow, /*enabled*/ true),
        };
        cmds->DrawInstancedObject(mesh_id2, mat, std::move(batch2));
    }
};

int WorldXToPixel(float world_x) {
    const double tan_half = 0.5773502691896257;  // tan(30°)
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

bool Dominant(const Rgb& c, int channel) {  // 0=r,1=g,2=b
    const int v = (channel == 0) ? c.r : ((channel == 1) ? c.g : c.b);
    const int o1 = (channel == 0) ? c.g : ((channel == 1) ? c.r : c.r);
    const int o2 = (channel == 2) ? c.g : c.b;
    return v > 2 * o1 + 10 && v > 2 * o2 + 10;
}

bool NearWhite(const Rgb& c) {
    const int mx = std::max(std::max(c.r, c.g), c.b);
    const int mn = std::min(std::min(c.r, c.g), c.b);
    return mx >= 40 && mx - mn <= 6;
}

}  // namespace

int main() {
    const std::string outdir = jpov::GetOutputDir() + "jpov_instanced_tint_test/";
    std::system(("mkdir -p " + outdir).c_str());
    const std::string png = outdir + "frame.png";

    JPOV::Config cfg;
    cfg.title = "JPOV Instanced Tint Test";
    cfg.headless = true;
    InstancedTintApp app(cfg);
    app.Init();

    app.mesh_id1 = app.RegisterMesh(MakeTintedBox(1));
    app.mesh_id2 = app.RegisterMesh(MakeTintedBox(2));

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
    const Rgb c[5] = {
        SamplePixel(rgb, w, WorldXToPixel(kBoxX[0]), y),
        SamplePixel(rgb, w, WorldXToPixel(kBoxX[1]), y),
        SamplePixel(rgb, w, WorldXToPixel(kBoxX[2]), y),
        SamplePixel(rgb, w, WorldXToPixel(kBoxX[3]), y),
        SamplePixel(rgb, w, WorldXToPixel(kBoxX[4]), y),
    };
    stbi_image_free(rgb);

    for (int i = 0; i < 5; ++i) {
        LOG(INFO) << "box " << i << " = (" << c[i].r << "," << c[i].g << "," << c[i].b << ")";
    }

    bool ok = true;
    if (!Dominant(c[0], 0)) {
        LOG(ERROR) << "实例 0（红）未被逐实例染色";
        ok = false;
    }
    if (!Dominant(c[1], 1)) {
        LOG(ERROR) << "实例 1（绿）未被逐实例染色";
        ok = false;
    }
    if (!Dominant(c[2], 2)) {
        LOG(ERROR) << "实例 2（蓝）未被逐实例染色";
        ok = false;
    }
    if (!NearWhite(c[3])) {
        LOG(ERROR) << "实例 3（enabled=false）不该被染色";
        ok = false;
    }
    // 通道 2（黄 = R+G）：R、G 高且接近，B 低。
    if (!(c[4].r > 2 * c[4].b + 10 && c[4].g > 2 * c[4].b + 10 &&
          std::abs(c[4].r - c[4].g) <= 12)) {
        LOG(ERROR) << "实例 4（id2 + color1 黄）不对";
        ok = false;
    }
    // 逐实例隔离：红实例的 G/B 不该高；绿实例的 R/B 不该高。
    if (c[0].g > 20 || c[0].b > 20 || c[1].r > 20 || c[1].b > 20) {
        LOG(ERROR) << "逐实例颜色串了";
        ok = false;
    }

    if (!ok) {
        LOG(ERROR) << "TEST FAILED: instanced tint（图: " << png << "）";
        return 1;
    }
    LOG(INFO) << "TEST PASSED: 逐实例 tint（红/绿/蓝/禁用白/id2 黄）";
    return 0;
}
