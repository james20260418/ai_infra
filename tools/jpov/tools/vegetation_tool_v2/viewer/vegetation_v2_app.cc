// 植被 v2 查看器 —— 场景 + 渲染 + 交互的**全部实现**（只编译一次）。
//
// 对外只有一个入口 RunViewer()（见 vegetation_v2_app.h）。命令行解析在
// vegetation_v2_viewer.cc；gold 基准图也是用这个二进制跑出来的。

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <optional>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <glog/logging.h>
#include "third_party/stb/stb_image.h"
#include "third_party/stb/stb_image_write.h"

#include "tools/common/utils.h"
#include "tools/jpov/demo/skylight_scene.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/ui.h"
#include "tools/jpov/tools/vegetation_tool_v2/bake/vegetation_glb_baker.h"
#include "tools/jpov/tools/vegetation_tool_v2/leaf_texture.h"
#include "tools/jpov/tools/vegetation_tool_v2/shape.h"
#include "tools/jpov/tools/vegetation_tool_v2/sweep.h"


#include "tools/jpov/tools/vegetation_tool_v2/viewer/vegetation_v2_app.h"

namespace vegv2 {



// 渲染分辨率（与其它 viewer 一致）。不从 viewer_app.h 取：那个头文件只为这几个常量
// 会把整个交互 App 基类拉进来。
constexpr int kViewerWidth = 1280;
constexpr int kViewerHeight = 720;
constexpr float kViewerFps = 60.0f;
using jpov::vegetation::Branch;
using jpov::vegetation::BranchSample;
using jpov::vegetation::LeafCard;
using jpov::vegetation::LeafType;
using jpov::vegetation::MeshData;
using jpov::vegetation::Shape;
using jpov::vegetation::Vec3d;

constexpr int kMaxCardTypes = 4;  // 4 种绿
constexpr float kCardThickness = 0.02f;  // 盒子厚度（米）：只是为了看得见朝向



// 只把 RGB 往透明区扩散，**不动 alpha**（alpha 是 cutout 的判据）。
// 目的：mip / 双线性采样时，叶子边缘不会混到背景的黑色（否则一圈黑边）。

// 上下翻转 RGBA 行。
// stb_image 解码后第 0 行是**图像顶部**，而 OpenGL 的 v=0 取第 0 行 ⇒ 直接上传时
// v=0 = 图顶。Blender / glTF 的约定是 v=0 = 图**底**，用户按"根部画在图片底部"做的素材
// 一旦直传就会整张上下颠倒。所以叶卡贴图在上传前翻一次行，让 uv(0,0) = 图左下。

// 由 alpha 生成切线空间法线贴图：alpha 当高度场 → 模糊 → 中央差分。
// green = +v（OpenGL 约定，与引擎 B = cross(N,T) 一致）；图像 y 向下，所以要取反。

namespace {

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
  // 姿态与 UV 锚点只从 ComputeCardFrame 来（和真实叶卡、debug 标签同一份来源）。
  const jpov::vegetation::CardFrame frame = jpov::vegetation::ComputeCardFrame(shape, card);
  const Vec3d& up = frame.up;
  const Vec3d& n = frame.normal;
  const Vec3d& right = frame.right;
  const double au = frame.au;
  const double av = frame.av;

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

  // 释放 glb 的 GPU 资源。**必须在 Finalize() 之前调**（Finalize 之后 GL 上下文已销毁，
  // ReleaseGltf 会崩），所以 RunViewer 统一走 Shutdown()。
  void Shutdown() {
    ReleaseGlb();
    Finalize();
  }

  // ── 状态机：原材料态（shape/leaf/bark 现搭）↔ glb 展示态（烘焙产物 + 风）──
  enum class Mode { kRaw, kGlb };

  Shape shape_;
  bool has_shape_ = false;        // 有没有原材料（纯 glb 查看模式下为 false）
  float args_radius_ = 0.0f;      // --cam_r（0 = 自动：按包围盒 fit）
  Mode mode_ = Mode::kRaw;
  jpov::GltfObject glb_;          // glb 展示态的资源（独占，切回原材料态要 Release）
  bool glb_loaded_ = false;
  std::string glb_path_;
  // 烘焙参数（面板可调）
  float bake_thick_spacing_ = 1.5f;
  float bake_thin_spacing_ = 0.5f;
  float bake_max_joints_f_ = 8.0f;
  float bake_phase_scale_ = 1.0f;
  float bake_droop_scale_ = 0.5f;
  float bake_leaf_black_ = 30.0f;
  float bake_leaf_normal_ = 1.0f;
  float bake_normal_scale_ = 0.75f;
  // 烘焙结果（面板显示）
  std::string bake_msg_;
  bool bake_ok_ = false;
  float bake_ms_ = 0.0f;
  // 风（glb 展示态）
  bool wind_on_ = true;
  float wind_azimuth_deg_ = 35.0f;
  float wind_amplitude_ = 0.1167f;   // = 旧默认 0.35 的 1/3（2026-10-22 Danis）
  float wind_frequency_ = 0.5f;
  float wind_turb_amp_ = 0.04f;     // = 旧默认 0.12 的 1/3
  float wind_turb_freq_ = 2.5f;
  float wind_gust_freq_ = 0.6f;
  float wind_gust_str_ = 0.7f;
  float wind_time_scale_ = 1.0f;
  float wind_blow_ratio_ = 0.0f;   // 吹歪比例（0 = 关；随指数变量纲，是观感参数）
  float wind_blow_exp_ = 2.0f;     // 吹歪指数 n（1 = 线性，2 = 梢部加速）
  float scene_height_ = 1.0f;      // 参考高度 H（米）：shape/glb 的 Y 向包围盒高
  bool wind_paused_ = false;
  float wind_time_ = 0.0f;        // 秒；由帧号推（确定性）
  // 面板折叠状态
  // 面板点击**不能当帧**就释放/重建 GPU 资源：本帧的绘制命令已经带着旧 mesh_id 入队了，
  // 立刻 Release 会让渲染器拿到已释放的 id。所以只记一个待办，下一帧开头执行。
  enum class Pending { kNone, kBake, kBackToRaw };
  Pending pending_ = Pending::kNone;
  bool bake_panel_open_ = true;
  bool wind_panel_open_ = true;
  bool scene_panel_open_ = true;
  bool leaf_panel_open_ = true;
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
  // 叶面弯曲**没有**滑条：弯曲存在 shape 的 leaf type 里，查看器只负责把它画出来。
  // 面板是看资产的地方，不是调生成参数的地方 —— 否则预览与最终资产会变成两套真相。
  float leaf_normal_scale_ = 1.0f;    // B：法线贴图强度（面板可调）
  float leaf_black_ = 12.0f;
  std::vector<jpov::PBRMaterial> leaf_mat_;
  std::vector<uint32_t> leaf_mesh_;               // 每 type 一个 mesh
  // 每个 type 的卡子集（重建网格用：面板改弯曲参数时要重算顶点）。
  // 和 leaf_mesh_ 同下标；空的 = 该 type 没有卡。
  std::vector<Shape> leaf_subset_;
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
  float normal_scale_ = 0.75f;  // 面板可调；BuildScene 里来自 --normal-scale（默认 = 旧 1.5 的 1/2）
  // = 贴图宽/高；交给 SweepBranches 烘进 UV（v 方向），不是 shader 里改。
  float uv_v_scale_ = 1.0f;
  jpov::PBRMaterial bark_mat_;
  uint32_t ground_mesh_ = 0;
  jpov::PBRMaterial ground_mat_;
  jpov::Ui ui_;
  std::string shape_path_;   // 只用于帮助打印（H 键）
  std::string bark_dir_;     // 烘焙用（原材料态加载时记下）
  std::string leaf_path_;    // 同上
  std::string leaf2_path_;   // type 1（镜像手性）素材
  std::string bake_out_path_;  // 非空 = 烘焙输出固定到这个路径（--bake）
  float args_uv_vscale_ = 0.0f;  // --uv-vscale（0 = 自动）
  jpov_skylight::SkyDegrees deg_;

  // 相机 HUD：**给截图用** —— 报出复现这张图需要的全部数字（角度、距离、相机位置、
  // 注视点、地面高度），以及资产里树根的位置。对齐类问题（树干没站在原点、树根浮空、
  // 树干整体偏移）看这两行就能判断是数据错还是相机错。
  std::string CamString() const {
    char buf[320];
    const jpov::Vec3f position = view_.Position();
    std::snprintf(buf, sizeof(buf),
                  "cam phi=%.1f theta=%.1f R=%.2f | pos(%.2f, %.2f, %.2f) -> "
                  "target(%.2f, %.2f, %.2f) | ground_y=%.2f | level<=%d types=%s",
                  view_.phi * 180.0 / 3.14159265358979323846,
                  view_.theta * 180.0 / 3.14159265358979323846, view_.R, position.x(),
                  position.y() + cam_target_y_, position.z(), 0.0f, cam_target_y_, ground_y_,
                  max_level_,
                  show_all_card_types_ ? "all" : std::to_string(type_filter_).c_str());
    return std::string(buf);
  }

  // 第二行：资产侧的关键数字（树根位置 + 包围盒 + 文件名）。
  // 树干"对齐"出问题时，先看根枝起点是不是 (0,0,0)、包围盒是不是关于原点对称。
  std::string AssetString() const {
    if (!has_shape_ || shape_.branches.empty() ||
        shape_.branches.front().samples.empty()) {
      return std::string("asset: (无 shape)");
    }
    const Branch& root = shape_.branches.front();
    const Vec3d& base = root.samples.front().position;
    double low[3] = {base.x, base.y, base.z};
    double high[3] = {base.x, base.y, base.z};
    for (const Branch& branch : shape_.branches) {
      for (const BranchSample& sample : branch.samples) {
        const double values[3] = {sample.position.x, sample.position.y, sample.position.z};
        for (int axis = 0; axis < 3; ++axis) {
          low[axis] = std::min(low[axis], values[axis]);
          high[axis] = std::max(high[axis], values[axis]);
        }
      }
    }
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "root(%.3f, %.3f, %.3f) | bbox x[%.2f, %.2f] y[%.2f, %.2f] z[%.2f, %.2f] | %s",
                  base.x, base.y, base.z, low[0], high[0], low[1], high[1], low[2], high[2],
                  shape_path_.empty() ? "(shape)" : shape_path_.c_str());
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
    // 与 GLB 烘焙共用同一份 CPU 流水线（leaf_texture）：用户看到的叶子与写进 glb 的叶子
    // 必须是同一批像素，否则「预览↔产物」迟早漂移。
    jpov::vegetation::LeafTexture tex;
    std::string err;
    CHECK(jpov::vegetation::LoadLeafTextureFromPng(path, leaf_black_, /*dilate_passes=*/6, &tex,
                                                   &err))
        << err;
    size_t opaque = 0;
    for (size_t i = 0; i < static_cast<size_t>(tex.width) * tex.height; ++i) {
      if (tex.rgba[i * 4 + 3] > 0) {
        ++opaque;
      }
    }
    LOG(INFO) << "叶卡: " << path << " " << tex.width << "x" << tex.height << "，不透明 "
              << (100.0 * static_cast<double>(opaque) /
                  static_cast<double>(static_cast<size_t>(tex.width) * tex.height))
              << "%";

    std::vector<unsigned char> albedo = tex.rgba;
    std::vector<unsigned char> normal_rgba = jpov::vegetation::BuildLeafNormalMap(tex, 4.0f);
    // albedo 与法线一起翻行：v=0 = 图底（glTF 约定）。
    jpov::vegetation::FlipRows(&albedo, tex.width, tex.height);
    jpov::vegetation::FlipRows(&normal_rgba, tex.width, tex.height);

    jpov::TextureOptions opts;
    opts.mipmap = true;
    opts.repeat = false;
    opts.alpha_coverage.enabled = true;
    opts.alpha_coverage.cutoff = jpov::vegetation::kLeafAlphaCutoff;
    *out = jpov::PBRMaterial::SolidColor({1.0f, 1.0f, 1.0f, 1.0f});
    std::vector<unsigned char> albedo_png;
    std::vector<unsigned char> normal_png;
    CHECK(jpov::vegetation::EncodePng(albedo, tex.width, tex.height, &albedo_png));
    CHECK(jpov::vegetation::EncodePng(normal_rgba, tex.width, tex.height, &normal_png));
    out->base_color_tex =
        RegisterTextureFromMemory(albedo_png, "leaf_albedo:" + path, opts);
    out->normal_tex =
        RegisterTextureFromMemory(normal_png, "leaf_normal:" + path, opts);
    out->normal_scale = leaf_normal_scale_;
    out->alpha_mode = jpov::AlphaMode::kMask;
    out->alpha_cutoff = jpov::vegetation::kLeafAlphaCutoff;
    out->double_sided = true;
  }

  // 叶卡：每个 leaf type 一份材质 + 一份网格（type 0 用 --leaf，type 1 用 --leaf2，缺省回退）。
  //
  // 叶面弯曲早就烘在**几何**里了（BuildLeafCards 按解析式求位置/法线/切线），
  // 这里只做「shape 网格 → 引擎网格」的搬运。以前这里是「先摆平面 quad、再把法线
  // 球面化」的 hack —— 法线弯了、几何仍是平的，而且烘焙器不认那套法线 ⇒ 预览与
  // glb 资产不是同一个东西。现在两边走同一条 BuildLeafCards。
  void BuildLeafScene(const CliArgs& args) {
    leaf_black_ = args.leaf_black;
    leaf_normal_scale_ = args.leaf_normal;
    has_leaf_ = true;

    leaf_mat_.assign(kMaxCardTypes, jpov::PBRMaterial());
    leaf_mesh_.assign(kMaxCardTypes, 0);
    leaf_subset_.assign(kMaxCardTypes, Shape());

    for (int t = 0; t < kMaxCardTypes; ++t) {
      // 子集只保留「这一类卡」：卡网格只需要 cards + leaf_types，枝干（占 99% 内存）
      // 留在 shape_ 里就够了（烘焙/重建网格都直接读 shape_）。
      Shape subset;
      subset.leaf_types = shape_.leaf_types;
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

      leaf_subset_[t] = subset;
      leaf_mesh_[t] = RegisterMesh(ToEngineLeafMesh(jpov::vegetation::BuildLeafCards(subset)));
    }
    for (const LeafType& t : shape_.leaf_types) {
      LOG(INFO) << "leaf type " << t.type << " 弯曲（来自资产）：along=" << t.bend.along
                << " across=" << t.bend.across << " crease=" << t.bend.crease;
    }
    LOG(INFO) << "叶卡材质就绪，法线强度 = " << leaf_normal_scale_;
  }

  // shape 网格 → 引擎网格：只搬数据，几何早就是最终形态（含弯曲）。
  jpov::MeshData ToEngineLeafMesh(const MeshData& src) const {
    jpov::MeshData md;
    md.flags = static_cast<jpov::MeshVertexFlags>(
        static_cast<uint8_t>(jpov::MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kNormal) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kUV) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kTangent));
    const size_t nverts = src.positions.size() / 3;
    md.positions.reserve(nverts);
    md.normals.reserve(nverts);
    md.uvs.reserve(nverts);
    md.tangents.reserve(nverts);
    for (size_t v = 0; v < nverts; ++v) {
      md.positions.push_back(jpov::Vec3f{src.positions[v * 3], src.positions[v * 3 + 1],
                                         src.positions[v * 3 + 2]});
      md.normals.push_back(jpov::Vec3f{src.normals[v * 3], src.normals[v * 3 + 1],
                                       src.normals[v * 3 + 2]});
      md.uvs.push_back(jpov::Vec2f{src.uvs[v * 2], src.uvs[v * 2 + 1]});
      // 切线是解析的 ∂P/∂u（弯曲卡的切线不再等于「卡片 +u 边」，差分近似会把曲率吃进去）。
      md.tangents.push_back(jpov::Vec3f{src.tangents[v * 3], src.tangents[v * 3 + 1],
                                        src.tangents[v * 3 + 2]});
    }
    md.indices = src.indices;
    return md;
  }

  // 与 shape **无关**的公共状态（纯 glb 查看模式也要有）：材质/烘焙命令行的继承、
  // 风的覆盖参数、UV v 缩放、面板/HUD/相机的开关。
  //
  // 为什么单独一层：`--glb` 可以不配 `--shape`（纯查看模式），那条路径没有 shape、没有
  // 扫管/叶卡，但仍然要能调风、要能画面板、要继承 --normal-scale 之类的参数。
  void ApplyCommonArgs(const CliArgs& args) {
    if (args.hide_hud) {
      // HUD 与叶卡 UV 标签都是 2D 文字覆盖层；gold 要的是纯 3D 画面 ⇒ 一个开关全关。
      // 必须放在这里（两条建场景路径都经过）：只在原材料态关的话，纯 glb 查看模式
      // 仍会画出相机 HUD，而 glb gold 正好走那条路。
      show_hud_ = false;
      show_labels_ = false;
    }
    normal_scale_ = args.normal_scale;
    args_uv_vscale_ = args.uv_vscale;
    args_radius_ = static_cast<float>(args.radius);
    bark_dir_ = args.bark_dir;
    leaf_path_ = args.leaf_path;
    leaf2_path_ = args.leaf2_path;
    // 烘焙默认参数**继承命令行**：否则 --leaf-black 50 只影响预览、产物还是 30，
    // 预览与产物会悄悄不一致（这正是「同一份数据两条流水线」的经典坑）。
    bake_leaf_black_ = args.leaf_black;
    bake_leaf_normal_ = args.leaf_normal;
    bake_normal_scale_ = args.normal_scale;
    bake_out_path_ = args.bake_glb_path;
    if (args.wind_off) {
      wind_on_ = false;
    }
    if (args.wind_amp >= 0.0f) {
      wind_amplitude_ = args.wind_amp;
    }
    if (args.wind_turb >= 0.0f) {
      wind_turb_amp_ = args.wind_turb;
    }
    if (args.wind_blow >= 0.0f) {
      wind_blow_ratio_ = args.wind_blow;
    }
    if (args.wind_blow_exp >= 0.0f) {
      wind_blow_exp_ = args.wind_blow_exp;
    }
    if (args.wind_time >= 0.0f) {
      wind_paused_ = true;
      wind_time_ = args.wind_time;
    }
    // v 方向补偿必须在生成 mesh 之前确定：UV 是扫管时烘进去的，不是 shader 里改的。
    // 0 = 自动（只对 --bark 有意义）：没有 bark 时必须落回 1.0，
    // 否则 SweepBranches 会因 uv_v_scale<=0 直接 abort。
    uv_v_scale_ = (args.uv_vscale > 0.0f) ? args.uv_vscale : 1.0f;
    if (!args.bark_dir.empty() && args.uv_vscale <= 0.0f) {
      int tw = 0, th = 0;
      const std::string albedo = args.bark_dir + "/albedo.png";
      if (jpov::vegetation::ReadPngSize(albedo, &tw, &th)) {
        uv_v_scale_ = static_cast<float>(tw) / static_cast<float>(th);
        LOG(INFO) << "UV v 缩放 = " << uv_v_scale_ << "（贴图 " << tw << "x" << th
                  << "，v *= 宽/高）";
      } else {
        uv_v_scale_ = 1.0f;
        LOG(WARNING) << "读不到 albedo 像素尺寸，uv_v_scale 取 1.0: " << albedo;
      }
    }
    show_panel_ = !args.capture || args.show_panel;   // headless 默认不画面板
    cam_target_y_ = args.cam_target_y;
    // 没有字体图集时画文字会 CHECK 崩（font_renderer 只跳过 2D，3D 标签仍会查 alias）。
    // 统一在这里关掉：headless 出图 / CI sandbox 都不需要文字。
    // **面板也是文字**（标题/滑条标签）⇒ 没字体时必须连面板一起关，否则
    // `--show-panel` 在无字体环境直接崩（颜色块没法代替标签，关掉才是合理降级）。
    if (!has_fonts_) {
      show_hud_ = false;
      show_labels_ = false;
      show_panel_ = false;
    }
    deg_.sun_elev_deg = 55.0f;
    deg_.sun_azim_deg = 35.0f;
  }

  // 原材料态：shape/leaf/bark 现搭。纯 glb 查看模式不会调它。
  void BuildScene(const CliArgs& args) {
    has_shape_ = true;
    ApplyCommonArgs(args);
    // 吹歪的参考高度 H：树高（Y 向包围盒）。每帧重算太浪费，这里算一次。
    // （纯 glb 模式没有 shape，H 在 LoadGlbFile 里按 glb 包围盒补。）
    {
      float ymin = 1e30f;
      float ymax = -1e30f;
      for (const jpov::vegetation::Branch& b : shape_.branches) {
        for (const jpov::vegetation::BranchSample& smp : b.samples) {
          ymin = std::min(ymin, static_cast<float>(smp.position.y));
          ymax = std::max(ymax, static_cast<float>(smp.position.y));
        }
      }
      scene_height_ = (ymax > ymin) ? (ymax - ymin) : 1.0f;
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
      // 过滤的是**颜色桶下标**（= card.type % kMaxCardTypes），不是原始 type 编号：
      // 本工具只有 4 个桶，type 与桶不是一一对应。直接拿 type 当桶会画到别的类型上
      // （例：types {0,1,2,3,5}，--only-type 5 会被夹到 3 → 显示 type 3）。
      type_filter_ = static_cast<uint32_t>(args.only_type) % kMaxCardTypes;
      if (static_cast<uint32_t>(args.only_type) != type_filter_) {
        LOG(WARNING) << "--only-type " << args.only_type << " 按颜色桶取模 → 桶 "
                     << type_filter_ << "（type % " << kMaxCardTypes << "）";
      }
      if (type_filter_ >= leaf_type_count_) {
        LOG(WARNING) << "--only-type " << args.only_type << " 对应的桶 " << type_filter_
                     << " 在 shape 里没有叶子，夹到 " << (leaf_type_count_ - 1);
        type_filter_ = static_cast<uint32_t>(leaf_type_count_ - 1);
      }
    }
    if (args.hide_leaf) show_leaf_ = false;
    if (args.hide_bark) show_bark_ = false;
    if (args.hide_hud) {
      // HUD 是 2D 文字覆盖层，叶卡上的 UV 标签也是 —— gold 比对要的是纯 3D 画面，
      // 一个开关全关掉（以前只有 HUD，标签靠"没字体"顺带关掉；字体补上后必须显式关）。
      show_hud_ = false;
      show_labels_ = false;
    }
    LOG(INFO) << "shape: " << shape_.branches.size() << " 枝 / " << shape_.cards.size()
              << " 卡 | " << CamString();
  }

  // 纯 glb 查看模式：**没有 shape**，只画 glb + 地面 + 面板。
  //
  // 触发条件见 RunViewer：给了 `--glb` 但找不到 shape 文件（典型是"别人发来一个 glb，
  // 我只想看看它长什么样"）。此时没有原材料态可回，也就没有烘焙/扫管/叶卡相关的一切。
  void BuildGlbOnlyScene(const CliArgs& args) {
    has_shape_ = false;
    ApplyCommonArgs(args);
    ground_mesh_ = RegisterMesh(jpov_skylight::MakeGroundQuad());
    ground_mat_ = jpov_skylight::GroundMaterial();
    view_ = jpov_viewer::DefaultView();
    view_.phi = args.phi_deg * 3.14159265358979323846 / 180.0;
    view_.theta = args.theta_deg * 3.14159265358979323846 / 180.0;
    if (args.radius > 0.0) {
      view_.R = args.radius;
    }
    LOG(INFO) << "纯 glb 查看模式"
              << (args.glb_only ? "（--glb-only：故意不加载 shape）"
                                : "（shape 不存在：" + args.shape_path + "）")
              << "：只有 glb 展示态，不能回到原材料态/不能烘焙";
  }

  // 一个 leaf type 的 debug 绘制单元：绿卡 + 卡面 UV 标签。
  // 两者用同一个 type 下标、同一份 shape 数据，所以永远不会"绿卡画了、标签没画"。
  // 烘焙一份 glb 并（成功后）切到 glb 展示态。
  void BakeNow() {
    if (!has_shape_) {
      bake_ok_ = false;
      bake_msg_ = "纯 glb 模式：没有 shape，无处烘焙";
      LOG(WARNING) << bake_msg_;
      return;
    }
    jpov::vegetation::GlbBakeOptions o;
    o.thick_spacing = bake_thick_spacing_;
    o.thin_spacing = bake_thin_spacing_;
    o.max_joints_per_branch = static_cast<int>(bake_max_joints_f_ + 0.5f);
    o.phase_scale = bake_phase_scale_;
    o.droop_scale = bake_droop_scale_;
    o.bark_dir = bark_dir_;
    o.leaf_png = leaf_path_;
    o.leaf2_png = leaf2_path_;
    o.leaf_black = bake_leaf_black_;
    o.leaf_normal = bake_leaf_normal_;
    o.normal_scale = bake_normal_scale_;
    o.uv_v_scale = args_uv_vscale_;   // 与预览一致（0 = 自动按树皮 albedo 尺寸）

    const std::string path = BakeOutputPath();
    std::string err;
    jpov::vegetation::GlbBakeStats stats;
    const bool ok = jpov::vegetation::BakeVegetationGlb(shape_, path, o, &stats, &err);
    bake_ok_ = ok;
    bake_ms_ = static_cast<float>(stats.milliseconds);
    if (!ok) {
      bake_msg_ = "失败: " + err;
      LOG(ERROR) << "烘焙 glb 失败: " << err;
      return;
    }
    bake_msg_ = "关节 " + std::to_string(stats.joints) + " / 树干 " +
                std::to_string(stats.trunk_vertices) + " v / 叶 " +
                std::to_string(stats.leaf_vertices) + " v";
    LOG(INFO) << "烘焙 glb 成功: " << path << "（" << bake_msg_ << "，" << bake_ms_ << " ms）";
    LoadGlbFile(path);
  }

  // 烘焙产物路径：优先命令行给的，否则写到 output/vegetation_v2/。
  std::string BakeOutputPath() const {
    if (!bake_out_path_.empty()) {
      return bake_out_path_;
    }
    const std::string dir = jpov::GetOutputDir() + "vegetation_v2";
    std::system(("mkdir -p '" + dir + "'").c_str());
    return dir + "/baked_vegetation.glb";
  }

  // 载入 glb 并切到展示态（失败保留原材料态）。
  // 返回 false = 载入失败（调用方据此决定是否继续：命令行给了 --glb 却打不开 ⇒ 退出码 1）。
  bool LoadGlbFile(const std::string& path) {
    ReleaseGlb();
    jpov::GltfObject obj = LoadGltf(path);
    if (obj.empty()) {
      bake_ok_ = false;
      bake_msg_ = "glb 载入失败: " + path;
      LOG(ERROR) << bake_msg_;
      return false;
    }
    glb_ = std::move(obj);
    glb_loaded_ = true;
    glb_path_ = path;
    if (glb_.bounds_valid) {
      const float h = glb_.bounds_max[1] - glb_.bounds_min[1];
      if (h > 1e-3f) {
        scene_height_ = h;
      }
      // 纯 glb 模式没有 shape 可供取景（BuildScene 里那段 fit），这里按 glb 包围盒补。
      // 有 shape 时不覆盖：那是原材料的构图，用户已经"看过一眼"了。
      if (!has_shape_ && args_radius_ <= 0.0f) {
        view_.R = jpov_viewer::ViewConfig::FitRadius(glb_.bounds_min, glb_.bounds_max, 60.0);
      }
    }
    mode_ = Mode::kGlb;
    LOG(INFO) << "进入 glb 展示态: " << path << "（" << glb_.size() << " 个 primitive）";
    return true;
  }

  void ReleaseGlb() {
    if (glb_loaded_) {
      ReleaseGltf(glb_);
      glb_ = jpov::GltfObject();
      glb_loaded_ = false;
    }
  }

  void BackToRaw() {
    if (!has_shape_) {
      LOG(WARNING) << "纯 glb 模式：没有原材料态可回";
      return;
    }
    ReleaseGlb();
    mode_ = Mode::kRaw;
    LOG(INFO) << "回到原材料态（shape/leaf/bark 现搭）";
  }

  // 当前风参数 → WindCommand（时间由帧号推，确定性）。
  jpov::WindCommand MakeWind() const {
    jpov::WindCommand w;
    w.time = wind_time_;
    w.amplitude = wind_amplitude_;
    w.frequency = wind_frequency_;
    const float az = wind_azimuth_deg_ * 3.14159265358979323846f / 180.0f;
    w.direction = jpov::Vec3f{std::cos(az), 0.0f, std::sin(az)};
    w.turb_amplitude = wind_turb_amp_;
    w.turb_frequency = wind_turb_freq_;
    w.gust_frequency = wind_gust_freq_;
    w.gust_strength = wind_gust_str_;
    w.blow_ratio = wind_blow_ratio_;
    w.blow_away_exponent = wind_blow_exp_;
    w.blow_ref_length = (scene_height_ > 1e-3f) ? scene_height_ : 1.0f;
    return w;
  }

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

      // 同一份来源（ComputeCardFrame）：标签永远和绿卡、真实叶卡对齐。
      const jpov::vegetation::CardFrame frame =
          jpov::vegetation::ComputeCardFrame(shape_, card);
      const Vec3d& up = frame.up;
      const Vec3d& n = frame.normal;
      const Vec3d& right = frame.right;
      const double au = frame.au;
      const double av = frame.av;

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
    // 面板待办（烘焙 / 回原材料态）：**在入队绘制命令之前**执行，避免释放本帧还在用的资源。
    if (pending_ == Pending::kBake) {
      pending_ = Pending::kNone;
      BakeNow();
    } else if (pending_ == Pending::kBackToRaw) {
      pending_ = Pending::kNone;
      BackToRaw();
    }

    // 风时间：**累加**而不是「帧号 × 缩放」——后者在拖动「时间缩放」滑条时会按比例
    // 跳变（时间倒退/前跳，树会瞬移）。固定步长 1/fps × 缩放 ⇒ 改缩放只改速度。
    // 暂停时冻结；headless 用 --wind-time 直接钉住初值（同样可复现）。
    if (!wind_paused_) {
      wind_time_ += (1.0f / kViewerFps) * wind_time_scale_;
    }
    (void)frame_count;
    // 叶卡唯一可调的呈现参数是法线强度（材质常量）；弯曲来自资产，不在这里改。
    if (has_leaf_) {
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

    if (mode_ == Mode::kGlb && glb_loaded_) {
      // glb 展示态：整份产物一次画出（树干 + 叶卡两个 primitive），带风。
      std::optional<jpov::WindCommand> wind;
      if (wind_on_) {
        wind = MakeWind();
      }
      cmds->DrawGltfObject(glb_, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},
                           /*scale=*/1.0f, /*highlight=*/false, /*picking_id=*/0, wind);
    }
    for (size_t i = 0; mode_ == Mode::kRaw && i < branch_mesh_.size(); ++i) {
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
    const bool debug_cards = mode_ == Mode::kRaw && !(has_leaf_ && show_leaf_);
    // 一个 type 一个绘制单元：type 过滤对「贴图 / 绿卡 / 标签」同时生效，
    // 不会再出现「绿卡还在、标签已经按 type 过滤掉」这种不一致。
    for (size_t t = 0; mode_ == Mode::kRaw && t < leaf_type_count_; ++t) {
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
      // 两行：第一行相机（复现截图用），第二行资产（判断"对齐"是数据问题还是相机问题）。
      // 深色描边由 DrawText 自己不做，所以用两遍绘制：先暗色偏移 1px 当描边，再画白字。
      // 天空是浅色时纯白字看不见 —— 这是 HUD 唯一的呈现要求。
      const jpov::Color shadow{0.0f, 0.0f, 0.0f, 0.85f};
      const jpov::Color ink{1.0f, 1.0f, 1.0f, 1.0f};
      cmds->DrawText(CamString(), {9.0f, 9.0f}, 18.0f, shadow,
                     jpov::TextAlignment::kTopLeft, jpov::kFontBuiltinCJK);
      cmds->DrawText(CamString(), {8.0f, 8.0f}, 18.0f, ink,
                     jpov::TextAlignment::kTopLeft, jpov::kFontBuiltinCJK);
      cmds->DrawText(AssetString(), {9.0f, 31.0f}, 16.0f, shadow,
                     jpov::TextAlignment::kTopLeft, jpov::kFontBuiltinCJK);
      cmds->DrawText(AssetString(), {8.0f, 30.0f}, 16.0f, ink,
                     jpov::TextAlignment::kTopLeft, jpov::kFontBuiltinCJK);
    }

    // ── 面板 ──
    // 左列（自底向上）：场景 /［原材料态］叶卡过滤 /［glb 态］风调试
    // 右列（自底向上）：烘焙 glb
    if (show_panel_) {
      const float w = static_cast<float>(kViewerWidth);
      const float h = static_cast<float>(kViewerHeight);
      jpov::UiTheme theme = jpov::UiTheme::Default(16.0f);
      theme.font_alias = jpov::kFontBuiltinCJK;
      ui_.Begin(input, theme, w, h, 1000.0f / kViewerFps);

      const float colw = 0.34f * w;
      const float xl = 0.02f * w;
      const float xr = w - 0.02f * w - colw;
      const bool raw = (mode_ == Mode::kRaw);

      // 一个面板 = 标题栏（可折叠）+ 展开时的 N 行；整列自底向上排。
      // draw_row(i, rect) 画第 i 行（24px 高、行距 32px）。
      auto panel = [&](float x, float* y_top, const char* title, bool* open, int rows,
                       const std::function<void(int, const jpov::UiRect&)>& draw_row) {
        // 自底向上排：-y_top 是这一块的**底边**，标题栏在最上面、展开行在它下面。
        const float hdr = 24.0f;
        const float body_h = *open ? (rows * 32.0f) : 0.0f;
        const float top = *y_top - (hdr + body_h);
        // 半透明底板：面板压在场景上，浅色地面时白字会糊掉。
        cmds->DrawFillRect({x - 8.0f, top - 6.0f}, {colw + 16.0f, hdr + body_h + 12.0f},
                           jpov::Color{0.0f, 0.0f, 0.0f, 0.55f},
                           jpov::Color{0.0f, 0.0f, 0.0f, 0.0f}, 0.0f, 6.0f);
        ui_.AccordionHeader(title, open, {{x, top}, {colw, hdr}});
        if (*open) {
          for (int i = 0; i < rows; ++i) {
            draw_row(i, {{x, top + hdr + i * 32.0f}, {colw, 24.0f}});
          }
        }
        *y_top = top - 10.0f;
      };

      // ── 左列 ──
      float yl = h - 16.0f;
      const int scene_rows = raw ? (has_bark_ ? 4 : 2) : 1;
      panel(xl, &yl, "场景", &scene_panel_open_, scene_rows,
            [&](int i, const jpov::UiRect& r) {
              if (i == 0) {
                ui_.SliderFloat("地面 y (m)", &ground_y_, r, -3.0f, 3.0f, 2);
              } else if (i == 1) {
                ui_.SliderFloat("最大层级", &max_level_f_, r, 0.0f, 20.0f, 0);
              } else if (i == 2) {
                ui_.Checkbox("树皮纹理", &show_bark_, r);
              } else {
                ui_.SliderFloat("法线强度", &normal_scale_, r, 0.0f, 3.0f, 2);
                bark_mat_.normal_scale = normal_scale_;
              }
            });

      if (raw) {
        panel(xl, &yl, "叶卡", &leaf_panel_open_, 5, [&](int i, const jpov::UiRect& r) {
          if (i == 0) {
            ui_.Checkbox("显示全部叶子", &show_all_card_types_, r);
          } else if (i == 1) {
            // 下标是【颜色桶】(type % kMaxCardTypes)，不是原始 type 编号（面板/HUD 一致）。
            const float label_w = 0.28f * colw;
            const jpov::UiRect box{{r.pos.x() + label_w, r.pos.y()},
                                   {r.size.x() - label_w, 24.0f}};
            cmds->DrawText("叶 type 桶", {r.pos.x(), r.pos.y() + 4.0f}, 16.0f,
                           jpov::Color{0.95f, 0.95f, 0.95f, 1.0f},
                           jpov::TextAlignment::kTopLeft, jpov::kFontBuiltinCJK);
            if (ui_.InputText("", type_buf_, sizeof(type_buf_), box)) {
              const int parsed = std::atoi(type_buf_);
              const int clamped =
                  std::max(0, std::min(static_cast<int>(leaf_type_count_) - 1, parsed));
              type_filter_ = static_cast<uint32_t>(clamped);
              if (clamped != parsed) {
                std::snprintf(type_buf_, sizeof(type_buf_), "%d", clamped);
              }
            }
          } else if (i == 2) {
            ui_.Checkbox("叶卡贴图", &show_leaf_, r);
          } else if (i == 3) {
            ui_.Checkbox("叶卡标签", &show_labels_, r);
          } else {
            ui_.SliderFloat("叶法线强度", &leaf_normal_scale_, r, 0.0f, 3.0f, 2);
          }
        });
      } else {
        // 风调试（glb 展示态）：参考 v1 wind viewer，精简到 10 行、近似方形。
        panel(xl, &yl, "风调试", &wind_panel_open_, 12,
              [&](int i, const jpov::UiRect& r) {
                if (i == 0) {
                  ui_.Checkbox("启用风", &wind_on_, r);
                } else if (i == 1) {
                  ui_.SliderFloat("风向 °", &wind_azimuth_deg_, r, 0.0f, 360.0f, 0);
                } else if (i == 2) {
                  ui_.SliderFloat("pivot 振幅", &wind_amplitude_, r, 0.0f, 1.5f, 2);
                } else if (i == 3) {
                  ui_.SliderFloat("pivot 频率 Hz", &wind_frequency_, r, 0.05f, 2.0f, 2);
                } else if (i == 4) {
                  ui_.SliderFloat("平湍 幅度", &wind_turb_amp_, r, 0.0f, 0.6f, 3);
                } else if (i == 5) {
                  ui_.SliderFloat("平湍 频率 Hz", &wind_turb_freq_, r, 0.0f, 8.0f, 2);
                } else if (i == 6) {
                  ui_.SliderFloat("阵风 频率 Hz", &wind_gust_freq_, r, 0.0f, 2.0f, 2);
                } else if (i == 7) {
                  ui_.SliderFloat("阵风 强度", &wind_gust_str_, r, 0.0f, 1.0f, 2);
                } else if (i == 8) {
                  ui_.SliderFloat("吹歪比例", &wind_blow_ratio_, r, 0.0f, 0.5f, 3);
                } else if (i == 9) {
                  ui_.SliderFloat("吹歪指数 n", &wind_blow_exp_, r, 1.0f, 4.0f, 1);
                } else if (i == 10) {
                  ui_.SliderFloat("时间缩放", &wind_time_scale_, r, 0.0f, 2.0f, 2);
                } else {
                  ui_.Checkbox("暂停", &wind_paused_, r);
                }
              });
      }

      // ── 右列：烘焙 glb ──
      float yr = h - 16.0f;
      const int bake_rows = 11;
      panel(xr, &yr, "烘焙 glb", &bake_panel_open_, bake_rows,
            [&](int i, const jpov::UiRect& r) {
              if (i == 0) {
                ui_.SliderFloat("粗枝关节间距 m", &bake_thick_spacing_, r, 0.5f, 4.0f, 2);
              } else if (i == 1) {
                ui_.SliderFloat("细枝关节间距 m", &bake_thin_spacing_, r, 0.1f, 2.0f, 2);
              } else if (i == 2) {
                ui_.SliderFloat("每枝最多关节", &bake_max_joints_f_, r, 1.0f, 16.0f, 0);
              } else if (i == 3) {
                ui_.SliderFloat("相位空间频率", &bake_phase_scale_, r, 0.2f, 3.0f, 2);
              } else if (i == 4) {
                ui_.SliderFloat("下垂软度", &bake_droop_scale_, r, 0.1f, 2.0f, 2);
              } else if (i == 5) {
                ui_.SliderFloat("叶卡黑阈", &bake_leaf_black_, r, 0.0f, 80.0f, 0);
              } else if (i == 6) {
                ui_.SliderFloat("叶法线强度", &bake_leaf_normal_, r, 0.0f, 3.0f, 2);
              } else if (i == 7) {
                ui_.SliderFloat("树皮法线强度", &bake_normal_scale_, r, 0.0f, 3.0f, 2);
              } else if (i == 8) {
                if (!has_shape_) {
                  // 纯 glb 模式没有 shape 可烘：给一行说明，不给按钮（点了也只能报错）。
                  cmds->DrawText("纯 glb 模式：无 shape，不能烘焙",
                                 {r.pos.x(), r.pos.y() + 4.0f}, 15.0f,
                                 jpov::Color{0.72f, 0.72f, 0.72f, 1.0f},
                                 jpov::TextAlignment::kTopLeft, jpov::kFontBuiltinCJK);
                } else if (ui_.Button(glb_loaded_ ? "重新烘焙 glb" : "烘焙 glb", r)) {
                  pending_ = Pending::kBake;
                }
              } else if (i == 9) {
                if (glb_loaded_ && has_shape_) {   // 纯 glb 模式没有原材料态可回
                  if (ui_.Button("回到原材料态", r)) {
                    pending_ = Pending::kBackToRaw;
                  }
                }
              } else {
                // 状态行：一行、短文本（长路径不放进来，日志/控制台里有）。
                char buf[160];
                if (bake_msg_.empty()) {
                  std::snprintf(buf, sizeof(buf), "%s",
                                glb_loaded_ ? "glb 已就绪（风调试在左下）" : "未烘焙");
                } else {
                  std::snprintf(buf, sizeof(buf), "%s · %.0f ms", bake_msg_.c_str(),
                                bake_ms_);
                }
                cmds->DrawText(buf, {r.pos.x(), r.pos.y() + 4.0f}, 15.0f,
                               bake_ok_ ? jpov::Color{0.75f, 1.0f, 0.75f, 1.0f}
                                        : jpov::Color{1.0f, 0.85f, 0.7f, 1.0f},
                               jpov::TextAlignment::kTopLeft, jpov::kFontBuiltinCJK);
              }
            });

      ui_.End();
      ui_.Emit(cmds);
      max_level_ = static_cast<int>(max_level_f_ + 0.5f);
    }
  }
};


}  // namespace

int RunViewer(const CliArgs& args) {
  JPOV::Config cfg;
  cfg.title = "JPOV - vegetation v2 viewer";
  cfg.width = kViewerWidth;
  cfg.height = kViewerHeight;
  // 烘焙 / capture 都不需要可见窗口（烘焙其实连 GL 都不用）。
  cfg.headless = args.capture || !args.bake_glb_path.empty();
  // 字体只用来画 HUD 和 debug 标签。缺字体不该让整个查看器崩掉（CI / 无字体环境）：
  // 有就注册，没有就降级成「不画文字」，由 app 自己把 HUD/标签关掉。
  // 字体要按顺序找几个位置：从仓库根跑（字体在 tools/jpov/fonts）和从 output 目录跑
  // （JPOV 会把字体复制到那里）都得找到 —— 否则 HUD 静默消失，截图里就没有相机参数，
  // 排查「树干对齐」这类问题就没了依据。
  const struct {
    const char* label;
    bool cjk;
    const char* candidates[3];
  } kWantFonts[] = {
      {"fonts/NotoSansCJK-Regular.ttc", true,
       {"fonts/NotoSansCJK-Regular.ttc", "tools/jpov/fonts/NotoSansCJK-Regular.ttc", nullptr}},
      {"fonts/DejaVuSans.ttf", false,
       {"fonts/DejaVuSans.ttf", "tools/jpov/fonts/DejaVuSans.ttf", nullptr}},
  };
  for (const auto& want : kWantFonts) {
    bool found = false;
    for (const char* candidate : want.candidates) {
      if (candidate == nullptr || !std::ifstream(candidate).good()) {
        continue;
      }
      cfg.fonts.push_back({candidate, 0,
                           want.cjk ? jpov::kFontBuiltinCJK : jpov::kFontBuiltinLatin});
      found = true;
      break;
    }
    if (!found) {
      LOG(WARNING) << "缺字体（跳过，不画文字）: " << want.label;
    }
  }

  VegetationV2App app(cfg);
  app.has_fonts_ = !cfg.fonts.empty();
  app.Init();
  // 纯 glb 查看模式：显式 `--glb-only`，或给了 `--glb` 但 shape 文件根本不存在。
  const bool has_shape_file = std::ifstream(args.shape_path).good() && !args.glb_only;
  if (has_shape_file) {
    std::string err;
    if (!jpov::vegetation::LoadShapeFile(args.shape_path, &app.shape_, &err)) {
      LOG(ERROR) << "shape 加载失败: " << err;
      return 1;
    }
    app.shape_path_ = args.shape_path;
    app.PrintLeafTypeHelp(args.shape_path);
    app.BuildScene(args);
  } else {
    // 纯 glb 查看模式（只有 --glb 才会走到这里；ParseCli 已经挡掉"既没 shape 又没 glb"）。
    app.BuildGlbOnlyScene(args);
  }

  // 只烘焙：一条命令跑完就退出（CI / 批量用）。
  if (!args.bake_glb_path.empty()) {
    app.BakeNow();
    const bool ok = app.bake_ok_;
    app.Shutdown();
    return ok ? 0 : 1;
  }
  // 直接进 glb 展示态（已有产物时跳过原材料态）。
  if (!args.glb_path.empty()) {
    if (!app.LoadGlbFile(args.glb_path)) {
      LOG(ERROR) << "打不开 --glb 指定的文件: " << args.glb_path;
      return 1;
    }
  }

  if (!args.capture) {
    app.Run();
    app.Shutdown();
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
  app.Shutdown();
  return 0;
}

}  // namespace vegv2
