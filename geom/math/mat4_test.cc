// Mat4 单元测试
//
// 覆盖：
//   1. 构造：单位阵 / 平移阵 / 旋转阵（与四元数旋转向量交叉验证）
//   2. 复合：Mat4Mul 结合律 + 「先 b 后 a」的语义（用作用在点上验证）
//   3. 点/方向变换：平移只影响点不影响方向
//   4. 求逆：M × M⁻¹ == I、平移被正确反掉、旋转被正确反掉
//   5. 求逆的非仿射 / 奇异输入 → CHECK 崩溃（不静默给错值）
//   6. JointLocalRest / JointLocal：pose 恒等时退化一致

#include "geom/math/mat4.h"

#include <cmath>

#include "gtest/gtest.h"

namespace geom {
namespace math {

namespace {

// 逐元素近似比较（矩阵元素是 float，累乘后有精度损失）。
void ExpectMat4Near(const Mat4& a, const Mat4& b, float eps = 1e-5f) {
    for (int i = 0; i < 16; ++i) {
        EXPECT_NEAR(a.m[i], b.m[i], eps) << "元素 " << i << " 不一致";
    }
}

Vec3<float> ExpectVec3Near(const Vec3<float>& a, const Vec3<float>& b,
                           float eps = 1e-5f) {
    EXPECT_NEAR(a.x(), b.x(), eps);
    EXPECT_NEAR(a.y(), b.y(), eps);
    EXPECT_NEAR(a.z(), b.z(), eps);
    return a;
}

constexpr float kPi = 3.14159265358979323846f;

}  // namespace

// ── 构造 ──

TEST(Mat4Test, IdentityIsIdentity) {
    const Mat4 i = Mat4Identity();
    const Vec3<float> p(1.0f, 2.0f, 3.0f);
    ExpectVec3Near(Mat4TransformPoint(i, p), p);
}

TEST(Mat4Test, TranslationMovesPointsButNotVectors) {
    const Mat4 t = Mat4Translation(1.0f, 2.0f, 3.0f);
    // 点被平移
    ExpectVec3Near(Mat4TransformPoint(t, Vec3<float>(0.0f, 0.0f, 0.0f)),
                   Vec3<float>(1.0f, 2.0f, 3.0f));
    // 方向不被平移（w=0）
    ExpectVec3Near(Mat4TransformVector(t, Vec3<float>(0.0f, 0.0f, 0.0f)),
                   Vec3<float>(0.0f, 0.0f, 0.0f));
    ExpectVec3Near(Mat4TransformVector(t, Vec3<float>(1.0f, 0.0f, 0.0f)),
                   Vec3<float>(1.0f, 0.0f, 0.0f));
    // 平移分量提取
    ExpectVec3Near(Mat4TranslationOf(t), Vec3<float>(1.0f, 2.0f, 3.0f));
}

TEST(Mat4Test, RotationMatchesQuaternionRotateVector) {
    // 绕 Z 轴 +90°：+X 应转到 +Y。
    const Quaternion<float> q =
        Quaternion<float>::FromAxisAngle(Vec3<float>(0.0f, 0.0f, 1.0f), kPi / 2.0f);
    const Mat4 m = Mat4Rotation(q);
    const Vec3<float> out = Mat4TransformVector(m, Vec3<float>(1.0f, 0.0f, 0.0f));
    ExpectVec3Near(out, Vec3<float>(0.0f, 1.0f, 0.0f));

    // 与 geom 自带 RotateVector 交叉验证（两条独立实现应一致）。
    const Vec3<float> via_quat = geom::RotateVector(q, Vec3<float>(1.0f, 0.0f, 0.0f));
    ExpectVec3Near(out, via_quat);
}

TEST(Mat4Test, RotationAxisAngleMatchesQuaternionForm) {
    const Vec3<float> axis(1.0f, 2.0f, 3.0f);
    const float angle = 0.7f;
    const Mat4 a = Mat4RotationAxisAngle(axis, angle);
    const Mat4 b = Mat4Rotation(Quaternion<float>::FromAxisAngle(axis.Unit(), angle));
    ExpectMat4Near(a, b);
}

TEST(Mat4Test, RotationAxisAngleWithZeroAxisIsIdentity) {
    // 零轴：不崩、给单位阵（防御，不引入 NaN）。
    const Mat4 m = Mat4RotationAxisAngle(Vec3<float>(0.0f, 0.0f, 0.0f), 1.0f);
    ExpectMat4Near(m, Mat4Identity());
}

// ── 复合语义 ──

TEST(Mat4Test, MulAppliesRightThenLeft) {
    // a = 平移 (10,0,0)；b = 绕 Z +90°。
    // 期望：先转再平移 → (1,0,0) --b--> (0,1,0) --a--> (10,1,0)
    const Mat4 a = Mat4Translation(10.0f, 0.0f, 0.0f);
    const Mat4 b = Mat4Rotation(
        Quaternion<float>::FromAxisAngle(Vec3<float>(0.0f, 0.0f, 1.0f), kPi / 2.0f));
    const Mat4 c = Mat4Mul(a, b);
    ExpectVec3Near(Mat4TransformPoint(c, Vec3<float>(1.0f, 0.0f, 0.0f)),
                   Vec3<float>(10.0f, 1.0f, 0.0f));

    // 顺序反过来（先平移再旋转）结果不同 —— 证明不是可交换的糊弄实现。
    const Mat4 d = Mat4Mul(b, a);
    ExpectVec3Near(Mat4TransformPoint(d, Vec3<float>(1.0f, 0.0f, 0.0f)),
                   Vec3<float>(0.0f, 11.0f, 0.0f));
}

TEST(Mat4Test, MulIsAssociative) {
    const Mat4 a = Mat4Translation(1.0f, -2.0f, 0.5f);
    const Mat4 b = Mat4Rotation(
        Quaternion<float>::FromAxisAngle(Vec3<float>(0.0f, 1.0f, 0.0f), 0.9f));
    const Mat4 c = Mat4Translation(-3.0f, 1.0f, 2.0f);
    ExpectMat4Near(Mat4Mul(Mat4Mul(a, b), c), Mat4Mul(a, Mat4Mul(b, c)));
}

// ── 求逆 ──

TEST(Mat4Test, InverseOfTranslation) {
    const Mat4 t = Mat4Translation(1.5f, -2.5f, 3.5f);
    ExpectMat4Near(Mat4Mul(t, Mat4InverseAffine(t)), Mat4Identity());
    ExpectMat4Near(Mat4Mul(Mat4InverseAffine(t), t), Mat4Identity());
}

TEST(Mat4Test, InverseOfRotation) {
    const Mat4 r = Mat4Rotation(
        Quaternion<float>::FromAxisAngle(Vec3<float>(1.0f, 1.0f, 0.0f).Unit(), 1.1f));
    // 纯旋转的逆 = 转置（正交阵）。
    ExpectMat4Near(Mat4InverseAffine(r), Mat4Rotation(
        Quaternion<float>::FromAxisAngle(Vec3<float>(1.0f, 1.0f, 0.0f).Unit(), -1.1f)));
    ExpectMat4Near(Mat4Mul(r, Mat4InverseAffine(r)), Mat4Identity());
}

TEST(Mat4Test, InverseOfCombinedTRS) {
    // M = T · R：逆应把点送回原处。
    const Mat4 t = Mat4Translation(3.0f, -1.0f, 0.25f);
    const Mat4 r = Mat4Rotation(
        Quaternion<float>::FromAxisAngle(Vec3<float>(0.3f, 0.9f, 0.1f).Unit(), 0.8f));
    const Mat4 m = Mat4Mul(t, r);
    const Mat4 inv = Mat4InverseAffine(m);
    ExpectMat4Near(Mat4Mul(m, inv), Mat4Identity());

    const Vec3<float> p(0.7f, -1.3f, 2.1f);
    ExpectVec3Near(Mat4TransformPoint(inv, Mat4TransformPoint(m, p)), p);
}

TEST(Mat4Test, InverseWithScale) {
    // 含缩放的仿射：scale(2,3,4) 后平移。
    Mat4 s = Mat4Identity();
    s.m[0] = 2.0f;
    s.m[5] = 3.0f;
    s.m[10] = 4.0f;
    const Mat4 m = Mat4Mul(Mat4Translation(1.0f, 1.0f, 1.0f), s);
    ExpectMat4Near(Mat4Mul(m, Mat4InverseAffine(m)), Mat4Identity());
}

TEST(Mat4Test, InverseRejectsNonAffine) {
    Mat4 m = Mat4Identity();
    m.m[3] = 0.5f;  // 末行不再 (0,0,0,1) → 透视，非仿射
    EXPECT_DEATH(Mat4InverseAffine(m), "");
}

TEST(Mat4Test, InverseRejectsSingular) {
    Mat4 m = Mat4Identity();
    m.m[0] = 0.0f;  // 3x3 奇异
    EXPECT_DEATH(Mat4InverseAffine(m), "");
}

// ── 骨架组合式 ──

TEST(Mat4Test, JointLocalWithIdentityPoseEqualsRest) {
    const Vec3<float> offset(0.0f, 0.1f, 0.0f);
    const Quaternion<float> bind =
        Quaternion<float>::FromAxisAngle(Vec3<float>(0.0f, 0.0f, 1.0f), 0.4f);
    // pose = 恒等 → JointLocal 退化为 JointLocalRest（即 T-pose 下的关节局部变换）。
    ExpectMat4Near(JointLocal(offset, bind, Quaternion<float>::Identity()),
                   JointLocalRest(offset, bind));
}

TEST(Mat4Test, JointLocalRestIsTranslateThenRotate) {
    // 约定：先平移骨长、再旋转。旋转不改变自身位置。
    // 验证：local 的平移分量 == rest_offset（旋转不影响平移列）。
    const Vec3<float> offset(1.0f, 2.0f, 3.0f);
    const Quaternion<float> bind =
        Quaternion<float>::FromAxisAngle(Vec3<float>(0.0f, 1.0f, 0.0f), 0.9f);
    const Mat4 local = JointLocalRest(offset, bind);
    ExpectVec3Near(Mat4TranslationOf(local), offset);
}

}  // namespace math
}  // namespace geom
