#include "tools/jpov/tools/vegetation_tool_v2/leaf_card/leaf_card_normalize.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include "tools/jpov/tools/vegetation_tool_v2/leaf_card/leaf_card_spec.h"

namespace jpov {
namespace vegetation {
namespace {

constexpr double kPi = 3.14159265358979323846;

// 内容分量的最小面积：小于这个数当噪点丢掉。
constexpr int kMinComponentArea = 64;

// 判定「细端」时取的轴长比例（两端各 10%）。
constexpr double kTipBandRatio = 0.10;

double AlphaFromChannel(unsigned char r, unsigned char g, unsigned char b) {
  // 与 leaf_texture 的黑底→alpha 同一个软过档：[0.5t, 1.5t] 线性。
  const int peak = std::max({ static_cast<int>(r), static_cast<int>(g), static_cast<int>(b) });
  const double lo = kLeafBlackThreshold * 0.5;
  const double hi = kLeafBlackThreshold * 1.5;
  const double alpha = (static_cast<double>(peak) - lo) / (hi - lo);
  return std::clamp(alpha, 0.0, 1.0);
}

struct Vec2 {
  double x = 0.0;
  double y = 0.0;
};

Vec2 Sub(const Vec2& a, const Vec2& b) {
  return Vec2{ a.x - b.x, a.y - b.y };
}

Vec2 Mul(const Vec2& a, double s) {
  return Vec2{ a.x * s, a.y * s };
}

double Dot(const Vec2& a, const Vec2& b) {
  return a.x * b.x + a.y * b.y;
}

double Cross(const Vec2& a, const Vec2& b) {
  return a.x * b.y - a.y * b.x;
}

double Len(const Vec2& a) {
  return std::sqrt(Dot(a, a));
}

Vec2 Normalize(const Vec2& a) {
  const double length = Len(a);
  if (length < 1e-12) {
    return Vec2{ 1.0, 0.0 };
  }
  return Vec2{ a.x / length, a.y / length };
}

// 主轴（二阶矩）与质心。mask 只含 0/1。
void PrincipalAxis(const std::vector<unsigned char>& mask, int width, int height, Vec2* centroid,
                   Vec2* axis, double* extent_min, double* extent_max) {
  double sum_x = 0.0;
  double sum_y = 0.0;
  long long count = 0;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      if (mask[static_cast<std::size_t>(y) * width + x] == 0) {
        continue;
      }
      sum_x += x;
      sum_y += y;
      ++count;
    }
  }
  *centroid = Vec2{ sum_x / std::max(1LL, count), sum_y / std::max(1LL, count) };

  double cxx = 0.0;
  double cyy = 0.0;
  double cxy = 0.0;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      if (mask[static_cast<std::size_t>(y) * width + x] == 0) {
        continue;
      }
      const double dx = x - centroid->x;
      const double dy = y - centroid->y;
      cxx += dx * dx;
      cyy += dy * dy;
      cxy += dx * dy;
    }
  }
  // 2x2 协方差的主特征向量。
  const double angle = 0.5 * std::atan2(2.0 * cxy, cxx - cyy);
  *axis = Vec2{ std::cos(angle), std::sin(angle) };

  double lo = std::numeric_limits<double>::infinity();
  double hi = -std::numeric_limits<double>::infinity();
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      if (mask[static_cast<std::size_t>(y) * width + x] == 0) {
        continue;
      }
      const double t = Dot(Sub(Vec2{ static_cast<double>(x), static_cast<double>(y) }, *centroid), *axis);
      lo = std::min(lo, t);
      hi = std::max(hi, t);
    }
  }
  *extent_min = lo;
  *extent_max = hi;
}

// 只保留足够大的连通分量（把压缩噪点、孤立小点去掉）。
void KeepLargeComponents(std::vector<unsigned char>* mask, int width, int height) {
  const std::size_t pixels = static_cast<std::size_t>(width) * height;
  std::vector<int> label(pixels, 0);
  std::vector<int> area;
  int next_label = 0;
  std::vector<int> stack;
  for (std::size_t start = 0; start < pixels; ++start) {
    if ((*mask)[start] == 0 || label[start] != 0) {
      continue;
    }
    ++next_label;
    int count = 0;
    stack.clear();
    stack.push_back(static_cast<int>(start));
    label[start] = next_label;
    while (!stack.empty()) {
      const int index = stack.back();
      stack.pop_back();
      ++count;
      const int x = index % width;
      const int y = index / width;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const int nx = x + dx;
          const int ny = y + dy;
          if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
            continue;
          }
          const std::size_t neighbour = static_cast<std::size_t>(ny) * width + nx;
          if ((*mask)[neighbour] != 0 && label[neighbour] == 0) {
            label[neighbour] = next_label;
            stack.push_back(static_cast<int>(neighbour));
          }
        }
      }
    }
    area.push_back(count);
  }
  if (area.empty()) {
    return;
  }
  const int largest = *std::max_element(area.begin(), area.end());
  const int threshold = std::max(kMinComponentArea, largest / 200);
  for (std::size_t i = 0; i < pixels; ++i) {
    if (label[i] == 0) {
      continue;
    }
    if (area[static_cast<std::size_t>(label[i] - 1)] < threshold) {
      (*mask)[i] = 0;
    }
  }
}

// 倒角距离变换（3/4 近似），返回「到最近背景像素」的距离（像素单位）。
std::vector<float> DistanceTransform(const std::vector<unsigned char>& mask, int width,
                                     int height) {
  const float kBig = 1e9f;
  std::vector<float> distance(static_cast<std::size_t>(width) * height, 0.0f);
  for (std::size_t i = 0; i < distance.size(); ++i) {
    distance[i] = mask[i] != 0 ? kBig : 0.0f;
  }
  const float kOrth = 1.0f;
  const float kDiag = 1.41421356f;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      float value = distance[static_cast<std::size_t>(y) * width + x];
      for (int dy = -1; dy <= 0; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          if (dy == 0 && dx > 0) {
            continue;
          }
          const int nx = x + dx;
          const int ny = y + dy;
          if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
            continue;
          }
          const float step = (dx != 0 && dy != 0) ? kDiag : kOrth;
          value = std::min(value, distance[static_cast<std::size_t>(ny) * width + nx] + step);
        }
      }
      distance[static_cast<std::size_t>(y) * width + x] = value;
    }
  }
  for (int y = height - 1; y >= 0; --y) {
    for (int x = width - 1; x >= 0; --x) {
      float value = distance[static_cast<std::size_t>(y) * width + x];
      for (int dy = 0; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          if (dy == 0 && dx < 0) {
            continue;
          }
          const int nx = x + dx;
          const int ny = y + dy;
          if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
            continue;
          }
          const float step = (dx != 0 && dy != 0) ? kDiag : kOrth;
          value = std::min(value, distance[static_cast<std::size_t>(ny) * width + nx] + step);
        }
      }
      distance[static_cast<std::size_t>(y) * width + x] = value;
    }
  }
  return distance;
}

// 双线性采样（预乘 alpha），落在图外当作全透明。
void SamplePremultiplied(const std::vector<float>& premul, int width, int height, double x,
                         double y, float out[4]) {
  out[0] = 0.0f;
  out[1] = 0.0f;
  out[2] = 0.0f;
  out[3] = 0.0f;
  if (x < 0.0 || y < 0.0 || x > width - 1.0 || y > height - 1.0) {
    return;
  }
  const int x0 = static_cast<int>(std::floor(x));
  const int y0 = static_cast<int>(std::floor(y));
  const int x1 = std::min(x0 + 1, width - 1);
  const int y1 = std::min(y0 + 1, height - 1);
  const double fx = x - x0;
  const double fy = y - y0;
  const double w00 = (1.0 - fx) * (1.0 - fy);
  const double w10 = fx * (1.0 - fy);
  const double w01 = (1.0 - fx) * fy;
  const double w11 = fx * fy;
  const int offsets[4][2] = { { x0, y0 }, { x1, y0 }, { x0, y1 }, { x1, y1 } };
  const double weights[4] = { w00, w10, w01, w11 };
  for (int c = 0; c < 4; ++c) {
    double value = 0.0;
    for (int k = 0; k < 4; ++k) {
      const int px = offsets[k][0];
      const int py = offsets[k][1];
      value += weights[k] * premul[(static_cast<std::size_t>(py) * width + px) * 4 +
                                   static_cast<std::size_t>(c)];
    }
    out[c] = static_cast<float>(value);
  }
}

// 目标朝向：叶柄应当从锚点指向格子内部。
// 返回值是图像坐标（+x 右、+y 下）里的单位向量。
bool TargetStemDirection(const std::string& anchor_mode, Vec2* direction) {
  const double k = 0.7071067811865476;
  if (anchor_mode == "bottom_left") {
    *direction = Vec2{ k, -k };    // 右上
    return true;
  }
  if (anchor_mode == "bottom_right") {
    *direction = Vec2{ -k, -k };   // 左上
    return true;
  }
  if (anchor_mode == "bottom_center") {
    *direction = Vec2{ 0.0, -1.0 };  // 正上
    return true;
  }
  if (anchor_mode == "top_center") {
    *direction = Vec2{ 0.0, 1.0 };   // 正下
    return true;
  }
  return false;
}

bool TargetAnchorUv(const std::string& anchor_mode, double* u, double* v) {
  if (anchor_mode == "bottom_left") {
    *u = 0.0;
    *v = 0.0;
    return true;
  }
  if (anchor_mode == "bottom_right") {
    *u = 1.0;
    *v = 0.0;
    return true;
  }
  if (anchor_mode == "bottom_center") {
    *u = 0.5;
    *v = 0.0;
    return true;
  }
  if (anchor_mode == "top_center") {
    *u = 0.5;
    *v = 1.0;
    return true;
  }
  return false;
}

}  // namespace

bool NormalizeLeafCard(const LeafCardImage& input, const LeafCardNormalizeOptions& options,
                       LeafCardImage* output /*output*/,
                       LeafCardNormalizeReport* report /*output*/,
                       std::string* error /*output*/) {
  const int width = input.width;
  const int height = input.height;
  if (width < 2 || height < 2 ||
      input.rgba.size() != static_cast<std::size_t>(width) * height * 4) {
    *error = "输入图尺寸/字节数不对";
    return false;
  }
  Vec2 target_direction;
  if (!TargetStemDirection(options.anchor_mode, &target_direction)) {
    *error = "anchor_mode 只能是 bottom_left/bottom_center/bottom_right/top_center";
    return false;
  }
  double anchor_u = 0.0;
  double anchor_v = 0.0;
  TargetAnchorUv(options.anchor_mode, &anchor_u, &anchor_v);

  // ---- 1. alpha（黑底软过档）+ 内容掩膜（用内容下限，避免边缘抗锯齿干扰） ----
  std::vector<float> alpha(static_cast<std::size_t>(width) * height, 0.0f);
  std::vector<unsigned char> mask(static_cast<std::size_t>(width) * height, 0);
  long long ambiguous = 0;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t index = static_cast<std::size_t>(y) * width + x;
      const unsigned char* pixel = &input.rgba[index * 4];
      const int peak = std::max({ static_cast<int>(pixel[0]), static_cast<int>(pixel[1]),
                                  static_cast<int>(pixel[2]) });
      alpha[index] = static_cast<float>(AlphaFromChannel(pixel[0], pixel[1], pixel[2]));
      if (peak >= kLeafContentMinChannel) {
        mask[index] = 1;
      } else if (peak > kLeafBackgroundMaxChannel) {
        ++ambiguous;
      }
    }
  }
  KeepLargeComponents(&mask, width, height);
  long long content = 0;
  for (const unsigned char value : mask) {
    content += value != 0 ? 1 : 0;
  }
  report->ambiguous_fraction =
      content > 0 ? static_cast<double>(ambiguous) / static_cast<double>(content) : 0.0;
  if (content < kMinComponentArea) {
    *error = "图里没有识别到叶子内容（max(r,g,b) >= " + std::to_string(kLeafContentMinChannel) +
             "）：背景太亮、或者叶子画得太暗（生图合同见 leaf_card_spec.h）";
    return false;
  }

  // ---- 2. 主轴 + 两端的「粗细」，细的那端是叶柄 ----
  Vec2 centroid;
  Vec2 axis;
  double extent_min = 0.0;
  double extent_max = 0.0;
  PrincipalAxis(mask, width, height, &centroid, &axis, &extent_min, &extent_max);
  const std::vector<float> distance = DistanceTransform(mask, width, height);
  const double span = extent_max - extent_min;
  const double band = std::max(1.0, span * kTipBandRatio);

  double thin_low = 0.0;
  double thin_high = 0.0;
  long long count_low = 0;
  long long count_high = 0;
  Vec2 tip_low{ 0.0, 0.0 };
  Vec2 tip_high{ 0.0, 0.0 };
  double best_low = std::numeric_limits<double>::infinity();
  double best_high = -std::numeric_limits<double>::infinity();
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t index = static_cast<std::size_t>(y) * width + x;
      if (mask[index] == 0) {
        continue;
      }
      const Vec2 position{ static_cast<double>(x), static_cast<double>(y) };
      const double t = Dot(Sub(position, centroid), axis);
      if (t <= extent_min + band) {
        thin_low += distance[index];
        ++count_low;
        if (t < best_low) {
          best_low = t;
          tip_low = position;
        }
      }
      if (t >= extent_max - band) {
        thin_high += distance[index];
        ++count_high;
        if (t > best_high) {
          best_high = t;
          tip_high = position;
        }
      }
    }
  }
  const double mean_low = thin_low / std::max(1LL, count_low);
  const double mean_high = thin_high / std::max(1LL, count_high);
  const bool root_at_low = mean_low <= mean_high;
  const Vec2 root = root_at_low ? tip_low : tip_high;

  // 叶柄朝向 = 主轴、从根部指向另一端。
  Vec2 stem_direction = Normalize(Sub(centroid, root));
  if (Dot(stem_direction, axis) < 0.0) {
    stem_direction = Mul(axis, -1.0);
  } else {
    stem_direction = axis;
  }
  report->stem_angle_deg = std::atan2(stem_direction.y, stem_direction.x) * 180.0 / kPi;

  // ---- 3. 旋转：把叶柄转到「从锚点指向格子内部」 ----
  const double cross = Cross(stem_direction, target_direction);
  const double dot = Dot(stem_direction, target_direction);
  const double rotation = std::atan2(cross, dot);  // 图像坐标（+y 向下）里的顺时针角
  report->rotation_deg = rotation * 180.0 / kPi;
  const double cos_r = std::cos(rotation);
  const double sin_r = std::sin(rotation);

  // 预乘 RGBA，供旋转时插值（直接插值非预乘颜色会让边缘发黑）。
  std::vector<float> premul(static_cast<std::size_t>(width) * height * 4, 0.0f);
  for (std::size_t i = 0; i < alpha.size(); ++i) {
    for (int c = 0; c < 3; ++c) {
      // 注意单位：RGB 保持 0..255（只是乘上覆盖率），alpha 保持 0..1。
      // 两边都除以 255 的话写回 uchar 时会全变成 0（踩过）。
      premul[i * 4 + static_cast<std::size_t>(c)] =
          input.rgba[i * 4 + static_cast<std::size_t>(c)] * alpha[i];
    }
    premul[i * 4 + 3] = alpha[i];
  }

  const int out_width =
      static_cast<int>(std::ceil(std::abs(width * cos_r) + std::abs(height * sin_r))) + 2;
  const int out_height =
      static_cast<int>(std::ceil(std::abs(width * sin_r) + std::abs(height * cos_r))) + 2;
  std::vector<float> rotated(static_cast<std::size_t>(out_width) * out_height * 4, 0.0f);
  const Vec2 center_in{ width * 0.5, height * 0.5 };
  const Vec2 center_out{ out_width * 0.5, out_height * 0.5 };
  for (int y = 0; y < out_height; ++y) {
    for (int x = 0; x < out_width; ++x) {
      // 逆向映射：输出像素 → 输入像素（用 -rotation 反变换）。
      const Vec2 offset{ x + 0.5 - center_out.x, y + 0.5 - center_out.y };
      const Vec2 source{ offset.x * cos_r + offset.y * sin_r + center_in.x,
                         -offset.x * sin_r + offset.y * cos_r + center_in.y };
      float sample[4];
      SamplePremultiplied(premul, width, height, source.x - 0.5, source.y - 0.5, sample);
      const std::size_t index = (static_cast<std::size_t>(y) * out_width + x) * 4;
      for (int c = 0; c < 4; ++c) {
        rotated[index + static_cast<std::size_t>(c)] = sample[c];
      }
    }
  }

  // ---- 4. 裁剪：根点必须**正好**落在 anchor 上 ----
  //
  // 只按 AABB 裁是不够的：叶柄如果不在内容的极值处（叶子下垂、或检测到的根不在最外沿），
  // 根点就会落在格内 10% 的位置 —— 贴到枝上时整片叶子悬空。所以这里按「根点对齐锚点」
  // 反推画布：锚点在左/右/中时，画布边界分别贴到根点（或关于根点对称）。
  // 落在画布外侧的内容会被裁掉，裁掉多少会报出来（dropped_fraction）。
  int x0 = out_width;
  int y0 = out_height;
  int x1 = -1;
  int y1 = -1;
  for (int y = 0; y < out_height; ++y) {
    for (int x = 0; x < out_width; ++x) {
      if (rotated[(static_cast<std::size_t>(y) * out_width + x) * 4 + 3] > kLeafCardAlphaCutoff) {
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
      }
    }
  }
  if (x1 < x0 || y1 < y0) {
    *error = "旋转后没有内容（alpha 全低）";
    return false;
  }

  // 根点在旋转图里的坐标（与上面的正向变换同一个公式）。
  const Vec2 root_offset = Sub(root, center_in);
  const Vec2 root_rotated{ root_offset.x * cos_r - root_offset.y * sin_r + center_out.x - 0.5,
                           root_offset.x * sin_r + root_offset.y * cos_r + center_out.y - 0.5 };
  const double root_x = root_rotated.x;
  const double root_y = root_rotated.y;

  int left = 0;
  int crop_width = 0;
  if (anchor_u <= 0.0) {
    left = static_cast<int>(std::lround(root_x));
    crop_width = x1 - left + 1;
  } else if (anchor_u >= 1.0) {
    crop_width = static_cast<int>(std::lround(root_x)) - x0 + 1;
    left = x0;
  } else {
    const double half = std::max(root_x - x0, x1 - root_x);
    crop_width = static_cast<int>(std::lround(2.0 * half)) + 1;
    left = static_cast<int>(std::lround(root_x - half));
  }
  int top = 0;
  int crop_height = 0;
  if (anchor_v <= 0.0) {
    crop_height = static_cast<int>(std::lround(root_y)) - y0 + 1;
    top = y0;
  } else {
    crop_height = y1 - static_cast<int>(std::lround(root_y)) + 1;
    top = static_cast<int>(std::lround(root_y));
  }
  crop_width = std::max(2, crop_width);
  crop_height = std::max(2, crop_height);

  output->width = crop_width;
  output->height = crop_height;
  output->rgba.assign(static_cast<std::size_t>(crop_width) * crop_height * 4, 0);

  long long dropped = 0;
  for (int y = 0; y < crop_height; ++y) {
    for (int x = 0; x < crop_width; ++x) {
      const int sx = left + x;
      const int sy = top + y;
      if (sx < 0 || sy < 0 || sx >= out_width || sy >= out_height) {
        continue;
      }
      const std::size_t source = (static_cast<std::size_t>(sy) * out_width + sx) * 4;
      const double a = std::clamp(static_cast<double>(rotated[source + 3]), 0.0, 1.0);
      const std::size_t target = (static_cast<std::size_t>(y) * crop_width + x) * 4;
      // 输出：RGB 预乘、A = 覆盖率。
      //   * 消费方（leaf_texture）按「max(r,g,b) 黑底→alpha」读，预乘正好等价；
      //   * 认 alpha 的看图软件也能看对（背景是全透明黑）。
      for (int c = 0; c < 3; ++c) {
        output->rgba[target + static_cast<std::size_t>(c)] =
            static_cast<unsigned char>(std::clamp(static_cast<double>(rotated[source + c]), 0.0, 255.0));
      }
      output->rgba[target + 3] = static_cast<unsigned char>(std::lround(a * 255.0));
    }
  }
  // 被裁掉的内容（原来的 AABB 里、但落在画布外的部分）。
  const long long bbox_area = static_cast<long long>(x1 - x0 + 1) * (y1 - y0 + 1);
  dropped = std::max(0LL, bbox_area - static_cast<long long>(crop_width) * crop_height);
  report->dropped_fraction = bbox_area > 0 ? static_cast<double>(dropped) / bbox_area : 0.0;

  // ---- 5. 诊断：根点实际落在格内哪儿（按构造应当 == anchor） ----
  const double root_u = (root_x - static_cast<double>(left) + 0.5) / std::max(1, crop_width);
  const double root_v = 1.0 - (root_y - static_cast<double>(top) + 0.5) / std::max(1, crop_height);

  report->anchor_mode = options.anchor_mode;
  report->root_offset_u = root_u - anchor_u;
  report->root_offset_v = root_v - anchor_v;
  report->width = crop_width;
  report->height = crop_height;
  report->aspect = static_cast<double>(crop_width) / std::max(1, crop_height);
  return true;
}

}  // namespace vegetation
}  // namespace jpov
