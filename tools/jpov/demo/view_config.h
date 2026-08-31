// JPOV glTF 交互查看器 — 共享视角配置与正午光照（header-only）
//
// 架构文档：tools/jpov/docs/jpov_gltf_viewer_arch.md（task #2）
//
// 核心抽象：ViewConfig —— 一条独占地决定相机姿态的元数据。
// 交互模式与 --four_views 拍照模式都只是“持有并消费一个 ViewConfig”：
//   - 交互：由右键 drag + 滚轮实时改 view_
//   - 拍照：由固定 (phi, theta) 序列赋给 view_
// 二者走同一条 OneIteration 渲染体、同一份 MakeNoonLighting()，保证 zero 分叉。
//
// 本头文件纯函数化（ViewConfig / ApplyInput / MakeNoonLighting 均为纯函数），
// 可被 demo 主程序与单测（view_config_test.cc）共用。

#ifndef JPOV_DEMO_VIEW_CONFIG_H_
#define JPOV_DEMO_VIEW_CONFIG_H_

#include <algorithm>
#include <cmath>

#include "tools/jpov/include/jpov/jpov.h"

namespace jpov_viewer {

// 相机绕目标点 (0,0,0) 环绕的球面角配置（y-up）。
//
// 相机位置由公式推导：
//   position.x = R * cos(phi) * sin(theta)
//   position.y = R * sin(phi)
//   position.z = R * cos(phi) * cos(theta)
//   target     = {0, 0, 0}
//
// 量纲约定：phi/theta 统一用弧度存储（避免“这个接口收度还是收弧度”的歧义）。
// 交互映射用“度”表达更直观，在 ApplyInput 边界处换算；拍照固定角度直接用度。
struct ViewConfig {
    double phi   = 0.0;   // 仰角（弧度），clamp 到 [-π/2, +π/2]
    double theta = 0.0;   // 方位角（弧度），无 clamp（可无限转）
    double R     = 10.0;  // 相机到目标点距离（米），范围 [0.1, 300]

    // 最小 / 最大相机距离（米），滚轮 clamp。
    static constexpr double kRMin = 0.1;
    static constexpr double kRMax = 300.0;

    // 目标点：恒定 (0,0,0)。
    static jpov::Vec3f Target() { return {0.0f, 0.0f, 0.0f}; }

    // 由 phi/theta/R 推导相机位置（y-up）。
    jpov::Vec3f Position() const {
        const float cosp = static_cast<float>(std::cos(phi));
        const float sinp = static_cast<float>(std::sin(phi));
        const float sint = static_cast<float>(std::sin(theta));
        const float cost = static_cast<float>(std::cos(theta));
        const float r    = static_cast<float>(R);
        return {r * cosp * sint, r * sinp, r * cosp * cost};
    }

    // 由模型包围盒计算“能看清全貌”的初始相机距离 R（模型自适应）。
    //
    // 目标点假设对准包围盒中心（查看器把模型画在原点、相机 lookAt 原点，
    // 故两者一致）。为使最远顶点落在垂直视锥的 half-fov 内，需
    //   R ≥ d_max / tan(fov_deg/2)
    // 其中 d_max = 包围盒 8 个顶点到目标点（原点）的最大距离（即相对原点的
    // 最远包围球半径）。再乘 1.3 的保险因子，避免模型贴边/角部被切。
    //
    // 返回 clamp 到 [kRMin, kRMax] 的值，可直接赋给 ViewConfig::R。
    // fov_deg 为垂直视野（度），>0 且 <180。bounds 用 GltfObject::bounds_*。
    static double FitRadius(const float bounds_min[3], const float bounds_max[3],
                            double fov_deg) {
        constexpr double kMargin = 1.3;
        const double half_fov = 0.5 * fov_deg * 3.14159265358979323846 / 180.0;
        CHECK(half_fov > 0.0 && half_fov < 3.14159265358979323846 / 2.0)
            << "FitRadius: fov_deg 必须在 (0,180) 内, got " << fov_deg;

        // d_max = 包围盒 8 顶点到原点（目标点）的最大距离。
        double d2_max = 0.0;
        for (int ix = 0; ix <= 1; ++ix) {
            const double x = (ix == 0) ? bounds_min[0] : bounds_max[0];
            for (int iy = 0; iy <= 1; ++iy) {
                const double y = (iy == 0) ? bounds_min[1] : bounds_max[1];
                for (int iz = 0; iz <= 1; ++iz) {
                    const double z = (iz == 0) ? bounds_min[2] : bounds_max[2];
                    d2_max = std::max(d2_max, x * x + y * y + z * z);
                }
            }
        }
        const double d_max = std::sqrt(d2_max);
        if (d_max <= 0.0) return kRMin;  // 退化（点/空），取最小距离。

        const double r = (d_max / std::tan(half_fov)) * kMargin;
        return std::clamp(r, kRMin, kRMax);
    }
};

// 交互输入 → 更新 ViewConfig（仅 interactive 模式调用；four_views 直接赋固定角）。
//
//   dx/dy      — 本帧右键 drag 位移（像素）。右拖水平 dx → θ 变化，dy → φ 变化。
//   scroll     — 本帧滚轮增量（格，+1 上滚 / -1 下滚）。每格 R ×√2 或 ×1/√2。
//   window_w/h — 窗口尺寸（像素），把像素位移映射成角度。
//
// 映射规则（需求定稿）：
//   - 水平右拖 1280px = θ 转 360°（2π）→ θ += dx * (2π/window_w)
//   - 垂直右拖 720px = φ 变 180°（π）→ φ += dy * (π/window_h)
//   注意窗口像素 y 向下，而 φ 向上为正 → dy 取负（推鼠标向下 → 相机仰角降）。
//   - 滚轮每格 R ×√2（上滚）/ ×(1/√2)（下滚）；R clamp 到 [kRMin, kRMax]
//   - φ clamp 到 ±90°（±π/2 弧度）；θ 不 clamp（可无限转）
inline void ApplyInput(ViewConfig* v /*inout*/, float dx, float dy,
                       float scroll, int window_w, int window_h) {
    CHECK_NOTNULL(v);
    CHECK_GT(window_w, 0);
    CHECK_GT(window_h, 0);

    constexpr double kPiHalf = 3.14159265358979323846 / 2.0;

    // 像素 → 角度（用双精度累积，避免 long 会话漂移）。
    v->theta += static_cast<double>(dx) * (2.0 * 3.14159265358979323846 /
                                           static_cast<double>(window_w));
    v->phi   += -static_cast<double>(dy) * (3.14159265358979323846 /
                                            static_cast<double>(window_h));

    // 滚轮 zoom：+1 上滚放大（R ×1/√2），-1 下滚缩小（R ×√2）。
    // scroll 是帧内累计的格数增量（GLFW 一次 fast flick 可能累计多格），
    // 每格按同一倍率连乘，避免“多格当作一格放大”的错误：
    //   上滚 n 格 → R × (1/√2)^n；下滚 n 格 → R × (√2)^n。
    const double ticks = static_cast<double>(scroll);
    if (ticks != 0.0) {
        v->R *= std::pow(std::sqrt(2.0), -ticks);  // -n：上滚为正→缩小
    }

    // clamp。
    v->phi = std::clamp(v->phi, -kPiHalf, kPiHalf);
    v->R   = std::clamp(v->R, ViewConfig::kRMin, ViewConfig::kRMax);
}

// 默认初始视角：目标点 (0,0,0)，方位 45°，仰角 0°，R=√2（单位尺度模型）。
// 初始相机位置 = (1, 0, 1)（r=√2, theta=π/4, phi=0 → {cos·sin·R, 0, cos·cos·R}）。
//
// 注意：加载真实模型后，main 会按模型包围盒用 ViewConfig::FitRadius 覆盖
// 这里的固定 R（模型大小自适应）；DefaultView 的 R 仅作为无模型/退化时的兜底。
inline ViewConfig DefaultView() {
    constexpr double kPi = 3.14159265358979323846;
    ViewConfig v;
    v.phi   = 0.0;
    v.theta = kPi / 4.0;   // 45°
    v.R     = std::sqrt(2.0);
    return v;
}

// 正午晴天光照（sky + 太阳方向光 + 环境光）—— 交互 / four_views / gold 三处共用。
//
// 需求：光照用 DaySkyCommand 正午配置，同 sun_path 测试同款方式
// （由 SkyCommand 推导平行光、推导全局 Ambient）。太阳方向默认 (0,-1,-1)。
// 平行光与 ambient 的 color/intensity 均由 sky 推导：color 用 DirectionalColor()/
// AmbientColor()（色调随太阳仰角变），intensity 用 DirectionalIntensity()/
// AmbientIntensity()（相对衰减随太阳仰角变），但绝对强度锚定在
// LIGHT_INTENSITY.md 三·五 晴天正午基准（sun=3.0 : ambient=0.3，5:1，PR#60 教训
// —— 绝不是 0.5 或裸 AmbientIntensity()=1.0，否则影子会被 ACES 压没）。
struct NoonLighting {
    jpov::DaySkyCommand  sky;
    jpov::DirectionalLight sun;
    jpov::AmbientLight   ambient;
};

// 构造正午光照：太阳光传播方向 (0,-1,-1)。
inline NoonLighting MakeNoonLighting() {
    const jpov::Vec3f sun_light_dir = {0.0f, -1.0f, -1.0f};
    jpov::DaySkyCommand sky{
        /*sun_dir*/ jpov::Vec3f(-sun_light_dir.x(), -sun_light_dir.y(),
                                -sun_light_dir.z()),  // = {0,1,1}
        /*turbidity*/ 2.0f,
        /*season*/ {1.0f, 1.0f, 1.0f, 1.0f},
        /*intensity*/ 1.0f,
        /*ground_color*/ {0.05f, 0.06f, 0.08f, 1.0f},
        /*sun_radius*/ 0.02,
        /*sun_brightness*/ 1e3,
        /*sun_glow*/ 0.0,
    };
    NoonLighting nl;
    nl.sky = sky;
    nl.sun = jpov::DirectionalLight{
        /*direction*/ sun_light_dir,
        /*color*/ sky.DirectionalColor(),
        /*intensity*/ sky.DirectionalIntensity(),
    };
    // ambient 强度也由 SkyCommand 推导（同 sun_path/太阳同款方式），但把绝对强度
    // 锚定到 LIGHT_INTENSITY.md 三·五 的晴天正午基准（sun=3.0 : ambient=0.3，5:1，
    // ACES 线性区防影子被压缩消失）。AmbientIntensity(0.3) = 0.3 × 相对天光衰减曲线
    // (sun_dir)，正午据此自动变暗/变亮——既满足 task#3『Ambient 由推导得到』，
    // 又守住 PR #60 定标的 0.3 基准（勿改成 0.5 或裸 AmbientIntensity()=1.0）。
    nl.ambient = jpov::AmbientLight{
        .color = sky.AmbientColor(),
        .intensity = sky.AmbientIntensity(0.3f),
    };
    return nl;
}

// 参数化光照 —— 交互滑条版（窗口下方滑条共用）。
//
// 与 MakeNoonLighting()（固定正午 (0,-1,-1)/3.0/0.3）保持同一套 sky 推导规则（color
// 仍由 sky 自动推导，色调随太阳仰角变），但把 key 调整成“细调太阳光照”需要的语义：
// 太阳角度直接用【仰角】表达，intensity/ambient 直接赋【实际强度】（不再乘 PWL
// 相对衰减曲线）。这是 gltf viewer 作为标定调试工具的预期行为——滑条所见即所得，
// 方便对照 LIGHT_INTENSITY.md / sun_path 的锚点反推要重标的两个 PWL 表。
//
//   elev_deg         — 太阳仰角（度，[0, 90]）。0°=贴地/日出日落，90°=天顶/正午。
//                      太阳光传播方向（sun.direction）取 {−cosx, −sinelev, 0} 这种
//                      y 分量随仰角抬升（仰角越高 y 越负，光从越高处照向场景）；
//                      传给 sky 的 sun_dir 取反，y = sin(elev_deg)，正好驱动
//                      DirectionalColor()/AmbientColor() 的仰角色调曲线。
//                      （注意：滑条直接给仰角，不再像旧版 theta 那样 0=正午、
//                      越大越贴地，左到右 0°→90° 就是日出→正午，完整覆盖标定工况。）
//   turbidity        — 大气浊度 [2,8]。同时影响 (a) 天空画色（shader 霾化/日盘）
//                      + (b) sun/ambient 的 turb 衰减乘子（TurbSunLoss/TurbAmbLoss）。
//                      滑条用它肉眼标定“高浊度下 sun/ambient 强度分别该衰减成多少”。
//   season_r         — 季节色温 R 通道乘子 [0.5, 2.0]，默认 1.0 中性（见 SeasonTintScale
//                      归一化，只偏色不改亮度），联动 sky 天空背景 + sun/ambient 色温。
//
// 注：sun/ambient 的 intensity 这里**不再由调用方给定**，全部走 sky 自动推导
// （sky.DirectionalIntensity()/sky.AmbientIntensity()，含 Turb*Loss 乘子 + 仰角曲线），
// 因此不再需要 sun_intensity/ambient_intensity 两个参数（gltf viewer 已移除对应滑条）。
// season 这里由 season_r（R 通道）给定（G/B=1.0）；sky 其余参数（intensity/ground_color/
// sun_radius/sun_brightness/sun_glow）由本函数固定。
inline NoonLighting MakeLighting(float elev_deg, float turbidity,
                                 float season_r = 1.0f) {
    // 太阳仰角（度）→ 弧度。sun_dir（指向太阳）y 分量 = sin(仰角)：
    // 仰角 0° → y=0（贴地），仰角 90° → y=1（天顶）。
    const float elev_rad = elev_deg * (static_cast<float>(M_PI) / 180.0f);
    const float sy = std::sin(elev_rad);
    const float sx = std::cos(elev_rad);   // 水平分量（固定 +x 方向）
    const jpov::Vec3f sun_dir = {sx, sy, 0.0f};          // 指向太阳
    const jpov::Vec3f sun_light_dir = {-sx, -sy, 0.0f};  // = −sun_dir（传播方向）
    jpov::DaySkyCommand sky{
        /*sun_dir*/ sun_dir,
        /*turbidity*/ turbidity,   // 影响天空画色（霾化/日盘）+ Turb*Loss 乘子
        /*season*/ {season_r, 1.0f, 1.0f, 1.0f},  // 只调 R 通道（滑条季节色温）
        /*intensity*/ 1.0f,
        /*ground_color*/ {0.05f, 0.06f, 0.08f, 1.0f},
        /*sun_radius*/ 0.02,
        /*sun_brightness*/ 1e3,
        /*sun_glow*/ 0.0,
    };
    NoonLighting nl;
    nl.sky = sky;
    // 基准强度（滑条所见即所得）× 浊度衰减（turb=2 时 Loss=1.0，不改变基准）。
    // 高浊度下 sun 掉得快（直射 DNI 强衰减），ambient 掉得慢（散射缓和），
    // 两者各自标定。
    nl.sun = jpov::DirectionalLight{
        /*direction*/ sun_light_dir,
        /*color*/ sky.DirectionalColor(),
        /*intensity*/ sky.DirectionalIntensity(),
    };
    nl.ambient = jpov::AmbientLight{
        .color = sky.AmbientColor(),
        .intensity = sky.AmbientIntensity(),
    };
    return nl;
}

// 构造 300×300 米高粗糙灰色地平面的 CPU 网格数据（单平铺 quad，法线 +Y）。
// y 可调（默认 -3）：让加载在原点附近的模型（尤其底面在原点的）不受地板穿插影响；
// 需求初定为 y=-3，后由查看器“地面高度”滑条 [-3,+3] 实时调节（看物体落地产影）。
inline jpov::MeshData MakeGroundQuad(float y = -3.0f) {
    const float half = 150.0f;  // 300m 半宽
    jpov::MeshData mesh;
    mesh.flags = static_cast<jpov::MeshVertexFlags>(
        static_cast<uint8_t>(jpov::MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kNormal));
    // 4 角点（y=-3），CCW 从上方看（法线 +Y 向上）。
    mesh.positions = {
        {-half, y,  half},  // 0
        { half, y,  half},  // 1
        { half, y, -half},  // 2
        {-half, y, -half},  // 3
    };
    mesh.normals = {
        {0.0f, 1.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
    };
    mesh.indices = {0, 1, 2, 0, 2, 3};
    mesh.Validate();
    return mesh;
}

// 高粗糙灰色地面材质（roughness≈1、金属度 0）。
inline jpov::PBRMaterial GroundMaterial() {
    return jpov::PBRMaterial::SolidColorMR(
        /*color*/ {0.5f, 0.5f, 0.5f, 1.0f},
        /*metallic*/ 0.0f,
        /*roughness*/ 1.0f);
}

}  // namespace jpov_viewer

#endif  // JPOV_DEMO_VIEW_CONFIG_H_
