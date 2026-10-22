// 植被 v2 查看器 —— **场景 + 渲染**部分（与 CLI 壳分离，gold test / generator 复用同一份）。
//
// 这里只做「给我 shape / bark / leaf，我把植物画出来」：
//   * 树干：按格式定义逐枝扫管，每根枝一个 mesh（可逐枝上色/逐枝隐藏）
//   * 叶卡：按 anchor + up + normal + width/height 铺 quad（UV 只由 anchor_mode 推导）
//   * 树皮 / 叶材质、UV 尺度、法线、球面化（curl）、面板、交互都在这里
// CLI 解析与 main 在 vegetation_v2_viewer.cc。

#ifndef JPOV_TOOLS_VEGETATION_TOOL_V2_VIEWER_VEGETATION_V2_APP_H_
#define JPOV_TOOLS_VEGETATION_TOOL_V2_VIEWER_VEGETATION_V2_APP_H_

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

namespace vegv2 {


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

// 上下翻转 RGBA 行。
// stb_image 解码后第 0 行是**图像顶部**，而 OpenGL 的 v=0 取第 0 行 ⇒ 直接上传时
// v=0 = 图顶。Blender / glTF 的约定是 v=0 = 图**底**，用户按"根部画在图片底部"做的素材
// 一旦直传就会整张上下颠倒。所以叶卡贴图在上传前翻一次行，让 uv(0,0) = 图左下。
void FlipRows(std::vector<unsigned char>* rgba, int w, int h) {
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
  int only_type = -1;          // >=0：只显示该 leaf type（等价于面板取消"显示全部叶子"）
  bool hide_leaf = false;      // 强制看 debug 盒子（叶卡贴图关）
  bool hide_bark = false;      // 强制看 level 棕色（树皮关）
  bool hide_hud = false;       // 关掉左上角相机 HUD（gold 比对用，免得文字进基准图）
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
  std::string leaf2_path;   // leaf type 1（镜像手性）用的素材；空 = 回退 leaf_path
  float leaf_black = 12.0f;   // max(r,g,b) < 此值 → alpha 0（带 0.5x~1.5x 软过渡）
  float leaf_curl = 0.5f;     // A：顶点法线球面化强度
  float leaf_normal = 1.0f;   // B：法线贴图强度（uNormalScale）
};

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
// 角的**参照点**是该 UV 子矩形的角：缩小时角点不动。
// 盒子本身用引擎里久经测试的 MeshData::MakeOrientedBox（24 顶点 / 12 三角形 /
// 每面独立法线），不再手写 8 顶点索引表 —— 手写版会在某些朝向下缺面。
void AppendUvBox(const Shape& shape, const jpov::vegetation::LeafCard& card, double u0,
                 double u1, double v0, double v1, double thickness,
                 std::vector<float>* pos, std::vector<float>* nrm,
                 std::vector<uint32_t>* idx) {
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

  // 子矩形中心（相对 anchor）与半尺寸。
  const double cu = 0.5 * (u0 + u1) - au;
  const double cv = 0.5 * (v0 + v1) - av;
  const jpov::Vec3f center{static_cast<float>(card.anchor.x + right.x * (cu * card.width) +
                                              up.x * (cv * card.height)),
                           static_cast<float>(card.anchor.y + right.y * (cu * card.width) +
                                              up.y * (cv * card.height)),
                           static_cast<float>(card.anchor.z + right.z * (cu * card.width) +
                                              up.z * (cv * card.height))};
  // MakeOrientedBox：local +Z = front（= 卡面法线）、+Y = up、+X = left(= cross(up,front) = right)。
  const jpov::MeshData box = jpov::MeshData::MakeOrientedBox(
      0.5f * static_cast<float>(thickness),
      0.5f * static_cast<float>((v1 - v0) * card.height),
      0.5f * static_cast<float>((u1 - u0) * card.width),
      jpov::Vec3f{static_cast<float>(up.x), static_cast<float>(up.y), static_cast<float>(up.z)},
      jpov::Vec3f{static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z)},
      center);
  CHECK(box.positions.size() == box.normals.size());

  const uint32_t base = static_cast<uint32_t>(pos->size() / 3);
  for (size_t v = 0; v < box.positions.size(); ++v) {
    pos->push_back(box.positions[v].x());
    pos->push_back(box.positions[v].y());
    pos->push_back(box.positions[v].z());
    nrm->push_back(box.normals[v].x());
    nrm->push_back(box.normals[v].y());
    nrm->push_back(box.normals[v].z());
  }
  for (uint32_t i : box.indices) {
    idx->push_back(base + i);
  }
}

class VegetationV2App : public JPOV {
 public:
  using JPOV::JPOV;

  Shape shape_;
  std::vector<uint32_t> branch_mesh_;   // 与 shape_.branches 下标对齐
  std::vector<int> branch_level_;
  // 绿卡：整张 quad 的盒子（空间覆盖），**每个 leaf type 一个 mesh**，
  // 与同 type 的标签一起画/一起隐藏（type 过滤对两者同时生效）。
  std::vector<uint32_t> debug_card_mesh_;
  jpov::PBRMaterial card_box_mat_;

  // ── 叶卡材质（--leaf）──
  bool has_leaf_ = false;
  bool show_leaf_ = true;   // 关掉 → 回到 shape debug 的绿色盒子
  bool show_labels_ = true; // debug 视图里的 UV 文字标签
  float leaf_aspect_ = 1.0f;          // 单张图 = 一格 → 宽/高
  float curl_ = 0.5f;                 // A：顶点法线球面化（面板可调）
  float curl_applied_ = -1.0f;        // 上次烘进 mesh 的值（变了才 UpdateMesh）
  float leaf_normal_scale_ = 1.0f;    // B：法线贴图强度（面板可调）
  float leaf_black_ = 12.0f;
  std::vector<jpov::PBRMaterial> leaf_mat_;
  std::vector<uint32_t> leaf_mesh_;               // 每 type 一个 mesh
  std::vector<jpov::MeshData> leaf_mesh_base_;    // 平法线基准（curl 变化时重算）
  // 逐层棕色材质缓存：每帧给 183 根枝各造一个 PBRMaterial 纯属浪费。
  std::vector<jpov::PBRMaterial> level_mats_;
  jpov_viewer::ViewConfig view_;

  bool show_panel_ = true;   // 面板只在交互窗口画（headless 出纯 3D）
  bool show_hud_ = true;     // 左上角相机 HUD（要 CJK 字体；没字体时 BuildScene 会关掉）
  bool has_fonts_ = true;    // 有没有注册字体图集（没有就不画任何文字）
  // 叶卡过滤：勾了"显示全部叶子"就全画；否则只画 type_filter_ 这一个下标。
  bool show_all_card_types_ = true;
  uint32_t type_filter_ = 0;
  size_t leaf_type_count_ = 1;   // shape 里实际存在的 leaf type 数（用于循环/下标夹紧）
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
  // 一张叶卡贴图 → PBR 材质（黑底转 alpha、由 alpha+luma 生成法线、覆盖率 mip）。
  // key 用路径本身：同路径重复调用会被 TextureManager 去重。
  void LoadLeafMaterial(const std::string& path, jpov::PBRMaterial* out) {
    std::vector<unsigned char> file;
    CHECK(ReadFileBytes(path, &file)) << "读不到叶卡图: " << path;
    int w = 0;
    int h = 0;
    int comp = 0;
    unsigned char* px = stbi_load_from_memory(file.data(), static_cast<int>(file.size()), &w, &h,
                                              &comp, 4);
    CHECK(px != nullptr) << "解码失败: " << path;
    std::vector<unsigned char> rgba(px, px + static_cast<size_t>(w) * h * 4);
    stbi_image_free(px);

    // 「近乎黑 = 透明」：max(r,g,b) 在 0.5t~1.5t 之间线性过渡。
    const float lo = leaf_black_ * 0.5f;
    const float hi = leaf_black_ * 1.5f;
    const float span = std::max(1e-3f, hi - lo);
    std::vector<unsigned char> mask(static_cast<size_t>(w) * h);
    size_t opaque = 0;
    for (size_t i = 0; i < mask.size(); ++i) {
      const float mx = static_cast<float>(std::max(
          std::max(rgba[i * 4 + 0], rgba[i * 4 + 1]), rgba[i * 4 + 2]));
      float t = (mx - lo) / span;
      t = std::min(1.0f, std::max(0.0f, t));
      const unsigned char a = static_cast<unsigned char>(t * 255.0f + 0.5f);
      // 黑底素材：观测色 = 叶片色 × 覆盖率，边缘要按覆盖率反解，否则发黑。
      if (t > 0.0f && t < 1.0f) {
        for (int c = 0; c < 3; ++c) {
          rgba[i * 4 + c] =
              static_cast<unsigned char>(std::min(255.0f, rgba[i * 4 + c] / t));
        }
      }
      rgba[i * 4 + 3] = a;
      mask[i] = a > 0 ? 1 : 0;
      if (a > 0) ++opaque;
    }
    CHECK(opaque > 0) << "叶卡图里没有非黑像素（阈值 --leaf-black " << leaf_black_ << "）: "
                      << path;
    LOG(INFO) << "叶卡: " << path << " " << w << "x" << h << "，不透明 "
              << (100.0 * static_cast<double>(opaque) / static_cast<double>(mask.size())) << "%";
    DilateRgbIntoTransparent(mask, &rgba, w, h, 6);
    std::vector<unsigned char> normal_rgba = BuildNormalFromAlpha(rgba, w, h, 4.0f);
    // albedo 与法线一起翻行：v=0 = 图底（Blender/glTF 约定）。
    FlipRows(&rgba, w, h);
    FlipRows(&normal_rgba, w, h);

    jpov::TextureOptions opts;
    opts.mipmap = true;
    opts.repeat = false;
    opts.alpha_coverage.enabled = true;
    opts.alpha_coverage.cutoff = 0.5f;
    *out = jpov::PBRMaterial::SolidColor({1.0f, 1.0f, 1.0f, 1.0f});
    out->base_color_tex =
        RegisterTextureFromMemory(EncodePng(rgba, w, h), "leaf_albedo:" + path, opts);
    out->normal_tex =
        RegisterTextureFromMemory(EncodePng(normal_rgba, w, h), "leaf_normal:" + path, opts);
    out->normal_scale = leaf_normal_scale_;
    out->alpha_mode = jpov::AlphaMode::kMask;
    out->alpha_cutoff = 0.5f;
    out->double_sided = true;
  }

  // 叶卡：每个 leaf type 一份材质（type 0 用 --leaf，type 1 用 --leaf2，缺省回退）。
  void BuildLeafScene(const CliArgs& args) {
    leaf_black_ = args.leaf_black;
    leaf_normal_scale_ = args.leaf_normal;
    curl_ = args.leaf_curl;
    has_leaf_ = true;

    leaf_mat_.assign(kMaxCardTypes, jpov::PBRMaterial());
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

      // type 1 = 镜像手性。少给 --leaf2 时会回退到 --leaf，两种 type 用同一张图，
      // 手性就白拆了（叶子会朝同一边）——这不是错误数据，是漏参数，所以只警告不中止。
      if (t == 1 && args.leaf2_path.empty()) {
        LOG(WARNING) << "shape 有 type 1（镜像手性），但没给 --leaf2：将回退用 --leaf，"
                        "两种 type 的叶子朝向会一样";
      }
      const std::string path =
          (t == 1 && !args.leaf2_path.empty()) ? args.leaf2_path : args.leaf_path;
      LoadLeafMaterial(path, &leaf_mat_[t]);
      LOG(INFO) << "leaf type " << t << " ← " << path << "（" << subset.cards.size() << " 卡）";

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
      leaf_mesh_[t] = RegisterMesh(bent);
    }
    curl_applied_ = curl_;
    LOG(INFO) << "叶卡材质就绪：curl = " << curl_ << "，法线强度 = " << leaf_normal_scale_;
  }

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

    // 绿卡：每个 leaf type 一份盒子 mesh（同一 type 的绿卡 + 标签由同一个函数画）。
    debug_card_mesh_.assign(kMaxCardTypes, 0);
    card_box_mat_ = jpov::PBRMaterial::SolidColor({0.18f, 0.45f, 0.16f, 1.0f});
    card_box_mat_.double_sided = true;
    for (int t = 0; t < kMaxCardTypes; ++t) {
      std::vector<float> cpos;
      std::vector<float> cnrm;
      std::vector<uint32_t> cidx;
      for (const auto& card : shape_.cards) {
        if (static_cast<int>(card.type % kMaxCardTypes) != t) continue;
        // 绿卡做得很薄（4mm）：与 8cm 的角标不会抢深度。
        AppendUvBox(shape_, card, 0.0, 1.0, 0.0, 1.0, 0.004, &cpos, &cnrm, &cidx);
      }
      if (cpos.empty()) continue;
      jpov::MeshData md;
      md.flags = static_cast<jpov::MeshVertexFlags>(
          static_cast<uint8_t>(jpov::MeshVertexFlags::kPosition) |
          static_cast<uint8_t>(jpov::MeshVertexFlags::kNormal));
      for (size_t v = 0; v + 2 < cpos.size(); v += 3) {
        md.positions.push_back(jpov::Vec3f{cpos[v], cpos[v + 1], cpos[v + 2]});
        md.normals.push_back(jpov::Vec3f{cnrm[v], cnrm[v + 1], cnrm[v + 2]});
      }
      md.indices = cidx;
      debug_card_mesh_[t] = RegisterMesh(std::move(md));
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
    leaf_type_count_ = 1;
    for (const auto& c : shape_.cards) {
      leaf_type_count_ = std::max(leaf_type_count_,
                                  static_cast<size_t>(c.type % kMaxCardTypes) + 1);
    }
    max_level_ = std::min(args.max_level, deepest);
    max_level_f_ = static_cast<float>(max_level_);
    ground_y_ = args.ground_y;
    if (args.only_type >= 0) {
      show_all_card_types_ = false;
      type_filter_ = static_cast<uint32_t>(args.only_type);
      if (type_filter_ >= leaf_type_count_) {
        LOG(WARNING) << "--only-type " << args.only_type << " 超出实际 type 数 "
                     << leaf_type_count_ << "，夹到 " << (leaf_type_count_ - 1);
        type_filter_ = static_cast<uint32_t>(leaf_type_count_ - 1);
      }
    }
    if (args.hide_leaf) show_leaf_ = false;
    if (args.hide_bark) show_bark_ = false;
    if (args.hide_hud) show_hud_ = false;
    cam_target_y_ = args.cam_target_y;
    // 没有字体图集时画文字会 CHECK 崩（font_renderer 只跳过 2D，3D 标签仍会查 alias）。
    // 统一在这里关掉：headless 出图 / CI sandbox 都不需要文字。
    if (!has_fonts_) {
      show_hud_ = false;
      show_labels_ = false;
    }
    LOG(INFO) << "shape: " << shape_.branches.size() << " 枝 / " << shape_.cards.size()
              << " 卡 | " << CamString();
  }

  // 一个 leaf type 的 debug 绘制单元：绿卡 + 卡面 UV 标签。
  // 两者用同一个 type 下标、同一份 shape 数据，所以永远不会"绿卡画了、标签没画"。
  void DrawLeafDebugForType(size_t type_index, jpov::RenderCommandList* cmds) {
    if (type_index < debug_card_mesh_.size() && debug_card_mesh_[type_index] != 0) {
      cmds->DrawObject3D(debug_card_mesh_[type_index], card_box_mat_, {0.0f, 0.0f, 0.0f},
                         {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
    }
    if (!show_labels_) return;

    const float kFaceOffset = 0.004f;
    const jpov::Vec3f cam = cmds->camera.position;
    jpov::Vec3f fwd{cmds->camera.target.x() - cam.x(), cmds->camera.target.y() - cam.y(),
                    cmds->camera.target.z() - cam.z()};
    const float fl = std::sqrt(fwd.x() * fwd.x() + fwd.y() * fwd.y() + fwd.z() * fwd.z());
    if (fl > 1e-6f) {
      fwd = jpov::Vec3f{fwd.x() / fl, fwd.y() / fl, fwd.z() / fl};
    }

    for (const auto& card : shape_.cards) {
      if (static_cast<size_t>(card.type % kMaxCardTypes) != type_index) continue;

      const auto norm = [](const Vec3d& q) {
        const double l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
        return (l < 1e-12) ? Vec3d{0, 1, 0} : Vec3d{q.x / l, q.y / l, q.z / l};
      };
      const auto cross = [](const Vec3d& a2, const Vec3d& b2) {
        return Vec3d{a2.y * b2.z - a2.z * b2.y, a2.z * b2.x - a2.x * b2.z,
                     a2.x * b2.y - a2.y * b2.x};
      };
      const Vec3d up = norm(card.up);
      const Vec3d n = norm(card.normal);
      const Vec3d right = norm(cross(up, n));

      std::string mode = "bottom_center";
      for (const auto& t : shape_.leaf_types) {
        if (t.type == card.type) {
          mode = t.anchor_mode;
          break;
        }
      }
      double au = 0.5;
      double av = 0.0;
      if (!jpov::vegetation::AnchorModeToUv(mode, &au, &av)) continue;

      const float fh = static_cast<float>(0.125 * card.height);   // 与 1/8 角标同量级
      const jpov::Color white{1.0f, 1.0f, 1.0f, 1.0f};
      const jpov::Vec3f face{static_cast<float>(n.x), static_cast<float>(n.y),
                             static_cast<float>(n.z)};
      const jpov::Vec3f back{static_cast<float>(-n.x), static_cast<float>(-n.y),
                             static_cast<float>(-n.z)};
      const jpov::Vec3f upv{static_cast<float>(up.x), static_cast<float>(up.y),
                            static_cast<float>(up.z)};

      // side = +1 正面 / -1 背面；文字平面沿 side*normal 抬 kFaceOffset。
      const auto Point = [&](double u, double v, int side) {
        const double su = (u - au) * card.width;
        const double sv = (v - av) * card.height;
        const double off = side * kFaceOffset;
        return jpov::Vec3f{
            static_cast<float>(card.anchor.x + right.x * su + up.x * sv + n.x * off),
            static_cast<float>(card.anchor.y + right.y * su + up.y * sv + n.y * off),
            static_cast<float>(card.anchor.z + right.z * su + up.z * sv + n.z * off)};
      };
      const auto Inside = [&](const jpov::Vec3f& p) {
        return (p.x() - cam.x()) * fwd.x() + (p.y() - cam.y()) * fwd.y() +
                   (p.z() - cam.z()) * fwd.z() >
               0.1f;
      };
      const auto Label = [&](const char* text, const jpov::Vec3f& p, const jpov::Vec3f& f,
                             jpov::TextAlignment align) {
        if (!Inside(p)) return;
        cmds->DrawText3D(text, p, f, upv, fh, white, jpov::kFontBuiltinCJK, align);
      };

      Label("左下", Point(0.0, 0.0, +1), face, jpov::TextAlignment::kBottomLeft);
      Label("右下", Point(1.0, 0.0, +1), face, jpov::TextAlignment::kBottomRight);
      Label("右上", Point(1.0, 1.0, +1), face, jpov::TextAlignment::kTopRight);
      Label("左上", Point(0.0, 1.0, +1), face, jpov::TextAlignment::kTopLeft);
      Label("背面", Point(0.5, 0.5, -1), back, jpov::TextAlignment::kCenter);
    }
  }

  void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                    const jpov::WindowInfo& winfo,
                    jpov::RenderCommandList* cmds) override {
    (void)frame_count;
    // 面板滑条改的是「呈现」参数：curl 要重烘顶点法线，法线强度是材质常量。
    if (has_leaf_) {
      // 精确比较是安全的：Ui::SliderFloat 只在拖动/点击时写 *value，
      // 空闲帧只做 *value = clamp(*value)（同值写回，位模式不变），不会产生浮点噪声。
      // 拖动期间本来就是每帧重烘（实时预览），epsilon 也省不掉这笔开销。
      if (curl_ != curl_applied_) {
        ApplyCurl();
      }
      for (auto& m : leaf_mat_) {
        m.normal_scale = leaf_normal_scale_;
      }
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
    // 一个 type 一个绘制单元：type 过滤对「贴图 / 绿卡 / 标签」同时生效，
    // 不会再出现「绿卡还在、标签已经按 type 过滤掉」这种不一致。
    for (size_t t = 0; t < leaf_type_count_; ++t) {
      if (!show_all_card_types_ && t != type_filter_) continue;
      if (debug_cards) {
        DrawLeafDebugForType(t, cmds);
      } else if (t < leaf_mesh_.size() && leaf_mesh_[t] != 0) {
        cmds->DrawObject3D(leaf_mesh_[t], leaf_mat_[t], {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
                           {0.0f, 0.0f, 1.0f});
      }
    }
    // 左上角相机 HUD。headless 且没配字体（gold test）时关掉：文字要 CJK 图集，
    // 没有图集就只剩「Known font alias: none」的 CHECK 崩溃，而 gold 保护的是 3D 观感。
    if (show_hud_) {
      cmds->DrawText(CamString(), {8.0f, 8.0f}, 18.0f, jpov::Color{1.0f, 1.0f, 1.0f, 1.0f},
                     jpov::TextAlignment::kTopLeft, jpov::kFontBuiltinCJK);
    }

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
      const int leaf_rows = has_leaf_ ? 6 : 2;   // 全部叶子/type +（贴图/标签/球面化/法线强度）
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
        ui_.Checkbox("叶卡标签", &show_labels_, leaf_row(3));
        ui_.SliderFloat("叶法线球面化", &curl_, leaf_row(4), 0.0f, 2.0f, 2);
        ui_.SliderFloat("叶法线强度", &leaf_normal_scale_, leaf_row(5), 0.0f, 3.0f, 2);
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
        const int clamped = std::max(0, std::min(static_cast<int>(leaf_type_count_) - 1, parsed));
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

}  // namespace vegv2

#endif  // JPOV_TOOLS_VEGETATION_TOOL_V2_VIEWER_VEGETATION_V2_APP_H_
