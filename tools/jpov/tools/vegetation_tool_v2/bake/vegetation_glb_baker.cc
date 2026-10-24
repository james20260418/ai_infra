// 植被 GLB 烘焙器实现。约定与数据语义见头文件；风公式见
// docs/jpov_vegetation_wind_design.md。

#include "tools/jpov/tools/vegetation_tool_v2/bake/vegetation_glb_baker.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"
#include "tools/jpov/src/gltf_saver.h"
#include "tools/jpov/tools/vegetation_tool_v2/bake/vegetation_glb_joints.h"
#include "tools/jpov/tools/vegetation_tool_v2/leaf_texture.h"
#include "tools/jpov/tools/vegetation_tool_v2/sweep.h"

namespace jpov {
namespace vegetation {
namespace {

// 与查看器一致：最多 4 个叶卡颜色桶（`type % 4`）。烘焙按桶出 primitive。
constexpr int kLeafBuckets = 4;

double Distance(const Vec3d& a, const Vec3d& b) {
  const double dx = a.x - b.x;
  const double dy = a.y - b.y;
  const double dz = a.z - b.z;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
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

// 内嵌贴图的内容哈希（saver 用它做"同一张图只落一份"的去重键）。
// 用常量键（如 "mem:leaf:albedo"）在当前只烘一种叶子时看不出问题，但一旦将来多套叶卡
// 共用同一份 bake，saver 会把不同像素去重成同一张图 —— 所以键必须跟内容走。
std::string ContentKey(const char* tag, const std::vector<unsigned char>& bytes) {
  uint32_t h = 2166136261u;   // FNV-1a
  for (unsigned char c : bytes) {
    h ^= c;
    h *= 16777619u;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%s_%08x", tag, h);
  return buf;
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
  // 构造细节（抽稀/soft/父子链）全在 vegetation_glb_joints.cc，这里只用结果。
  JointLayout layout = BuildJointLayout(shape, opts);

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
      const BranchJoints& bj = layout.per_branch[bi];
      for (size_t i = 0; i < b.samples.size(); ++i) {
        const int jid = JointCoveringSample(bj, layout.joint_of_kept[bi], static_cast<uint32_t>(i));
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
  trunk.wind_joints = layout.joints;
  trunk.flags = static_cast<jpov::MeshVertexFlags>(
      static_cast<uint8_t>(trunk.flags) | static_cast<uint8_t>(jpov::MeshVertexFlags::kWind));

  double card_attach_max = 0.0;   // 叶卡 anchor 到其关节的最大距离（诊断：是否挂错枝）
  std::vector<GltfSaveMesh> meshes;
  const int trunk_vertices = static_cast<int>(trunk.VertexCount());
  int total_triangles = static_cast<int>(trunk.indices.size() / 3);

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
    bark_mi.metallic_roughness_tex.key = ContentKey("bark_orm", orm);
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

  // ── 4. 叶卡网格 + 材质（**逐 leaf type 一个 primitive**）──
  // 一棵树可以有内容完全不同的多类卡（叶簇 / 柔荑花序 / 枯叶），它们绑不同 atlas。
  // 只烘一类的做法会让 2-type 资产静默用同一张贴图（镜像手性也白拆），所以按
  // `type % kMaxCardTypes` 分桶、每桶一个 primitive + 一份材质。
  int leaf_vertices = 0;
  if (!opts.leaf_png.empty()) {
    // 素材：type 0 用 leaf_png，type 1 用 leaf2_png（缺省回退）——与查看器一致。
    std::string paths[kLeafBuckets];
    for (int b = 0; b < kLeafBuckets; ++b) {
      paths[b] = (b == 1 && !opts.leaf2_png.empty()) ? opts.leaf2_png : opts.leaf_png;
    }
    // 每种素材只处理一次（内存/时间都省；同素材的桶共用一份 PNG 字节）。
    std::map<std::string, jpov::GltfMaterialInfo> mat_by_path;

    for (int bucket = 0; bucket < kLeafBuckets; ++bucket) {
      Shape subset = shape;
      subset.cards.clear();
      for (const LeafCard& c : shape.cards) {
        if (static_cast<int>(c.type % kLeafBuckets) == bucket) {
          subset.cards.push_back(c);
        }
      }
      if (subset.cards.empty()) {
        continue;
      }

      // BuildLeafCards 自带切线（∂P/∂u，解析求得）——弯曲卡的切线不是「卡片 +u 边」了，
      // 差分近似会把曲率吃进去，所以别再自己算。
      MeshData leaves_raw = BuildLeafCards(subset);
      std::vector<Vec2f> leaf_wind(leaves_raw.VertexCount());
      double card_attach_max_bucket = 0.0;
      // 每张卡占 (seg+1)² 个顶点，seg 随该 type 的弯曲参数变（平面仍是 4 个顶点）。
      // 这里必须逐卡问 CardVertexCount，**不能假设恒等于 4**。
      size_t card_vertex_begin = 0;
      for (size_t c = 0; c < subset.cards.size(); ++c) {
        const LeafCard& card = subset.cards[c];
        const uint32_t card_vertices = CardVertexCount(subset, card);
        // 卡片挂到「最近的枝条采样点」（它实际挂在哪根管子的哪一环），再用该环的
        // 覆盖关节。这样卡片与它旁边那一圈顶点**用同一条链**，风里不会各转各的。
        int jid = 0;
        double phase = 0.0;
        {
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
          jid = JointCoveringSample(layout.per_branch[best_bi], layout.joint_of_kept[best_bi],
                                    best_si);
          const Vec3d& pv = shape.branches[best_bi].samples[best_si].position;
          phase = SmoothPhase01(pv.x, pv.y, pv.z, opts.phase_scale);
          card_attach_max_bucket = std::max(card_attach_max_bucket, best);
        }
        for (size_t k = 0; k < card_vertices; ++k) {
          leaf_wind[card_vertex_begin + k] =
              Vec2f(static_cast<float>(jid), static_cast<float>(phase));
        }
        card_vertex_begin += card_vertices;
      }
      if (card_vertex_begin != leaves_raw.VertexCount()) {
        // 顶点数与「逐卡步进」对不上 = 有一方改了布局另一方没跟上（风数据会串到别的卡上）。
        return fail("叶卡顶点数 " + std::to_string(leaves_raw.VertexCount()) +
                    " 与逐卡步进累计 " + std::to_string(card_vertex_begin) + " 不一致");
      }
      card_attach_max = std::max(card_attach_max, card_attach_max_bucket);

      jpov::MeshData leaves = ToEngineMesh(leaves_raw, /*with_tangents=*/true);
      leaves.wind_link = std::move(leaf_wind);
      leaves.wind_joints = layout.joints;
      leaves.flags = static_cast<jpov::MeshVertexFlags>(
          static_cast<uint8_t>(leaves.flags) | static_cast<uint8_t>(jpov::MeshVertexFlags::kWind));

      const std::string& png_path = paths[bucket];
      auto it = mat_by_path.find(png_path);
      if (it == mat_by_path.end()) {
        LeafTexture tex;
        if (!LoadLeafTextureFromPng(png_path, static_cast<float>(opts.leaf_black), 6, &tex,
                                    error)) {
          return false;
        }
        // 叶卡材质：albedo（黑底→alpha 后、**行翻转**写进 glb）+ 由 alpha/luma 生成的法线。
        std::vector<unsigned char> albedo_rgba = tex.rgba;
        std::vector<unsigned char> normal_rgba = BuildLeafNormalMap(tex, 4.0f);
        FlipRows(&albedo_rgba, tex.width, tex.height);
        FlipRows(&normal_rgba, tex.width, tex.height);

        GltfMaterialInfo leaf_mi;
        if (!EncodePng(albedo_rgba, tex.width, tex.height, &leaf_mi.base_color_tex.bytes)) {
          return fail("叶卡 albedo 编码失败: " + png_path);
        }
        leaf_mi.base_color_tex.key = ContentKey("leaf_albedo", leaf_mi.base_color_tex.bytes);
        if (!EncodePng(normal_rgba, tex.width, tex.height, &leaf_mi.normal_tex.bytes)) {
          return fail("叶卡法线编码失败: " + png_path);
        }
        leaf_mi.normal_tex.key = ContentKey("leaf_normal", leaf_mi.normal_tex.bytes);
        leaf_mi.normal_scale = static_cast<float>(opts.leaf_normal);
        leaf_mi.alpha_mode = AlphaMode::kMask;
        leaf_mi.alpha_cutoff = kLeafAlphaCutoff;
        leaf_mi.double_sided = true;
        leaf_mi.metallic_factor = 0.0f;
        leaf_mi.roughness_factor = 0.85f;
        it = mat_by_path.emplace(png_path, std::move(leaf_mi)).first;
      }
      leaf_vertices += static_cast<int>(leaves.VertexCount());
      meshes.push_back(GltfSaveMesh{std::move(leaves), it->second});
    }
    LOG(INFO) << "烘焙: 叶卡 anchor→所属环 最大距离 = " << card_attach_max << " m";
  }

  // ── 5. 写盘 ──
  // 三角形数是**整份 GLB 的总数**：树干 + 每个叶卡 bucket（按 type 分桶后不止一个
  // primitive）。字段口径见 bake_options.h 的 GlbBakeStats（顶点数分树干/叶子，三角形数不分）。
  for (size_t i = 1; i < meshes.size(); ++i) {
    total_triangles += static_cast<int>(meshes[i].mesh.indices.size() / 3);
  }
  GltfSaveAsset asset;
  asset.name = "vegetation";
  asset.meshes = std::move(meshes);
  if (!WriteGlb(asset, out_path)) {
    return fail("写 GLB 失败: " + out_path);
  }

  const auto t1 = std::chrono::steady_clock::now();
  if (stats != nullptr) {
    stats->joints = static_cast<int>(layout.joints.size());
    stats->trunk_vertices = trunk_vertices;
    stats->leaf_vertices = leaf_vertices;
    stats->triangles = total_triangles;
    stats->milliseconds =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    stats->path = out_path;
  }
  return true;
}

}  // namespace vegetation
}  // namespace jpov
