// JPOV 纹理工具 — 法线贴图生成
//
// 用法:
//   jpov_texture_normal <input.png> <output.png> [normal_scale]
//
// 从输入的原始漫反射/高度图生成一张「法线贴图」。
// 输入按灰度高度场处理，使用 Sobel 算子估计每个像素的梯度，
// 再由梯度构造切线空间法线：
//
//   gx = (left - right) 的水平梯度
//   gy = (top - bottom) 的垂直梯度
//   normal = normalize(-gx*scale, -gy*scale, 1)
//   写入 RGB: normal = (n + 1) / 2  →  把 [-1,1] 映射到 [0,1]
//
// normal_scale 控制法线扰动的强度（类似 PBR 的 normal_scale）：
//   - 越大 → 法线越陡、光影越强
//   - 越小 → 法线越平、越接近 (0,0,1)（纯粹的面朝上）
//   - 默认 2.0，通常地面/砖类 1~4 之间表现自然。
//
// 输出是 8-bit RGB PNG（JPOV PBRMaterial::normal_tex 可直接用的格式）。
// 像素值 0.5,0.5,1.0 (128,128,255) 表示无扰动（垂直朝上）。

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb/stb_image_write.h"

namespace {

const char* kUsage =
    "Usage: jpov_texture_normal <input.png> <output.png> [normal_scale] [out_name]\n"
    "\n"
    "Generate a normal map from a height/color image.\n"
    "\n"
    "  input.png       source image (loaded as grayscale height field)\n"
    "  output.png      normal map output (8-bit RGB PNG)\n"
    "  normal_scale    strength, 0 disables perturbation, default 2.0\n";

// Sobel 方向的权重（3x3，中心省略）。
//   水平: [-1  0  1]       垂直: [-1 -2 -1]
//         [-2  0  2]              [ 0  0  0]
//         [-1  0  1]              [ 1  2  1]
// 采用左右/上下采样顺序，使负号与法线朝向自洽（测试时统一约定）。
int SobelGx(int left, int right, int tl, int tr, int bl, int br) {
    // standard sobel x
    return (tr + 2 * right + br) - (tl + 2 * left + bl);
}
int SobelGy(int top, int bottom, int tl, int tr, int bl, int br) {
    // standard sobel y
    return (bl + 2 * bottom + br) - (tl + 2 * top + tr);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "%s", kUsage);
        return 1;
    }

    const std::string in_path = argv[1];
    const std::string out_path = argv[2];
    const float scale = (argc >= 4) ? std::atof(argv[3]) : 2.0f;

    int w = 0, h = 0, ch = 0;
    unsigned char* img = stbi_load(in_path.c_str(), &w, &h, &ch, 0);
    if (!img) {
        std::fprintf(stderr, "error: failed to load '%s': %s\n",
                     in_path.c_str(), stbi_failure_reason());
        return 1;
    }
    if (w <= 0 || h <= 0) {
        std::fprintf(stderr, "error: bad dimensions %dx%d\n", w, h);
        stbi_image_free(img);
        return 1;
    }
    const int nch = (ch == 0) ? 1 : ch;

    // 转成单通道灰度高度场。已有灰度直接复用，否则按 luma 加权。
    std::vector<unsigned char> height(w * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const unsigned char* p = img + (y * w + x) * nch;
            unsigned char v;
            if (nch == 1) {
                v = p[0];
            } else {
                // Rec.601 luma: 0.299 R + 0.587 G + 0.114 B
                v = static_cast<unsigned char>(
                    0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2] + 0.5f);
            }
            height[y * w + x] = v;
        }
    }
    stbi_image_free(img);

    std::vector<unsigned char> out(w * h * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            // 周期采样（wrap），保证生成的法线图也能无缝平铺。
            const int xm1 = (x - 1 + w) % w;
            const int xp1 = (x + 1) % w;
            const int ym1 = (y - 1 + h) % h;
            const int yp1 = (y + 1) % h;

            const auto& at = [&](int xx, int yy) -> unsigned char {
                return height[yy * w + xx];
            };

            const unsigned char tl = at(xm1, ym1);
            const unsigned char tr = at(xp1, ym1);
            const unsigned char bl = at(xm1, yp1);
            const unsigned char br = at(xp1, yp1);
            const unsigned char l  = at(xm1, y);
            const unsigned char r  = at(xp1, y);
            const unsigned char t  = at(x, ym1);
            const unsigned char b  = at(x, yp1);

            const float gx = static_cast<float>(SobelGx(l, r, tl, tr, bl, br));
            const float gy = static_cast<float>(SobelGy(t, b, tl, tr, bl, br));

            // 切线空间法线: (-gx*scale, -gy*scale, 1) 归一化。
            // 负号：高度场梯度指向下坡方向，法线应指向上坡，
            // 故取负号使凹凸与高度吻合（与常规 sobel normal 一致）。
            float nx = -gx * scale;
            float ny = -gy * scale;
            float nz = 1.0f;
            const float inv_len = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
            nx *= inv_len;
            ny *= inv_len;
            nz *= inv_len;

            unsigned char* o = &out[(y * w + x) * 3];
            o[0] = static_cast<unsigned char>((nx * 0.5f + 0.5f) * 255.0f + 0.5f);
            o[1] = static_cast<unsigned char>((ny * 0.5f + 0.5f) * 255.0f + 0.5f);
            o[2] = static_cast<unsigned char>((nz * 0.5f + 0.5f) * 255.0f + 0.5f);
        }
    }

    // 写入 PNG。输出为 3 通道（RGB）。
    if (!stbi_write_png(out_path.c_str(), w, h, 3, out.data(), w * 3)) {
        std::fprintf(stderr, "error: failed to write '%s'\n", out_path.c_str());
        return 1;
    }

    std::printf("jpov_texture_normal: wrote %dx%d normal map -> %s (scale=%.2f)\n",
                w, h, out_path.c_str(), scale);
    return 0;
}
