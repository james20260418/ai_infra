#include "geom/common/common.h"

#include<string>
#include<filesystem>
#include <iostream> 
#include <sstream>  
#include <iomanip>
#include <cfloat>

namespace geom { 

	std::string ToString(double x) {
		std::stringstream buffer;
		buffer << std::scientific << std::setprecision(DBL_DIG);
		buffer << x;
		return buffer.str();
	}

	double RandomDouble(double min_v, double max_v, std::mt19937* seed) {
		return std::uniform_real_distribution<>(min_v, max_v)(*seed);
	}

	// Result value is in [min_v, max_v]. 
	int RandomInt(int min_v, int max_v, std::mt19937* seed) {
		return std::uniform_int_distribution<>(min_v, max_v)(*seed);
	}

	void EnsurePathForFilename(const std::string& filename) {
		size_t slash_pos = filename.find_last_of("/");
		if (slash_pos != std::string::npos) {
			std::string folder_path = filename.substr(0, slash_pos);
			if (!std::filesystem::exists(folder_path)) {
				std::filesystem::create_directories(folder_path);
			}
		}
	}

	std::string ExtractFolderPathForFilename(const std::string& filename) {
		size_t slash_pos = filename.find_last_of("/");
		if (slash_pos != std::string::npos) {
			return filename.substr(0, slash_pos);
		}
		else {
			return "";
		}
	}

	std::string RemoveFileExtension(const std::string& file_path) {
		size_t dot_pos = file_path.find_last_of('.');

		if (dot_pos != std::string::npos && dot_pos > 0) {
			return file_path.substr(0, dot_pos);
		}

		return file_path;
	}

}  // namespace geom
