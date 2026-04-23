#pragma once

#include <vector>

#include "geom/2d/segment_2.h"
#include "geom/common/vec.h" 
#include "geom/common/math_util.h"

namespace geom {

	// This class is used to storage a 2d polyline
	// s is defined as the distance a point moves along the polyline.
	// s = 0 corresponds to the start vertice.
	// It provides some basic utils to obtain a polyline point from s.
	// Index structure of the polyline:
	// 
	//   vertex[0]---------->vertex[1]---------->vertex[2]
	//            <--seg[0]->         <--seg[1]->
	//
	class Polyline2d {
	public:
		static constexpr double kMinSegmentLength = 1e-9;
		// Pre-condition:
		// 1, points must contain finite value.
		// 2, points.size() must > 1.
		// 3, each segment must be at least kMinSegmentLength long.
		explicit Polyline2d(std::vector<Vec2d> vertex_points);

		// The number of vertex points.
		int NumVertex() const;

		// The number of segments, it is necessarily NumVertex() - 1.
		int NumSegment() const;

		// The length of the polyline, it equals the s of the last vertex.
		double Length() const;

		// Pre-condition:
		// 1, 0 <= index < NumVertex();
		// The position of a specific vertex point given its index.
		const Vec2d& GetVertexPoint(int index) const;

		// Pre-condition:
		// 1, 0 <= index < NumVertex();
		// The s of a specific vertex point given its index.
		double GetVertexS(int index) const;

		// Pre-condition:
		// 1, 0 <= index < NumSegment();
		// Get the length of the i'th segment.
		// (Positive-definite.)
		double GetSegmentLength(int index) const;

		// Pre-condition:
		// 1, 0 <= index < NumSegment();
		// Get the angle(points from i to i+1) of the i'th segment. 
		// The result is normalized.
		double GetSegmentAngle(int index) const;

		// Given s, find the index of the segment that this s is onside.
		// The result is in [0, NumSegment() - 1].
		// The function uses binary search and is O(log(NumVertex()))
		// WARNING: s out of [0, Length()] will be clamped in the first place.
		int GetSegmentIndex(double s) const;

		// Find the point at s.
		// WARNING: s out of [0, Length()] will be clamped in the first place.
		Vec2d PointAtS(double s) const;


		const std::vector<Vec2d>& vertex_points() const {
			return vertex_points_;
		};

	private:
		static void CheckInputVertexPoints(const std::vector<Vec2d>& points);

		std::vector<Vec2d> vertex_points_;
		std::vector<double> vertex_s_;
		std::vector<double> segment_angles_;
	};

}  // namespace geom
