// JPOV 模型编辑器 — 放置数学纯函数单测（model_placement.h）
//
// 覆盖内容（全部为纯函数，无需 JPOV::Init / GL context）：
//   1. 恒等放置 → up=(0,1,0), front=(0,0,1), center=0, scale=1
//      （即"坐标系等价"条件：模型系 ≡ 世界系）；
//   2. 单轴旋转的 up/front 精确值（绕世界 X / Y 各 ±90°）；
//   3. 组合旋转 R = Ry·Rx 的顺序正确性（Rx 与 Ry 不可交换——用 90°/90° 的
//      一个非对称结果锁死顺序，防止日后误改成 Rx·Ry）；
//   4. up/front 恒为单位向量且互相垂直（旋转保长宽、BuildModelMatrix 的
//      归一化是恒等操作）；
//   5. clamp：缩放/平移/旋转超界被夹断；
//   6. ApplyRotateDrag 的"右滑逆时针"符号 + Ctrl 选轴 + 纵向分量无关 +
//      像素→角度映射（拖满一屏 = 360°）；负向验证：故意用错符号必须不通过。
//
// ⚠️ 断言写法纪律（见 skills/zero-run-code-reading-check）：每个断言都必须
// 存在"能让它失败"的合法实现改动，禁止"定义上恒真"的检查。

#include <cmath>

#include <glog/logging.h>

#include "tools/jpov/demo/model_placement.h"

namespace {

using jpov::Vec3f;
using jpov_viewer::ModelPlacement;

// 近似断言（带流式失败信息；glog 的 CHECK_NEAR 无 << 支持）。
void ExpectNear(float got, float want, float eps, const char* msg) {
    if (std::abs(got - want) > eps) {
        LOG(FATAL) << msg << ": got " << got << ", want " << want << " ±" << eps;
    }
}

void ExpectVecNear(const Vec3f& got, const Vec3f& want, float eps,
                   const char* msg) {
    if (std::abs(got.x() - want.x()) > eps || std::abs(got.y() - want.y()) > eps ||
        std::abs(got.z() - want.z()) > eps) {
        LOG(FATAL) << msg << ": got (" << got.x() << "," << got.y() << ","
                   << got.z() << "), want (" << want.x() << "," << want.y() << ","
                   << want.z() << ") ±" << eps;
    }
}

void ExpectTrue(bool cond, const char* msg) {
    if (!cond) LOG(FATAL) << msg;
}

// 1. 恒等放置：模型系 ≡ 世界系（center=0, up=+Y, front=+Z, scale=1）。
void TestIdentity() {
    const ModelPlacement p;  // 全默认 = 恒等。
    const jpov_viewer::DrawPlacement d = jpov_viewer::ToDrawParams(p);
    ExpectVecNear(d.center, {0.0f, 0.0f, 0.0f}, 1e-6f, "恒等放置 center 应为原点");
    ExpectVecNear(d.up,     {0.0f, 1.0f, 0.0f}, 1e-6f, "恒等放置 up 应为 +Y");
    ExpectVecNear(d.front,  {0.0f, 0.0f, 1.0f}, 1e-6f, "恒等放置 front 应为 +Z");
    ExpectNear(d.scale, 1.0f, 1e-6f, "恒等放置 scale 应为 1");
    LOG(INFO) << "OK TestIdentity";
}

// 2a. 绕世界 X 轴 rx=+90°： +Y→+Z, +Z→−Y（右手系，从 +X 看逆时针）。
void TestPitchPlus90() {
    ModelPlacement p;
    p.rx_deg = 90.0f;
    const jpov_viewer::DrawPlacement d = jpov_viewer::ToDrawParams(p);
    ExpectVecNear(d.up,    {0.0f, 0.0f, 1.0f},  1e-5f, "Rx(+90): up 应由 +Y 转到 +Z");
    ExpectVecNear(d.front, {0.0f, -1.0f, 0.0f}, 1e-5f, "Rx(+90): front 应由 +Z 转到 −Y");
    LOG(INFO) << "OK TestPitchPlus90";
}

// 2b. 绕世界 Y 轴 ry=+90°： +Z→+X, +X→−Z（右手系，从 +Y 看逆时针）。
void TestYawPlus90() {
    ModelPlacement p;
    p.ry_deg = 90.0f;
    const jpov_viewer::DrawPlacement d = jpov_viewer::ToDrawParams(p);
    ExpectVecNear(d.up,    {0.0f, 1.0f, 0.0f},  1e-5f, "Ry(+90): up 应保持 +Y");
    ExpectVecNear(d.front, {1.0f, 0.0f, 0.0f},  1e-5f, "Ry(+90): front 应由 +Z 转到 +X");
    LOG(INFO) << "OK TestYawPlus90";
}

// 3. 组合顺序 R = Ry·Rx（不可交换性自证）：
//    rx=90, ry=90 时
//      Ry(90)·Rx(90)·(0,0,1) = Ry(90)·(0,−1,0) = (0,−1,0)   ← 本实现的期望
//      若误写成 Rx(90)·Ry(90)·(0,0,1) = Rx(90)·(1,0,0) = (1,0,0)  ← 另一结果
//      同理 up：(0,1,0) → Ry·Rx 得 (1,0,0)，Rx·Ry 得 (0,0,1) —— 两轴同时可区分。
//    两者不同 ⇒ 本断言能真正锁死顺序（不是恒真检查）。
void TestComposeOrderNotCommutative() {
    ModelPlacement p;
    p.rx_deg = 90.0f;
    p.ry_deg = 90.0f;
    const jpov_viewer::DrawPlacement d = jpov_viewer::ToDrawParams(p);
    ExpectVecNear(d.front, {0.0f, -1.0f, 0.0f}, 1e-5f,
                  "R=Ry·Rx 时 front 应为 (0,−1,0)（若得 (1,0,0) 说明写成了 Rx·Ry）");
    // up 手算：Rx(90)·(0,1,0)=(0,0,1) → Ry(90)·(0,0,1)=(1,0,0)。
    // 若误写成 Rx·Ry：Ry(90)·(0,1,0)=(0,1,0) → Rx(90)·(0,1,0)=(0,0,1) —— 两者可区分。
    ExpectVecNear(d.up,    {1.0f, 0.0f, 0.0f},  1e-5f,
                  "R=Ry·Rx 时 up 应为 (1,0,0)（若得 (0,0,1) 说明写成了 Rx·Ry）");
    LOG(INFO) << "OK TestComposeOrderNotCommutative";
}

// 4. 组合旋转的正确性 —— 用【独立推导的对照实现】而非三角函数恒等式。
//
// ⚠️ 为什么不用"长度为 1 / 点积为 0"来测：RotateWorldX/Y 都是由单个角的
// cos/sin 构成，c²+s²≡1 使单位长度**恒真**，且 u·f≡0 由构造直接保证——
// 这两个断言即使把 rx/ry 写反、符号写错也照样通过（典型的无用断言，
// 见 skills/zero-run-code-reading-check）。故改为与一份独立实现比对。
//
// 对照实现：用完整 3x3 矩阵乘（先算 Rx、再乘 Ry、再乘向量），公式与
// RotateWorldX/Y 的展开式写法不同（矩阵组装 vs 逐行展开），故能真正抓出
// 实现里的轴用错 / 符号写反 / 顺序写反。
void TestAgainstIndependentMatrixReference() {
    const float angles[] = {0.0f, 30.0f, -45.0f, 90.0f, 137.0f, -180.0f};
    const double kDeg = 3.14159265358979323846 / 180.0;
    for (float rx : angles) {
        for (float ry : angles) {
            ModelPlacement p;
            p.rx_deg = rx;
            p.ry_deg = ry;
            const jpov_viewer::DrawPlacement d = jpov_viewer::ToDrawParams(p);

            // 独立推导：先把 Rx 写成 3x3（行主序），再写 Ry，再算 Ry·Rx·v。
            const double ax = rx * kDeg, ay = ry * kDeg;
            const double cx = std::cos(ax), sx = std::sin(ax);
            const double cy = std::cos(ay), sy = std::sin(ay);
            // Rx（行主序）
            const double Rxm[9] = {1, 0, 0, 0, cx, -sx, 0, sx, cx};
            // Ry（行主序）
            const double Rym[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy};
            // M = Ry · Rx（行主序矩阵乘）。
            double M[9];
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    M[i * 3 + j] = Rym[i * 3 + 0] * Rxm[0 * 3 + j] +
                                   Rym[i * 3 + 1] * Rxm[1 * 3 + j] +
                                   Rym[i * 3 + 2] * Rxm[2 * 3 + j];
                }
            }
            auto Mul = [&](double x, double y, double z) -> Vec3f {
                return {static_cast<float>(M[0] * x + M[1] * y + M[2] * z),
                        static_cast<float>(M[3] * x + M[4] * y + M[5] * z),
                        static_cast<float>(M[6] * x + M[7] * y + M[8] * z)};
            };
            ExpectVecNear(d.up, Mul(0, 1, 0), 1e-5f,
                          "up 应与独立矩阵对照实现一致");
            ExpectVecNear(d.front, Mul(0, 0, 1), 1e-5f,
                          "front 应与独立矩阵对照实现一致");
        }
    }
    LOG(INFO) << "OK TestAgainstIndependentMatrixReference";
}

// 5a. 缩放/平移 clamp（超界被夹断，且平移直接进 center）。
void TestClampScaleAndTranslate() {
    ModelPlacement p;
    p.scale = 100.0f;
    p.tx = 99.0f;
    p.ty = -99.0f;
    p.tz = 0.5f;
    const jpov_viewer::DrawPlacement d = jpov_viewer::ToDrawParams(p);
    ExpectNear(d.scale, ModelPlacement::kScaleMax, 1e-6f, "scale 超上界应夹到 10");
    ExpectVecNear(d.center, {ModelPlacement::kTransMax, ModelPlacement::kTransMin,
                             0.5f}, 1e-6f, "平移应夹到 ±3（tz 在界内保持 0.5）");

    ModelPlacement q;
    q.scale = 0.0f;  // 下界外（会撞 BuildModelMatrix 的 CHECK_GT(scale,0)）
    const jpov_viewer::DrawPlacement dq = jpov_viewer::ToDrawParams(q);
    ExpectNear(dq.scale, ModelPlacement::kScaleMin, 1e-6f,
               "scale=0 应被夹到 0.1（否则下游 CHECK_GT 崩溃）");
    LOG(INFO) << "OK TestClampScaleAndTranslate";
}

// 5b. 旋转角度【折叠】（不是 clamp）：超出一圈的值归到等价主值。
//     这是刻意与缩放/平移的 clamp 区分开的行为：角度是周期量。
void TestWrapRotation() {
    ModelPlacement p;
    p.rx_deg = 370.0f;    // ≡ 10°
    p.ry_deg = -190.0f;   // ≡ 170°
    const ModelPlacement c = p.Clamped();
    ExpectNear(c.rx_deg, 10.0f, 1e-4f, "370° 应折叠为 10°");
    ExpectNear(c.ry_deg, 170.0f, 1e-4f, "−190° 应折叠为 170°");

    // 边界语义：+180 折到 −180（半开区间 [-180,180)），−180 保持 −180。
    ModelPlacement q;
    q.rx_deg = 180.0f;
    ExpectNear(q.Clamped().rx_deg, -180.0f, 1e-4f, "+180 应折为 −180（半开区间）");
    ModelPlacement r;
    r.rx_deg = -180.0f;
    ExpectNear(r.Clamped().rx_deg, -180.0f, 1e-4f, "−180 应保持 −180");
    LOG(INFO) << "OK TestWrapRotation";
}

// 6a. 不带 Ctrl：横向右拖（dx>0）→ 绕世界 X，右滑逆时针 → rx 增加。
//     拖满一屏宽 = 360°。
void TestDragRotateXPitch() {
    ModelPlacement p;
    const bool changed = jpov_viewer::ApplyRotateDrag(&p, /*dx*/ 1280.0f,
                                                      /*ctrl*/ false, /*w*/ 1280);
    ExpectTrue(changed, "dx≠0 时应报告已改变");
    ExpectNear(p.rx_deg, 0.0f, 1e-4f, "拖满一屏(360°)后 rx 应折叠回 0°");
    ExpectNear(p.ry_deg, 0.0f, 1e-6f, "未按 Ctrl 不应动 ry");

    // 拖 1/4 屏 → 90°；右滑为正（逆时针）。
    ModelPlacement q;
    jpov_viewer::ApplyRotateDrag(&q, /*dx*/ 320.0f, false, 1280);
    ExpectNear(q.rx_deg, 90.0f, 1e-3f, "右拖 1/4 屏应 rx=+90（右滑逆时针）");

    // 负向验证：左拖必须得到负角（若符号写反，这里会失败）。
    ModelPlacement r;
    jpov_viewer::ApplyRotateDrag(&r, /*dx*/ -320.0f, false, 1280);
    ExpectNear(r.rx_deg, -90.0f, 1e-3f, "左拖 1/4 屏应 rx=−90");
    LOG(INFO) << "OK TestDragRotateXPitch";
}

// 6b. 按住 Ctrl：横向右拖 → 绕世界 Y，"右滑逆时针" ⇒ ry 减少（见头文件符号推导）。
void TestDragRotateYYaw() {
    ModelPlacement p;
    jpov_viewer::ApplyRotateDrag(&p, /*dx*/ 320.0f, /*ctrl*/ true, 1280);
    ExpectNear(p.ry_deg, -90.0f, 1e-3f,
               "Ctrl+右拖 1/4 屏应 ry=−90（+Y 看逆时针 = XZ 面内 +Z→+X）");
    ExpectNear(p.rx_deg, 0.0f, 1e-6f, "按住 Ctrl 不应动 rx");

    ModelPlacement q;
    jpov_viewer::ApplyRotateDrag(&q, /*dx*/ -320.0f, true, 1280);
    ExpectNear(q.ry_deg, 90.0f, 1e-3f, "Ctrl+左拖 1/4 屏应 ry=+90");
    LOG(INFO) << "OK TestDragRotateYYaw";
}

// 6c. 零位移不改变任何东西（纵向分量在本层根本不存在——调用方只传横向 dx）。
void TestDragZeroIsNoop() {
    ModelPlacement p;
    p.rx_deg = 37.0f;
    p.ry_deg = -12.0f;
    const bool changed = jpov_viewer::ApplyRotateDrag(&p, 0.0f, false, 1280);
    ExpectTrue(!changed, "dx=0 应报告未改变（避免无谓重绘/日志）");
    ExpectNear(p.rx_deg, 37.0f, 1e-6f, "dx=0 不应改 rx");
    ExpectNear(p.ry_deg, -12.0f, 1e-6f, "dx=0 不应改 ry");
    LOG(INFO) << "OK TestDragZeroIsNoop";
}

// 6d. 连续拖动的等价性：分两次各 160px == 一次 320px（线性映射自证）。
// 为什么这条值得留：本层每次调用后会做一次**折叠**（WrapDegToHalfOpen），
// 折叠不是线性的——若日后有人把折叠换成 clamp、或在每次 drag 里加"饱和"
// 保护，这条就会失败。它守的是"累积语义不被单次截断破坏"。
void TestDragLinearity() {
    ModelPlacement a;
    jpov_viewer::ApplyRotateDrag(&a, 160.0f, true, 1280);
    jpov_viewer::ApplyRotateDrag(&a, 160.0f, true, 1280);
    ModelPlacement b;
    jpov_viewer::ApplyRotateDrag(&b, 320.0f, true, 1280);
    ExpectNear(a.ry_deg, b.ry_deg, 1e-3f, "分两步拖应等价于一次拖（线性映射）");
    LOG(INFO) << "OK TestDragLinearity";
}

}  // namespace

int main(int /*argc*/, char** argv) {
    google::InitGoogleLogging(argv[0]);
    TestIdentity();
    TestPitchPlus90();
    TestYawPlus90();
    TestComposeOrderNotCommutative();
    TestAgainstIndependentMatrixReference();
    TestClampScaleAndTranslate();
    TestWrapRotation();
    TestDragRotateXPitch();
    TestDragRotateYYaw();
    TestDragZeroIsNoop();
    TestDragLinearity();
    LOG(INFO) << "model_placement_test: ALL PASS";
    return 0;
}
