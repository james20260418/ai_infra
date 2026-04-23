#include "geom/common/vec_test.h"
#include "geom/common/check.h"


namespace geom::test {

	void TestVec() {
		LOG(INFO) << "running TestVec";
		// Construction.
		{
			Vec2d a(1, 2);
			Vec3d b({ 3,4,5 });
			Vec1d c;

			CHECK_EQ(a.x(), 1);
			CHECK_EQ(a.y(), 2);
			CHECK_EQ(b[0], 3);
			CHECK_EQ(b[1], 4);
			CHECK_EQ(b[2], 5);
			CHECK_EQ(c[0], 0);

			// Look, I don't know why I am doing this.
			// TODO(huaiyuan): Investigate the benefit later.
			static_assert(std::is_trivially_copyable<Vec2d>::value, "");
		}

		// N-d algebra.
		{
			const Vec2d a(1, 2);
			const Vec2d b(3, 4);

			// operator +, +=
			const Vec2d sum = a + b;
			const Vec2d sum_gt(4, 6);
			CHECK_EQ(sum, sum_gt);
			Vec2d a_t = a;
			a_t += b;
			CHECK_EQ(a_t, sum_gt);

			// operator -, -=
			const Vec2d subs = a - b;
			const Vec2d subs_gt(-2, -2);
			CHECK_EQ(subs, subs_gt);
			a_t = a;
			a_t -= b;
			CHECK_EQ(a_t, subs_gt);

			// Scalar multiplication
			const Vec2d gain_1 = a * 4.0;
			const Vec2d gain_1_gt(4.0, 8.0);
			CHECK_EQ(gain_1, gain_1_gt);

			// Scalar multiplication
			const Vec2d gain_2 = 5.0 * b;
			const Vec2d gain_2_gt(15, 20);
			CHECK_EQ(gain_2, gain_2_gt);

			// Norm 
			CHECK_EQ(a.Norm(), std::sqrt(5.0));
			CHECK_EQ(b.Norm(), 5.0);

			Vec2d mutable_a = a;
			mutable_a.Normalize();
			CHECK_LT(std::abs(mutable_a.Norm() - 1.0), 1e-9);
			CHECK_LT((b.Unit().Norm() - 1.0), 1e-9);
		}

		// 2d utils.
		{
			const Vec2d a(1, 2);
			const Vec2d b(3, 4);
			// Dot
			const double dot = a.Dot(b);
			const double dot_gt = 11;
			CHECK_EQ(dot_gt, dot);

			// Cross
			const double cross = a.Cross(b);
			const double cross_gt = -2;
			CHECK_EQ(cross, cross_gt);

			// Angles.
			auto is_near = [](const Vec2d& a, const Vec2d& b) {
				constexpr double kEpsilonSqr = 1e-18;
				return (a - b).Sqr() < kEpsilonSqr;
			};

			const Vec2d u1 = Vec2d::UnitFromAngle(M_PI_4);
			CHECK(is_near(u1, Vec2d(M_SQRT1_2, M_SQRT1_2)));

			CHECK(is_near(u1.Rotated90(), Vec2d(-M_SQRT1_2, M_SQRT1_2)));
			CHECK(is_near(u1.Rotated180(), Vec2d(-M_SQRT1_2, -M_SQRT1_2)));
			CHECK(is_near(u1.Rotated270(), Vec2d(M_SQRT1_2, -M_SQRT1_2)));

			CHECK(is_near(u1.Rotated(M_PI_4), Vec2d(0.0, 1.0)));
			CHECK(is_near(u1.Rotated(-5 * M_PI_4), Vec2d(-1.0, 0.0)));
			CHECK(is_near(u1.Rotated(9 * M_PI_4), Vec2d(0.0, 1.0)));

			Vec2d u1_t = u1;
			u1_t.Rotate90();
			CHECK(is_near(u1_t, Vec2d(-M_SQRT1_2, M_SQRT1_2)));
			u1_t.Rotate270();
			CHECK(is_near(u1_t, u1));
			u1_t.Rotate180();
			CHECK(is_near(u1_t, -Vec2d::UnitFromAngle(M_PI_4)));

			u1_t.Rotate(M_PI_4);
			CHECK(is_near(u1_t, Vec2d(0.0, -1.0)));


			// Square angles.
			{
				// Simple case.

				EXPECT_NEAR(Vec2d(3.0, 4.0).SquareAngle(), 1.25, kEpsilon);

				EXPECT_NEAR(Vec2d(-2.0, -2.0).SquareAngle(), -3.0, kEpsilon);

				EXPECT_TRUE(Vec2d::SquareUnitFromSquareAngle(2.0).IsNear({ 0.0,1.0 }));

				// Test montonicity.
				constexpr int kNumStep = 100;
				constexpr double kAngleStep = M_PI * 2.0 / kNumStep;

				double prev_s_a = -4.0;
				for (int i = 1; i < kNumStep; ++i) {

					const double angle = i * kAngleStep - M_PI;
					const Vec2d unit = Vec2d::UnitFromAngle(angle);
					const double s_angle = unit.SquareAngle();

					EXPECT_GT(s_angle, prev_s_a);
					prev_s_a = s_angle;
				}

				// Test SquareAngle & SquareUnitFromSquareAngle inter-transition.
				std::mt19937 seed(0);

				constexpr int kNumTest = 100;
				for (int i = 0; i < kNumTest; ++i) {
					double s_a_0 = RandomDouble(-100.0, 100.0, &seed);;

					const double s_a_1 = Vec2d::SquareUnitFromSquareAngle(s_a_0).SquareAngle();
					const double s_a_diff = geom::NormalizeSquareAngle(s_a_0 - s_a_1);

					EXPECT_NEAR(s_a_diff, 0.0, kEpsilon);
				}
			}

			// Protobuf
			{
				Vec2dProto proto = Vec2d(1, 2).ToProto();
				Vec2d load_from_proto_vec = Vec2d::FromProto(proto);
				EXPECT_EQ(load_from_proto_vec.x(), 1);
				EXPECT_EQ(load_from_proto_vec.y(), 2);
			}


		}

		// 3d utils.
		{
			// Cross prod.
			{
				const Vec3d nx(1, 0, 0);
				const Vec3d ny(0, 1, 0);
				const Vec3d nz = nx.Cross(ny);
				EXPECT_NEAR((nz - Vec3d{ 0,0,1 }).Norm(), 0.0, kEpsilon);
			}
			{
				const Vec3d nx(1, 1, 1);
				const Vec3d ny(2, 2, 2);
				const Vec3d nz = nx.Cross(ny);
				EXPECT_NEAR((nz - Vec3d{ 0,0,0 }).Norm(), 0.0, kEpsilon);
			}

			// Inner Prod.
			{
				const Vec3d nx(1, 2, 3);
				const Vec3d ny(1, 1, -1);
				const double prod = nx.Dot(ny);
				EXPECT_NEAR(prod, 0.0, kEpsilon);
			}

			// Rotation.
			{
				const Vec3d n1(1, 2, 3);
				const Vec3d n2(0, 0, 1);

				const Vec3d n3 = n1.RotatedBy(n2, M_PI_2);
				EXPECT_NEAR((n3 - Vec3d{ -2,1,3 }).Norm(), 0.0, kEpsilon);
			}
		}
	}



}  // namespace geom::test

