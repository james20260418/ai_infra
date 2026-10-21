// 植被工具 v2 —— 只做一件事：把 ShapeProto（pb.txt）渲染出来看形状对不对。
//
// 刻意【不接材质】：到这一阶段要判断的是**拓扑/UV 尺度/分叉**，贴图会干扰判断。
//   * 树干：按格式定义逐枝扫管，**每根枝单独一个 mesh**（便于逐枝上色/逐枝隐藏）
//   * 层级配色：level 0 深棕 → 越深越浅（一眼看出父子关系）
//   * 叶卡：用**盒子**表示（不是薄片）—— 盒子能看清朝向与尺寸，薄片侧面看不见
//   * 类型配色：最多 4 种绿，按 type 取模
//
// 交互：K 切换叶卡、[ / ] 调最大显示层级、P 打印相机、H 打印叶卡帮助。
//
// 用法：
//   jpov_vegetation_v2_viewer --shape /tmp/willow.pbtxt
//   jpov_vegetation_v2_viewer --shape ... --capture /tmp/shots --cam_phi 10 --cam_r 4
//   --normal-scale <f>   法线强度（默认 1.5；0~3）
//   --uv-vscale 0.5      v 方向缩放（默认 0 = 自动取 albedo 宽/高）
//   --leaf leaf.png      叶卡贴图（整图一格；近乎黑的像素当透明）
//   --curl 0.5           顶点法线球面化强度 A（0 = 平面法线）
//   --leaf-normal 1.0    由 alpha 生成的法线贴图强度 B（0 = 关）
//   --cam-target-y 3.0   注视点高度（配合 --cam_r 0.9 看叶卡细节）

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <glog/logging.h>
#include "third_party/stb/stb_image.h"
#include "third_party/stb/stb_image_write.h"
#include <google/protobuf/text_format.h>

#include "tools/jpov/demo/skylight_scene.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/ui.h"
#include "tools/jpov/tools/vegetation_tool_v2/shape.h"
#include "tools/jpov/tools/vegetation_tool_v2/sweep.h"

namespace {

// 渲染分辨率（与其它 viewer 一致）。不从 viewer_app.h 取：那个头文件只为这几个常量
// 会把整个交互 App 基类拉进来。
constexpr int kViewerWidth = 1280;
constexpr int kViewerHeight = 720;
constexpr float kViewerFps = 60.0f;
using jpov::vegetation::LeafCard;
using jpov::vegetation::LeafType;
using jpov::vegetation::MeshData;
using jpov::vegetation::Shape;
using jpov::vegetation::Vec3d;

constexpr int kMaxCardTypes = 4;  // 4 种绿
constexpr float kCardThickness = 0.02f;  // 盒子厚度（米）：只是为了看得见朝向

bool ReadFileBytes(const std::string& path, std::vector<unsigned char>* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return !out->empty();
}

std::vector<unsigned char> EncodePng(const std::vector<unsigned char>& rgba, int w, int h) {
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

// 只把 RGB 往透明区扩散，**不动 alpha**（alpha 是 cutout 的判据）。
// 目的：mip / 双线性采样时，叶子边缘不会混到背景的黑色（否则一圈黑边）。
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

// 由 alpha 生成切线空间法线贴图：alpha 当高度场 → 模糊 → 中央差分。
// green = +v（OpenGL 约定，与引擎 B = cross(N,T) 一致）；图像 y 向下，所以要取反。
std::vector<unsigned char> BuildNormalFromAlpha(const std::vector<unsigned char>& rgba,
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

// A：顶点法线球面化。卡片本来 4 个顶点法线全等于面法线 → 整卡一个亮度（纸片感）。
// 按格内 uv 把法线往「叶片中心鼓起」的方向掰：角上法线向外张，中央仍朝面外。
// k = 0 时逐位等价于原平面法线（零回归）。
void BendCardNormals(jpov::MeshData* md, float k) {
  // 注意：jpov::MeshData.positions 是 vector<Vec3f>，size() 已经是顶点数。
  const size_t nverts = md->positions.size();
  for (size_t v = 0; v + 3 < nverts; v += 4) {
    const jpov::Vec3f& p0 = md->positions[v + 0];
    const jpov::Vec3f& p1 = md->positions[v + 1];
    const jpov::Vec3f& p3 = md->positions[v + 3];
    jpov::Vec3f right = p1 - p0;
    jpov::Vec3f up = p3 - p0;
    const float rl = std::sqrt(right.x() * right.x() + right.y() * right.y() +
                               right.z() * right.z());
    const float ul = std::sqrt(up.x() * up.x() + up.y() * up.y() + up.z() * up.z());
    if (rl < 1e-9f || ul < 1e-9f) continue;
    right = right * (1.0f / rl);
    up = up * (1.0f / ul);
    jpov::Vec3f n{right.y() * up.z() - right.z() * up.y(),
                  right.z() * up.x() - right.x() * up.z(),
                  right.x() * up.y() - right.y() * up.x()};
    const float nl = std::sqrt(n.x() * n.x() + n.y() * n.y() + n.z() * n.z());
    if (nl < 1e-9f) continue;
    n = n * (1.0f / nl);

    for (size_t i = 0; i < 4; ++i) {
      const jpov::Vec2f& uv = md->uvs[v + i];
      const float u = uv.x();
      const float w = uv.y();
      jpov::Vec3f bent = n + right * ((2.0f * u - 1.0f) * k) + up * ((2.0f * w - 1.0f) * k);
      const float bl = std::sqrt(bent.x() * bent.x() + bent.y() * bent.y() +
                                 bent.z() * bent.z());
      if (bl < 1e-9f) continue;
      md->normals[v + i] = bent * (1.0f / bl);
    }
  }
}

// 读 PNG 的 IHDR 拿像素尺寸（只为贴图长宽比，不值得为此加一条 stb 依赖）。
// 非 PNG / 损坏 → 返回 false，调用方回退 1.0。
bool ReadPngSize(const std::string& path, int* w, int* h) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  unsigned char buf[24] = {0};
  f.read(reinterpret_cast<char*>(buf), sizeof(buf));
  if (f.gcount() != static_cast<std::streamsize>(sizeof(buf))) return false;
  static const unsigned char kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  for (int i = 0; i < 8; ++i) {
    if (buf[i] != kSig[i]) return false;
  }
  const auto be32 = [](const unsigned char* p) {
    return (static_cast<int>(p[0]) << 24) | (static_cast<int>(p[1]) << 16) |
           (static_cast<int>(p[2]) << 8) | static_cast<int>(p[3]);
  };
  // PNG 规范要求 IHDR 是第一个 chunk：长度(4) + "IHDR"(4) + w(4) + h(4)。
  if (buf[12] != 'I' || buf[13] != 'H' || buf[14] != 'D' || buf[15] != 'R') return false;
  *w = be32(buf + 16);
  *h = be32(buf + 20);
  return *w > 0 && *h > 0;
}

struct CliArgs {
  std::string shape_path;
  std::string bark_dir;   // 空 = 不贴树皮（用 level 棕色）
  bool capture = false;
  std::string out_dir;
  double phi_deg = 12.0;
  double theta_deg = 35.0;
  double radius = 0.0;   // 0 = 自动
  int max_level = 99;
  float ground_y = 0.0f;
  float cam_target_y = 0.0f;   // 相机注视点高度（看树冠/叶卡细节用）
  bool show_panel = false;     // capture 时也画面板（截图/自检用）
  bool hide_leaf = false;      // 强制看 debug 盒子（叶卡贴图关）
  bool hide_bark = false;      // 强制看 level 棕色（树皮关）
  // 法线贴图强度（uNormalScale）。默认 1.5：同一树皮 A/B 实测
  // 1.0 偏平、3.0 发塑料，1.5 最自然（Skylight + 单平行光下 ambient 占比高，
  // 1.0 的凹凸容易被环境光冲淡）。面板滑条同一量程。
  float normal_scale = 1.5f;
  // UV 的 v 方向缩放 = 贴图宽/高。0 = 自动从 albedo 的像素尺寸读。
  // 非方形贴图必须补偿，否则整张图被按正方形铺开：1:2 的图横向拉宽 2×，
  // 凸起在 u 方向被压扁、纹素不再方正（法线细节跟着糊）。
  float uv_vscale = 0.0f;

  // 叶卡（--leaf 给了才启用）。整张图 = 一格；黑底当透明。
  std::string leaf_path;
  float leaf_black = 12.0f;   // max(r,g,b) < 此值 → alpha 0（带 0.5x~1.5x 软过渡）
  float leaf_curl = 0.5f;     // A：顶点法线球面化强度
  float leaf_normal = 1.0f;   // B：法线贴图强度（uNormalScale）
};

CliArgs ParseCli(int argc, char** argv) {
  CliArgs a;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_val = (i + 1 < argc);
    if (arg == "--bark" && has_val) {
      a.bark_dir = argv[++i];
    } else if (arg == "--shape" && has_val) {
      a.shape_path = argv[++i];
    } else if (arg == "--capture") {
      a.capture = true;
      if (has_val && argv[i + 1][0] != '-') {
        a.out_dir = argv[++i];
      }
    } else if (arg == "--cam_phi" && has_val) {
      a.phi_deg = std::atof(argv[++i]);
    } else if (arg == "--cam_theta" && has_val) {
      a.theta_deg = std::atof(argv[++i]);
    } else if (arg == "--cam_r" && has_val) {
      a.radius = std::atof(argv[++i]);
      CHECK_GT(a.radius, 0.0) << "--cam_r 必须 > 0";
    } else if (arg == "--leaf" && has_val) {
      a.leaf_path = argv[++i];
    } else if (arg == "--leaf-black" && has_val) {
      a.leaf_black = static_cast<float>(std::atof(argv[++i]));
      CHECK_GE(a.leaf_black, 0.0f) << "--leaf-black 不能为负";
    } else if (arg == "--curl" && has_val) {
      a.leaf_curl = static_cast<float>(std::atof(argv[++i]));
      CHECK_GE(a.leaf_curl, 0.0f) << "--curl 不能为负";
    } else if (arg == "--leaf-normal" && has_val) {
      a.leaf_normal = static_cast<float>(std::atof(argv[++i]));
      CHECK_GE(a.leaf_normal, 0.0f) << "--leaf-normal 不能为负（0 = 关掉法线贴图）";
    } else if (arg == "--normal-scale" && has_val) {
      a.normal_scale = static_cast<float>(std::atof(argv[++i]));
      CHECK_GT(a.normal_scale, 0.0f) << "--normal-scale 必须 > 0";
    } else if (arg == "--uv-vscale" && has_val) {
      a.uv_vscale = static_cast<float>(std::atof(argv[++i]));
      CHECK_GT(a.uv_vscale, 0.0f) << "--uv-vscale 必须 > 0";
    } else if (arg == "--show-panel") {
      a.show_panel = true;
    } else if (arg == "--no-leaf") {
      a.hide_leaf = true;
    } else if (arg == "--no-bark") {
      a.hide_bark = true;
    } else if (arg == "--cam-target-y" && has_val) {
      a.cam_target_y = static_cast<float>(std::atof(argv[++i]));
    } else if (arg == "--ground-y" && has_val) {
      a.ground_y = static_cast<float>(std::atof(argv[++i]));
    } else if (arg == "--max-level" && has_val) {
      a.max_level = std::atoi(argv[++i]);
      CHECK_GE(a.max_level, 0) << "--max-level 不能为负";
    } else if (arg.rfind("--", 0) == 0) {
      LOG(WARNING) << "未知参数: " << arg << "（已忽略）";
    }
  }
  CHECK(!a.shape_path.empty()) << "必须给 --shape <path.pbtxt>";
  return a;
}

// 层级 → 棕色（level 0 最深）。超过 5 层饱和，避免深树整片发白。
jpov::Color BrownForLevel(int level) {
  const float t = std::min(1.0f, static_cast<float>(level) / 5.0f);
  return jpov::Color{0.16f + 0.46f * t, 0.09f + 0.38f * t, 0.05f + 0.27f * t, 1.0f};
}

// 叶卡类型 → 4 种绿（按 type 取模）。
jpov::Color GreenForType(uint32_t type) {
  static const jpov::Color kGreens[kMaxCardTypes] = {
      {0.10f, 0.34f, 0.12f, 1.0f},
      {0.18f, 0.48f, 0.16f, 1.0f},
      {0.26f, 0.58f, 0.20f, 1.0f},
      {0.36f, 0.64f, 0.26f, 1.0f},
  };
  return kGreens[type % kMaxCardTypes];
}

// 在卡片 UV 子矩形 [u0,u1]×[v0,v1] 上放一个有厚度的盒子（世界坐标）。
// 角点徽标就用它：把 quad 的某个 UV 角缩成 1/8，**角点本身作为参照点**。
void AppendUvBox(const Shape& shape, const jpov::vegetation::LeafCard& card, double u0,
                 double u1, double v0, double v1, double thickness,
                 std::vector<float>* pos, std::vector<uint32_t>* idx) {
  const auto norm = [](const Vec3d& q) {
    const double l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    return (l < 1e-12) ? Vec3d{0, 1, 0} : Vec3d{q.x / l, q.y / l, q.z / l};
  };
  const auto cross = [](const Vec3d& a, const Vec3d& b) {
    return Vec3d{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
  };
  const Vec3d up = norm(card.up);
  const Vec3d n = norm(card.normal);
  const Vec3d right = norm(cross(up, n));

  // 与 BuildLeafCards 完全相同的 pivot：uv=(au,av) 落在 anchor 上。
  std::string mode = "bottom_center";
  for (const auto& t : shape.leaf_types) {
    if (t.type == card.type) {
      mode = t.anchor_mode;
      break;
    }
  }
  double au = 0.5;
  double av = 0.0;
  if (!jpov::vegetation::AnchorModeToUv(mode, &au, &av)) {
    std::fprintf(stderr, "AppendUvBox: anchor_mode \"%s\" 非法\n", mode.c_str());
    std::abort();
  }
  const auto P = [&](double u, double v, double z) {
    const double su = (u - au) * card.width;
    const double sv = (v - av) * card.height;
    return Vec3d{card.anchor.x + right.x * su + up.x * sv + n.x * z,
                 card.anchor.y + right.y * su + up.y * sv + n.y * z,
                 card.anchor.z + right.z * su + up.z * sv + n.z * z};
  };
  const double hz = 0.5 * thickness;
  const Vec3d c[8] = {P(u0, v0, -hz), P(u1, v0, -hz), P(u0, v1, -hz), P(u1, v1, -hz),
                      P(u0, v0, +hz), P(u1, v0, +hz), P(u0, v1, +hz), P(u1, v1, +hz)};
  static const int kFaces[12][3] = {{0, 1, 3}, {0, 3, 2}, {4, 6, 7}, {4, 7, 5},
                                    {0, 4, 5}, {0, 5, 1}, {2, 3, 7}, {2, 7, 6},
                                    {0, 2, 6}, {0, 6, 4}, {1, 5, 7}, {1, 7, 3}};
  const uint32_t base = static_cast<uint32_t>(pos->size() / 3);
  for (const Vec3d& q : c) {
    pos->push_back(static_cast<float>(q.x));
    pos->push_back(static_cast<float>(q.y));
    pos->push_back(static_cast<float>(q.z));
  }
  for (const auto& f : kFaces) {
    idx->push_back(base + f[0]);
    idx->push_back(base + f[1]);
    idx->push_back(base + f[2]);
  }
}

class VegetationV2App : public JPOV {
 public:
  using JPOV::JPOV;

  Shape shape_;
  std::vector<uint32_t> branch_mesh_;   // 与 shape_.branches 下标对齐
  std::vector<int> branch_level_;
  // 绿卡：整张 quad 的盒子（空间覆盖），debug 视图和角标一起显示/隐藏。
  uint32_t card_box_mesh_ = 0;
  jpov::PBRMaterial card_box_mat_;
  // UV 角徽标（红/黄/蓝 = uv(0,0)/(1,1)/(0,1)），每个颜色一个合并 mesh。
  uint32_t corner_mesh_[3] = {0, 0, 0};
  jpov::PBRMaterial corner_mat_[3];

  // ── 叶卡材质（--leaf）──
  bool has_leaf_ = false;
  bool show_leaf_ = true;   // 关掉 → 回到 shape debug 的绿色盒子
  float leaf_aspect_ = 1.0f;          // 单张图 = 一格 → 宽/高
  float curl_ = 0.5f;                 // A：顶点法线球面化（面板可调）
  float curl_applied_ = -1.0f;        // 上次烘进 mesh 的值（变了才 UpdateMesh）
  float leaf_normal_scale_ = 1.0f;    // B：法线贴图强度（面板可调）
  jpov::PBRMaterial leaf_mat_;
  std::vector<uint32_t> leaf_mesh_;               // 每 type 一个 mesh
  std::vector<jpov::MeshData> leaf_mesh_base_;    // 平法线基准（curl 变化时重算）
  // 逐层棕色材质缓存：每帧给 183 根枝各造一个 PBRMaterial 纯属浪费。
  std::vector<jpov::PBRMaterial> level_mats_;
  jpov_viewer::ViewConfig view_;

  bool show_panel_ = true;   // 面板只在交互窗口画（headless 出纯 3D）
  // 叶卡过滤：勾了"显示全部叶子"就全画；否则只画 type_filter_ 这一个下标。
  bool show_all_card_types_ = true;
  uint32_t type_filter_ = 0;
  char type_buf_[8] = "0";   // InputText 的编辑缓冲（跨帧持有，Ui 需要稳定地址）
  int max_level_ = 99;
  float max_level_f_ = 99.0f;   // 滑条用（int 版才是真值）
  float ground_y_ = 0.0f;       // 地面平面高度（米）
  float cam_target_y_ = 0.0f;   // 中键纵向 pan 的注视点偏移
  // 树皮材质（--bark 给了才加载）。tile 由 UV 决定 = 该枝周长，与贴图尺寸无关。
  bool has_bark_ = false;
  bool show_bark_ = true;
  float normal_scale_ = 1.5f;   // 面板可调；BuildScene 里来自 --normal-scale
  // = 贴图宽/高；交给 SweepBranches 烘进 UV（v 方向），不是 shader 里改。
  float uv_v_scale_ = 1.0f;
  jpov::PBRMaterial bark_mat_;
  uint32_t ground_mesh_ = 0;
  jpov::PBRMaterial ground_mat_;
  jpov::Ui ui_;
  std::string shape_path_;   // 只用于帮助打印（H 键）
  jpov_skylight::SkyDegrees deg_;

  std::string CamString() const {
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "cam phi=%.1f theta=%.1f R=%.2f | level<=%d types=%s",
                  view_.phi * 180.0 / 3.14159265358979323846,
                  view_.theta * 180.0 / 3.14159265358979323846, view_.R, max_level_,
                  show_all_card_types_ ? "all" : std::to_string(type_filter_).c_str());
    return std::string(buf);
  }

  // 叶卡帮助：把「造图时需要知道的东西」按类型列出来（控制台里可复制）。
  // 本 viewer 不读 atlas，所以这里给的是 shape 里声明的推荐值 + 语义解释，
  // 让造图的人知道根部该画在格子的哪个位置、宽高比该多少。
  void PrintLeafTypeHelp(const std::string& shape_path) const {
    std::printf("\n=== 叶卡帮助（造图视角）===\n");
    std::printf("shape: %s\n", shape_path.c_str());
    std::printf("%zu 枝 / %zu 卡 / %zu 种叶卡\n", shape_.branches.size(), shape_.cards.size(),
                shape_.leaf_types.size());

    for (const LeafType& t : shape_.leaf_types) {
      size_t cards_of_type = 0;
      for (const LeafCard& c : shape_.cards) {
        if (c.type == t.type) ++cards_of_type;
      }

      // anchor_mode 的「图里根部在哪」——这是造图时唯一要对齐的东西。
      double au = 0.0;
      double av = 0.0;
      const char* name = "非法 anchor_mode";
      if (jpov::vegetation::AnchorModeToUv(t.anchor_mode, &au, &av)) {
        if (t.anchor_mode == "bottom_left") name = "格子左下角";
        else if (t.anchor_mode == "bottom_center") name = "格子底边中点";
        else if (t.anchor_mode == "bottom_right") name = "格子右下角";
        else if (t.anchor_mode == "top_center") name = "格子顶边中点";
      }
      char where[96];
      if (jpov::vegetation::IsValidAnchorMode(t.anchor_mode)) {
        std::snprintf(where, sizeof(where), "%s (u=%.1f, v=%.1f)", name, au, av);
      } else {
        std::snprintf(where, sizeof(where), "%s", name);
      }

      std::printf("\n  type %u   卡数 %zu\n", t.type, cards_of_type);
      std::printf("    anchor_mode = %s\n", t.anchor_mode.c_str());
      std::printf("    根部位置   = %s\n", where);
      if (t.preferred_aspect > 0.0) {
        std::printf("    推荐宽/高  = %.3f（height=H 时 width=%.3f·H；格子宽:高 = %.3f : 1）\n",
                    t.preferred_aspect, t.preferred_aspect, t.preferred_aspect);
      } else {
        std::printf("    推荐宽/高  = 未指定（装配时取该类型第一套 atlas 的格子比例）\n");
      }
      if (!t.description.empty()) {
        std::printf("    描述       = %s\n", t.description.c_str());
      }
    }

    std::printf("\n  规则：width = height × (格子宽/格子高)，格子内**内容必须撑满宽度方向**；\n"
                "  根部按上面的 anchor_mode 对齐，上下可留白。交互时按 H 重新打印。\n\n");
    std::fflush(stdout);
  }

  // 叶卡：黑底 → alpha；由 alpha 生成法线贴图（B）；再建四边形 mesh（A 在 normals 上）。
  void BuildLeafScene(const CliArgs& args) {
    std::vector<unsigned char> file;
    CHECK(ReadFileBytes(args.leaf_path, &file)) << "读不到叶卡图: " << args.leaf_path;
    int w = 0;
    int h = 0;
    int comp = 0;
    unsigned char* px = stbi_load_from_memory(file.data(), static_cast<int>(file.size()), &w, &h,
                                              &comp, 4);
    CHECK(px != nullptr) << "解码失败: " << args.leaf_path;
    std::vector<unsigned char> rgba(px, px + static_cast<size_t>(w) * h * 4);
    stbi_image_free(px);

    // 「近乎黑 = 透明」：max(r,g,b) 在 0.5t~1.5t 之间线性过渡，避免硬边锯齿。
    const float lo = args.leaf_black * 0.5f;
    const float hi = args.leaf_black * 1.5f;
    const float span = std::max(1e-3f, hi - lo);
    std::vector<unsigned char> mask(static_cast<size_t>(w) * h);
    size_t opaque = 0;
    for (size_t i = 0; i < mask.size(); ++i) {
      const float mx = static_cast<float>(std::max(
          std::max(rgba[i * 4 + 0], rgba[i * 4 + 1]), rgba[i * 4 + 2]));
      float t = (mx - lo) / span;
      t = std::min(1.0f, std::max(0.0f, t));
      const unsigned char a = static_cast<unsigned char>(t * 255.0f + 0.5f);
      // 素材是「黑底」：观测色 = 叶片色 × 覆盖率。边缘像素直接当颜色用会发黑，
      // 所以按覆盖率反解（un-premultiply from black）：C = C_obs / a。
      // 不做这一步，缩放/mip 后叶子轮廓会有一圈黑边。
      if (t > 0.0f && t < 1.0f) {
        for (int c = 0; c < 3; ++c) {
          const float v = rgba[i * 4 + c] / t;
          rgba[i * 4 + c] = static_cast<unsigned char>(std::min(255.0f, v));
        }
      }
      rgba[i * 4 + 3] = a;
      mask[i] = a > 0 ? 1 : 0;
      if (a > 0) ++opaque;
    }
    CHECK(opaque > 0) << "叶卡图里没有非黑像素（阈值 --leaf-black " << args.leaf_black << "）";
    LOG(INFO) << "叶卡: " << args.leaf_path << " " << w << "x" << h << "，不透明 "
              << (100.0 * static_cast<double>(opaque) / static_cast<double>(mask.size())) << "%";
    // RGB 往透明区扩散：mip/双线性不会混进背景黑。
    DilateRgbIntoTransparent(mask, &rgba, w, h, 6);
    const std::vector<unsigned char> normal_rgba = BuildNormalFromAlpha(rgba, w, h, 4.0f);

    jpov::TextureOptions opts;
    opts.mipmap = true;
    opts.repeat = false;
    // 叶卡是 alpha cutout 且会被 mip 缩小：普通 glGenerateMipmap 会把 alpha 平均到
    // cutoff 以下 → 远处叶片变稀、出现「一根根细线」。用覆盖率保持的 mip 链。
    opts.alpha_coverage.enabled = true;
    opts.alpha_coverage.cutoff = 0.5f;   // 必须与 leaf_mat_.alpha_cutoff 一致
    leaf_mat_ = jpov::PBRMaterial::SolidColor({1.0f, 1.0f, 1.0f, 1.0f});
    leaf_mat_.base_color_tex =
        RegisterTextureFromMemory(EncodePng(rgba, w, h), "leaf_albedo", opts);
    leaf_mat_.normal_tex =
        RegisterTextureFromMemory(EncodePng(normal_rgba, w, h), "leaf_normal", opts);
    leaf_mat_.normal_scale = args.leaf_normal;
    leaf_mat_.alpha_mode = jpov::AlphaMode::kMask;
    leaf_mat_.alpha_cutoff = 0.5f;
    leaf_mat_.double_sided = true;
    leaf_aspect_ = static_cast<float>(w) / static_cast<float>(h);
    curl_ = args.leaf_curl;
    leaf_normal_scale_ = args.leaf_normal;
    has_leaf_ = true;

    leaf_mesh_.assign(kMaxCardTypes, 0);
    leaf_mesh_base_.assign(kMaxCardTypes, jpov::MeshData());
    for (int t = 0; t < kMaxCardTypes; ++t) {
      Shape subset = shape_;
      subset.cards.clear();
      for (const auto& c : shape_.cards) {
        if (static_cast<int>(c.type % kMaxCardTypes) == t) {
          subset.cards.push_back(c);
        }
      }
      if (subset.cards.empty()) continue;
      const MeshData src = jpov::vegetation::BuildLeafCards(subset);

      jpov::MeshData md;
      md.flags = static_cast<jpov::MeshVertexFlags>(
          static_cast<uint8_t>(jpov::MeshVertexFlags::kPosition) |
          static_cast<uint8_t>(jpov::MeshVertexFlags::kNormal) |
          static_cast<uint8_t>(jpov::MeshVertexFlags::kUV) |
          static_cast<uint8_t>(jpov::MeshVertexFlags::kTangent));
      const size_t nverts = src.positions.size() / 3;
      for (size_t v = 0; v < nverts; ++v) {
        md.positions.push_back(jpov::Vec3f{src.positions[v * 3], src.positions[v * 3 + 1],
                                           src.positions[v * 3 + 2]});
        md.normals.push_back(jpov::Vec3f{src.normals[v * 3], src.normals[v * 3 + 1],
                                         src.normals[v * 3 + 2]});
        md.uvs.push_back(jpov::Vec2f{src.uvs[v * 2], src.uvs[v * 2 + 1]});
        // 切线 = ∂P/∂u = 卡片的 right（每 4 个顶点一张卡，right 相同）。
        const size_t c0 = v - (v % 4);
        const jpov::Vec3f d = md.positions[c0 + 1] - md.positions[c0];
        const float len = std::sqrt(d.x() * d.x() + d.y() * d.y() + d.z() * d.z());
        const float inv = (len > 1e-9f) ? 1.0f / len : 0.0f;
        md.tangents.push_back(jpov::Vec3f{d.x() * inv, d.y() * inv, d.z() * inv});
      }
      md.indices = src.indices;
      leaf_mesh_base_[t] = md;

      jpov::MeshData bent = md;
      BendCardNormals(&bent, curl_);
      for (const auto& nn : bent.normals) {
      }
      leaf_mesh_[t] = RegisterMesh(bent);
    }
    curl_applied_ = curl_;
    LOG(INFO) << "叶卡材质: 宽/高 = " << leaf_aspect_ << "，curl = " << curl_
              << "，法线强度 = " << leaf_normal_scale_;
  }

  // curl 变了才重算法线（位置/UV/切线不变 → UpdateMesh 安全）。
  void ApplyCurl() {
    for (size_t t = 0; t < leaf_mesh_.size(); ++t) {
      if (leaf_mesh_[t] == 0) continue;
      jpov::MeshData bent = leaf_mesh_base_[t];
      BendCardNormals(&bent, curl_);
      UpdateMesh(leaf_mesh_[t], bent);
    }
    curl_applied_ = curl_;
  }

  void BuildScene(const CliArgs& args) {
    normal_scale_ = args.normal_scale;
    // v 方向补偿必须在生成 mesh 之前确定：UV 是扫管时烘进去的，不是 shader 里改的。
    // 0 = 自动（只对 --bark 有意义）：没有 bark 时必须落回 1.0，
    // 否则 SweepBranches 会因 uv_v_scale<=0 直接 abort。
    uv_v_scale_ = (args.uv_vscale > 0.0f) ? args.uv_vscale : 1.0f;
    if (!args.bark_dir.empty() && args.uv_vscale <= 0.0f) {
      int tw = 0, th = 0;
      const std::string albedo = args.bark_dir + "/albedo.png";
      if (ReadPngSize(albedo, &tw, &th)) {
        uv_v_scale_ = static_cast<float>(tw) / static_cast<float>(th);
        LOG(INFO) << "UV v 缩放 = " << uv_v_scale_ << "（贴图 " << tw << "x" << th
                  << "，v *= 宽/高）";
      } else {
        uv_v_scale_ = 1.0f;
        LOG(WARNING) << "读不到 albedo 像素尺寸，uv_v_scale 取 1.0: " << albedo;
      }
    }
    branch_mesh_.resize(shape_.branches.size());
    branch_level_.resize(shape_.branches.size());

    // 逐枝 mesh：便于逐枝上色 + 逐枝隐藏
    jpov::vegetation::SweepOptions sweep_opts;
    sweep_opts.uv_v_scale = uv_v_scale_;
    const MeshData whole = jpov::vegetation::SweepBranches(shape_, sweep_opts);
    const std::vector<MeshData> parts = jpov::vegetation::SplitMeshByBranch(shape_, whole);
    CHECK_EQ(parts.size(), shape_.branches.size());
    for (size_t i = 0; i < parts.size(); ++i) {
      jpov::MeshData md;
      // 带上 UV：贴树皮时 PBR 完整版 shader 需要 UV，否则采样不到（mesh 无 kUV 会退回无贴图路径）。
      md.flags = static_cast<jpov::MeshVertexFlags>(
          static_cast<uint8_t>(jpov::MeshVertexFlags::kPosition) |
          static_cast<uint8_t>(jpov::MeshVertexFlags::kNormal) |
          static_cast<uint8_t>(jpov::MeshVertexFlags::kUV) |
          static_cast<uint8_t>(jpov::MeshVertexFlags::kTangent));
      for (size_t v = 0; v < parts[i].positions.size() / 3; ++v) {
        md.positions.push_back(jpov::Vec3f{parts[i].positions[v * 3],
                                           parts[i].positions[v * 3 + 1],
                                           parts[i].positions[v * 3 + 2]});
        md.normals.push_back(jpov::Vec3f{parts[i].normals[v * 3],
                                         parts[i].normals[v * 3 + 1],
                                         parts[i].normals[v * 3 + 2]});
        md.uvs.push_back(jpov::Vec2f{parts[i].uvs[v * 2], parts[i].uvs[v * 2 + 1]});
        md.tangents.push_back(jpov::Vec3f{parts[i].tangents[v * 3],
                                          parts[i].tangents[v * 3 + 1],
                                          parts[i].tangents[v * 3 + 2]});
      }
      md.indices = parts[i].indices;
      branch_mesh_[i] = RegisterMesh(std::move(md));
      branch_level_[i] = static_cast<int>(shape_.branches[i].level);
    }

    if (!args.leaf_path.empty()) {
      BuildLeafScene(args);
    }

    // 绿卡：整张 quad 的盒子（厚度只为"看得见"，不代表卡片有厚度）。
    // 与角标共用同一套 pivot / 同一份 width，保证四者永远对齐。
    {
      std::vector<float> cpos;
      std::vector<uint32_t> cidx;
      for (const auto& card : shape_.cards) {
        AppendUvBox(shape_, card, 0.0, 1.0, 0.0, 1.0, 0.02, &cpos, &cidx);
      }
      if (!cpos.empty()) {
        jpov::MeshData md;
        md.flags = static_cast<jpov::MeshVertexFlags>(
            static_cast<uint8_t>(jpov::MeshVertexFlags::kPosition) |
            static_cast<uint8_t>(jpov::MeshVertexFlags::kNormal));
        for (size_t v = 0; v + 2 < cpos.size(); v += 3) {
          md.positions.push_back(jpov::Vec3f{cpos[v], cpos[v + 1], cpos[v + 2]});
          md.normals.push_back(jpov::Vec3f{0.0f, 1.0f, 0.0f});
        }
        md.indices = cidx;
        card_box_mesh_ = RegisterMesh(std::move(md));
        card_box_mat_ = jpov::PBRMaterial::SolidColor({0.18f, 0.45f, 0.16f, 1.0f});
        card_box_mat_.double_sided = true;
      }
    }

    // 角点徽标：把 quad 的三个 UV 角各缩成 1/8 的小盒子（厚 3cm），角点作为参照点。
    //   红 = uv(0,0)   黄 = uv(1,1)   蓝 = uv(0,1)
    // 这样用户一眼就知道素材的四个角该往哪贴；盒子厚度只为"看得见"，不代表卡片有厚度。
    {
      const double kBadge = 1.0 / 8.0;
      const double kThick = 0.03;
      const double uv[3][4] = {{0.0, kBadge, 0.0, kBadge},
                               {1.0 - kBadge, 1.0, 1.0 - kBadge, 1.0},
                               {0.0, kBadge, 1.0 - kBadge, 1.0}};
      // 红别给满：1.0 经 tone map 会压成粉（255,111,111），0.55 反而更像正红。
      const jpov::Color kCol[3] = {{0.55f, 0.0f, 0.0f, 1.0f},
                                   {0.95f, 0.85f, 0.05f, 1.0f},
                                   {0.05f, 0.15f, 0.85f, 1.0f}};
      for (int c = 0; c < 3; ++c) {
        std::vector<float> cpos;
        std::vector<uint32_t> cidx;
        for (const auto& card : shape_.cards) {
          AppendUvBox(shape_, card, uv[c][0], uv[c][1], uv[c][2], uv[c][3], kThick, &cpos,
                      &cidx);
        }
        if (cpos.empty()) continue;
        jpov::MeshData md;
        md.flags = static_cast<jpov::MeshVertexFlags>(
            static_cast<uint8_t>(jpov::MeshVertexFlags::kPosition) |
            static_cast<uint8_t>(jpov::MeshVertexFlags::kNormal));
        for (size_t v = 0; v + 2 < cpos.size(); v += 3) {
          md.positions.push_back(jpov::Vec3f{cpos[v], cpos[v + 1], cpos[v + 2]});
          md.normals.push_back(jpov::Vec3f{0.0f, 1.0f, 0.0f});
        }
        md.indices = cidx;
        corner_mesh_[c] = RegisterMesh(std::move(md));
        // base 黑 + emissive：靠自发光上色，不被 sun/ambient 冲淡。
        corner_mat_[c] = jpov::PBRMaterial::SolidColor({0.0f, 0.0f, 0.0f, 1.0f});
        corner_mat_[c].emissive = kCol[c];
        corner_mat_[c].roughness = 1.0f;
      }
    }

    // 相机：按包围盒自动取景
    float bmin[3] = {1e9f, 1e9f, 1e9f};
    float bmax[3] = {-1e9f, -1e9f, -1e9f};
    for (const auto& b : shape_.branches) {
      for (const auto& s : b.samples) {
        const float p[3] = {static_cast<float>(s.position.x), static_cast<float>(s.position.y),
                            static_cast<float>(s.position.z)};
        for (int k = 0; k < 3; ++k) {
          bmin[k] = std::min(bmin[k], p[k]);
          bmax[k] = std::max(bmax[k], p[k]);
        }
      }
    }
    view_ = jpov_viewer::DefaultView();
    view_.R = jpov_viewer::ViewConfig::FitRadius(bmin, bmax, 60.0);
    view_.phi = args.phi_deg * 3.14159265358979323846 / 180.0;
    view_.theta = args.theta_deg * 3.14159265358979323846 / 180.0;
    if (args.radius > 0.0) {
      view_.R = args.radius;
    }
    deg_.sun_elev_deg = 55.0f;
    deg_.sun_azim_deg = 35.0f;

    // 6 档棕色（level 0..5，>=5 饱和）
    level_mats_.clear();
    for (int lv = 0; lv <= 5; ++lv) {
      jpov::PBRMaterial m = jpov::PBRMaterial::SolidColor(BrownForLevel(lv));
      m.double_sided = true;
      level_mats_.push_back(m);
    }

    if (!args.bark_dir.empty()) {
      // 材质目录约定（make_bark_material.sh 的产物）：albedo/normal/roughness.png
      const std::string albedo = args.bark_dir + "/albedo.png";
      const std::string normal = args.bark_dir + "/normal.png";
      const std::string rough = args.bark_dir + "/roughness.png";
      jpov::TextureOptions opts;
      opts.mipmap = true;   // 树干是透视大平面，不生成 mip 会摩尔纹
      opts.repeat = true;   // UV 按周长循环：v 会远超 1
      bark_mat_ = jpov::PBRMaterial::SolidColor({1.0f, 1.0f, 1.0f, 1.0f});
      bark_mat_.base_color_tex = RegisterTexture(albedo, opts);
      bark_mat_.normal_tex = RegisterTexture(normal, opts);
      bark_mat_.has_roughness_tex = true;
      bark_mat_.roughness_tex = RegisterTexture(rough, opts);
      bark_mat_.normal_scale = normal_scale_;
      bark_mat_.double_sided = true;
      has_bark_ = true;
      LOG(INFO) << "树皮材质: " << args.bark_dir;
    }

    ground_mesh_ = RegisterMesh(jpov_skylight::MakeGroundQuad());
    ground_mat_ = jpov_skylight::GroundMaterial();

    show_panel_ = !args.capture || args.show_panel;   // headless 默认不画面板
    int deepest = 0;
    for (const auto& b : shape_.branches) {
      deepest = std::max(deepest, static_cast<int>(b.level));
    }
    max_level_ = std::min(args.max_level, deepest);
    max_level_f_ = static_cast<float>(max_level_);
    ground_y_ = args.ground_y;
    if (args.hide_leaf) show_leaf_ = false;
    if (args.hide_bark) show_bark_ = false;
    cam_target_y_ = args.cam_target_y;
    LOG(INFO) << "shape: " << shape_.branches.size() << " 枝 / " << shape_.cards.size()
              << " 卡 | " << CamString();
  }

  void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                    const jpov::WindowInfo& winfo,
                    jpov::RenderCommandList* cmds) override {
    (void)frame_count;
    // 面板滑条改的是「呈现」参数：curl 要重烘顶点法线，法线强度是材质常量。
    if (has_leaf_) {
      if (curl_ != curl_applied_) {
        ApplyCurl();
      }
      leaf_mat_.normal_scale = leaf_normal_scale_;
    }
    // 交互：[ / ] 调层级、P 打印相机
    if (input.GetKey(jpov::KeyCode::LeftBracket).IsClick()) {
      max_level_ = std::max(0, max_level_ - 1);
      max_level_f_ = static_cast<float>(max_level_);  // 同步滑条，否则面板每帧覆盖回去
      LOG(INFO) << "max_level = " << max_level_;
    }
    if (input.GetKey(jpov::KeyCode::RightBracket).IsClick()) {
      max_level_ = std::min(99, max_level_ + 1);
      max_level_f_ = static_cast<float>(max_level_);
      LOG(INFO) << "max_level = " << max_level_;
    }
    if (input.GetKey(jpov::KeyCode::P).IsClick()) {
      LOG(INFO) << "[camera] " << CamString();
    }
    if (input.GetKey(jpov::KeyCode::H).IsClick()) {
      PrintLeafTypeHelp(shape_path_);
    }

    float dx = 0.0f;
    float dy = 0.0f;
    if (input.right.IsDrag()) {
      dx = input.mouse_dx;
      dy = input.mouse_dy;
    }
    jpov_viewer::ApplyInput(&view_, dx, dy, input.scroll_delta,
                            static_cast<int>(winfo.width), static_cast<int>(winfo.height));
    // 中键纵向 drag = 平移注视点（相机位置与 target 同步平移，朝向不变）。
    // 与 clothing tool 同一套手感：半屏 = 1m（jpov_viewer::VerticalPanDeltaY）。
    if (input.middle.IsDrag()) {
      cam_target_y_ += jpov_viewer::VerticalPanDeltaY(input.mouse_dy, winfo.height);
    }

    cmds->camera.fbo_3d_width_ = kViewerWidth;
    cmds->camera.fbo_3d_height_ = kViewerHeight;
    // 相机整体跟随中键 pan 的 Y 偏移（position 与 target 同加，朝向不变）
    const jpov::Vec3f cam_pos = view_.Position();
    cmds->camera.position = {cam_pos.x(), cam_pos.y() + cam_target_y_, cam_pos.z()};
    cmds->camera.target = {0.0f, cam_target_y_, 0.0f};
    cmds->camera.up = {0.0f, 1.0f, 0.0f};
    cmds->camera.fov = 60.0f;
    cmds->camera.near = 0.02f;
    cmds->camera.far = 500.0f;

    const jpov_skylight::SkyLighting nl = jpov_skylight::MakeSkyLighting(deg_, false);
    cmds->sky = nl.sky;
    cmds->sun = nl.dir_light;
    cmds->ambient = nl.ambient;
    cmds->tone_mapping = true;

    // 地面：高度可调（默认 0，与树根同面）
    cmds->DrawObject3D(ground_mesh_, ground_mat_, {0.0f, ground_y_, 0.0f}, {0.0f, 1.0f, 0.0f},
                       {0.0f, 0.0f, 1.0f});

    for (size_t i = 0; i < branch_mesh_.size(); ++i) {
      if (branch_level_[i] > max_level_) {
        continue;
      }
      if (has_bark_ && show_bark_) {
        cmds->DrawObject3D(branch_mesh_[i], bark_mat_, {0.0f, 0.0f, 0.0f},
                           {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
        continue;
      }
      const int lv = std::min(branch_level_[i], 5);
      cmds->DrawObject3D(branch_mesh_[i], level_mats_[lv], {0.0f, 0.0f, 0.0f},
                         {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
    }
    const bool debug_cards = !(has_leaf_ && show_leaf_);
    if (debug_cards) {
      if (card_box_mesh_ != 0) {
        cmds->DrawObject3D(card_box_mesh_, card_box_mat_, {0.0f, 0.0f, 0.0f},
                           {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
      }
      for (int c = 0; c < 3; ++c) {
        if (corner_mesh_[c] == 0) continue;
        cmds->DrawObject3D(corner_mesh_[c], corner_mat_[c], {0.0f, 0.0f, 0.0f},
                           {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
      }
    } else {
      for (size_t t = 0; t < leaf_mesh_.size(); ++t) {
        if (leaf_mesh_[t] == 0) continue;
        if (!show_all_card_types_ && t != type_filter_) continue;
        cmds->DrawObject3D(leaf_mesh_[t], leaf_mat_, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
                           {0.0f, 0.0f, 1.0f});
      }
    }
    cmds->DrawText(CamString(), {8.0f, 8.0f}, 18.0f, jpov::Color{1.0f, 1.0f, 1.0f, 1.0f},
                   jpov::TextAlignment::kTopLeft, jpov::kFontBuiltinCJK);

    // 交互面板：地面高度 / 最大层级 / 叶卡开关。
    if (show_panel_) {
      const float w = static_cast<float>(kViewerWidth);
      const float h = static_cast<float>(kViewerHeight);
      jpov::UiTheme theme = jpov::UiTheme::Default(16.0f);
      theme.font_alias = jpov::kFontBuiltinCJK;

      // 布局：两组面板都贴左下角，下面是【场景】，上面是【叶卡过滤】。
      const float colw = 0.34f * w;   // 0.26w 装不下"地面 y (m) + 数值"，标签会压在滑轨上
      const float cx = 0.02f * w;
      const int scene_rows = has_bark_ ? 4 : 3;   // 地面/层级/树皮/法线强度
      const float scene_top = h - 16.0f - (scene_rows * 32.0f);
      const int leaf_rows = has_leaf_ ? 5 : 2;   // 全部叶子/type +（贴图/球面化/法线强度）
      const float leaf_top = scene_top - 12.0f - (leaf_rows * 32.0f);
      auto scene_row = [&](int i) {
        return jpov::UiRect{{cx, scene_top + i * 32.0f}, {colw, 24.0f}};
      };
      auto leaf_row = [&](int i) {
        return jpov::UiRect{{cx, leaf_top + i * 32.0f}, {colw, 24.0f}};
      };

      // 黑底：两组控件压在场景上，浅色地面时白字会糊掉。
      // 直接推 fillrect2d（在 Emit 之前 → 在向量里更早 → 画在控件下面）。
      const float panel_top = leaf_top - 10.0f;
      const float panel_bottom = scene_top + (scene_rows - 1) * 32.0f + 24.0f + 10.0f;
      // 必须走 DrawFillRect：它会同时登记到 cmds->order（渲染器按 order 遍历、
      // 再用下标回查 fillrect2d）。直接 push fillrect2d 不进 order → 根本不会被画。
      cmds->DrawFillRect({cx - 10.0f, panel_top}, {colw + 20.0f, panel_bottom - panel_top},
                         jpov::Color{0.0f, 0.0f, 0.0f, 0.55f},
                         jpov::Color{0.0f, 0.0f, 0.0f, 0.0f}, 0.0f, 6.0f);

      ui_.Begin(input, theme, w, h, 1000.0f / kViewerFps);

      ui_.SliderFloat("地面 y (m)", &ground_y_, scene_row(0), -3.0f, 3.0f, 2);
      ui_.SliderFloat("最大层级", &max_level_f_, scene_row(1), 0.0f, 20.0f, 0);
      if (has_bark_) {
        ui_.Checkbox("树皮纹理", &show_bark_, scene_row(2));
        ui_.SliderFloat("法线强度", &normal_scale_, scene_row(3), 0.0f, 3.0f, 2);
        bark_mat_.normal_scale = normal_scale_;
      }

      ui_.Checkbox("显示全部叶子", &show_all_card_types_, leaf_row(0));
      if (has_leaf_) {
        ui_.Checkbox("叶卡贴图", &show_leaf_, leaf_row(2));
        ui_.SliderFloat("叶法线球面化", &curl_, leaf_row(3), 0.0f, 2.0f, 2);
        ui_.SliderFloat("叶法线强度", &leaf_normal_scale_, leaf_row(4), 0.0f, 3.0f, 2);
      }
      // 注：本工具只有 4 个颜色桶，卡片的 type 按 `type % kMaxCardTypes` 归桶，
      // 所以这个下标是【颜色桶下标】，不是原始 type 编号。要按真实 type 过滤，
      // 得改成逐 type 一个 mesh（面数索引翻 4 倍，暂不做）。
      // InputText 编辑的是字符串，这里解析成下标并 clamp 到合法范围——
      // 越界值不静默忽略，而是夹紧，避免"输入 9 却什么都没有"的黑箱。
      // Ui::InputText 自带的标签在 24px 行高里不显示（实测只剩编辑框），
      // 所以给它空标签、自己画左侧文字，并把编辑框往右让出标签宽度。
      const float label_w = 0.11f * w;
      const jpov::UiRect row1 = leaf_row(1);
      const jpov::UiRect type_box{{cx + label_w, row1.pos.y()}, {colw - label_w, 24.0f}};
      cmds->DrawText("叶 type 下标", {cx, row1.pos.y() + 4.0f}, 16.0f,
                     jpov::Color{0.95f, 0.95f, 0.95f, 1.0f},
                     jpov::TextAlignment::kTopLeft, jpov::kFontBuiltinCJK);
      if (ui_.InputText("", type_buf_, sizeof(type_buf_), type_box)) {
        const int parsed = std::atoi(type_buf_);
        const int clamped = std::max(0, std::min(kMaxCardTypes - 1, parsed));
        type_filter_ = static_cast<uint32_t>(clamped);
        if (clamped != parsed) {
          std::snprintf(type_buf_, sizeof(type_buf_), "%d", clamped);
        }
      }
      ui_.End();
      ui_.Emit(cmds);
      max_level_ = static_cast<int>(max_level_f_ + 0.5f);
    }
  }
};

bool LoadShape(const std::string& path, Shape* out) {
  std::ifstream in(path);
  if (!in) {
    LOG(ERROR) << "打不开 shape 文件: " << path;
    return false;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  jpov::vegetation::ShapeProto proto;
  if (!google::protobuf::TextFormat::ParseFromString(ss.str(), &proto)) {
    LOG(ERROR) << "解析 ShapeProto 文本失败（proto/shape.proto 的字段名对不上？）: " << path;
    return false;
  }
  *out = jpov::vegetation::FromProto(proto);
  std::string err;
  if (!jpov::vegetation::Validate(*out, &err)) {
    LOG(ERROR) << "shape 校验失败: " << err;
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const CliArgs args = ParseCli(argc, argv);

  JPOV::Config cfg;
  cfg.title = "JPOV - vegetation v2 viewer";
  cfg.width = kViewerWidth;
  cfg.height = kViewerHeight;
  cfg.headless = args.capture;
  cfg.fonts = {
      {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
      {"fonts/DejaVuSans.ttf", 0, jpov::kFontBuiltinLatin},
  };

  VegetationV2App app(cfg);
  app.Init();
  if (!LoadShape(args.shape_path, &app.shape_)) {
    return 1;
  }
  app.shape_path_ = args.shape_path;
  app.PrintLeafTypeHelp(args.shape_path);
  app.BuildScene(args);

  if (!args.capture) {
    app.Run();
    app.Finalize();
    return 0;
  }

  // headless：先热身几帧（字体/材质上传），再按三个角度各拍一张
  jpov::WindowInfo winfo;
  winfo.width = kViewerWidth;
  winfo.height = kViewerHeight;
  jpov::InputSnapshot input{};
  for (int i = 0; i < 3; ++i) {
    app.RunOnce(input, winfo, "/tmp/_veg_v2_warm.png");
  }
  const std::string dir = args.out_dir.empty() ? "/tmp" : args.out_dir;
  const std::pair<const char*, double> shots[] = {
      {"front", 0.0}, {"three_quarter", 45.0}, {"side", 90.0}};
  for (const auto& s : shots) {
    app.view_.theta = s.second * 3.14159265358979323846 / 180.0;
    const std::string path = dir + "/veg_v2_" + s.first + ".png";
    LOG(INFO) << "[camera] " << app.CamString();
    app.RunOnce(input, winfo, path.c_str());
    LOG(INFO) << "capture: " << path;
  }
  app.Finalize();
  return 0;
}
