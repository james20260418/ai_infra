#include "geom/2d/polyline_2d.h"

namespace geom {

	Polyline2d::Polyline2d(std::vector<Vec2d> vertex_points) : vertex_points_(std::move(vertex_points)) {
		// Check input quality
		// vertex_points_.size() > 1 and all finite.
		// Adjacent points are disjoint.
		CheckInputVertexPoints(vertex_points_);

		// Compute segment lengths.
		vertex_s_.reserve(vertex_points_.size());
		vertex_s_.push_back(0.0);
		for (size_t i = 1; i < vertex_points_.size(); ++i) {
			vertex_s_.push_back(vertex_s_.back() + (vertex_points_[i] - vertex_points_[i - 1]).Norm());
		}

		// Compute segment angles.
		segment_angles_.reserve(vertex_points_.size() - 1);
		for (size_t i = 1; i < vertex_points_.size(); ++i) {
			// Since polyline's consecutive vertices are disjoint, call to .Angle() is safe.
			segment_angles_.push_back(NormalizeAngle((vertex_points_[i] - vertex_points_[i - 1]).Angle()));
		}
	}

	void Polyline2d::CheckInputVertexPoints(const std::vector<Vec2d>& vertex_points) {
		CHECK_GT(vertex_points.size(), 1U) << "Polyline2d requires vertex_points.size() > 1U";
		for (const Vec2d& point : vertex_points) {
			CHECK(std::isfinite(point.x())) << "Polyline2d input contains non-finite value.";
			CHECK(std::isfinite(point.y())) << "Polyline2d input contains non-finite value.";
		}
		for (size_t i = 1; i < vertex_points.size(); ++i) {
			const double segment_length_sqr = (vertex_points[i] - vertex_points[i - 1]).Sqr();
			CHECK_GE(segment_length_sqr, Sqr(kMinSegmentLength)) <<"Polyline2d requires vertex_points to be dis-joint";
		}
	}

	int Polyline2d::NumVertex() const {
		return static_cast<int>(vertex_points_.size());
	}

	int Polyline2d::NumSegment() const {
		return NumVertex() - 1;
	}

	double Polyline2d::Length() const {
		return vertex_s_.back();
	}

	const Vec2d& Polyline2d::GetVertexPoint(int index) const {
		CHECK_GE(index, 0);
		CHECK_LT(index, NumVertex());
		return vertex_points_[index];
	}

	double Polyline2d::GetVertexS(int index) const {
		CHECK_GE(index, 0);
		CHECK_LT(index, NumVertex());
		return vertex_s_[index];
	}

	double Polyline2d::GetSegmentLength(int index) const {
		CHECK_GE(index, 0);
		CHECK_LT(index, NumSegment());
		return vertex_s_[index + 1] - vertex_s_[index];
	}

	double Polyline2d::GetSegmentAngle(int index) const {
		CHECK_GE(index, 0);
		CHECK_LT(index, NumSegment());
		return segment_angles_[index];
	}

	int Polyline2d::GetSegmentIndex(double s) const {
		s = std::clamp(s, 0.0, Length());
		// The first element > s.
		auto iter = std::upper_bound(vertex_s_.begin(), vertex_s_.end(), s);
		const int i1 = static_cast<int>(std::distance(vertex_s_.begin(), iter));
		const int i0 = i1 - 1;
		return std::clamp(i0, 0, NumSegment() - 1);
	}

	Vec2d Polyline2d::PointAtS(double s) const {
		s = std::clamp(s, 0.0, Length());

		// Locate the segment.
		const int i0 = GetSegmentIndex(s);
		const int i1 = i0 + 1;

		// Use linear interpolation.
		double ratio = (s - GetVertexS(i0)) / GetSegmentLength(i0);
		ratio = std::clamp(ratio, 0.0, 1.0);
		return Lerp(vertex_points_[i0], vertex_points_[i1], ratio);
	}

}  // namespace geom
