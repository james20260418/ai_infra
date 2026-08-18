// JPOV 纹理工具 — 周期性（seamless/tileable）化
//
// 用法:
//   jpov_texture_seamless <input.png> <output.png> [blend_width] [mode]
//
// 把一张原始图改造成「可无缝平铺」的周期纹理：四个边缘分别与对侧边缘
// 做加权混合，消除接缝，使 GL_REPEAT 平铺时看不出拼接边界。
//
// 对每个像素，取关于图像中心 180° 对称的对侧位置，按其离最近边缘的
// 距离加权混合：
//
//   t = clamp(min(左,右,上,下 距离) / blend_width, 0, 1)
//   out = cur * t + opposite(180°对侧) * (1 - t)
//
// 靠边处（t→0）取对侧、中间（t→1）保留原值，平铺后接缝处数值连续。
//
// 参数:
//   blend_width   混合带宽度（像素），默认取 min(w, h) / 4。
//                 越大接缝越柔，但会损失更多内边缘原始细节。
//   mode          blend（默认，加权混合） | mirror（镜像填充，更硬但保真）
//
// 适用场景：给「地面/砖/墙面」这类周期性纹理去接缝，让场景里
// 多个 UV 对齐的地块（如 scene 的地砖）能无缝铺满。

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
    "Usage: jpov_texture_seamless <input.png> <output.png> [blend_width] [mode]\n"
    "\n"
    "Make an image seamlessly tileable (wrap-able without seams).\n"
    "\n"
    "  input.png      source image (any channel count)\n"
    "  output.png     seamless PNG output (same channel count)\n"
    "  blend_width    edge blend band in px, default min(w,h)/4\n"
    "  mode           blend (default) | mirror\n";

enum class Mode { kBlend, kMirror };

Mode ParseMode(const std::string& s) {
    if (s == "mirror") return Mode::kMirror;
    return Mode::kBlend;
}

int MirrorCoord(int x, int w) {
    int p = x % (2 * w);
    if (p < 0) p += 2 * w;
    return p < w ? p : (2 * w - 1 - p);
}

inline float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// 线性插值并四舍五入到字节：out = a + (b - a) * t, t∈[0,1]
inline unsigned char LerpByte(unsigned char a, unsigned char b, float t) {
    float v = static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t;
    if (v < 0.0f) v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    return static_cast<unsigned char>(v + 0.5f);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "%s", kUsage);
        return 1;
    }

    const std::string in_path = argv[1];
    const std::string out_path = argv[2];
    const Mode mode = ParseMode(argc >= 5 ? argv[4] : "blend");

    int w = 0, h = 0, ch = 0;
    unsigned char* img = stbi_load(in_path.c_str(), &w, &h, &ch, 0);
    if (!img) {
        std::fprintf(stderr, "error: failed to load '%s': %s\n",
                     in_path.c_str(), stbi_failure_reason());
        return 1;
    }
    if (w <= 0 || h <= 0 || ch <= 0) {
        std::fprintf(stderr, "error: bad image %dx%d ch=%d\n", w, h, ch);
        stbi_image_free(img);
        return 1;
    }

    int blend = (argc >= 4) ? std::atoi(argv[3]) : std::min(w, h) / 4;
    if (blend < 1) blend = 1;
    if (blend > std::min(w, h) / 2) blend = std::min(w, h) / 2;

    std::vector<unsigned char> out(w * h * ch);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            // 离最近边缘的像素距离：min(左, 右, 上, 下)。
            // 该值 < blend 的像素会被“对侧采样”混合，使平铺接缝闭和。
            const int dx = std::min(x, w - 1 - x);
            const int dy = std::min(y, h - 1 - y);
            const float edge_dist = static_cast<float>(std::min(dx, dy));

            for (int c = 0; c < ch; ++c) {
                const unsigned char cur = img[(y * w + x) * ch + c];

                // 对侧采样：与当前像素关于图像中心成 180° 对称的位置。
                //   blend  — 周期 wrap（(x+w/2, y+h/2) mod 图像尺寸）
                //   mirror — 镜像 wrap（跨中心镜面对称，接缝处更平滑）
                int sx, sy;
                if (mode == Mode::kMirror) {
                    sx = MirrorCoord(x + w / 2, w);
                    sy = MirrorCoord(y + h / 2, h);
                } else {
                    sx = (x + w / 2) % w;
                    sy = (y + h / 2) % h;
                }
                const unsigned char opp = img[(sy * w + sx) * ch + c];

                // 混合系数 t = 离最近边缘的距离 / blend。
                //   t→1（中间）保留原值；t→0（接缝处）取对侧，使平铺闭和。
                const float t = Clamp01(edge_dist / static_cast<float>(blend));
                out[(y * w + x) * ch + c] = LerpByte(cur, opp, 1.0f - t);
            }
        }
    }

    stbi_image_free(img);

    if (!stbi_write_png(out_path.c_str(), w, h, ch, out.data(), w * ch)) {
        std::fprintf(stderr, "error: failed to write '%s'\n", out_path.c_str());
        return 1;
    }

    std::printf("jpov_texture_seamless: wrote %dx%d ch=%d seamless -> %s "
                "(blend=%d, mode=%s)\n",
                w, h, ch, out_path.c_str(), blend,
                (mode == Mode::kMirror) ? "mirror" : "blend");
    return 0;
}
