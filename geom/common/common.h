#pragma once
 
#include "geom/common/check.h"

#include<string>

#include <iostream> 
#include <sstream>  
#include <iomanip>
#include <random>

#ifndef EXPECT_TRUE

#define EXPECT_TRUE(x) CHECK(x)<<"(In Unit Test)"
#define EXPECT_FALSE(x) CHECK(!(x))<<"(In Unit Test)"

#define EXPECT_GE(x,y) CHECK_GE(x,y)<<"(In Unit Test)"
#define EXPECT_LE(x,y) CHECK_LE(x,y)<<"(In Unit Test)"
#define EXPECT_GT(x,y) CHECK_GT(x,y)<<"(In Unit Test)"
#define EXPECT_LT(x,y) CHECK_LT(x,y)<<"(In Unit Test)"
#define EXPECT_EQ(x,y) CHECK_EQ(x,y)<<"(In Unit Test)"
#define EXPECT_NEAR(a,b,e) CHECK_LE(std::abs((a)-(b)),e)<<"(In Unit Test)"

#endif

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
