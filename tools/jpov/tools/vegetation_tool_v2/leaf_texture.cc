// 叶卡贴图的 CPU 处理实现（查看器与 GLB 烘焙共用同一份像素流水线）。

#include "tools/jpov/tools/vegetation_tool_v2/leaf_texture.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"
#include "third_party/stb/stb_image_write.h"

namespace jpov {
namespace vegetation {

namespace {

void DilateRgbIntoTransparent(const std::vector<unsigned char>& alpha_mask,
                              std::vector<unsigned char>* rgba, int w, int h, int passes) {
  std::vector<unsigned char> mask = alpha_mask;   // 0/1：该像素有没有"内容"
  for (int p = 0; p < passes; ++p) {
    std::vector<unsigned char> next = mask;
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const size_t idx = static_cast<size_t>(y) * w + x;
        if (mask[idx]) continue;
        int acc[3] = {0, 0, 0};
        int n = 0;
        const int dx[4] = {-1, 1, 0, 0};
        const int dy[4] = {0, 0, -1, 1};
        for (int k = 0; k < 4; ++k) {
          const int nx = x + dx[k];
          const int ny = y + dy[k];
          if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
          const size_t nidx = static_cast<size_t>(ny) * w + nx;
          if (!mask[nidx]) continue;
          acc[0] += (*rgba)[nidx * 4 + 0];
          acc[1] += (*rgba)[nidx * 4 + 1];
          acc[2] += (*rgba)[nidx * 4 + 2];
          ++n;
        }
        if (n > 0) {
          (*rgba)[idx * 4 + 0] = static_cast<unsigned char>(acc[0] / n);
          (*rgba)[idx * 4 + 1] = static_cast<unsigned char>(acc[1] / n);
          (*rgba)[idx * 4 + 2] = static_cast<unsigned char>(acc[2] / n);
          next[idx] = 1;
        }
      }
    }
    mask.swap(next);
  }
}

std::vector<unsigned char> BuildLeafNormalMapImpl(const std::vector<unsigned char>& rgba,
                                                   int w, int h, float depth) {
  std::vector<float> height(static_cast<size_t>(w) * h);
  for (size_t i = 0; i < height.size(); ++i) {
    // 高度场 = 轮廓(alpha) + 叶面明暗(luma)。
    //   alpha：给轮廓一圈软倒角（治"剃刀切出来的纸边"）
    //   luma ：给叶脉/叶面起伏（照片本身的光影被当成凹凸，经典的 emboss 用法）
    // 只用 alpha 时叶面内部是纯平的，近看仍然是贴纸；加上 luma 才有叶面细节。
    const float a = rgba[i * 4 + 3] / 255.0f;
    const float luma = (0.299f * rgba[i * 4 + 0] + 0.587f * rgba[i * 4 + 1] +
                        0.114f * rgba[i * 4 + 2]) /
                       255.0f;
    height[i] = 0.45f * a + 0.55f * luma;
  }
  // 两次 3-tap 盒模糊（等效半径 2）：把硬边变成斜坡，法线才有倒角。
  for (int pass = 0; pass < 2; ++pass) {
    std::vector<float> tmp = height;
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const int xm = (x == 0) ? 0 : x - 1;
        const int xp = (x + 1 == w) ? w - 1 : x + 1;
        tmp[static_cast<size_t>(y) * w + x] =
            (height[static_cast<size_t>(y) * w + xm] +
             height[static_cast<size_t>(y) * w + x] +
             height[static_cast<size_t>(y) * w + xp]) / 3.0f;
      }
    }
    height.swap(tmp);
    tmp = height;
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const int ym = (y == 0) ? 0 : y - 1;
        const int yp = (y + 1 == h) ? h - 1 : y + 1;
        tmp[static_cast<size_t>(y) * w + x] =
            (height[static_cast<size_t>(ym) * w + x] +
             height[static_cast<size_t>(y) * w + x] +
             height[static_cast<size_t>(yp) * w + x]) / 3.0f;
      }
    }
    height.swap(tmp);
  }

  std::vector<unsigned char> out(height.size() * 4);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t i = static_cast<size_t>(y) * w + x;
      const int xm = (x == 0) ? 0 : x - 1;
      const int xp = (x + 1 == w) ? w - 1 : x + 1;
      const int ym = (y == 0) ? 0 : y - 1;
      const int yp = (y + 1 == h) ? h - 1 : y + 1;
      const float dx = 0.5f * (height[static_cast<size_t>(y) * w + xp] -
                               height[static_cast<size_t>(y) * w + xm]);
      const float dy_img = 0.5f * (height[static_cast<size_t>(yp) * w + x] -
                                   height[static_cast<size_t>(ym) * w + x]);
      float nx = -dx * depth;
      float ny = dy_img * depth;   // +v 与图像 y 反向
      float nz = 1.0f;
      const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
      nx /= len;
      ny /= len;
      nz /= len;
      out[i * 4 + 0] = static_cast<unsigned char>((nx * 0.5f + 0.5f) * 255.0f + 0.5f);
      out[i * 4 + 1] = static_cast<unsigned char>((ny * 0.5f + 0.5f) * 255.0f + 0.5f);
      out[i * 4 + 2] = static_cast<unsigned char>((nz * 0.5f + 0.5f) * 255.0f + 0.5f);
      out[i * 4 + 3] = 255;
    }
  }
  return out;
}

std::vector<unsigned char> EncodePngBytes(const std::vector<unsigned char>& rgba, int w, int h) {
  std::vector<unsigned char> out;
  stbi_write_png_to_func(
      [](void* ctx, void* data, int size) {
        std::vector<unsigned char>* v = static_cast<std::vector<unsigned char>*>(ctx);
        const unsigned char* p = static_cast<const unsigned char*>(data);
        v->insert(v->end(), p, p + size);
      },
      &out, w, h, 4, rgba.data(), w * 4);
  return out;
}

void FlipRowsImpl(std::vector<unsigned char>* rgba, int w, int h) {
  const size_t stride = static_cast<size_t>(w) * 4;
  std::vector<unsigned char> tmp(stride);
  for (int y = 0; y < h / 2; ++y) {
    unsigned char* a = rgba->data() + static_cast<size_t>(y) * stride;
    unsigned char* b = rgba->data() + static_cast<size_t>(h - 1 - y) * stride;
    std::memcpy(tmp.data(), a, stride);
    std::memcpy(a, b, stride);
    std::memcpy(b, tmp.data(), stride);
  }
}

}  // namespace

bool LoadLeafTextureFromPng(const std::string& path, float black, int dilate_passes,
                            LeafTexture* out, std::string* error) {
  auto fail = [error](const std::string& why) {
    if (error != nullptr) {
      *error = why;
    }
    return false;
  };
  if (out == nullptr) {
    return fail("LoadLeafTextureFromPng: out 不能为空");
  }
  int w = 0;
  int h = 0;
  int comp = 0;
  unsigned char* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
  if (px == nullptr) {
    return fail("读不到/解码失败: " + path);
  }
  std::vector<unsigned char> rgba(px, px + static_cast<size_t>(w) * h * 4);
  stbi_image_free(px);

  // 「近乎黑 = 透明」：max(r,g,b) 在 0.5t~1.5t 之间线性过渡（与查看器 --leaf-black 一致）。
  const float lo = black * 0.5f;
  const float hi = black * 1.5f;
  const float span = std::max(1e-3f, hi - lo);
  std::vector<unsigned char> mask(static_cast<size_t>(w) * h);
  size_t opaque = 0;
  for (size_t i = 0; i < mask.size(); ++i) {
    const float mx = static_cast<float>(
        std::max(std::max(rgba[i * 4 + 0], rgba[i * 4 + 1]), rgba[i * 4 + 2]));
    float t = (mx - lo) / span;
    t = std::min(1.0f, std::max(0.0f, t));
    const unsigned char a = static_cast<unsigned char>(t * 255.0f + 0.5f);
    if (t > 0.0f && t < 1.0f) {
      // 黑底素材：观测色 = 叶片色 × 覆盖率，边缘要按覆盖率反解，否则发黑。
      for (int c = 0; c < 3; ++c) {
        rgba[i * 4 + c] =
            static_cast<unsigned char>(std::min(255.0f, rgba[i * 4 + c] / t));
      }
    }
    rgba[i * 4 + 3] = a;
    mask[i] = a > 0 ? 1 : 0;
    if (a > 0) {
      ++opaque;
    }
  }
  if (opaque == 0) {
    return fail("叶卡图里没有非黑像素（阈值 --leaf-black " + std::to_string(black) +
                "）: " + path);
  }
  DilateRgbIntoTransparent(mask, &rgba, w, h, dilate_passes);

  out->width = w;
  out->height = h;
  out->rgba.swap(rgba);
  return true;
}

std::vector<unsigned char> BuildLeafNormalMap(const LeafTexture& tex, float depth) {
  return BuildLeafNormalMapImpl(tex.rgba, tex.width, tex.height, depth);
}

bool ReadPngSize(const std::string& path, int* width, int* height) {
  CHECK(width != nullptr);
  CHECK(height != nullptr);
  int w = 0;
  int h = 0;
  int comp = 0;
  if (!stbi_info(path.c_str(), &w, &h, &comp)) {
    return false;
  }
  if (w <= 0 || h <= 0) {
    return false;
  }
  *width = w;
  *height = h;
  return true;
}

bool EncodePng(const std::vector<unsigned char>& rgba, int w, int h,
               std::vector<unsigned char>* out) {
  if (out == nullptr) {
    return false;
  }
  *out = EncodePngBytes(rgba, w, h);
  return !out->empty();
}

void FlipRows(std::vector<unsigned char>* rgba, int w, int h) {
  if (rgba == nullptr) {
    return;
  }
  FlipRowsImpl(rgba, w, h);
}

}  // namespace vegetation
}  // namespace jpov
