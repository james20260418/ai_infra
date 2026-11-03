// JPOV 粒子 kVelocity 对齐 GPU 像素测试 —— 锁住「速度对齐的面片必须正对相机」
//
// 被测代码：src/particles/particle_renderer.h 顶点着色器的 kVelocity（uAlign == 3）分支。
//
// 这个分支曾经写成：
//     right = u × n;                 // u = 速度单位向量，n = 指向相机的单位向量
//     up    = cross(right, u);       // ❌ 结果 = ±n（面片法线），面片侧对相机
// 于是面片的「高度轴」指向视线，整块 quad 退化成一条**沿 right 的细线**：
//   * 速度 ⟂ 视线时最明显（火花/余烬被压成细横线或细竖线）
//   * 一旦沿速度拉伸，拉伸沿着「看不见的轴」发生 —— 越拉越细，而不是变成流线
// 正确写法：up = cross(n, right) = u 在屏幕平面上的投影（⟂ n），quad 始终面向相机。
//
// 为什么必须用像素测试：这个错误在 CPU 侧完全看不出来（顶点数据都对），只有投影到屏幕
// 才暴露；而且不同速度方向下错误表现不同（横线 / 竖线 / 正常），单个固定场景可能漏掉。
//
// 断言（每张图都是「一个粒子 + 纯色 + 无几何背景」）：
//   0. 速度沿世界 +X（⟂ 视线、水平）：亮区应接近正方形（宽高比 ≈ 1，面积够大）
//   1. 速度沿世界 +Y（⟂ 视线、竖直）：同上
//   2. 速度沿 +Y 且 stretched=true（stretch_scale=3，|v|=0.5 ⇒ 长度 ×2.5）：
//      亮区应是**竖长条**（高/宽 ≥ 1.8）—— 这一条同时验证「拉伸轴 = 速度轴」
//   3. 同 2，但 rotation=1.2rad：kVelocity 必须**忽略滚转**（否则滚转会把拉伸轴转走，
//      火星变成随机朝向的斜条 —— 实际观感就是"拉长方向不对"）
//
// 反向验证（开发时做过）：把 up 改回 cross(right, u) 后，四个用例全部变成 0 亮像素（全红）；
// 改回 cross(n, right) 后全绿。另：把「kVelocity 忽略 roll」去掉，用例 3 的高宽比掉到 ~0.69。

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"
#include "tools/common/utils.h"
#include "tools/jpov/include/jpov/jpov.h"

namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 360;
constexpr float kSize = 0.40f;          // 面片世界尺寸（米）
constexpr float kCamDistance = 3.0f;    // 相机到粒子距离（米）
constexpr float kVelocity = 0.5f;       // 速度模长（m/s）
constexpr float kStretchScale = 3.0f;   // 拉伸系数（每 m/s）→ 长度 ×(1 + 0.5*3) = 2.5

struct Image {
    int w = 0;
    int h = 0;
    std::vector<unsigned char> px;   // RGB，行 0 = 图像顶部
};

Image LoadRgb(const std::string& path) {
    Image img;
    int comp = 0;
    unsigned char* data = stbi_load(path.c_str(), &img.w, &img.h, &comp, 3);
    CHECK(data != nullptr) << "读不回渲染结果：" << path;
    img.px.assign(data, data + static_cast<std::size_t>(img.w) * img.h * 3);
    stbi_image_free(data);
    return img;
}

int LuminanceAt(const Image& img, int x, int y) {
    const std::size_t o = static_cast<std::size_t>(y) * img.w * 3 + x * 3;
    return (static_cast<int>(img.px[o]) + img.px[o + 1] + img.px[o + 2]) / 3;
}

// 背景亮度上界：这个场景的天空/环境最多 ~80（实测四角 35..79），
// 而白色发光粒子经 tone map 后在 200 以上。取 150 一刀切，不需要逐像素估计背景。
constexpr int kLitThreshold = 150;

struct LitStats {
    int count = 0;
    int min_x = 0;
    int max_x = 0;
    int min_y = 0;
    int max_y = 0;

    int width() const { return max_x - min_x + 1; }
    int height() const { return max_y - min_y + 1; }
};

// 「亮像素」= 亮度 > kLitThreshold。粒子是白色加法发光，背景（天空/黑盒子）远低于此。
LitStats MeasureLit(const Image& img) {
    LitStats s;
    s.min_x = img.w;
    s.min_y = img.h;
    s.max_x = -1;
    s.max_y = -1;
    for (int y = 0; y < img.h; ++y) {
        for (int x = 0; x < img.w; ++x) {
            if (LuminanceAt(img, x, y) <= kLitThreshold) {
                continue;
            }
            ++s.count;
            s.min_x = std::min(s.min_x, x);
            s.max_x = std::max(s.max_x, x);
            s.min_y = std::min(s.min_y, y);
            s.max_y = std::max(s.max_y, y);
        }
    }
    return s;
}

// 0 = 水平速度（不拉伸）；1 = 竖直速度（不拉伸）；2 = 竖直速度 + 拉伸；
// 3 = 竖直速度 + 拉伸 + 滚转 1.2rad（应被 kVelocity 忽略）。
class VelocityAlignApp : public JPOV {
public:
    using JPOV::JPOV;

    void SetCase(int c) { case_ = c; }

    // 背景板：一块远处的大黑盒子。作用有两个：
    //   ① 让场景深度有确定值（没有不透明几何时，粒子 pass 的软/硬深度判据会读到垃圾）；
    //   ② 背景恒为纯黑，亮区统计不受天空渐变干扰。
    void Setup() {
        backdrop_mesh_ = RegisterMesh(jpov::MeshData::MakeBox(20.0f, 20.0f, 2.0f));
        backdrop_mat_ = jpov::PBRMaterial::SolidColor({0.0f, 0.0f, 0.0f, 1.0f});
        CHECK_NE(backdrop_mesh_, 0u);
    }

    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;
        cmds->camera.fbo_3d_width_ = kWidth;
        cmds->camera.fbo_3d_height_ = kHeight;
        cmds->camera.position = {0.0f, 0.0f, kCamDistance};
        cmds->camera.target = {0.0f, 0.0f, 0.0f};
        cmds->camera.up = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov = 45.0f;
        cmds->camera.near = 0.05f;
        cmds->camera.far = 100.0f;

        // 天光/太阳/环境光 + tone map：与粒子 gold 场景保持一致的最小可用组合。
        // 太阳压到地平线以下 ⇒ 天空是暗夜色，背景仍是「深色 vs 白粒子」的高对比。
        cmds->sky = jpov::CreateDefaultSkyCommand(
            2.0f, jpov::Color{1.0f, 1.0f, 1.0f, 1.0f},
            jpov::Vec3f{0.0f, -1.0f, 0.0f}, jpov::Vec3f{0.0f, -1.0f, 0.0f});
        jpov::DirectionalLight sun;
        sun.direction = {0.0f, -1.0f, 0.0f};
        sun.color = {1.0f, 1.0f, 1.0f, 1.0f};
        sun.intensity = 0.0f;
        cmds->sun = sun;
        jpov::AmbientLight ambient;
        ambient.color = {0.0f, 0.0f, 0.0f, 1.0f};
        ambient.intensity = 0.0f;
        cmds->ambient = ambient;
        cmds->tone_mapping = true;

        cmds->DrawObject3D(backdrop_mesh_, backdrop_mat_,
                           /*center=*/{0.0f, 0.0f, -6.0f},
                           /*up=*/{0.0f, 1.0f, 0.0f},
                           /*front=*/{0.0f, 0.0f, 1.0f},
                           /*scale=*/1.0f);

        jpov::ParticleInstance p;
        p.position = {0.0f, 0.0f, 0.0f};
        p.width = kSize;
        p.height = kSize;
        // 火星的 rotation 是随机的：kVelocity 下必须忽略它，否则拉伸轴会被转走。
        p.rotation = (case_ == 3) ? 1.2f : 0.0f;
        p.color = {1.0f, 1.0f, 1.0f, 1.0f};
        p.age01 = 0.5f;
        p.velocity = (case_ == 0) ? jpov::Vec3f{kVelocity, 0.0f, 0.0f}
                                  : jpov::Vec3f{0.0f, kVelocity, 0.0f};

        jpov::ParticleRenderCommand pc;
        pc.particles.push_back(p);
        pc.texture_id = 0;              // 纯色 texel，不依赖任何贴图
        pc.flipbook = false;
        pc.align = jpov::AlignMode::kVelocity;
        pc.stretched = (case_ == 2 || case_ == 3);
        pc.stretch_scale = kStretchScale;
        cmds->DrawParticles(std::move(pc));
    }

private:
    int case_ = 0;
    uint32_t backdrop_mesh_ = 0;
    jpov::PBRMaterial backdrop_mat_;
};

void RenderTo(const std::string& out_path, int case_index) {
    JPOV::Config cfg;
    cfg.title = "JPOV particle velocity align test";
    cfg.width = kWidth;
    cfg.height = kHeight;
    cfg.headless = true;
    cfg.resizable = false;

    VelocityAlignApp app(cfg);
    app.Init();
    app.Setup();
    app.SetCase(case_index);

    jpov::WindowInfo winfo;
    winfo.width = static_cast<float>(kWidth);
    winfo.height = static_cast<float>(kHeight);
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, out_path.c_str());
    app.Finalize();
}

// 正方形（面向相机）的检查：面积够大 + 宽高比接近 1。
bool IsFaceOnSquare(const LitStats& s, const std::string& name) {
    if (s.count <= 0) {
        LOG(ERROR) << name << ": 一个亮像素都没有（面片退化成零面积？）";
        return false;
    }
    const float aspect = static_cast<float>(s.width()) / static_cast<float>(s.height());
    // 实测（修好后）：0.4m @ 3m / fov45 / 640x360 ⇒ 118×118 px、13920 个亮像素。
    // 阈值取 1200（≈ 边长的 1/3），只用来卡「退化成细线/零面积」，不限制正常尺寸。
    LOG(INFO) << name << ": lit=" << s.count << " bbox=" << s.width() << "x" << s.height()
              << " aspect=" << aspect;
    if (s.count < 1200) {
        LOG(ERROR) << name << ": 亮区面积过小（" << s.count << " < 1200）";
        return false;
    }
    if (aspect < 0.75f || aspect > 1.33f) {
        LOG(ERROR) << name << ": 宽高比 " << aspect << " 不在 [0.75, 1.33]（面片没正对相机）";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    const std::string out_dir = jpov::GetOutputDir() + "jpov_particle_velocity_align_test/";
    const std::string path_flat_x = out_dir + "velocity_x.png";
    const std::string path_flat_y = out_dir + "velocity_y.png";
    const std::string path_stretched = out_dir + "velocity_y_stretched.png";
    const std::string path_rolled = out_dir + "velocity_y_stretched_roll.png";
    RenderTo(path_flat_x, 0);
    RenderTo(path_flat_y, 1);
    RenderTo(path_stretched, 2);
    RenderTo(path_rolled, 3);

    bool ok = true;
    ok = IsFaceOnSquare(MeasureLit(LoadRgb(path_flat_x)), "速度+X（不拉伸）") && ok;
    ok = IsFaceOnSquare(MeasureLit(LoadRgb(path_flat_y)), "速度+Y（不拉伸）") && ok;

    // 拉伸：up 轴必须就是速度轴 ⇒ 竖直速度应拉成竖长条。
    const LitStats stretched = MeasureLit(LoadRgb(path_stretched));
    if (stretched.count <= 0) {
        LOG(ERROR) << "速度+Y（拉伸）: 一个亮像素都没有";
        ok = false;
    } else {
        const float ratio = static_cast<float>(stretched.height()) /
                            static_cast<float>(stretched.width());
        LOG(INFO) << "速度+Y（拉伸）: lit=" << stretched.count << " bbox="
                  << stretched.width() << "x" << stretched.height() << " h/w=" << ratio;
        // 期望 h/w = 1 + |v|·scale = 2.5；阈值 1.8（允许边缘/AA 的少量误差）。
        if (ratio < 1.8f) {
            LOG(ERROR) << "速度+Y（拉伸）: 高宽比 " << ratio
                       << " < 1.8（拉伸轴不是速度轴，或面片侧对相机）";
            ok = false;
        }
        if (stretched.width() < 20) {
            LOG(ERROR) << "速度+Y（拉伸）: 宽度只有 " << stretched.width()
                       << " px（面片侧对相机被压成细线）";
            ok = false;
        }
    }

    // 滚转必须被忽略：加了 1.2rad 的 roll 后，长条仍然是竖的（h/w ≥ 1.8）。
    const LitStats rolled = MeasureLit(LoadRgb(path_rolled));
    if (rolled.count <= 0) {
        LOG(ERROR) << "速度+Y（拉伸+滚转）: 一个亮像素都没有";
        ok = false;
    } else {
        const float ratio = static_cast<float>(rolled.height()) /
                            static_cast<float>(rolled.width());
        LOG(INFO) << "速度+Y（拉伸+滚转 1.2rad）: lit=" << rolled.count << " bbox="
                  << rolled.width() << "x" << rolled.height() << " h/w=" << ratio;
        if (ratio < 1.8f) {
            LOG(ERROR) << "速度+Y（拉伸+滚转）: 高宽比 " << ratio
                       << " < 1.8（滚转把拉伸轴转走了 —— kVelocity 必须忽略 roll）";
            ok = false;
        }
    }

    if (!ok) {
        return 1;
    }
    LOG(INFO) << "kVelocity 对齐像素测试通过：四张图均为正对相机 / 沿速度拉伸的面片";
    return 0;
}
