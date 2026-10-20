// JPOV 体积火（raymarch）Gold Test —— 保护「.jpovvol 体纹理 → 逐像素 raymarch →
// 薄层写透明累积缓冲 → 合成」这条链路，以及**入库的篝火样例资产**本身。
//
// 用法：
//   bazel run //tools/jpov/test/volume_fire:volume_fire_gold_generator   # 重出 gold
//   bazel test //tools/jpov/test/volume_fire:volume_fire_gold_test       # 回归
//
// 为什么需要它（本轮踩过的坑）：
//   曾出现「样例资产退化」——入库的 .jpovvol 只剩一根竖直细柱（95% 体素为 0），
//   raymarch 渲染出来是一根"发光棒"，但**没有任何测试会红**。所以这里除了逐字节 gold，
//   还加了一条**非恒真断言**：火焰像素必须落在合理区间（太少=没渲出来/资产坏了，
//   太多=整屏过曝）。资产退化时，即使有人把 gold 一起重出，这条断言也会拦住。
//
// 确定性：固定相机 / 固定帧号 / 固定 σ·发射·步数·抖动 / 固定天光；无时间推进。

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/common/utils.h"
#include "tools/jpov/demo/skylight_scene.h"
#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/test_utils.h"
#include "third_party/stb/stb_image.h"

namespace {

constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr float kFrame = 0.0f;      // 帧号钉死（可复现）

// 渲染参数：与 viewer 默认一致 —— 换默认值时这里要跟着改（gold 会重出）。
constexpr float kSigma = 12.0f;
constexpr float kEmission = 4.0f;
constexpr int kSteps = 64;
constexpr float kJitter = 1.0f;

// 火焰像素判据（暖色 + 够亮）：R 明显大于 B 且亮度够。用于非恒真断言。
constexpr int kWarmPixelsMin = 3000;
constexpr int kWarmPixelsMax = 40000;

class VolumeFireGoldApp : public JPOV {
public:
    using JPOV::JPOV;

    void Setup() {
        // 资产路径：bazel 下走 runfiles（data/fire_vdb/campfire.jpovvol 是 data 依赖）。
        volume_path_ = jpov::ResolveResourcePath("data/fire_vdb/campfire.jpovvol");
        ground_mesh_ = RegisterMesh(jpov_skylight::MakeGroundQuad());
        ground_mat_ = jpov_skylight::GroundMaterial();
        // 1m³ 燃料块（与 viewer 同一尺寸/位置：半宽 0.5、中心 y=0.5）。
        fuel_mesh_ = RegisterMesh(jpov::MeshData::MakeBox(0.5f, 0.5f, 0.5f));
        fuel_mat_ = jpov::PBRMaterial::SolidColor({0.05f, 0.035f, 0.03f, 1.0f});
    }

    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;

        // 相机与 viewer 默认一致（phi=0.38 rad, theta=0.7, R=4.8）。
        cmds->camera.fbo_3d_width_ = kWidth;
        cmds->camera.fbo_3d_height_ = kHeight;
        cmds->camera.position = {4.8f * std::cos(0.38f) * std::sin(0.7f),
                                4.8f * std::sin(0.38f),
                                4.8f * std::cos(0.38f) * std::cos(0.7f)};
        cmds->camera.target = {0.0f, 0.0f, 0.0f};
        cmds->camera.up = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov = 60.0f;
        cmds->camera.near = 0.05f;
        cmds->camera.far = 1000.0f;

        cmds->sky = jpov::CreateDefaultSkyCommand(
            2.0f, jpov::Color{1.0f, 1.0f, 1.0f, 1.0f},
            jpov::Vec3f{0.45f, 0.35f, 0.82f},
            jpov::Vec3f{-0.45f, -0.35f, -0.82f});
        jpov::DirectionalLight sun;
        sun.direction = {-0.45f, -0.35f, -0.82f};
        sun.color = {1.0f, 1.0f, 1.0f, 1.0f};
        sun.intensity = 2.5f;
        cmds->sun = sun;
        jpov::AmbientLight ambient;
        ambient.color = {0.20f, 0.25f, 0.30f, 1.0f};
        ambient.intensity = 0.5f;
        cmds->ambient = ambient;
        cmds->tone_mapping = true;

        cmds->DrawObject3D(ground_mesh_, ground_mat_, {0.0f, 0.0f, 0.0f},
                           {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
        cmds->DrawObject3D(fuel_mesh_, fuel_mat_, {0.0f, 0.5f, 0.0f},
                           {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});

        if (volume_id_ == 0 && !volume_path_.empty()) {
            volume_id_ = RegisterVolumeTexture(volume_path_);
            CHECK_NE(volume_id_, 0u) << "体积加载失败: " << volume_path_;
            CHECK_GT(VolumeFrameCount(volume_id_), 0)
                << "体积帧数为 0（资产可能损坏）: " << volume_path_;
        }

        jpov::VolumeFireCommand vc;
        vc.volume_id = volume_id_;
        vc.aabb_min = {-0.57f, 1.0f, -0.56f};
        vc.aabb_max = {0.57f, 2.14f, 0.56f};
        vc.sigma_scale = kSigma;
        vc.emission_scale = kEmission;
        vc.steps = kSteps;
        vc.jitter = kJitter;
        vc.frame = kFrame;
        cmds->DrawVolumeFire(std::move(vc));
    }

private:
    uint32_t volume_id_ = 0;
    std::string volume_path_;
    uint32_t ground_mesh_ = 0;
    uint32_t fuel_mesh_ = 0;
    jpov::PBRMaterial ground_mat_;
    jpov::PBRMaterial fuel_mat_;
};

std::string GoldPath() {
    return jpov::GetTestDataDir() + "/volume_fire/campfire_1280x720.png";
}

void RenderTo(const std::string& out_path) {
    JPOV::Config cfg;
    cfg.title = "JPOV volume fire gold test";
    cfg.width = kWidth;
    cfg.height = kHeight;
    cfg.headless = true;
    cfg.resizable = false;

    VolumeFireGoldApp app(cfg);
    app.Init();
    app.Setup();

    jpov::WindowInfo winfo;
    winfo.width = static_cast<float>(kWidth);
    winfo.height = static_cast<float>(kHeight);
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, out_path.c_str());
    app.Finalize();
}

// 统计「暖色且够亮」的像素数（火）。返回 -1 = 解码失败。
long CountWarmPixels(const std::string& path) {
    int w = 0;
    int h = 0;
    int c = 0;
    unsigned char* img = stbi_load(path.c_str(), &w, &h, &c, 3);
    if (img == nullptr) {
        return -1;
    }
    long n = 0;
    for (int i = 0; i < w * h; ++i) {
        const int r = img[i * 3 + 0];
        const int b = img[i * 3 + 2];
        if (r > b + 40 && r > 80) {
            ++n;
        }
    }
    stbi_image_free(img);
    return n;
}

}  // namespace

int main(int argc, char** argv) {
    const bool generate = (argc > 1 && std::strcmp(argv[1], "--generate") == 0);
    if (generate) {
        std::filesystem::create_directories(
            std::filesystem::path(GoldPath()).parent_path());
        RenderTo(GoldPath());
        LOG(INFO) << "gold written: " << GoldPath()
                  << " (warm px = " << CountWarmPixels(GoldPath()) << ")";
        return 0;
    }

    std::vector<uint8_t> expected;
    if (!jpov::ReadFileBytes(GoldPath(), &expected)) {
        LOG(FATAL) << "missing gold: " << GoldPath();
    }

    const std::string out_dir = jpov::GetOutputDir() + "jpov_volume_fire_gold_test/";
    const std::string out_path = out_dir + "rendered.png";
    RenderTo(out_path);

    std::vector<uint8_t> rendered;
    if (!jpov::ReadFileBytes(out_path, &rendered)) {
        LOG(FATAL) << "missing render: " << out_path;
    }
    if (rendered != expected) {
        LOG(ERROR) << "gold mismatch: rendered=" << rendered.size()
                   << " bytes, expected=" << expected.size() << " bytes";
        return 1;
    }

    // 非恒真断言：必须有「像火」的像素量。资产退化（只剩一根细柱）或整屏过曝都会被拦。
    const long warm = CountWarmPixels(out_path);
    if (warm < 0) {
        LOG(ERROR) << "failed to decode render for warm-pixel check";
        return 1;
    }
    if (warm < kWarmPixelsMin || warm > kWarmPixelsMax) {
        LOG(ERROR) << "flame pixel count out of expected range: " << warm
                   << " (want " << kWarmPixelsMin << ".." << kWarmPixelsMax << ")";
        return 1;
    }
    LOG(INFO) << "gold match: " << rendered.size() << " bytes; warm px = " << warm;
    return 0;
}
