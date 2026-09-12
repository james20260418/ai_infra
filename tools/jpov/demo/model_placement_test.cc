// JPOV 模型编辑器 — 放置数学纯函数单测（model_placement.h）
//
// 覆盖内容（全部为纯函数，无需 JPOV::Init / GL context）：
//   1. 恒等放置 → up=(0,1,0), front=(0,0,1), center=0, scale=1
//      （即"坐标系等价"条件：模型系 ≡ 世界系）；
//   2. 单轴增量旋转的 up/front 精确值（绕世界 X / Y 各 ±90°）；
//   3. **任意顺序的世界轴累积**（本设计的核心动机）：绕 Y 转完再绕 X 转，
//      结果必须等于"两次世界轴旋转的实际复合"—— 这正是两个 float 欧拉角
//      做不到、而矢量状态做得到的（用固定轴序 Rx·Ry 参数化的实现会 FAIL）；
//   4. 正交归一不变量（up/front 永远单位长且垂直 —— 保证 BuildModelMatrix
//      内部归一化是恒等操作）；
//   5. clamp：缩放/平移超界被夹断；
//   6. ApplyRotateDrag 的"右滑逆时针"符号 + Ctrl 选轴 + 零位移无副作用 +
//      像素→角度映射（拖满一屏 = 360°）。
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

// 2a. 绕世界 X 轴增量 +90°： +Y→+Z（up）, +Z→−Y（front）。
void TestPitchPlus90() {
    ModelPlacement p;
    jpov_viewer::ApplyPitchDelta(&p, 90.0f);
    ExpectVecNear(p.up,    {0.0f, 0.0f, 1.0f},  1e-5f, "pitch(+90): up 应由 +Y 转到 +Z");
    ExpectVecNear(p.front, {0.0f, -1.0f, 0.0f}, 1e-5f, "pitch(+90): front 应由 +Z 转到 −Y");
    LOG(INFO) << "OK TestPitchPlus90";
}

// 2b. 绕世界 Y 轴增量 +90°： up 不动（在 Y 轴上），front: +Z→+X。
void TestYawPlus90() {
    ModelPlacement p;
    jpov_viewer::ApplyYawDelta(&p, 90.0f);
    ExpectVecNear(p.up,    {0.0f, 1.0f, 0.0f}, 1e-5f, "yaw(+90): up 应保持 +Y");
    ExpectVecNear(p.front, {1.0f, 0.0f, 0.0f}, 1e-5f, "yaw(+90): front 应由 +Z 转到 +X");
    LOG(INFO) << "OK TestYawPlus90";
}

// 3. ⭐ 任意顺序的世界轴累积（本设计的核心动机）：
//    先绕世界 Y 转 90°，再绕世界 X 转 90°。
//
//    矢量状态的做法（本实现）：每步都作用在【当前】矢量上 →
//      步1 yaw+90:  up=(0,1,0) front=(1,0,0)
//      步2 pitch+90: up 绕X 90 → (0,0,1)；front 绕X 90 → (1,0,0) 的 X 分量不变 → (1,0,0)
//    得 up=(0,0,1), front=(1,0,0)。
//
//    若用"两个 float 欧拉角 + 固定轴序 R=Ry(ry)·Rx(rx)"参数化（旧实现），
//    存的是 ry=90/rx=90，重算得 R·(0,1,0) = Ry(90)·Rx(90)·(0,1,0) = Ry(90)·(0,0,1)
//    = (1,0,0) —— 与矢量累积的 (0,0,1) **不同**。
//    本断言锁死"必须是矢量累积语义"，不是欧拉角参数化语义。
void TestWorldAxisAccumulationAnyOrder() {
    ModelPlacement p;
    jpov_viewer::ApplyYawDelta(&p, 90.0f);    // 先绕世界 Y
    jpov_viewer::ApplyPitchDelta(&p, 90.0f);  // 再绕世界 X
    ExpectVecNear(p.up, {0.0f, 0.0f, 1.0f}, 1e-5f,
                  "Y 转 90° 再 X 转 90°：up 应为 (0,0,1)（欧拉角参数化会得 (1,0,0)）");
    ExpectVecNear(p.front, {1.0f, 0.0f, 0.0f}, 1e-5f,
                  "Y 转 90° 再 X 转 90°：front 应为 (1,0,0)");

    // 反向顺序必须给出不同结果 → 证明顺序真的被累积（非交换）。
    ModelPlacement q;
    jpov_viewer::ApplyPitchDelta(&q, 90.0f);  // 先绕世界 X
    jpov_viewer::ApplyYawDelta(&q, 90.0f);    // 再绕世界 Y
    // up: 步1 pitch+90 → (0,0,1)；步2 yaw+90 作用 XZ 分量 → (1,0,0)
    ExpectVecNear(q.up, {1.0f, 0.0f, 0.0f}, 1e-5f,
                  "X 转 90° 再 Y 转 90°：up 应为 (1,0,0)（与反向顺序的 (0,0,1) 不同）");
    // front: 步1 pitch+90 → (0,−1,0)；步2 yaw 不动 Y 分量 → (0,−1,0)
    ExpectVecNear(q.front, {0.0f, -1.0f, 0.0f}, 1e-5f,
                  "X 转 90° 再 Y 转 90°：front 应为 (0,−1,0)）");
    LOG(INFO) << "OK TestWorldAxisAccumulationAnyOrder";
}

// 4. 正交归一不变量：任意增量序列后，up/front 仍是单位矢量且垂直。
//
//    ⚠️ 单次旋转下"单位长/垂直"是恒真的（cos²+sin²≡1），故这里**必须跑一段
//    多步、多轴、非平凡的累积序列**才有区分力：如果实现里对 up/front 用了
//    不同的旋转、或漏转了一个矢量，垂直性会立刻破（这才可能失败）。
void TestOrthonormalUnderAccumulation() {
    ModelPlacement p;
    // 非平凡序列：多轴交替、角度都不同、含负角与整圈。
    const struct { float deg; bool yaw; } kSeq[] = {
        {37.0f, true}, {-128.0f, false}, {90.0f, true},
        {360.0f, false}, {13.5f, true}, {-77.0f, false},
    };
    for (const auto& s : kSeq) {
        if (s.yaw) jpov_viewer::ApplyYawDelta(&p, s.deg);
        else       jpov_viewer::ApplyPitchDelta(&p, s.deg);
        const float lu = std::sqrt(p.up.x()*p.up.x() + p.up.y()*p.up.y() + p.up.z()*p.up.z());
        const float lf = std::sqrt(p.front.x()*p.front.x() + p.front.y()*p.front.y() +
                                   p.front.z()*p.front.z());
        ExpectNear(lu, 1.0f, 1e-5f, "累积中 up 必须保持单位长");
        ExpectNear(lf, 1.0f, 1e-5f, "累积中 front 必须保持单位长");
        const float dot = p.up.x()*p.front.x() + p.up.y()*p.front.y() + p.up.z()*p.front.z();
        ExpectNear(dot, 0.0f, 1e-5f, "累积中 up 与 front 必须保持垂直");
    }
    LOG(INFO) << "OK TestOrthonormalUnderAccumulation";
}

// 5a. 缩放/平移 clamp（超界被夹断，且平移直接进 center）；朝向不受影响。
void TestClampScaleAndTranslate() {
    ModelPlacement p;
    p.scale = 100.0f;
    p.tx = 99.0f;
    p.ty = -99.0f;
    p.tz = 0.5f;
    jpov_viewer::ApplyYawDelta(&p, 30.0f);   // 先转一下，验证 clamp 不动朝向
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

// 5b. ToDrawParams 原样透传朝向（不做任何重新参数化 → 无精度损失/顺序歧义）。
void TestToDrawParamsPassesOrientationThrough() {
    ModelPlacement p;
    jpov_viewer::ApplyYawDelta(&p, 41.0f);
    jpov_viewer::ApplyPitchDelta(&p, -23.0f);
    const jpov_viewer::DrawPlacement d = jpov_viewer::ToDrawParams(p);
    ExpectVecNear(d.up, p.up, 1e-7f, "up 应原样透传（不经欧拉角重算）");
    ExpectVecNear(d.front, p.front, 1e-7f, "front 应原样透传");
    LOG(INFO) << "OK TestToDrawParamsPassesOrientationThrough";
}

// 6a. 不带 Ctrl：横向右拖（dx>0）→ 绕世界 X，右滑逆时针 → pitch 增角。
void TestDragRotateXPitch() {
    // 拖 1/4 屏 → 90°；右滑为正（逆时针）→ up 由 +Y 转 +Z。
    ModelPlacement q;
    jpov_viewer::ApplyRotateDrag(&q, /*dx*/ 320.0f, false, 1280);
    ExpectVecNear(q.up, {0.0f, 0.0f, 1.0f}, 1e-4f,
                  "右拖 1/4 屏应绕世界 X 转 +90（右滑逆时针）");
    ExpectVecNear(q.front, {0.0f, -1.0f, 0.0f}, 1e-4f, "同时 front 应转到 −Y");

    // 负向验证：左拖必须反向（若符号写反，这里会失败）。
    ModelPlacement r;
    jpov_viewer::ApplyRotateDrag(&r, /*dx*/ -320.0f, false, 1280);
    ExpectVecNear(r.up, {0.0f, 0.0f, -1.0f}, 1e-4f, "左拖 1/4 屏应绕世界 X 转 −90");
    LOG(INFO) << "OK TestDragRotateXPitch";
}

// 6b. 按住 Ctrl：横向右拖 → 绕世界 Y，"右滑逆时针" ⇒ yaw 取负（见头文件推导）。
void TestDragRotateYYaw() {
    ModelPlacement p;
    jpov_viewer::ApplyRotateDrag(&p, /*dx*/ 320.0f, /*ctrl*/ true, 1280);
    ExpectVecNear(p.up, {0.0f, 1.0f, 0.0f}, 1e-4f, "绕 Y 旋转时 up 应保持 +Y");
    // yaw = −90° → front 由 +Z 转到 −X。
    ExpectVecNear(p.front, {-1.0f, 0.0f, 0.0f}, 1e-4f,
                  "Ctrl+右拖 1/4 屏应绕世界 Y 转 −90（+Y 看逆时针 = XZ 面内 +Z→+X）");

    ModelPlacement q;
    jpov_viewer::ApplyRotateDrag(&q, /*dx*/ -320.0f, true, 1280);
    ExpectVecNear(q.front, {1.0f, 0.0f, 0.0f}, 1e-4f, "Ctrl+左拖 1/4 屏应绕世界 Y 转 +90");
    LOG(INFO) << "OK TestDragRotateYYaw";
}

// 6c. 零位移不改变任何东西（纵向分量在本层根本不存在——调用方只传横向 dx）。
void TestDragZeroIsNoop() {
    ModelPlacement p;
    jpov_viewer::ApplyYawDelta(&p, 37.0f);
    const Vec3f up0 = p.up, fr0 = p.front;
    const bool changed = jpov_viewer::ApplyRotateDrag(&p, 0.0f, false, 1280);
    ExpectTrue(!changed, "dx=0 应报告未改变（避免无谓重绘/日志）");
    ExpectVecNear(p.up, up0, 1e-7f, "dx=0 不应改 up");
    ExpectVecNear(p.front, fr0, 1e-7f, "dx=0 不应改 front");
    LOG(INFO) << "OK TestDragZeroIsNoop";
}

// 6d. 连续拖动的等价性：分两次各 160px == 一次 320px（同类旋转可线性叠加）。
//     守的是"累积语义不被单次截断破坏"——若日后有人给每步加饱和/让步保护会 FAIL。
void TestDragLinearity() {
    ModelPlacement a;
    jpov_viewer::ApplyRotateDrag(&a, 160.0f, true, 1280);
    jpov_viewer::ApplyRotateDrag(&a, 160.0f, true, 1280);
    ModelPlacement b;
    jpov_viewer::ApplyRotateDrag(&b, 320.0f, true, 1280);
    ExpectVecNear(a.front, b.front, 1e-4f, "分两步拖应等价于一次拖（同类旋转线性叠加）");
    ExpectVecNear(a.up, b.up, 1e-4f, "同上，up 也应一致");
    LOG(INFO) << "OK TestDragLinearity";
}

// 6e. 绕世界轴累积：整圈 360° 必须回到原位（world-axis 旋转的周期自证）。
void TestFullTurnIdentity() {
    ModelPlacement p;
    jpov_viewer::ApplyRotateDrag(&p, /*dx*/ 1280.0f, /*ctrl*/ false, 1280);  // 绕X整圈
    ExpectVecNear(p.up, {0.0f, 1.0f, 0.0f}, 1e-4f, "绕世界 X 转整圈应回到 +Y");
    ExpectVecNear(p.front, {0.0f, 0.0f, 1.0f}, 1e-4f, "绕世界 X 转整圈应回到 +Z");
    jpov_viewer::ApplyRotateDrag(&p, 1280.0f, /*ctrl*/ true, 1280);           // 绕Y整圈
    ExpectVecNear(p.up, {0.0f, 1.0f, 0.0f}, 1e-4f, "绕世界 Y 转整圈应回到 +Y");
    ExpectVecNear(p.front, {0.0f, 0.0f, 1.0f}, 1e-4f, "绕世界 Y 转整圈应回到 +Z");
    LOG(INFO) << "OK TestFullTurnIdentity";
}

}  // namespace

int main(int /*argc*/, char** argv) {
    google::InitGoogleLogging(argv[0]);
    TestIdentity();
    TestPitchPlus90();
    TestYawPlus90();
    TestWorldAxisAccumulationAnyOrder();
    TestOrthonormalUnderAccumulation();
    TestClampScaleAndTranslate();
    TestToDrawParamsPassesOrientationThrough();
    TestDragRotateXPitch();
    TestDragRotateYYaw();
    TestDragZeroIsNoop();
    TestDragLinearity();
    TestFullTurnIdentity();
    LOG(INFO) << "model_placement_test: ALL PASS";
    return 0;
}
