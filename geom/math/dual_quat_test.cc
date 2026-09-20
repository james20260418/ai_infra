// dual_quat.h 单元测试
//
// 覆盖：
//   1. 与矩阵链路等价：由旋转+平移构造 / 由刚体矩阵构造 → 点变换、方向变换、矩阵往返
//   2. 复合：DualQuatMultiply ≡ Mat4Mul（「先 b 后 a」语义）
//   3. 单位元与「同姿态烘焙唯一」：identity 下变换**精确**等于原样（静态 gold 零回归的数学依据）
//   4. NLERP：端点精确、中点归一、最短路径符号翻转
//   5. **DLB 保体积 vs LBS 塌陷**（本改动要解决的问题的数学证据）
//   6. **抗对偶**：参考符号统一是必须的（含反例：不做符号统一会抵消成垃圾）
//   7. 非刚体矩阵 → LOG(FATAL)（不静默丢缩放）
//   8. **shader 同公式交叉验证**：GLSL 无法单测，故把 skinning_shader.h 里那条
//      「旋转 + 平移」展开式在 C++ 里重述一遍，断言与 DualQuatTransformPoint 逐位一致
//      —— 公式若被改错，这里会红。
//
// 说明：本测试只依赖 geom 层（GL-free），可独立跑。

#include "geom/math/dual_quat.h"

#include <cmath>

#include "gtest/gtest.h"

namespace geom {
namespace math {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// 逐分量比较对偶四元数（含符号：两处都应是同一个表示，不是“差一个负号也算过”）。
void ExpectDualQuatNear(const DualQuat& a, const DualQuat& b, float eps = 1e-5f) {
    EXPECT_NEAR(a.q.x, b.q.x, eps) << "q.x";
    EXPECT_NEAR(a.q.y, b.q.y, eps) << "q.y";
    EXPECT_NEAR(a.q.z, b.q.z, eps) << "q.z";
    EXPECT_NEAR(a.q.w, b.q.w, eps) << "q.w";
    EXPECT_NEAR(a.t.x, b.t.x, eps) << "t.x";
    EXPECT_NEAR(a.t.y, b.t.y, eps) << "t.y";
    EXPECT_NEAR(a.t.z, b.t.z, eps) << "t.z";
    EXPECT_NEAR(a.t.w, b.t.w, eps) << "t.w";
}

void ExpectVecNear(const Vec3<float>& a, const Vec3<float>& b, float eps = 1e-5f) {
    EXPECT_NEAR(a.x(), b.x(), eps);
    EXPECT_NEAR(a.y(), b.y(), eps);
    EXPECT_NEAR(a.z(), b.z(), eps);
}

// 绕任意轴转 deg 度的四元数（轴会归一化）。
Quaternion<float> RotDeg(const Vec3<float>& axis, float deg) {
    return Quaternion<float>::FromAxisAngle(axis.Unit(), deg * kPi / 180.0f);
}

// 一组「刚体矩阵」样本：旋转（含大角度）+ 平移，覆盖三条 Shepperd 分支。
struct RigidSample {
    Mat4 m;
    const char* name;
};

std::vector<RigidSample> RigidSamples() {
    std::vector<RigidSample> v;
    const Vec3<float> ax[4] = {Vec3<float>(1, 0, 0), Vec3<float>(0, 1, 0),
                               Vec3<float>(0, 0, 1), Vec3<float>(1, 2, -3)};
    const float degs[4] = {30.0f, 95.0f, 170.0f, 250.0f};
    const char* names[4] = {"rot30", "rot95", "rot170", "rot250"};
    for (int i = 0; i < 4; ++i) {
        const Mat4 r = Mat4Rotation(RotDeg(ax[i], degs[i]));
        const Mat4 t = Mat4Translation(Vec3<float>(1.5f, -2.25f, 0.75f));
        v.push_back(RigidSample{Mat4Mul(t, r), names[i]});
    }
    return v;
}

// ── 1. 与矩阵链路等价 ──

TEST(DualQuatTest, FromRotationAndTranslationMatchesMatrix) {
    const Quaternion<float> rot = RotDeg(Vec3<float>(1, 2, -3), 137.0f);
    const Vec3<float> trans(2.0f, -1.0f, 0.5f);
    const Mat4 m = Mat4Mul(Mat4Translation(trans), Mat4Rotation(rot));
    const DualQuat dq = DualQuatFromRotationTranslation(rot, trans);
    const Vec3<float> p(0.3f, -1.7f, 2.2f);
    ExpectVecNear(DualQuatTransformPoint(dq, p), Mat4TransformPoint(m, p));
    ExpectVecNear(DualQuatTranslationOf(dq), trans);
}

TEST(DualQuatTest, FromRigidMatrixRoundTripsToSameMatrix) {
    for (const RigidSample& s : RigidSamples()) {
        const DualQuat dq = DualQuatFromRigidMatrix(s.m);
        const Mat4 back = DualQuatToRigidMatrix(dq);
        for (int i = 0; i < 16; ++i) {
            EXPECT_NEAR(s.m.m[i], back.m[i], 1e-5f)
                << s.name << " 元素 " << i << " 往返不一致";
        }
    }
}

TEST(DualQuatTest, RotateVectorIgnoresTranslation) {
    const Quaternion<float> rot = RotDeg(Vec3<float>(0, 0, 1), 90.0f);
    const DualQuat dq = DualQuatFromRotationTranslation(rot, Vec3<float>(5, 6, 7));
    const Vec3<float> n(1.0f, 0.0f, 0.0f);
    // 绕 Z 转 90°：+X → +Y，与平移无关。
    ExpectVecNear(DualQuatRotateVector(dq, n), Vec3<float>(0, 1, 0));
}

// ── 2. 复合 ──

TEST(DualQuatTest, MultiplyMatchesMatrixComposition) {
    const std::vector<RigidSample> samples = RigidSamples();
    for (size_t i = 0; i < samples.size(); ++i) {
        for (size_t j = 0; j < samples.size(); ++j) {
            const DualQuat dq_a = DualQuatFromRigidMatrix(samples[i].m);
            const DualQuat dq_b = DualQuatFromRigidMatrix(samples[j].m);
            const DualQuat dq_ab = DualQuatMultiply(dq_a, dq_b);
            const Mat4 m_ab = Mat4Mul(samples[i].m, samples[j].m);  // 先 b 后 a
            const Mat4 from_dq = DualQuatToRigidMatrix(dq_ab);
            for (int k = 0; k < 16; ++k) {
                EXPECT_NEAR(m_ab.m[k], from_dq.m[k], 1e-5f)
                    << samples[i].name << "×" << samples[j].name << " 元素 " << k;
            }
        }
    }
}

// ── 3. 单位元精确性（静态 gold 零回归的数学依据）──

TEST(DualQuatTest, IdentityTransformIsExact) {
    const DualQuat id = DualQuatIdentity();
    const Vec3<float> p(0.125f, -3.0f, 7.5f);
    const Vec3<float> out = DualQuatTransformPoint(id, p);
    // **精确**相等（不是 NEAR）：q = (0,0,0,1)、t = 0 时公式各项恒为 0/1。
    EXPECT_FLOAT_EQ(out.x(), p.x());
    EXPECT_FLOAT_EQ(out.y(), p.y());
    EXPECT_FLOAT_EQ(out.z(), p.z());
}

TEST(DualQuatTest, BlendOfIdentityBonesIsExactIdentity) {
    // bind 姿态下每根骨的肤矩阵都是单位阵 ⇒ 对偶四元数全是单位元。混合（权重和为 1）
    // 后必须**精确**复原单位元 —— 这是「静态/rest gold 图逐字节不变」的前提。
    const DualQuat dqs[4] = {DualQuatIdentity(), DualQuatIdentity(), DualQuatIdentity(),
                             DualQuatIdentity()};
    const float w[4] = {0.5f, 0.25f, 0.125f, 0.125f};  // 和为 1
    const DualQuat b = DualQuatBlendWithReference(dqs, w, 4, /*reference_index=*/0);
    EXPECT_FLOAT_EQ(b.q.x, 0.0f);
    EXPECT_FLOAT_EQ(b.q.y, 0.0f);
    EXPECT_FLOAT_EQ(b.q.z, 0.0f);
    EXPECT_FLOAT_EQ(b.q.w, 1.0f);
    EXPECT_FLOAT_EQ(b.t.x, 0.0f);
    EXPECT_FLOAT_EQ(b.t.y, 0.0f);
    EXPECT_FLOAT_EQ(b.t.z, 0.0f);
    EXPECT_FLOAT_EQ(b.t.w, 0.0f);
}

// ── 4. NLERP ──

TEST(DualQuatTest, LerpEndpointsAreExactAndMidIsNormalized) {
    const DualQuat a = DualQuatFromRigidMatrix(RigidSamples()[0].m);
    const DualQuat b = DualQuatFromRigidMatrix(RigidSamples()[1].m);
    ExpectDualQuatNear(DualQuatLerp(a, b, /*ratio=*/0.0f), a);  // 端点短路，逐位等于 a
    ExpectDualQuatNear(DualQuatLerp(a, b, /*ratio=*/1.0f), b);
    const DualQuat mid = DualQuatLerp(a, b, /*ratio=*/0.25f);
    EXPECT_NEAR(mid.q.Norm(), 1.0f, 1e-5f) << "NLERP 中点必须归一（|q|=1）";
}

TEST(DualQuatTest, LerpTakesShortestPathWhenSignsDiffer) {
    const DualQuat a = DualQuatFromRigidMatrix(RigidSamples()[0].m);
    const DualQuat b = DualQuatFromRigidMatrix(RigidSamples()[2].m);
    const DualQuat neg_b = DualQuatNegated(b);  // 同一刚体变换的另一种表示
    ExpectDualQuatNear(DualQuatLerp(a, neg_b, 0.5f), DualQuatLerp(a, b, 0.5f), 1e-5f);
}

// ── 5. DLB 保体积 vs LBS 塌陷 ──

TEST(DualQuatTest, BlendIsRigidWhileMatrixBlendCollapses) {
    // 两根骨：同一位置（平移为 0），旋转相差 90°（典型的“肘部弯折”）。权重各半。
    const Quaternion<float> ra = RotDeg(Vec3<float>(0, 0, 1), 0.0f);
    const Quaternion<float> rb = RotDeg(Vec3<float>(0, 0, 1), 90.0f);
    const DualQuat dq_a = DualQuatFromRotationTranslation(ra, Vec3<float>(0, 0, 0));
    const DualQuat dq_b = DualQuatFromRotationTranslation(rb, Vec3<float>(0, 0, 0));
    const Mat4 m_a = DualQuatToRigidMatrix(dq_a);
    const Mat4 m_b = DualQuatToRigidMatrix(dq_b);

    const DualQuat dqs[2] = {dq_a, dq_b};
    const float w[2] = {0.5f, 0.5f};
    const DualQuat dq_blend = DualQuatBlendWithReference(dqs, w, 2, /*reference_index=*/0);
    // LBS：矩阵逐元素加权平均（当前实现的做法，作为对照保留）。
    Mat4 m_blend{};
    for (int k = 0; k < 16; ++k) {
        m_blend.m[k] = w[0] * m_a.m[k] + w[1] * m_b.m[k];
    }

    // 取两个点（**都在垂直于旋转轴的平面内**：沿轴方向的分量不受该旋转影响，
    // 取错方向会得到“距离不变”的假象），比较变换后两点间距 —— 刚体变换必须保距
    // ⇒ 保体积的直接体现。
    const Vec3<float> p1(1.0f, 0.0f, 0.0f);
    const Vec3<float> p2(0.0f, 1.0f, 0.0f);
    const float d0 = (p1 - p2).Norm();

    const Vec3<float> dq1 = DualQuatTransformPoint(dq_blend, p1);
    const Vec3<float> dq2 = DualQuatTransformPoint(dq_blend, p2);
    const float d_dq = (dq1 - dq2).Norm();
    EXPECT_NEAR(d_dq, d0, 1e-4f) << "DQS 混合结果必须保距（保体积）";

    const Vec3<float> lbs1 = Mat4TransformPoint(m_blend, p1);
    const Vec3<float> lbs2 = Mat4TransformPoint(m_blend, p2);
    const float d_lbs = (lbs1 - lbs2).Norm();
    // 90° 对分 ⇒ LBS 的那条“弦”把长度压到 cos(45°)≈0.707 倍量级。
    EXPECT_LT(d_lbs, 0.95f * d0) << "LBS 对照必须表现出塌陷（否则这条门禁本身失效）";
    LOG(INFO) << "保距对比: 原始 " << d0 << " / DQS " << d_dq << " / LBS " << d_lbs;
}

TEST(DualQuatTest, BlendAntipodalityNeedsUnifiedSigns) {
    // 两根骨：一根平移 z=1、一根平移 z=0（旋转都是 identity）。第二种用 −identity 表示
    // （q.w = −1，同一刚体变换）。正确做法（按参考统一符号）应得 z=0.5；
    // 若不统一符号，两个 q 相加直接抵消成 0（|q|→0）⇒ 平移算出天文数字/垃圾。
    const DualQuat dq_a = DualQuatFromRotationTranslation(Quaternion<float>::Identity(),
                                                          Vec3<float>(0, 0, 1));
    const DualQuat dq_neg = DualQuatNegated(
        DualQuatFromRotationTranslation(Quaternion<float>::Identity(), Vec3<float>(0, 0, 0)));
    const DualQuat dqs[2] = {dq_a, dq_neg};
    const float w[2] = {0.5f, 0.5f};

    const DualQuat good = DualQuatBlendWithReference(dqs, w, 2, /*reference_index=*/0);
    ExpectVecNear(DualQuatTranslationOf(good), Vec3<float>(0, 0, 0.5f));

    // 反例：不统一符号，直接加权求和（这正是不做抗对偶时会发生的事）。
    DualQuat naive;
    naive.q = Quaternion<float>(0.5f * dqs[0].q.x + 0.5f * dqs[1].q.x,
                                0.5f * dqs[0].q.y + 0.5f * dqs[1].q.y,
                                0.5f * dqs[0].q.z + 0.5f * dqs[1].q.z,
                                0.5f * dqs[0].q.w + 0.5f * dqs[1].q.w);
    naive.t = Quaternion<float>(0.5f * dqs[0].t.x + 0.5f * dqs[1].t.x,
                                0.5f * dqs[0].t.y + 0.5f * dqs[1].t.y,
                                0.5f * dqs[0].t.z + 0.5f * dqs[1].t.z,
                                0.5f * dqs[0].t.w + 0.5f * dqs[1].t.w);
    EXPECT_LT(naive.q.Norm(), 0.1f)
        << "反例应退化（|q|≈0）—— 说明不统一符号确实会抵消；否则这条门禁没测到东西";
}

TEST(DualQuatTest, BlendFallsBackToIdentityWhenNoWeight) {
    // 顶点没有任何有效权重 ⇒ 保持 rest（与 CPU 真值实现的 wsum<=0 分支一致），
    // 而不是塌到原点、也不是 NaN。
    const DualQuat dqs[4] = {DualQuatFromRigidMatrix(RigidSamples()[0].m),
                             DualQuatFromRigidMatrix(RigidSamples()[1].m),
                             DualQuatFromRigidMatrix(RigidSamples()[2].m),
                             DualQuatFromRigidMatrix(RigidSamples()[3].m)};
    const float w[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const DualQuat b = DualQuatBlendWithReference(dqs, w, 4, /*reference_index=*/0);
    const Vec3<float> p(1.0f, 2.0f, 3.0f);
    ExpectVecNear(DualQuatTransformPoint(b, p), p);
}

// ── 7. 非刚体输入 → 崩溃 ──

TEST(DualQuatTest, FromRigidMatrixRejectsScale) {
    Mat4 scaled = Mat4Rotation(RotDeg(Vec3<float>(0, 1, 0), 30.0f));
    scaled.m[0] *= 2.0f;   // 人为给 x 轴加 2 倍缩放
    scaled.m[1] *= 2.0f;
    scaled.m[2] *= 2.0f;
    EXPECT_DEATH(DualQuatFromRigidMatrix(scaled), "");
}

TEST(DualQuatTest, FromRigidMatrixRejectsShear) {
    Mat4 sheared = Mat4Identity();
    sheared.m[4] = 0.5f;   // R[0][1] 非零 ⇒ 剪切
    EXPECT_DEATH(DualQuatFromRigidMatrix(sheared), "");
}

// ── 8. shader 同公式交叉验证 ──
//
// GLSL 没法单测（没有 #include，见 SOUL.md「一片 shader 字符串不能单测，就不要分开管理」）。
// 折中：把 skinning_shader.h 里用到的那条展开式**在 C++ 里重述**，断言它与本模块
// （已经单测过的实部/对偶部正规算法）逐位一致。将来谁把 GLSL 公式抄错，这里的对照就是
// 唯一能提前发现的依据 —— 改 skinning_shader.h 的公式时，请同步改本函数。
Vec3<float> TransformPointViaShaderFormula(const DualQuat& dq, const Vec3<float>& p) {
    // 与 skinning_shader.h 的 DualQuatTransformPoint（GLSL）逐字对应：
    //   vec3 Rot = p + 2.0*q.w*cross(q.xyz, p) + 2.0*cross(q.xyz, cross(q.xyz, p));
    //   vec3 Tra = 2.0*(q.w*t.xyz - t.w*q.xyz + cross(q.xyz, t.xyz));
    //   return Rot + Tra;
    const Vec3<float> q_xyz(dq.q.x, dq.q.y, dq.q.z);
    const Vec3<float> t_xyz(dq.t.x, dq.t.y, dq.t.z);
    const Vec3<float> rot = p + q_xyz.Cross(p) * (2.0f * dq.q.w) +
                            q_xyz.Cross(q_xyz.Cross(p)) * 2.0f;
    const Vec3<float> tra =
        (t_xyz * dq.q.w - q_xyz * dq.t.w + q_xyz.Cross(t_xyz)) * 2.0f;
    return rot + tra;
}

TEST(DualQuatTest, ShaderFormulaMatchesRotationPlusTranslation) {
    for (const RigidSample& s : RigidSamples()) {
        const DualQuat dq = DualQuatFromRigidMatrix(s.m);
        // 用几个彼此独立的点扫一遍（含原点、负坐标、大坐标）。
        const Vec3<float> pts[4] = {Vec3<float>(0, 0, 0), Vec3<float>(1.5f, -2.0f, 0.25f),
                                    Vec3<float>(-3.0f, 4.0f, -5.0f), Vec3<float>(0, 1, 0)};
        for (int i = 0; i < 4; ++i) {
            ExpectVecNear(TransformPointViaShaderFormula(dq, pts[i]),
                          DualQuatTransformPoint(dq, pts[i]), 1e-5f);
        }
    }
}

}  // namespace
}  // namespace math
}  // namespace geom
