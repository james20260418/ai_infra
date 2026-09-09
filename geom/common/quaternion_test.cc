// geom/common/quaternion_test.cc — Quaternion 工具最小测试
#include "geom/common/quaternion.h"

#include <cmath>

#include <glog/logging.h>
#include <gtest/gtest.h>

namespace {

using geom::Quaternion;
using geom::Vec3;
using geom::Slerp;

TEST(QuaternionTest, IdentityIsIdentity) {
    Quaternion<float> q = Quaternion<float>::Identity();
    EXPECT_NEAR(q.Norm(), 1.0f, 1e-6f);  // identity is unit
    // identity rotate should leave vector unchanged
    Vec3<float> v(1.0f, 2.0f, 3.0f);
    Vec3<float> r = geom::RotateVector(q, v);
    EXPECT_NEAR(r.x(), v.x(), 1e-5f);
    EXPECT_NEAR(r.y(), v.y(), 1e-5f);
    EXPECT_NEAR(r.z(), v.z(), 1e-5f);
}

TEST(QuaternionTest, Rotate90AboutZMapsXtoY) {
    // q = axis (0,0,1), angle 90 deg => rotate +X to +Y.
    Quaternion<float> q = Quaternion<float>::FromAxisAngle(
        Vec3<float>(0.0f, 0.0f, 1.0f), static_cast<float>(M_PI_2));
    EXPECT_NEAR(q.Norm(), 1.0f, 1e-5f);
    Vec3<float> v(1.0f, 0.0f, 0.0f);
    Vec3<float> r = geom::RotateVector(q, v);
    EXPECT_NEAR(r.x(), 0.0f, 1e-4f);
    EXPECT_NEAR(r.y(), 1.0f, 1e-4f);
    EXPECT_NEAR(r.z(), 0.0f, 1e-4f);
}

TEST(QuaternionTest, SlerpEndpoints) {
    Quaternion<float> a = Quaternion<float>::Identity();
    Quaternion<float> b = Quaternion<float>::FromAxisAngle(
        Vec3<float>(0.0f, 1.0f, 0.0f), 1.0f);
    // t=0 -> a, t=1 -> b
    Quaternion<float> q0 = Slerp(a, b, 0.0f);
    EXPECT_NEAR(q0.x, a.x, 1e-5f);
    EXPECT_NEAR(q0.y, a.y, 1e-5f);
    EXPECT_NEAR(q0.w, a.w, 1e-5f);
    Quaternion<float> q1 = Slerp(a, b, 1.0f);
    EXPECT_NEAR(q1.x, b.x, 1e-5f);
    EXPECT_NEAR(q1.y, b.y, 1e-5f);
    EXPECT_NEAR(q1.w, b.w, 1e-5f);
}

// 组合乘法结果仍应为单位（两单位四元数相乘）。
TEST(QuaternionTest, ProductOfUnitIsUnit) {
    Quaternion<float> q1 = Quaternion<float>::FromAxisAngle(
        Vec3<float>(0.0f, 0.0f, 1.0f), 0.7f);
    Quaternion<float> q2 = Quaternion<float>::FromAxisAngle(
        Vec3<float>(1.0f, 0.0f, 0.0f), -0.4f);
    Quaternion<float> p = q1 * q2;
    EXPECT_NEAR(p.Norm(), 1.0f, 1e-4f);
}

}  // namespace
