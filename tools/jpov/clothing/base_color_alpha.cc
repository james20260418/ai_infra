// JPOV 穿衣工具 — base color 贴图 alpha 归一（图片编/解码实现）
//
// 见 base_color_alpha.h 的说明。本文件只实现 DecodeImageRgba / EncodeRgbaToPng：
//   - 解码用 stb_image（实现由 //third_party/stb:stb_image 的 src 提供，本文件只 include 声明）；
//   - 编码复用 //tools/jpov/src:orm_unpack 的 RgbaToPng（其 target 定义了
//     STB_IMAGE_WRITE_IMPLEMENTATION；实现宏不随依赖传播，故**不**在本文件重复定义，
//     以免与 orm_unpack 同链时符号冲突）。

#include "tools/jpov/clothing/base_color_alpha.h"

#include <cstddef>

#include <glog/logging.h>

#include "stb_image.h"                     // 解码（实现见 //third_party/stb:stb_image）
#include "tools/jpov/src/orm_unpack.h"     // jpov::RgbaToPng（PNG 编码）

namespace jpov {
namespace clothing {

bool DecodeImageRgba(const std::string& uri, const std::vector<unsigned char>& bytes,
                     std::vector<unsigned char>* out_rgba, int* out_w, int* out_h) {
    CHECK(out_rgba != nullptr);
    CHECK(out_w != nullptr);
    CHECK(out_h != nullptr);
    CHECK(!uri.empty() || !bytes.empty()) << "DecodeImageRgba: uri 与 bytes 均为空";

    int w = 0;
    int h = 0;
    int channels = 0;
    unsigned char* data = nullptr;
    if (!uri.empty()) {
        data = stbi_load(uri.c_str(), &w, &h, &channels, /*desired_channels=*/4);
    } else {
        data = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h,
                                     &channels, /*desired_channels=*/4);
    }
    if (data == nullptr) {
        LOG(ERROR) << "DecodeImageRgba 失败（" << (uri.empty() ? "内存字节" : uri)
                   << "）：" << stbi_failure_reason();
        return false;
    }
    CHECK_GT(w, 0);
    CHECK_GT(h, 0);
    out_rgba->assign(data, data + static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    stbi_image_free(data);
    *out_w = w;
    *out_h = h;
    return true;
}

bool EncodeRgbaToPng(const std::vector<unsigned char>& rgba, int width, int height,
                     std::vector<unsigned char>* out_png) {
    CHECK(out_png != nullptr);
    CHECK_GT(width, 0);
    CHECK_GT(height, 0);
    CHECK_EQ(rgba.size(), static_cast<size_t>(width) * static_cast<size_t>(height) * 4u)
        << "EncodeRgbaToPng: rgba 尺寸应为 width*height*4";

    std::vector<unsigned char> png = jpov::RgbaToPng(rgba.data(), width, height, /*comps=*/4);
    if (png.empty()) {
        LOG(ERROR) << "EncodeRgbaToPng: RgbaToPng 返回空";
        return false;
    }
    *out_png = png;
    return true;
}

}  // namespace clothing
}  // namespace jpov
