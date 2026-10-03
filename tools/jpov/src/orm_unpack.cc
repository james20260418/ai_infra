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

std::vector<unsigned char> RgbaToPng(
    const unsigned char* pixels,
    int width,
    int height,
    int comps) {
    CHECK(pixels != nullptr);
    CHECK_GT(width, 0);
    CHECK_GT(height, 0);
    if (comps < 1 || comps > 4) comps = 4;

    // 写入 PNG 到 vector。stride = width * comps（每行字节数）。
    std::vector<unsigned char> png_data;
    VecWriter ctx;
    ctx.data = &png_data;

    const int ok = stbi_write_png_to_func(
        VecWriteFunc, &ctx,
        width, height,
        comps,
        pixels,
        width * comps);
    if (ok == 0) {
        LOG(ERROR) << "RgbaToPng: stbi_write_png_to_func failed";
        return {};
    }
    return png_data;
}

}  // namespace jpov
