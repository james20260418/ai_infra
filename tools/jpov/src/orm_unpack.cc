// ORM 通道解包实现 — 用 stb_image_write 把单通道写入 PNG 字节流
//
// stb_image_write 的实现宏（STB_IMAGE_WRITE_IMPLEMENTATION）
// 经由 //third_party/stb:stb_image_write 的 copts 注入。

#include "tools/jpov/src/orm_unpack.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include <glog/logging.h>

#include "third_party/stb/stb_image_write.h"

namespace jpov {

namespace {

// stbi_write_png_to_func 的回调上下文：写入 std::vector<unsigned char>
struct VecWriter {
    std::vector<unsigned char>* data;
};

void VecWriteFunc(void* context, void* data, int size) {
    VecWriter* w = static_cast<VecWriter*>(context);
    const unsigned char* src = static_cast<const unsigned char*>(data);
    w->data->insert(w->data->end(), src, src + size);
}

}  // namespace

std::vector<unsigned char> ExtractChannelToPng(
    const unsigned char* pixels,
    int width,
    int height,
    int channel) {
    CHECK(pixels != nullptr);
    CHECK_GT(width, 0);
    CHECK_GT(height, 0);
    CHECK_GE(channel, 0);
    CHECK_LT(channel, 4);

    // 从 RGBA 逐像素提取指定通道
    std::vector<unsigned char> gray(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int src_idx = (y * width + x) * 4 + channel;
            gray[y * width + x] = pixels[src_idx];
        }
    }

    // 写入 PNG 到 vector
    std::vector<unsigned char> png_data;
    VecWriter ctx;
    ctx.data = &png_data;

    const int ok = stbi_write_png_to_func(
        VecWriteFunc, &ctx,
        width, height,
        1,  // 1 channel = grayscale
        gray.data(),
        width);  // stride = width bytes per row
    if (ok == 0) {
        LOG(ERROR) << "ExtractChannelToPng: stbi_write_png_to_func failed";
        return {};
    }
    return png_data;
}

}  // namespace jpov
