// JPOV 顶点 base tint（object3d 路径）像素校验 —— 无 gold 图，直接读回像素断言。
//
// 场景：4 个白盒一字排开，只有顶点 tint id / 材质开关不同。纯 ambient、无太阳/点光，
// 所以颜色 = ambient × baseColor ⇒ 像素值可直接判断「乘法有没有生效」：
//   A: id=0（不换色）
//   B: id=1 + 调色板 1 = 红，enabled=true   → 只该剩红通道
//   C: id=2 + 调色板 2 = 绿，enabled=true   → 只该剩绿通道
//   D: id=1 + 调色板 1 = 红，enabled=false  → **必须与 A 相同**（开关保护）
//
// 覆盖链路：MeshData.base_tint → loc6 vec4 上传 → VS flat varying → FS 调色板乘法 → 开关。

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

// 四个盒子的世界 x 坐标（相机在 (0,0,8) 看原点，fov 60）。
constexpr float kBoxX[4] = {-3.0f, -1.0f, 1.0f, 3.0f};

jpov::MeshData MakeTintedBox(uint8_t tint_id) {
    jpov::MeshData m = jpov::MeshData::MakeBox(0.6f, 0.6f, 0.6f);
    m.flags = static_cast<jpov::MeshVertexFlags>(
        static_cast<uint8_t>(m.flags) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kBaseTint));
    m.base_tint.assign(m.positions.size(), tint_id);
    m.Validate();
    return m;
}

class TintTestApp : public JPOV {
public:
    using JPOV::JPOV;

    uint32_t mesh_id[4] = {0, 0, 0, 0};
    jpov::PBRMaterial mat[4];

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

        // 纯 ambient（无太阳 / 无点光 / 无环境镜面）⇒ 输出对光照实现不敏感。
        cmds->ambient = jpov::AmbientLight{.color = {1.0f, 1.0f, 1.0f, 1.0f},
                                           .intensity = 1.0f};
        cmds->tone_mapping = false;

        for (int i = 0; i < 4; ++i) {
            cmds->DrawObject3D(mesh_id[i], mat[i],
                               /*center*/ {kBoxX[i], 0.0f, 0.0f},
                               /*up*/ {0.0f, 1.0f, 0.0f},
                               /*front*/ {0.0f, 0.0f, 1.0f});
        }
    }
};

// 世界 x → 屏幕像素（与上面相机参数一致）。
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

int MaxDiff(const Rgb& a, const Rgb& b) {
    const int dr = std::abs(a.r - b.r);
    const int dg = std::abs(a.g - b.g);
    const int db = std::abs(a.b - b.b);
    return std::max(std::max(dr, dg), db);
}

}  // namespace

int main() {
    const std::string outdir = jpov::GetOutputDir() + "jpov_object3d_tint_test/";
    std::system(("mkdir -p " + outdir).c_str());
    const std::string png = outdir + "frame.png";

    JPOV::Config cfg;
    cfg.title = "JPOV Object3D Tint Test";
    cfg.headless = true;
    TintTestApp app(cfg);
    app.Init();

    const uint8_t ids[4] = {0, 1, 2, 1};
    for (int i = 0; i < 4; ++i) {
        app.mesh_id[i] = app.RegisterMesh(MakeTintedBox(ids[i]));
        app.mat[i] = jpov::PBRMaterial::SolidColor(jpov::kColorWhite);
        app.mat[i].base_tint.color[0] = {1.0f, 0.0f, 0.0f, 1.0f};  // 通道 1 = 红
        app.mat[i].base_tint.color[1] = {0.0f, 1.0f, 0.0f, 1.0f};  // 通道 2 = 绿
        app.mat[i].base_tint.enabled = true;
    }
    app.mat[3].base_tint.enabled = false;  // D：调色板仍是红，但开关关掉

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
    const Rgb a = SamplePixel(rgb, w, WorldXToPixel(kBoxX[0]), y);
    const Rgb b = SamplePixel(rgb, w, WorldXToPixel(kBoxX[1]), y);
    const Rgb c = SamplePixel(rgb, w, WorldXToPixel(kBoxX[2]), y);
    const Rgb d = SamplePixel(rgb, w, WorldXToPixel(kBoxX[3]), y);
    stbi_image_free(rgb);

    LOG(INFO) << "A(id0) = (" << a.r << "," << a.g << "," << a.b << ")";
    LOG(INFO) << "B(id1,red) = (" << b.r << "," << b.g << "," << b.b << ")";
    LOG(INFO) << "C(id2,green) = (" << c.r << "," << c.g << "," << c.b << ")";
    LOG(INFO) << "D(id1,disabled) = (" << d.r << "," << d.g << "," << d.b << ")";

    bool ok = true;

    // A：白盒，三通道近似相等（光照后仍是灰白）。
    const int a_max = std::max(std::max(a.r, a.g), a.b);
    const int a_min = std::min(std::min(a.r, a.g), a.b);
    if (a_max < 40) {
        LOG(ERROR) << "A 太暗（ambient 没生效？）";
        ok = false;
    }
    if (a_max - a_min > 6) {
        LOG(ERROR) << "A 不是灰白（id=0 居然被染色？）";
        ok = false;
    }

    // B：红线（r 明显大于 g/b）。
    if (!(b.r > 2 * b.g + 10 && b.r > 2 * b.b + 10)) {
        LOG(ERROR) << "B 没被调色板 1（红）染色";
        ok = false;
    }
    // C：绿线。
    if (!(c.g > 2 * c.r + 10 && c.g > 2 * c.b + 10)) {
        LOG(ERROR) << "C 没被调色板 2（绿）染色";
        ok = false;
    }
    // 通道没串：B 的绿分量不该高，C 的红分量不该高。
    if (b.g > 20 || b.b > 20 || c.r > 20 || c.b > 20) {
        LOG(ERROR) << "调色板串通道（B/C 里出现了另一通道的颜色）";
        ok = false;
    }
    // D：开关关掉 ⇒ 与 A 逐通道几乎相同（这是「不做乘法」的负向对照）。
    if (MaxDiff(d, a) > 3) {
        LOG(ERROR) << "enable=false 时仍改变了颜色：D vs A 最大差 " << MaxDiff(d, a);
        ok = false;
    }

    if (!ok) {
        LOG(ERROR) << "TEST FAILED: base tint 渲染校验失败（图: " << png << "）";
        return 1;
    }
    LOG(INFO) << "TEST PASSED: base tint 两通道乘法 + id=0 不换色 + enable=false 零影响";
    return 0;
}
