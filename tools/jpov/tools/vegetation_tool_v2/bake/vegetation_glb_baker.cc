// 植被 GLB 烘焙器实现。约定与数据语义见头文件；风公式见
// docs/jpov_vegetation_wind_design.md。

#include "tools/jpov/tools/vegetation_tool_v2/bake/vegetation_glb_baker.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"
#include "tools/jpov/src/gltf_saver.h"
#include "tools/jpov/tools/vegetation_tool_v2/leaf_texture.h"
#include "tools/jpov/tools/vegetation_tool_v2/sweep.h"

namespace jpov {
namespace vegetation {
namespace {

constexpr double kPi = 3.14159265358979323846;

// 逐关节相位：**连续**低空间频率场（与 v1 wind_bake.py 同一条公式）。
// 不连续（哈希 / 逐枝常量）会让相邻顶点位移不一致 ⇒ 网格被撕开。
double SmoothPhase01(double x, double y, double z, double scale) {
  const double sx = x * scale;
  const double sy = y * scale;
  const double sz = z * scale;
  double v = 0.55 * std::sin(1.63 * sx + 0.41 * sz) +
             0.30 * std::sin(0.87 * sy + 1.12 * sz + 1.70) +
             0.15 * std::sin(1.41 * sx + 0.53 * sy + 2.90);
  v = std::max(-1.0, std::min(1.0, v));
  return 0.5 + 0.5 * v;
}

double Distance(const Vec3d& a, const Vec3d& b) {
  const double dx = a.x - b.x;
  const double dy = a.y - b.y;
  const double dz = a.z - b.z;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// 一根枝上的关节（保留采样点）。
struct BranchJoints {
  std::vector<uint32_t> kept;    // 保留的采样下标（升序，首元素必为 0）
  std::vector<double> arc;       // 每个保留点的累计弧长（米）
  std::vector<double> cum;       // 所有采样点的累计弧长
  std::vector<double> soft_at;   // 每个保留点的 soft（朝向继承度）
};

// 按弧长抽稀关节：m = clamp(round(len/spacing), 1, cap)，按下标均匀取 m+1 个。
BranchJoints BuildBranchJoints(const Branch& b, const GlbBakeOptions& opts) {
  BranchJoints out;
  const size_t n = b.samples.size();
  out.cum.assign(n, 0.0);
  for (size_t i = 1; i < n; ++i) {
    out.cum[i] = out.cum[i - 1] + Distance(b.samples[i - 1].position, b.samples[i].position);
  }
  const double len = out.cum.empty() ? 0.0 : out.cum.back();
  const bool thick = static_cast<int>(b.level) <= opts.thick_level_max;
  const double spacing = thick ? opts.thick_spacing : opts.thin_spacing;
  int m = 1;
  if (spacing > 1e-6) {
    m = static_cast<int>(std::lround(len / spacing));
  }
  m = std::max(1, std::min(opts.max_joints_per_branch, m));

  const size_t last = n - 1;
  std::vector<uint32_t> kept;
  for (int k = 0; k <= m; ++k) {
    const uint32_t idx = static_cast<uint32_t>(
        std::lround(static_cast<double>(k) * static_cast<double>(last) /
                    static_cast<double>(m)));
    if (kept.empty() || kept.back() != idx) {
      kept.push_back(idx);
    }
  }
  out.kept = kept;
  out.arc.resize(kept.size());
  for (size_t k = 0; k < kept.size(); ++k) {
    out.arc[k] = out.cum[kept[k]];
  }

  // soft：只有「下垂且极细」的枝才软（柳条）；沿枝从 0 渐变到 droop/droop_scale。
  double droop = b.samples.front().position.y - b.samples.back().position.y;
  double rsum = 0.0;
  for (const BranchSample& s : b.samples) {
    rsum += s.radius;
  }
  const double mean_radius = n > 0 ? rsum / static_cast<double>(n) : 0.0;
  double sp_soft = 0.0;
  if (len > 1e-6 && droop > opts.droop_min && mean_radius < opts.strand_radius_max) {
    sp_soft = std::max(0.0, std::min(1.0, droop / opts.droop_scale));
  }
  out.soft_at.resize(kept.size());
  for (size_t k = 0; k < kept.size(); ++k) {
    out.soft_at[k] = (len > 1e-6) ? sp_soft * (out.arc[k] / len) : 0.0;
  }
  return out;
}

// 关节表（写进 node extras；引擎按 7 float/关节解析）。
struct JointTable {
  std::vector<jpov::MeshData::WindJoint> joints;

  // 返回 joint 下标。
  int Add(const Vec3d& pivot, int parent, double level, double phase, double soft) {
    jpov::MeshData::WindJoint j;
    j.pivot[0] = static_cast<float>(pivot.x);
    j.pivot[1] = static_cast<float>(pivot.y);
    j.pivot[2] = static_cast<float>(pivot.z);
    j.parent = static_cast<float>(parent);
    j.level = static_cast<float>(level);
    j.phase = static_cast<float>(phase);
    j.soft = static_cast<float>(soft);
    joints.push_back(j);
    return static_cast<int>(joints.size()) - 1;
  }
};

// 把「枝 + 保留点下标」映射成关节下标（按 shape.branches 顺序建表）。
struct BranchJointMap {
  std::vector<BranchJoints> per_branch;         // 与 shape.branches 对齐
  std::vector<std::vector<int>> joint_of_kept;  // [branch][kept 序号] → 关节下标
};

int FindNearestSampleIndex(const Branch& b, const Vec3d& p) {
  int best = 0;
  double best_d = 1e300;
  for (size_t i = 0; i < b.samples.size(); ++i) {
    const double d = Distance(b.samples[i].position, p);
    if (d < best_d) {
      best_d = d;
      best = static_cast<int>(i);
    }
  }
  return best;
}

// 关节下标：覆盖「采样下标 i」的那个关节（floor by sample index）。
int JointCoveringSample(const BranchJoints& bj, const std::vector<int>& joint_of_kept,
                        uint32_t sample_index) {
  int k = 0;
  for (size_t q = 0; q < bj.kept.size(); ++q) {
    if (bj.kept[q] <= sample_index) {
      k = static_cast<int>(q);
    } else {
      break;
    }
  }
  return joint_of_kept[static_cast<size_t>(k)];
}

// 树皮 ORM：glTF 只有 metallicRoughness 三合一（R=AO / G=rough / B=metal）。
// 我们把「单独 roughness 灰度图」打包成 (255, rough, 0)：AO 全开、metallic=0。
std::vector<unsigned char> BuildOrmPngBytes(const std::string& roughness_png,
                                            std::string* error) {
  int w = 0;
  int h = 0;
  int comp = 0;
  unsigned char* px = stbi_load(roughness_png.c_str(), &w, &h, &comp, 1);
  if (px == nullptr) {
    if (error != nullptr) {
      *error = "读不到树皮 roughness: " + roughness_png;
    }
    return {};
  }
  std::vector<unsigned char> orm(static_cast<size_t>(w) * h * 4);
  for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
    orm[i * 4 + 0] = 255;
    orm[i * 4 + 1] = px[i];
    orm[i * 4 + 2] = 0;
    orm[i * 4 + 3] = 255;
  }
  stbi_image_free(px);
  std::vector<unsigned char> png;
  if (!EncodePng(orm, w, h, &png)) {
    if (error != nullptr) {
      *error = "ORM 编码失败";
    }
  }
  return png;
}

bool ReadPngSize(const std::string& path, int* w, int* h) {
  int comp = 0;
  if (!stbi_info(path.c_str(), w, h, &comp)) {
    return false;
  }
  return *w > 0 && *h > 0;
}

// vegetation::MeshData（工具层，flat float）→ jpov::MeshData（引擎层，Vec3f），
// 顺手打上顶点标志位。两套类型的尺寸语义不同（一个是 float 数、一个是顶点数），
// 历史上在这里踩过 size()/3 的坑，所以转换只写这一处。
jpov::MeshData ToEngineMesh(const MeshData& src, bool with_tangents) {
  jpov::MeshData dst;
  uint8_t flags = static_cast<uint8_t>(jpov::MeshVertexFlags::kPosition) |
                  static_cast<uint8_t>(jpov::MeshVertexFlags::kNormal) |
                  static_cast<uint8_t>(jpov::MeshVertexFlags::kUV);
  if (with_tangents) {
    flags |= static_cast<uint8_t>(jpov::MeshVertexFlags::kTangent);
  }
  const size_t n = src.VertexCount();
  dst.positions.reserve(n);
  dst.normals.reserve(n);
  dst.uvs.reserve(n);
  for (size_t v = 0; v < n; ++v) {
    dst.positions.push_back(jpov::Vec3f{src.positions[v * 3], src.positions[v * 3 + 1],
                                        src.positions[v * 3 + 2]});
    dst.normals.push_back(jpov::Vec3f{src.normals[v * 3], src.normals[v * 3 + 1],
                                      src.normals[v * 3 + 2]});
    dst.uvs.push_back(jpov::Vec2f{src.uvs[v * 2], src.uvs[v * 2 + 1]});
    if (with_tangents && src.tangents.size() >= n * 3) {
      dst.tangents.push_back(jpov::Vec3f{src.tangents[v * 3], src.tangents[v * 3 + 1],
                                         src.tangents[v * 3 + 2]});
    }
  }
  dst.flags = static_cast<jpov::MeshVertexFlags>(flags);
  dst.indices = src.indices;
  return dst;
}

// 给叶片网格补切线（= 卡片 +u 边，单位化）。BuildLeafCards 只出位置/法线/UV。
void AddCardTangents(MeshData* mesh) {
  const size_t vcount = mesh->VertexCount();
  mesh->tangents.assign(vcount * 3, 0.0f);
  for (size_t v = 0; v + 3 < vcount; v += 4) {
    const float ax = mesh->positions[(v + 1) * 3 + 0] - mesh->positions[v * 3 + 0];
    const float ay = mesh->positions[(v + 1) * 3 + 1] - mesh->positions[v * 3 + 1];
    const float az = mesh->positions[(v + 1) * 3 + 2] - mesh->positions[v * 3 + 2];
    const float len = std::sqrt(ax * ax + ay * ay + az * az);
    const float inv = (len > 1e-9f) ? 1.0f / len : 0.0f;
    for (size_t i = 0; i < 4; ++i) {
      mesh->tangents[(v + i) * 3 + 0] = ax * inv;
      mesh->tangents[(v + i) * 3 + 1] = ay * inv;
      mesh->tangents[(v + i) * 3 + 2] = az * inv;
    }
  }
}

}  // namespace

bool BakeVegetationGlb(const Shape& shape, const std::string& out_path,
                       const GlbBakeOptions& opts, GlbBakeStats* stats,
                       std::string* error) {
  auto fail = [error](const std::string& why) {
    if (error != nullptr) {
      *error = why;
    }
    return false;
  };
  if (out_path.empty()) {
    return fail("BakeVegetationGlb: 输出路径为空");
  }
  if (shape.branches.empty()) {
    return fail("BakeVegetationGlb: shape 没有枝条");
  }
  const auto t0 = std::chrono::steady_clock::now();

  // ── 1. 关节表 ──
  BranchJointMap map;
  map.per_branch.resize(shape.branches.size());
  map.joint_of_kept.resize(shape.branches.size());

  // 枝 id → 下标（Validate 保证 id 拓扑序，但这里仍按 id 查，避免假设下标==id）。
  std::vector<int> branch_index_by_id;
  {
    uint32_t max_id = 0;
    for (const Branch& b : shape.branches) {
      max_id = std::max(max_id, b.id);
    }
    branch_index_by_id.assign(max_id + 1, -1);
    for (size_t i = 0; i < shape.branches.size(); ++i) {
      branch_index_by_id[shape.branches[i].id] = static_cast<int>(i);
    }
  }

  JointTable table;
  for (size_t bi = 0; bi < shape.branches.size(); ++bi) {
    const Branch& b = shape.branches[bi];
    BranchJoints bj = BuildBranchJoints(b, opts);
    std::vector<int> joint_of_kept(bj.kept.size(), -1);

    for (size_t k = 0; k < bj.kept.size(); ++k) {
      const Vec3d& pivot = b.samples[bj.kept[k]].position;
      int parent = -1;
      if (k > 0) {
        parent = joint_of_kept[k - 1];
      } else if (b.parent >= 0 && static_cast<size_t>(b.parent) < branch_index_by_id.size() &&
                 branch_index_by_id[b.parent] >= 0) {
        // 分叉处：挂到父枝上**离本条枝首最近**的关节。父枝与子枝是两根独立的管子，
        // 挂点必须落在父枝链上，否则子枝的父链断掉（整根枝会飞出去）。
        const int pi = branch_index_by_id[b.parent];
        const BranchJoints& pbj = map.per_branch[static_cast<size_t>(pi)];
        const int nearest = FindNearestSampleIndex(shape.branches[static_cast<size_t>(pi)],
                                                   pivot);
        int q = 0;
        for (size_t kk = 0; kk < pbj.kept.size(); ++kk) {
          if (static_cast<int>(pbj.kept[kk]) <= nearest) {
            q = static_cast<int>(kk);
          } else {
            break;
          }
        }
        parent = map.joint_of_kept[static_cast<size_t>(pi)][static_cast<size_t>(q)];
      }
      // level = 距根的**关节数**（shader 端 clamp 到 [0,3]，这里如实写）。
      double level = 0.0;
      if (parent >= 0) {
        level = table.joints[static_cast<size_t>(parent)].level + 1.0;
      }
      const double phase = SmoothPhase01(pivot.x, pivot.y, pivot.z, opts.phase_scale);
      joint_of_kept[k] = table.Add(pivot, parent, level, phase, bj.soft_at[k]);
    }
    map.joint_of_kept[bi] = std::move(joint_of_kept);
    map.per_branch[bi] = std::move(bj);
  }

  // ── 2. 树干网格 + 逐顶点 (joint_id, phase) ──
  SweepOptions sweep_opts;
  if (opts.uv_v_scale > 0.0) {
    sweep_opts.uv_v_scale = opts.uv_v_scale;
  } else if (!opts.bark_dir.empty()) {
    int tw = 0;
    int th = 0;
    if (ReadPngSize(opts.bark_dir + "/albedo.png", &tw, &th)) {
      sweep_opts.uv_v_scale = static_cast<double>(tw) / static_cast<double>(th);
    }
  }
  LOG(INFO) << "烘焙: uv_v_scale = " << sweep_opts.uv_v_scale
            << "（0 = 按 bark albedo 自动）";
  MeshData trunk_raw = SweepBranches(shape, sweep_opts);
  std::vector<Vec2f> trunk_wind(trunk_raw.VertexCount());
  {
    size_t v = 0;
    for (size_t bi = 0; bi < shape.branches.size(); ++bi) {
      const Branch& b = shape.branches[bi];
      const uint32_t stride = b.EffectiveRingCount() + 1;
      const BranchJoints& bj = map.per_branch[bi];
      for (size_t i = 0; i < b.samples.size(); ++i) {
        const int jid = JointCoveringSample(bj, map.joint_of_kept[bi], static_cast<uint32_t>(i));
        const double phase =
            SmoothPhase01(b.samples[i].position.x, b.samples[i].position.y,
                          b.samples[i].position.z, opts.phase_scale);
        for (uint32_t k = 0; k < stride; ++k) {
          trunk_wind[v + k] = Vec2f(static_cast<float>(jid), static_cast<float>(phase));
        }
        v += stride;
      }
    }
    CHECK_EQ(v, trunk_raw.VertexCount()) << "烘焙: 树干顶点布局与 shape 不一致";
  }
  jpov::MeshData trunk = ToEngineMesh(trunk_raw, /*with_tangents=*/true);
  trunk.wind_link = std::move(trunk_wind);
  trunk.wind_joints = table.joints;
  trunk.flags = static_cast<jpov::MeshVertexFlags>(
      static_cast<uint8_t>(trunk.flags) | static_cast<uint8_t>(jpov::MeshVertexFlags::kWind));

  double card_attach_max = 0.0;   // 叶卡 anchor 到其关节的最大距离（诊断：是否挂错枝）
  std::vector<GltfSaveMesh> meshes;
  const int trunk_vertices = static_cast<int>(trunk.VertexCount());
  int trunk_triangles = static_cast<int>(trunk.indices.size() / 3);

  // ── 3. 材质 ──
  // 树皮：albedo / normal 走文件（保存器内嵌），roughness 打包成 ORM。
  GltfMaterialInfo bark_mi;
  if (!opts.bark_dir.empty()) {
    const std::string albedo = opts.bark_dir + "/albedo.png";
    const std::string normal = opts.bark_dir + "/normal.png";
    const std::string rough = opts.bark_dir + "/roughness.png";
    bark_mi.base_color_tex.uri = albedo;
    bark_mi.base_color_tex.key = albedo;
    bark_mi.normal_tex.uri = normal;
    bark_mi.normal_tex.key = normal;
    bark_mi.normal_scale = static_cast<float>(opts.normal_scale);
    bark_mi.metallic_factor = 0.0f;
    bark_mi.roughness_factor = 1.0f;
    const std::vector<unsigned char> orm = BuildOrmPngBytes(rough, error);
    if (orm.empty()) {
      return false;
    }
    bark_mi.metallic_roughness_tex.bytes = orm;
    bark_mi.metallic_roughness_tex.key = "mem:orm:bark";
  } else {
    // 纯色树干（没有材质贴图时给个中性棕，和查看器 debug 配色接近）。
    bark_mi.base_color[0] = 0.32f;
    bark_mi.base_color[1] = 0.24f;
    bark_mi.base_color[2] = 0.18f;
    bark_mi.base_color[3] = 1.0f;
    bark_mi.metallic_factor = 0.0f;
    bark_mi.roughness_factor = 0.85f;
  }
  meshes.push_back(GltfSaveMesh{std::move(trunk), bark_mi});

  // ── 4. 叶卡网格 + 材质 ──
  int leaf_vertices = 0;
  if (!opts.leaf_png.empty()) {
    LeafTexture tex;
    if (!LoadLeafTextureFromPng(opts.leaf_png, static_cast<float>(opts.leaf_black), 6, &tex,
                                error)) {
      return false;
    }
    MeshData leaves_raw = BuildLeafCards(shape);
    AddCardTangents(&leaves_raw);
    std::vector<Vec2f> leaf_wind(leaves_raw.VertexCount());
    for (size_t c = 0, v = 0; c < shape.cards.size(); ++c, v += 4) {
      const LeafCard& card = shape.cards[c];
      // 卡片挂到「**全局**最近关节」：整张卡共用同一 joint/phase ⇒ 刚性、不会被软度梯度
      // 剪开，也不会脱离挂点。
      // 为什么不按 card.branch 查：branch/s 在格式里只是**溯源**（渲染不依赖），导出的
      // 最近骨架点可能落在相邻的枝上；一旦信它，个别卡会挂到别的枝链上，风一吹就飞到
      // 别处（实测：一张卡被吹成天上的一条细线）。按距离找关节不依赖那份溯源信息。
      int jid = 0;
      double phase = 0.0;
      {
        // 找**最近的枝条采样点**（= 这张卡实际挂在哪根管子的哪一环），再用该环的
        // 覆盖关节。这样卡片与它旁边那一圈顶点**用同一条链**，风里不会各转各的
        // （离得最近的「关节 pivot」可能属于隔壁那一节，卡片会因此绕错支点、飘走）。
        double best = 1e300;
        size_t best_bi = 0;
        uint32_t best_si = 0;
        for (size_t bi = 0; bi < shape.branches.size(); ++bi) {
          const Branch& b = shape.branches[bi];
          for (size_t si = 0; si < b.samples.size(); ++si) {
            const double d = Distance(b.samples[si].position, card.anchor);
            if (d < best) {
              best = d;
              best_bi = bi;
              best_si = static_cast<uint32_t>(si);
            }
          }
        }
        jid = JointCoveringSample(map.per_branch[best_bi], map.joint_of_kept[best_bi], best_si);
        const Vec3d& pv = shape.branches[best_bi].samples[best_si].position;
        phase = SmoothPhase01(pv.x, pv.y, pv.z, opts.phase_scale);
        card_attach_max = std::max(card_attach_max, best);
      }
      for (size_t k = 0; k < 4; ++k) {
        leaf_wind[v + k] = Vec2f(static_cast<float>(jid), static_cast<float>(phase));
      }
    }
    jpov::MeshData leaves = ToEngineMesh(leaves_raw, /*with_tangents=*/true);
    leaves.wind_link = std::move(leaf_wind);
    leaves.wind_joints = table.joints;
    leaves.flags = static_cast<jpov::MeshVertexFlags>(
        static_cast<uint8_t>(leaves.flags) | static_cast<uint8_t>(jpov::MeshVertexFlags::kWind));

    // 叶卡材质：albedo（黑底→alpha 后、**行翻转**写进 glb）+ 由 alpha/luma 生成的法线。
    std::vector<unsigned char> albedo_rgba = tex.rgba;
    std::vector<unsigned char> normal_rgba = BuildLeafNormalMap(tex, 4.0f);
    FlipRows(&albedo_rgba, tex.width, tex.height);
    FlipRows(&normal_rgba, tex.width, tex.height);

    GltfMaterialInfo leaf_mi;
    if (!EncodePng(albedo_rgba, tex.width, tex.height, &leaf_mi.base_color_tex.bytes)) {
      return fail("叶卡 albedo 编码失败");
    }
    leaf_mi.base_color_tex.key = "mem:leaf:albedo";
    if (!EncodePng(normal_rgba, tex.width, tex.height, &leaf_mi.normal_tex.bytes)) {
      return fail("叶卡法线编码失败");
    }
    leaf_mi.normal_tex.key = "mem:leaf:normal";
    leaf_mi.normal_scale = static_cast<float>(opts.leaf_normal);
    leaf_mi.alpha_mode = AlphaMode::kMask;
    leaf_mi.alpha_cutoff = kLeafAlphaCutoff;
    leaf_mi.double_sided = true;
    leaf_mi.metallic_factor = 0.0f;
    leaf_mi.roughness_factor = 0.85f;
    leaf_vertices = static_cast<int>(leaves.VertexCount());
    LOG(INFO) << "烘焙: 叶卡 anchor→所属环 最大距离 = " << card_attach_max << " m";
    meshes.push_back(GltfSaveMesh{std::move(leaves), leaf_mi});
  }

  // ── 5. 写盘 ──
  if (!meshes.empty() && meshes.size() > 1) {
    trunk_triangles += static_cast<int>(meshes[1].mesh.indices.size() / 3);
  }
  GltfSaveAsset asset;
  asset.name = "vegetation";
  asset.meshes = std::move(meshes);
  if (!WriteGlb(asset, out_path)) {
    return fail("写 GLB 失败: " + out_path);
  }

  const auto t1 = std::chrono::steady_clock::now();
  if (stats != nullptr) {
    stats->joints = static_cast<int>(table.joints.size());
    stats->trunk_vertices = trunk_vertices;
    stats->leaf_vertices = leaf_vertices;
    stats->triangles = trunk_triangles;
    stats->milliseconds =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    stats->path = out_path;
  }
  return true;
}

}  // namespace vegetation
}  // namespace jpov
