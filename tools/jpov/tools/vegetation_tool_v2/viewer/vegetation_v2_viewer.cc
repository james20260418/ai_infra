// 植被工具 v2 —— 只做一件事：把 ShapeProto（pb.txt）渲染出来看形状对不对。
//
// 刻意【不接材质】：到这一阶段要判断的是**拓扑/UV 尺度/分叉**，贴图会干扰判断。
//   * 树干：按格式定义逐枝扫管，**每根枝单独一个 mesh**（便于逐枝上色/逐枝隐藏）
//   * 层级配色：level 0 深棕 → 越深越浅（一眼看出父子关系）
//   * 叶卡：用**盒子**表示（不是薄片）—— 盒子能看清朝向与尺寸，薄片侧面看不见
//   * 类型配色：最多 4 种绿，按 type 取模
//
// 交互：K 切换叶卡、[ / ] 调最大显示层级、P 打印相机。
//
// 用法：
//   jpov_vegetation_v2_viewer --shape /tmp/willow.pbtxt
//   jpov_vegetation_v2_viewer --shape ... --capture /tmp/shots --cam_phi 10 --cam_r 4
//   --normal-scale <f>   法线强度（默认 1.5；0~3）
//   --uv-vscale 0.5      v 方向缩放（默认 0 = 自动取 albedo 宽/高）

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <glog/logging.h>
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
using jpov::vegetation::MeshData;
using jpov::vegetation::Shape;
using jpov::vegetation::Vec3d;

constexpr int kMaxCardTypes = 4;  // 4 种绿
constexpr float kCardThickness = 0.02f;  // 盒子厚度（米）：只是为了看得见朝向

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
  // 法线贴图强度（uNormalScale）。默认 1.5：同一树皮 A/B 实测
  // 1.0 偏平、3.0 发塑料，1.5 最自然（Skylight + 单平行光下 ambient 占比高，
  // 1.0 的凹凸容易被环境光冲淡）。面板滑条同一量程。
  float normal_scale = 1.5f;
  // UV 的 v 方向缩放 = 贴图宽/高。0 = 自动从 albedo 的像素尺寸读。
  // 非方形贴图必须补偿，否则整张图被按正方形铺开：1:2 的图横向拉宽 2×，
  // 凸起在 u 方向被压扁、纹素不再方正（法线细节跟着糊）。
  float uv_vscale = 0.0f;
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
    } else if (arg == "--normal-scale" && has_val) {
      a.normal_scale = static_cast<float>(std::atof(argv[++i]));
      CHECK_GT(a.normal_scale, 0.0f) << "--normal-scale 必须 > 0";
    } else if (arg == "--uv-vscale" && has_val) {
      a.uv_vscale = static_cast<float>(std::atof(argv[++i]));
      CHECK_GT(a.uv_vscale, 0.0f) << "--uv-vscale 必须 > 0";
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

// 一张叶卡 → 一个盒子（8 顶点 / 12 三角形），世界坐标，直接烘进 mesh。
void AppendCardBox(const Shape& shape, const jpov::vegetation::LeafCard& card,
                   float width, std::vector<float>* pos, std::vector<float>* nrm,
                   std::vector<uint32_t>* idx) {
  const auto norm = [](const Vec3d& v) {
    const double l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return (l < 1e-12) ? Vec3d{0, 1, 0} : Vec3d{v.x / l, v.y / l, v.z / l};
  };
  const auto cross = [](const Vec3d& a, const Vec3d& b) {
    return Vec3d{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
  };
  const Vec3d up = norm(card.up);
  const Vec3d normal = norm(card.normal);
  const Vec3d right = norm(cross(up, normal));

  // anchor_mode 决定盒子从锚点往哪边长
  std::string mode = "bottom_center";
  for (const auto& t : shape.leaf_types) {
    if (t.type == card.type) {
      mode = t.anchor_mode;
      break;
    }
  }
  double base_off = 0.0;
  if (mode == "top_center") {
    base_off = -static_cast<double>(card.height);
  } else if (mode == "center") {
    base_off = -0.5 * static_cast<double>(card.height);
  }

  const double hw = 0.5 * width;
  const double ht = 0.5 * kCardThickness;
  const double h = card.height;
  Vec3d c[8];
  int k = 0;
  for (int iz = -1; iz <= 1; iz += 2) {
    for (int iy = 0; iy <= 1; ++iy) {
      for (int ix = -1; ix <= 1; ix += 2) {
        const double ox = ix * hw;
        const double oy = base_off + iy * h;
        const double oz = iz * ht;
        c[k++] = Vec3d{card.anchor.x + right.x * ox + up.x * oy + normal.x * oz,
                       card.anchor.y + right.y * ox + up.y * oy + normal.y * oz,
                       card.anchor.z + right.z * ox + up.z * oy + normal.z * oz};
      }
    }
  }
  // 顶点顺序：k = (iz, iy, ix) 嵌套 → 0:(-,0,-) 1:(+,0,-) 2:(-,1,-) 3:(+,1,-)
  //                                   4:(-,0,+) 5:(+,0,+) 6:(-,1,+) 7:(+,1,+)
  static const int kFaces[12][3] = {
      {0, 1, 3}, {0, 3, 2},  // -Z
      {4, 6, 7}, {4, 7, 5},  // +Z
      {0, 4, 5}, {0, 5, 1},  // -Y（根部侧）
      {2, 3, 7}, {2, 7, 6},  // +Y（梢部侧）
      {0, 2, 6}, {0, 6, 4},  // -X
      {1, 5, 7}, {1, 7, 3},  // +X
  };
  const uint32_t base = static_cast<uint32_t>(pos->size() / 3);
  for (int i = 0; i < 8; ++i) {
    pos->push_back(static_cast<float>(c[i].x));
    pos->push_back(static_cast<float>(c[i].y));
    pos->push_back(static_cast<float>(c[i].z));
    nrm->push_back(static_cast<float>(normal.x));
    nrm->push_back(static_cast<float>(normal.y));
    nrm->push_back(static_cast<float>(normal.z));
  }
  for (int f = 0; f < 12; ++f) {
    idx->push_back(base + kFaces[f][0]);
    idx->push_back(base + kFaces[f][1]);
    idx->push_back(base + kFaces[f][2]);
  }
}

class VegetationV2App : public JPOV {
 public:
  using JPOV::JPOV;

  Shape shape_;
  std::vector<uint32_t> branch_mesh_;   // 与 shape_.branches 下标对齐
  std::vector<int> branch_level_;
  std::vector<uint32_t> card_mesh_;     // 每种 type 一个合并盒子 mesh
  std::vector<jpov::PBRMaterial> card_mat_;
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

  void BuildScene(const CliArgs& args) {
    normal_scale_ = args.normal_scale;
    // v 方向补偿必须在生成 mesh 之前确定：UV 是扫管时烘进去的，不是 shader 里改的。
    uv_v_scale_ = args.uv_vscale;
    if (!args.bark_dir.empty() && uv_v_scale_ <= 0.0f) {
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

    // 叶卡：按 type 合并成盒子 mesh。宽度这里先给一个占位值（高度 × 0.6），
    // 真正的宽度来自叶材质 atlas 比例——本工具不接材质，只为看位置/朝向。
    std::vector<std::vector<float>> pos(kMaxCardTypes);
    std::vector<std::vector<float>> nrm(kMaxCardTypes);
    std::vector<std::vector<uint32_t>> idx(kMaxCardTypes);
    for (const auto& card : shape_.cards) {
      const uint32_t t = card.type % kMaxCardTypes;
      AppendCardBox(shape_, card, static_cast<float>(card.height * 0.6), &pos[t], &nrm[t],
                    &idx[t]);
    }
    card_mesh_.resize(kMaxCardTypes, 0);
    card_mat_.resize(kMaxCardTypes);
    for (int t = 0; t < kMaxCardTypes; ++t) {
      if (pos[t].empty()) {
        continue;
      }
      jpov::MeshData md;
      // 带上 UV：贴树皮时 PBR 完整版 shader 需要 UV，否则采样不到（mesh 无 kUV 会退回无贴图路径）。
      md.flags = static_cast<jpov::MeshVertexFlags>(
          static_cast<uint8_t>(jpov::MeshVertexFlags::kPosition) |
          static_cast<uint8_t>(jpov::MeshVertexFlags::kNormal) |
          static_cast<uint8_t>(jpov::MeshVertexFlags::kUV));
      for (size_t v = 0; v < pos[t].size() / 3; ++v) {
        md.positions.push_back(jpov::Vec3f{pos[t][v * 3], pos[t][v * 3 + 1], pos[t][v * 3 + 2]});
        md.normals.push_back(jpov::Vec3f{nrm[t][v * 3], nrm[t][v * 3 + 1], nrm[t][v * 3 + 2]});
        // 盒子顶点的 UV：AppendCardBox 的顶点序是 (iz, iy, ix) ⇒ bit0=ix, bit1=iy。
        // 现在只是给贴图留位（叶卡仍用纯色），但 MeshData 声明了 kUV 就必须给满。
        md.uvs.push_back(jpov::Vec2f{(v & 1) ? 1.0f : 0.0f, ((v >> 1) & 1) ? 1.0f : 0.0f});
      }
      md.indices = idx[t];
      card_mesh_[t] = RegisterMesh(std::move(md));
      card_mat_[t] = jpov::PBRMaterial::SolidColor(GreenForType(static_cast<uint32_t>(t)));
      card_mat_[t].double_sided = true;  // 盒子本来就朝外，但双面更保险
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

    show_panel_ = !args.capture;   // headless 出图不画面板（与其它 viewer 一致）
    int deepest = 0;
    for (const auto& b : shape_.branches) {
      deepest = std::max(deepest, static_cast<int>(b.level));
    }
    max_level_ = std::min(args.max_level, deepest);
    max_level_f_ = static_cast<float>(max_level_);
    ground_y_ = args.ground_y;
    LOG(INFO) << "shape: " << shape_.branches.size() << " 枝 / " << shape_.cards.size()
              << " 卡 | " << CamString();
  }

  void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                    const jpov::WindowInfo& winfo,
                    jpov::RenderCommandList* cmds) override {
    (void)frame_count;
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
    for (size_t t = 0; t < card_mesh_.size(); ++t) {
      if (card_mesh_[t] == 0) {
        continue;
      }
      // 只看某一类：用于确认"这棵树的某类叶卡到底长在哪、朝向对不对"
      if (!show_all_card_types_ && t != type_filter_) {
        continue;
      }
      cmds->DrawObject3D(card_mesh_[t], card_mat_[t], {0.0f, 0.0f, 0.0f},
                         {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
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
      const float leaf_top = scene_top - 12.0f - (2 * 32.0f);
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
