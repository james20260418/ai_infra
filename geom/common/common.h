#pragma once
 
#include "geom/common/check.h"

#include<string>

#include <iostream> 
#include <sstream>  
#include <iomanip>
#include <random>



namespace geom {
	constexpr double kEpsilon = 1e-9; 
	constexpr double kInf = std::numeric_limits<double>::infinity();

	template <typename T>
	constexpr bool ValueNear(T a, T b, T epsilon = kEpsilon) {
		return (a > b ? (a - b) : (b - a)) < epsilon;
	}
	 
	std::string ToString(double x);
	
	template <typename ...Types>
	std::string StrFormat(const std::string& fmt, Types ...args) {
		constexpr int kMaxStringSize = 1000;
		char buffer[kMaxStringSize];

		sprintf_s(buffer, fmt.c_str(), args...);
		return buffer;
	}
	
	double RandomDouble(double min_v, double max_v, std::mt19937* seed);

	int RandomInt(int min_v, int max_v, std::mt19937* seed);

	void EnsurePathForFilename(const std::string& filename);

	std::string ExtractFolderPathForFilename(const std::string& filename);

	std::string RemoveFileExtension(const std::string& file_path);

}  // namespace geom

