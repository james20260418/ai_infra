// JPOV glTF 查看器 — ViewConfig / ApplyInput / MakeNoonLighting 纯函数单测
//
// 覆盖架构文档 §2（ViewConfig + ApplyInput）里的可独立验证行为：
//   - Position()：默认视角精确复现 (1,1,1)→(0,1,0)
//   - ApplyInput：像素→角度映射（1280=360°、720=180°）、φ clamp ±90°、
//                 R clamp [0.1,300]、滚轮 √2 缩放
//   - MakeNoonLighting：正午日照三元组非空、平行光与 ambient 均"由 SkyCommand
//     推导"（color 用 DirectionalColor/AmbientColor，intensity 用
//     DirectionalIntensity/AmbientIntensity，绝对强度锚定 LIGHT_INTENSITY.md
//     三·五 晴天基准 sun=3.0 : ambient=0.3，5:1）
//
// 纯函数测试，无需 JPOV::Init（不创建 GL context）。

#include <cmath>

#include <glog/logging.h>

#include "tools/jpov/demo/view_config.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

// 断言两向量近似相等（glog 的 CHECK_NEAR 无 << 流支持，这里自写带流版本）。
void ExpectVecNear(const jpov::Vec3f& a, const jpov::Vec3f& b, float eps) {
    if (std::abs(a.x() - b.x()) > eps || std::abs(a.y() - b.y()) > eps ||
        std::abs(a.z() - b.z()) > eps) {
        LOG(FATAL) << "向量不近似相等: (" << a.x() << "," << a.y() << "," << a.z()
                   << ") vs (" << b.x() << "," << b.y() << "," << b.z()
                   << ") eps=" << eps;
    }
}

// 近似相等断言（带流式失败信息）。
template <typename T>
void ExpectNear(T val, T expected, double eps, const char* msg) {
    if (std::abs(static_cast<double>(val - expected)) > eps) {
        LOG(FATAL) << msg << ": got " << val << ", expected " << expected
                   << " (±" << eps << ")";
    }
}

void TestDefaultViewPosition() {
    // 默认视角 (1,1,1)→(0,1,0) 的 ViewConfig 是 {phi=0, theta=π/4, R=√2}。
    const jpov_viewer::ViewConfig v = jpov_viewer::DefaultView();
    ExpectVecNear(v.Position(), /*pos*/ {1.0f, 1.0f, 1.0f}, 1e-4f);
    LOG(INFO) << "OK TestDefaultViewPosition";
}

void TestThetaMapping() {
    // 水平右拖 1280px = θ 转 360°。
    jpov_viewer::ViewConfig v = jpov_viewer::DefaultView();
    const double theta0 = v.theta;
    jpov_viewer::ApplyInput(&v, /*dx*/ 1280.0f, /*dy*/ 0.0f, /*scroll*/ 0.0f,
                            /*w*/ 1280, /*h*/ 720);
    ExpectNear(v.theta - theta0, 2.0 * kPi, 1e-9, "右拖满屏 1280px 应 θ 转 360°");
    LOG(INFO) << "OK TestThetaMapping";
}

void TestPhiMappingAndClamp() {
    // 垂直 drag: 720px = φ 变 180°。且 φ clamp 到 ±90°。
    jpov_viewer::ViewConfig v = jpov_viewer::DefaultView();
    // 向下推鼠标（dy>0）→ φ 减小（负），推整整一屏 → -180°，clamp 到 -90°。
    jpov_viewer::ApplyInput(&v, 0.0f, /*dy*/ 720.0f, 0.0f, 1280, 720);
    ExpectNear(v.phi, -kPi / 2.0, 1e-9, "φ 应 clamp 到 -90°");

    // 一直向上拖 → +90° clamp。
    v = jpov_viewer::DefaultView();
    jpov_viewer::ApplyInput(&v, 0.0f, /*dy*/ -720.0f * 3.0f, 0.0f, 1280, 720);
    ExpectNear(v.phi, kPi / 2.0, 1e-9, "φ 应 clamp 到 +90°");
    LOG(INFO) << "OK TestPhiMappingAndClamp";
}

void TestRClampAndZoom() {
    // R 范围 [0.1, 300]。
    jpov_viewer::ViewConfig v = jpov_viewer::DefaultView();
    // 大量下滚（scroll=+1 每格 R/√2... 实际scroll>0 是缩小，见实现）：
    // 这里验证 clamp：直接把 R 推到极小（scroll 很大正 = 缩小）→ 触底 0.1。
    v.R = 300.0;
    for (int i = 0; i < 1000; ++i) {
        jpov_viewer::ApplyInput(&v, 0, 0, /*scroll*/ 1.0f, 1280, 720);
    }
    ExpectNear(v.R, jpov_viewer::ViewConfig::kRMin, 1e-9, "R 应触底 clamp 到 0.1");

    // scroll=-1 缩小→R 增大到 300。
    v.R = jpov_viewer::ViewConfig::kRMin;
    for (int i = 0; i < 1000; ++i) {
        jpov_viewer::ApplyInput(&v, 0, 0, /*scroll*/ -1.0f, 1280, 720);
    }
    ExpectNear(v.R, jpov_viewer::ViewConfig::kRMax, 1e-9, "R 应触顶 clamp 到 300");

    // 单格 √2 缩放：scroll=+1 上滚放大（R 减为 1/√2）。
    v = jpov_viewer::DefaultView();
    const double r0 = v.R;
    jpov_viewer::ApplyInput(&v, 0, 0, /*scroll*/ 1.0f, 1280, 720);
    ExpectNear(v.R, r0 / std::sqrt(2.0), 1e-9, "上滚一格 R 应 ×1/√2");

    // 同帧多格：scroll=+2 应看作两格连乘（×1/√2 两次），而非整体当一格。
    v = jpov_viewer::DefaultView();
    const double r1 = v.R;
    jpov_viewer::ApplyInput(&v, 0, 0, /*scroll*/ 2.0f, 1280, 720);
    ExpectNear(v.R, r1 / 2.0, 1e-9, "上滚两格 R 应 ×(1/√2)²=×1/2");

    // 下滚两格：R ×(√2)²=×2。
    v = jpov_viewer::DefaultView();
    const double r2 = v.R;
    jpov_viewer::ApplyInput(&v, 0, 0, /*scroll*/ -2.0f, 1280, 720);
    ExpectNear(v.R, r2 * 2.0, 1e-9, "下滚两格 R 应 ×(√2)²=×2");
    LOG(INFO) << "OK TestRClampAndZoom";
}

void TestMakeNoonLighting() {
    const jpov_viewer::NoonLighting nl = jpov_viewer::MakeNoonLighting();
    // 太阳光传播方向 (0,-1,-1)。
    ExpectVecNear(nl.sun.direction, {0.0f, -1.0f, -1.0f}, 1e-6f);
    // sky sun_dir = -(光传播方向) = (0,1,1)。
    ExpectVecNear(nl.sky.sun_dir, {0.0f, 1.0f, 1.0f}, 1e-6f);

    // sun.intensity 应等于“由 sky 推导”的 DirectionalIntensity(正午基准 3.0)，
    // 而非硬编码。正午方向 (0,1,1) 仰角 45° → DNI 系数 0.8513 → ≈2.55。
    // 这里不自造预期值，直接重算 sky 推导结果做一致性校验（防提交时手写死）。
    // sun.color 应由 sky.DirectionalColor() 推导（色调跟随太阳仰角自动变化）。
    const jpov::Color dc = nl.sky.DirectionalColor();
    ExpectNear(nl.sun.color.r, dc.r, 1e-6f, "sun.color.r 应由 sky 推导");
    ExpectNear(nl.sun.color.g, dc.g, 1e-6f, "sun.color.g 应由 sky 推导");
    ExpectNear(nl.sun.color.b, dc.b, 1e-6f, "sun.color.b 应由 sky 推导");
    ExpectNear(static_cast<double>(nl.sun.intensity),
               static_cast<double>(nl.sky.DirectionalIntensity()),
               1e-6, "sun.intensity 应由 sky.DirectionalIntensity() 推导");

    // ambient 强度应“由 SkyCommand 推导”，且锚定到 LIGHT_INTENSITY.md 三·五
    // 晴天正午基准 0.3（PR #60 定标，勿 0.5）。task#3 要求『Ambient 由推导得到』，
    // 故用 sky.AmbientIntensity(0.3) 作为一致性判据（同上方 sun 的推导即校验方式，
    // 不手写死预期值，防提交时被写死）。
    const float expect_amb = nl.sky.AmbientIntensity(0.3f);
    ExpectNear(nl.ambient.intensity, expect_amb, 1e-6f, "ambient.intensity 应由 sky.AmbientIntensity(0.3) 推导");
    LOG(INFO) << "OK TestMakeNoonLighting";
}

void TestGroundQuad() {
    const jpov::MeshData mesh = jpov_viewer::MakeGroundQuad();
    CHECK_EQ(mesh.positions.size(), 4u);
    CHECK_EQ(mesh.indices.size(), 6u);
    // y=0 且顶点跨度 ±150。
    for (const auto& p : mesh.positions) {
        ExpectNear(p.y(), 0.0f, 1e-6f, "地平面应在 y=0");
        CHECK_LE(std::abs(p.x()), 150.0f + 1e-6f);
        CHECK_LE(std::abs(p.z()), 150.0f + 1e-6f);
    }
    LOG(INFO) << "OK TestGroundQuad";
}

}  // namespace

int main() {
    TestDefaultViewPosition();
    TestThetaMapping();
    TestPhiMappingAndClamp();
    TestRClampAndZoom();
    TestMakeNoonLighting();
    TestGroundQuad();
    LOG(INFO) << "全部 ViewConfig 单测通过";
    return 0;
}
