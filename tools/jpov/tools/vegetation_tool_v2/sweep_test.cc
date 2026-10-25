// 扫管的单元测试：几何 + UV 的**数值**判据。
//
// 这里刻意不比对像素或截图，只查可证伪的几何不变量：
//   * 等距性：同一个三角形里，两条边的「UV 长度 / 世界长度」必须相等（格子保持方形）
//   * v 沿枝单调、且 dv/ds = 1/tile（非方贴图时再乘 uv_v_scale）
//   * 每圈的 u 恰好覆盖 [0,1]（一圈一张图，接缝在 u=1 处闭合）
//   * 子枝入口 v 从父枝出口接续（接口处没有横向重复线）
//   * 叶卡：平面 4 顶点 / 2 三角形 / UV 四角 / 宽度来自材质；弯曲卡 25 顶点 / 32 三角形，
//            anchor 恒不动、法线与切线严格正交、细分只影响轮廓

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "tools/jpov/tools/vegetation_tool_v2/shape.h"
#include "tools/jpov/tools/vegetation_tool_v2/sweep.h"

namespace jpov {
namespace vegetation {
namespace {

constexpr double kPi = 3.14159265358979323846;
int g_failures = 0;

void Expect(bool cond, const std::string& what) {
  if (!cond) {
    ++g_failures;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

bool Near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

struct P3 {
  double x, y, z;
};

P3 GetPos(const MeshData& m, uint32_t v) {
  return P3{m.positions[v * 3], m.positions[v * 3 + 1], m.positions[v * 3 + 2]};
}

struct P2 {
  double u, v;
};

P2 GetUV(const MeshData& m, uint32_t v) { return P2{m.uvs[v * 2], m.uvs[v * 2 + 1]}; }

double Dist3(const P3& a, const P3& b) {
  return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) +
                   (a.z - b.z) * (a.z - b.z));
}

double Dist2(const P2& a, const P2& b) {
  return std::sqrt((a.u - b.u) * (a.u - b.u) + (a.v - b.v) * (a.v - b.v));
}

// 一根竖直的枝：从 (0,0,0) 沿 +Y 长 length，逐点同一半径。
Shape StraightBranch(int samples, double radius, double length, uint32_t ring) {
  Shape shape;
  Branch b;
  b.id = 0;
  b.parent = -1;
  b.level = 0;
  b.ring_count = ring;
  b.seam_index = 0;
  for (int i = 0; i < samples; ++i) {
    BranchSample s;
    s.position = Vec3d{0.0, length * i / (samples - 1), 0.0};
    s.radius = radius;
    b.samples.push_back(s);
  }
  shape.branches.push_back(b);
  return shape;
}

// 等距性：每个三角形里两条边的「UV/世界」比例必须一致。返回最差偏差（相对值）。
double TestIsotropy(const Shape& shape, const char* label, double tolerance) {
  const MeshData mesh = SweepBranches(shape);
  Expect(!mesh.indices.empty(), std::string(label) + ": 应产出三角形");

  double worst = 0.0;
  for (size_t t = 0; t < mesh.indices.size(); t += 3) {
    const uint32_t a = mesh.indices[t];
    const uint32_t b = mesh.indices[t + 1];
    const uint32_t c = mesh.indices[t + 2];
    const double d_ab = Dist3(GetPos(mesh, a), GetPos(mesh, b));
    const double d_ac = Dist3(GetPos(mesh, a), GetPos(mesh, c));
    if (d_ab < 1e-9 || d_ac < 1e-9) {
      continue;
    }
    const double uv_ab = Dist2(GetUV(mesh, a), GetUV(mesh, b));
    const double uv_ac = Dist2(GetUV(mesh, a), GetUV(mesh, c));
    const double ratio_ab = uv_ab / d_ab;
    const double ratio_ac = uv_ac / d_ac;
    worst = std::fmax(worst, std::fabs(ratio_ab - ratio_ac) / ratio_ac);
  }
  // 1.5% 容差：多边形弦长与环向分段带来的离散误差，不是逻辑错误。
  Expect(worst < tolerance, std::string(label) + ": 三角形内 UV 应等距（格子保持方形），"
                                                "最大偏差 " + std::to_string(worst));
  return worst;
}

void TestCylinderUV() {
  const double radius = 0.15;
  const uint32_t ring = 12;
  Shape shape = StraightBranch(5, radius, 2.0, ring);
  const MeshData mesh = SweepBranches(shape);

  // 顶点数：(samples) × (ring+1)
  Expect(mesh.VertexCount() == 5 * (ring + 1), "每圈应为 ring+1 个顶点（接缝重复一个）");
  Expect(mesh.TriangleCount() == 4 * ring * 2, "4 段 × ring 个四边形 × 2 三角形");

  // 每圈 u 必须覆盖 [0,1]
  for (uint32_t i = 0; i < 5; ++i) {
    double umin = 1e9;
    double umax = -1e9;
    for (uint32_t j = 0; j <= ring; ++j) {
      const P2 uv = GetUV(mesh, i * (ring + 1) + j);
      umin = std::fmin(umin, uv.u);
      umax = std::fmax(umax, uv.u);
    }
    Expect(Near(umin, 0.0, 1e-6) && Near(umax, 1.0, 1e-6),
           "第 " + std::to_string(i) + " 圈 u 必须恰好 [0,1]（接缝闭合）");
  }

  // v 沿枝单调，且 dv/ds = 1/tile
  const double unit_perimeter = ring * 2.0 * std::sin(kPi / ring);
  const double tile = radius * unit_perimeter;
  for (uint32_t i = 1; i < 5; ++i) {
    const double v0 = GetUV(mesh, (i - 1) * (ring + 1)).v;
    const double v1 = GetUV(mesh, i * (ring + 1)).v;
    Expect(v1 > v0, "v 必须沿枝单调递增");
    const double ds = 2.0 / 4.0;
    Expect(Near((v1 - v0) / ds, 1.0 / tile, 1e-3),
           "dv/ds 必须等于 1/tile（tile = radius × 单位周长）");
  }

  TestIsotropy(shape, "等半径圆柱", 0.015);

  // 切线（法线贴图 TBN 用）：必须单位长、与法线正交。这两条不成立时，
  // 法线贴图会整体偏色或明暗反转，但**画面仍然"有纹理"**，肉眼很难判断。
  double worst_len = 0.0;
  double worst_dot = 0.0;
  for (uint32_t v = 0; v < mesh.VertexCount(); ++v) {
    const double tx = mesh.tangents[v * 3];
    const double ty = mesh.tangents[v * 3 + 1];
    const double tz = mesh.tangents[v * 3 + 2];
    const double nx = mesh.normals[v * 3];
    const double ny = mesh.normals[v * 3 + 1];
    const double nz = mesh.normals[v * 3 + 2];
    const double len = std::sqrt(tx * tx + ty * ty + tz * tz);
    worst_len = std::fmax(worst_len, std::fabs(len - 1.0));
    worst_dot = std::fmax(worst_dot, std::fabs(tx * nx + ty * ny + tz * nz));
  }
  Expect(worst_len < 1e-4, "切线必须是单位长，最大偏差 " + std::to_string(worst_len));
  Expect(worst_dot < 1e-4, "切线必须与法线正交，最大 |T·N| = " + std::to_string(worst_dot));
}

void TestTaper() {
  // 锥度是"一圈一张图"的固有代价：u 是角向的（与半径无关），而世界宽度随半径缩小，
  // 所以每一段的横向/纵向比例会差 (r_上/r_下)。这不是 bug，是选择——
  // 换来的是「接缝永远闭合 + 一圈正好一张图」。
  //
  // 这里把这条行为钉住：轻微锥度必须几乎无损，强锥度必须按比例变差。
  const auto make = [](double r0, double r1) {
    Shape shape = StraightBranch(6, r0, 2.0, 12);
    for (size_t i = 0; i < shape.branches[0].samples.size(); ++i) {
      const double t = static_cast<double>(i) / 5.0;
      shape.branches[0].samples[i].radius = r0 + (r1 - r0) * t;
    }
    return shape;
  };

  const double mild = TestIsotropy(make(0.150, 0.143), "轻微锥度（5%）", 0.03);
  const double strong = TestIsotropy(make(0.150, 0.050), "强锥度（3x）", 0.30);
  Expect(strong > mild * 3.0,
         "强锥度的 UV 偏差必须明显大于轻微锥度（证明偏差确实来自锥度，而不是别处）");

  // 强锥度下另两条性质仍然必须成立
  const MeshData mesh = SweepBranches(make(0.150, 0.050));
  for (uint32_t j = 0; j <= 12; ++j) {
    const P2 uv = GetUV(mesh, j);
    Expect(uv.u >= -1e-6 && uv.u <= 1.0 + 1e-6, "锥度枝 u 仍在 [0,1]");
  }
}

// 非方形贴图：v 乘 uv_v_scale（等价于 tile 除以它），u 完全不受影响。
// 这条钉住的是「贴图长宽比补偿只动 v」——若哪天顺手把 u 也缩了，
// 接缝就不再闭合（u 到不了 1），而且一圈不再是一张图。
void TestUvVScale() {
  const double radius = 0.15;
  const uint32_t ring = 12;
  Shape shape = StraightBranch(5, radius, 2.0, ring);
  const double unit_perimeter = ring * 2.0 * std::sin(kPi / ring);
  const double tile = radius * unit_perimeter;

  const MeshData base = SweepBranches(shape);
  SweepOptions scaled_opts;
  scaled_opts.uv_v_scale = 0.5;   // 例：512x1024 的贴图
  const MeshData scaled = SweepBranches(shape, scaled_opts);

  Expect(base.VertexCount() == scaled.VertexCount() && base.indices == scaled.indices,
         "uv_v_scale 不应改变几何/拓扑");
  for (size_t i = 0; i < base.uvs.size(); i += 2) {
    Expect(Near(base.uvs[i], scaled.uvs[i], 1e-9), "uv_v_scale 不能改 u（只有 v 缩放）");
  }

  // v 方向：dv/ds 按 1/uv_v_scale 放大（= tile 缩小同样的倍数）
  for (size_t i = 1; i < 5; ++i) {
    const double ds = Dist3(GetPos(base, i * (ring + 1)), GetPos(base, (i - 1) * (ring + 1)));
    const double dv_base = GetUV(base, i * (ring + 1)).v - GetUV(base, (i - 1) * (ring + 1)).v;
    const double dv_scaled =
        GetUV(scaled, i * (ring + 1)).v - GetUV(scaled, (i - 1) * (ring + 1)).v;
    Expect(Near(dv_base / ds, 1.0 / tile, 1e-6), "基准：dv/ds = 1/tile");
    Expect(Near(dv_scaled / ds, 0.5 / tile, 1e-6),
           "uv_v_scale=0.5：dv/ds = 0.5/tile（一比一，不是 2/tile）");
    Expect(Near(dv_scaled, dv_base * 0.5, 1e-9), "v 应整体等比缩放");
  }

  // 接缝：u 仍然是 0..1 且环末显式 1.0（否则接缝不闭合）
  for (uint32_t j = 0; j <= ring; ++j) {
    const double u = GetUV(scaled, j).u;
    Expect(u >= -1e-9 && u <= 1.0 + 1e-9, "缩 v 后 u 仍必须在 [0,1]");
  }
  Expect(Near(GetUV(scaled, ring).u, 1.0, 1e-9), "缩 v 后接缝顶点 u 仍必须是 1.0");
}

void TestNonCircularProfile() {
  // 椭圆截面（x 拉长 1.5 倍）：等距性仍须成立，且 u 仍覆盖 [0,1]。
  Shape shape = StraightBranch(4, 0.2, 1.0, 12);
  std::vector<ProfilePoint> ellipse;
  for (int i = 0; i < 12; ++i) {
    const double a = 2.0 * kPi * i / 12.0;
    ellipse.push_back(ProfilePoint{1.5 * std::cos(a), std::sin(a)});
  }
  for (BranchSample& s : shape.branches[0].samples) {
    s.profile = ellipse;
  }
  TestIsotropy(shape, "椭圆截面", 0.015);

  const MeshData mesh = SweepBranches(shape);
  double umax = -1e9;
  for (uint32_t j = 0; j <= 12; ++j) {
    umax = std::fmax(umax, GetUV(mesh, j).u);
  }
  Expect(Near(umax, 1.0, 1e-6), "非圆截面也必须铺满 u∈[0,1]");
}

void TestJunctionVContinuity() {
  // 父：竖直 2m，半径 0.15；子：从父末端（或末端上方 gap 米）长出去，半径 0.05。
  // 同时覆盖 gap=0 与 gap>0：后者的接续项带 uv_v_scale，漏乘在 gap=0 时查不出来。
  const auto build = [](double child_y) {
    Shape shape;
    Branch parent;
    parent.id = 0;
    parent.parent = -1;
    parent.ring_count = 12;
    parent.seam_index = 0;
    parent.samples.push_back(BranchSample{Vec3d{0, 0, 0}, 0.15, 0.0, {}});
    parent.samples.push_back(BranchSample{Vec3d{0, 2, 0}, 0.15, 0.0, {}});

    Branch child;
    child.id = 1;
    child.parent = 0;
    child.ring_count = 8;
    child.seam_index = 0;
    child.samples.push_back(BranchSample{Vec3d{0, child_y, 0}, 0.05, 0.0, {}});
    child.samples.push_back(BranchSample{Vec3d{1, child_y, 0}, 0.05, 0.0, {}});

    shape.branches.push_back(parent);
    shape.branches.push_back(child);
    return shape;
  };

  const double parent_tile = 0.15 * 12 * 2.0 * std::sin(kPi / 12);
  const uint32_t parent_verts = 2 * (12 + 1);   // 2 圈 × (ring+1)

  const double scales[2] = {1.0, 0.5};
  const double gaps[2] = {0.0, 0.5};
  for (double scale : scales) {
    for (double gap : gaps) {
      SweepOptions opts;
      opts.uv_v_scale = scale;
      const MeshData mesh = SweepBranches(build(2.0 + gap), opts);

      const double parent_v_exit = GetUV(mesh, parent_verts - 1).v;
      const double child_v_entry = GetUV(mesh, parent_verts).v;
      // 接续项必须与枝内累计项用同一个 uv_v_scale（见 sweep.cc）。
      const double expected =
          std::fmod(parent_v_exit + gap * scale / parent_tile, 1.0);
      Expect(Near(child_v_entry, expected, 1e-6),
             "子枝入口 v 必须从父枝出口接续（gap=" + std::to_string(gap) +
                 "，uv_v_scale=" + std::to_string(scale) + "）");
      Expect(child_v_entry < 1.0 && child_v_entry >= 0.0, "接续后的 v 必须在 [0,1)");
    }
  }
}

// ComputeCardFrame：把 anchor + anchor_mode 翻译成姿态与 UV 锚点的**唯一**入口。
// 这里钉住两件事：
//   1) (au, av) 与 AnchorModeToUv 一致（4 种 mode 全覆盖）；
//   2) (right, up, normal) 是右手系单位正交基 —— 三处调用方（真实叶卡 / 绿卡 / 标签）
//      全靠它，任何一处算错都会让叶子整体错位或翻面。
void TestCardFrame() {
  Shape shape = StraightBranch(2, 0.1, 1.0, 8);
  LeafType type;
  type.type = 0;
  type.anchor_mode = "bottom_center";
  shape.leaf_types.push_back(type);

  LeafCard card;
  card.anchor = Vec3d{0.0, 1.0, 0.0};
  card.up = Vec3d{0, 1, 0};
  card.normal = Vec3d{0, 0, 2};   // 故意不归一化：ComputeCardFrame 必须自己归一
  card.height = 0.2;
  card.width = 0.1;
  card.type = 0;

  const struct {
    const char* mode;
    double u;
    double v;
  } kCases[] = {{"bottom_left", 0.0, 0.0},
                {"bottom_center", 0.5, 0.0},
                {"bottom_right", 1.0, 0.0},
                {"top_center", 0.5, 1.0}};

  for (const auto& c : kCases) {
    shape.leaf_types[0].anchor_mode = c.mode;
    const CardFrame f = ComputeCardFrame(shape, card);
    Expect(Near(f.au, c.u, 1e-12) && Near(f.av, c.v, 1e-12),
           std::string(c.mode) + "：UV 锚点应为 (" + std::to_string(c.u) + ", " +
               std::to_string(c.v) + ")");
    const double lu = std::sqrt(f.up.x * f.up.x + f.up.y * f.up.y + f.up.z * f.up.z);
    const double ln = std::sqrt(f.normal.x * f.normal.x + f.normal.y * f.normal.y +
                                f.normal.z * f.normal.z);
    const double lr = std::sqrt(f.right.x * f.right.x + f.right.y * f.right.y +
                                f.right.z * f.right.z);
    Expect(Near(lu, 1.0, 1e-9) && Near(ln, 1.0, 1e-9) && Near(lr, 1.0, 1e-9),
           std::string(c.mode) + "：right/up/normal 必须都是单位向量");
    const double du = f.right.x * f.up.x + f.right.y * f.up.y + f.right.z * f.up.z;
    const double dn = f.right.x * f.normal.x + f.right.y * f.normal.y +
                      f.right.z * f.normal.z;
    const double un = f.up.x * f.normal.x + f.up.y * f.normal.y + f.up.z * f.normal.z;
    Expect(Near(du, 0.0, 1e-9) && Near(dn, 0.0, 1e-9) && Near(un, 0.0, 1e-9),
           std::string(c.mode) + "：right/up/normal 必须两两正交");
    // right = cross(up, normal) ⇒ right × up = normal（右手系，不翻面）
    const double cx = f.right.y * f.up.z - f.right.z * f.up.y;
    const double cy = f.right.z * f.up.x - f.right.x * f.up.z;
    const double cz = f.right.x * f.up.y - f.right.y * f.up.x;
    Expect(Near(cx, f.normal.x, 1e-9) && Near(cy, f.normal.y, 1e-9) &&
               Near(cz, f.normal.z, 1e-9),
           std::string(c.mode) + "：right × up 必须等于 normal（右手系）");
  }
}

// 造一张卡的 shape（anchor 落在竖直枝上，up = +Y，normal = +Z）。
Shape MakeCardShape(const char* mode, const LeafBend& bend, LeafCard* card /*output*/) {
  Shape shape = StraightBranch(2, 0.1, 1.0, 8);
  LeafType type;
  type.type = 0;
  type.description = "测试叶";
  type.anchor_mode = mode;
  type.bend = bend;
  shape.leaf_types.push_back(type);

  LeafCard c;
  c.anchor = Vec3d{0.0, 1.0, 0.0};
  c.up = Vec3d{0, 1, 0};
  c.normal = Vec3d{0, 0, 1};
  c.height = 0.2;
  c.width = 0.1;   // 存在 shape 里（装配时 = height × 格子宽/高）
  c.type = 0;
  c.branch = 0;
  c.s = 0.5;
  shape.cards.push_back(c);
  *card = c;
  return shape;
}

// 按 UV 找顶点（顶点布局 = i·(seg+1)+j、u = j/seg、v = i/seg 的**唯一**判据，
// 消费方按步进遍历顶点时全靠它）。找不到返回 UINT32_MAX。
uint32_t FindVertexByUv(const MeshData& mesh, double u, double v, double eps) {
  for (uint32_t i = 0; i < mesh.VertexCount(); ++i) {
    const P2 uv = GetUV(mesh, i);
    if (Near(uv.u, u, eps) && Near(uv.v, v, eps)) {
      return i;
    }
  }
  return UINT32_MAX;
}

double Dot3(const P3& a, const P3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

P3 Cross3(const P3& a, const P3& b) {
  return P3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

P3 Sub3(const P3& a, const P3& b) { return P3{a.x - b.x, a.y - b.y, a.z - b.z}; }

// 平面卡（bend 全 0）必须与旧资产**逐字节一致**：4 顶点 / 2 三角形 / 四角 UV /
// 尺寸只由 width/height 决定 / 法线全等于面法线。
void TestLeafCards() {
  const LeafBend flat;   // 全 0
  LeafCard card;
  Shape shape = MakeCardShape("bottom_center", flat, &card);
  const double width = card.width;

  // 顶点布局：下标 = i·(seg+1)+j，u = j/seg、v = i/seg（planar 时 seg = 1）。
  // 于是 0=(0,0) 1=(1,0) 2=(0,1) 3=(1,1)。用双线性插值算「参数 (au, av) 处的世界坐标」，
  // 再要求它等于 anchor —— 这才是 anchor 的定义。
  const auto point_at = [](const MeshData& m, double u, double v) {
    const P3 c00 = GetPos(m, 0);
    const P3 c10 = GetPos(m, 1);
    const P3 c01 = GetPos(m, 2);
    const P3 c11 = GetPos(m, 3);
    const double w00 = (1 - u) * (1 - v);
    const double w10 = u * (1 - v);
    const double w01 = (1 - u) * v;
    const double w11 = u * v;
    return P3{c00.x * w00 + c10.x * w10 + c01.x * w01 + c11.x * w11,
              c00.y * w00 + c10.y * w10 + c01.y * w01 + c11.y * w11,
              c00.z * w00 + c10.z * w10 + c01.z * w01 + c11.z * w11};
  };

  const struct {
    const char* mode;
    double u;
    double v;
  } kCases[] = {{"bottom_left", 0.0, 0.0},
                {"bottom_center", 0.5, 0.0},
                {"bottom_right", 1.0, 0.0},
                {"top_center", 0.5, 1.0}};

  for (const auto& c : kCases) {
    shape.leaf_types[0].anchor_mode = c.mode;
    const MeshData mesh = BuildLeafCards(shape);
    Expect(mesh.VertexCount() == 4, "平面卡应 4 个顶点（与旧资产逐字节一致）");
    Expect(mesh.TriangleCount() == 2, "平面卡应 2 个三角形");
    Expect(mesh.tangents.size() == 4 * 3, "平面卡也要出切线（解析 ∂P/∂u）");

    const P3 root = point_at(mesh, c.u, c.v);
    Expect(Near(root.x, card.anchor.x, 1e-6) && Near(root.y, card.anchor.y, 1e-6) &&
               Near(root.z, card.anchor.z, 1e-6),
           std::string(c.mode) + "：图里 (u=" + std::to_string(c.u) +
               ", v=" + std::to_string(c.v) + ") 那一点必须落在 anchor 上");

    // 尺寸与朝向不随 mode 变：宽度来自材质、高度沿 up。
    double min_x = 1e9, max_x = -1e9, min_y = 1e9, max_y = -1e9;
    for (uint32_t v = 0; v < 4; ++v) {
      const P3 p = GetPos(mesh, v);
      min_x = std::fmin(min_x, p.x);
      max_x = std::fmax(max_x, p.x);
      min_y = std::fmin(min_y, p.y);
      max_y = std::fmax(max_y, p.y);
      // 平面卡：法线全等于 +normal（card.normal = +Z），切线 = +X
      Expect(Near(mesh.normals[v * 3 + 2], 1.0, 1e-6), "平面卡法线必须是 +Z");
      Expect(Near(mesh.tangents[v * 3 + 0], 1.0, 1e-6), "平面卡切线必须是 +X（∂P/∂u）");
    }
    Expect(Near(max_x - min_x, width, 1e-6), "宽度必须来自材质");
    Expect(Near(max_y - min_y, card.height, 1e-6), "高度必须沿 up 且等于 card.height");

    // UV = 整格四角（atlas 的格子子矩形由调用方再映射）：下标 → (u, v)
    const double expected_uv[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    for (uint32_t v = 0; v < 4; ++v) {
      const P2 uv = GetUV(mesh, v);
      Expect(Near(uv.u, expected_uv[v][0], 1e-6) && Near(uv.v, expected_uv[v][1], 1e-6),
             "叶卡 UV 必须是整格四角");
    }
  }
}

// 弯曲卡：细分 / UV 网格 / anchor 不动 / 解析法线与切线正交。
void TestBentCardGeometry() {
  LeafBend bend;
  bend.along = 0.3;
  bend.across = 0.2;
  bend.crease = 0.25;

  const struct {
    const char* mode;
    double u;
    double v;
  } kCases[] = {{"bottom_left", 0.0, 0.0},
                {"bottom_center", 0.5, 0.0},
                {"bottom_right", 1.0, 0.0},
                {"top_center", 0.5, 1.0}};

  for (const auto& c : kCases) {
    LeafCard card;
    Shape shape = MakeCardShape(c.mode, bend, &card);
    const MeshData mesh = BuildLeafCards(shape);

    Expect(mesh.VertexCount() == 25, std::string(c.mode) + "：弯曲卡应 4×4 = 25 顶点");
    Expect(mesh.TriangleCount() == 32, std::string(c.mode) + "：弯曲卡应 32 三角形");

    // 顶点布局与 UV：下标 = i·5 + j，u = j/4、v = i/4
    constexpr uint32_t kSeg = 4;
    constexpr uint32_t kStride = kSeg + 1;
    for (uint32_t i = 0; i <= kSeg; ++i) {
      for (uint32_t j = 0; j <= kSeg; ++j) {
        const uint32_t v = i * kStride + j;
        const P2 uv = GetUV(mesh, v);
        Expect(Near(uv.u, static_cast<double>(j) / kSeg, 1e-6) &&
                   Near(uv.v, static_cast<double>(i) / kSeg, 1e-6),
               std::string(c.mode) + "：顶点下标 → UV 必须是网格布局");
      }
    }

    // anchor 点**恒不动**（z(anchor) ≡ 0）：弯曲是绕 anchor 长的，叶柄不能脱枝。
    const uint32_t anchor_vertex = FindVertexByUv(mesh, c.u, c.v, 1e-6);
    Expect(anchor_vertex != UINT32_MAX, std::string(c.mode) + "：弯曲卡里找不到 anchor 顶点");
    if (anchor_vertex != UINT32_MAX) {
      const P3 p = GetPos(mesh, anchor_vertex);
      Expect(Near(p.x, card.anchor.x, 1e-6) && Near(p.y, card.anchor.y, 1e-6) &&
                 Near(p.z, card.anchor.z, 1e-6),
             std::string(c.mode) + "：弯曲后 anchor 顶点必须仍在 anchor 上");
    }

    // 解析法线：单位长 + 与解析切线严格正交（差分/面法线累加做不到 1e-12 这么干净）
    double worst_perp = 0.0;
    double worst_len = 0.0;
    for (uint32_t v = 0; v < mesh.VertexCount(); ++v) {
      const P3 n{mesh.normals[v * 3], mesh.normals[v * 3 + 1], mesh.normals[v * 3 + 2]};
      const P3 t{mesh.tangents[v * 3], mesh.tangents[v * 3 + 1], mesh.tangents[v * 3 + 2]};
      worst_len = std::fmax(worst_len, std::fabs(std::sqrt(Dot3(n, n)) - 1.0));
      worst_perp = std::fmax(worst_perp, std::fabs(Dot3(n, t)));
    }
    Expect(worst_len < 1e-6, std::string(c.mode) + "：法线必须是单位向量");
    Expect(worst_perp < 1e-6,
           std::string(c.mode) + "：解析法线与解析切线必须正交（T·N == 0）");

    // 解析法线**方向**：与「同一张曲面上相邻网格点的中心差分」一致（内部顶点）。
    // 中心差分只有 O(h²) 精度（h = 1/4、弯曲量 3~6cm），实测最差 2.45°（对角长轴
    // 那两种 anchor_mode）；所以这里给 4° 余量 —— 它只负责抓粗错（轴搞反、符号反、
    // 法线漏了某项），精确性由上面「z 值 = 解析公式」的断言保证。
    double worst_angle_deg = 0.0;
    for (uint32_t i = 1; i + 1 <= kSeg; ++i) {
      for (uint32_t j = 1; j + 1 <= kSeg; ++j) {
        const P3 du = Sub3(GetPos(mesh, i * kStride + j + 1),
                           GetPos(mesh, i * kStride + j - 1));
        const P3 dv = Sub3(GetPos(mesh, (i + 1) * kStride + j),
                           GetPos(mesh, (i - 1) * kStride + j));
        const P3 fd = Cross3(du, dv);
        const double len = std::sqrt(Dot3(fd, fd));
        const uint32_t v = i * kStride + j;
        const P3 n{mesh.normals[v * 3], mesh.normals[v * 3 + 1], mesh.normals[v * 3 + 2]};
        const double cos_angle = Dot3(fd, n) / len;
        const double angle_deg =
            std::acos(std::fmax(-1.0, std::fmin(1.0, cos_angle))) * 180.0 / kPi;
        worst_angle_deg = std::fmax(worst_angle_deg, angle_deg);
      }
    }
    Expect(worst_angle_deg < 4.0,
           std::string(c.mode) + "：解析法线必须与曲面的中心差分法线一致（实测 " +
               std::to_string(worst_angle_deg) + "°）");
  }
}

// 弯曲的方向语义：along > 0 ⇒ 叶尖朝 +normal 翘；across/crease > 0 ⇒ 两缘朝 +normal 抬，
// 且中脉线（s = 0）上不抬。用 anchor_mode = bottom_center（叶柄在底边中点、
// 长轴 = +v、中脉 = u = 0.5）最直观。
void TestBentCardDirections() {
  LeafCard card;

  {
    LeafBend bend;
    bend.along = 0.3;   // 只要长轴项
    Shape shape = MakeCardShape("bottom_center", bend, &card);
    const MeshData mesh = BuildLeafCards(shape);
    // 叶柄行（v = 0）仍贴着 anchor 高度；叶尖行（v = 1）整体沿 +Z 抬起 0.3 × height
    const double tip_z = mesh.positions[(4 * 5 + 2) * 3 + 2];   // i=4, j=2 → v=1, u=0.5
    Expect(Near(tip_z, 0.3 * card.height, 1e-6), "along>0：叶尖应朝 +normal 抬起 0.3×height");
    for (uint32_t j = 0; j <= 4; ++j) {
      Expect(Near(mesh.positions[j * 3 + 2], 0.0, 1e-9), "along 项不能让叶柄一侧离开卡面");
    }
  }

  {
    LeafBend bend;
    bend.across = 0.2;   // 只要杯状项
    Shape shape = MakeCardShape("bottom_center", bend, &card);
    const MeshData mesh = BuildLeafCards(shape);
    // 中脉（u = 0.5）不动；两缘（u = 0 / 1）抬高 0.2 × height
    for (uint32_t i = 0; i <= 4; ++i) {
      const double mid = mesh.positions[(i * 5 + 2) * 3 + 2];
      const double left = mesh.positions[(i * 5 + 0) * 3 + 2];
      const double right = mesh.positions[(i * 5 + 4) * 3 + 2];
      Expect(Near(mid, 0.0, 1e-9), "across 项：中脉线必须停在卡面上");
      Expect(Near(left, 0.2 * card.height, 1e-6) && Near(right, 0.2 * card.height, 1e-6),
             "across>0：两缘应朝 +normal 抬高 0.2×height");
    }
  }

  {
    LeafBend bend;
    bend.crease = 0.25;   // 只要中脉折痕项
    Shape shape = MakeCardShape("bottom_center", bend, &card);
    const MeshData mesh = BuildLeafCards(shape);
    // ridge(0) = 0 ⇒ 中脉不动；ridge(1) = sqrt(1 + 0.25²) − 0.25 ≈ 0.7806
    const double ridge_edge = std::sqrt(1.0 + 0.25 * 0.25) - 0.25;
    const double edge_z = mesh.positions[(0 * 5 + 4) * 3 + 2];
    Expect(Near(edge_z, 0.25 * ridge_edge * card.height, 1e-6),
           "crease 项的边缘抬升 = crease × height × ridge(1)");
    Expect(Near(mesh.positions[(2 * 5 + 2) * 3 + 2], 0.0, 1e-9),
           "crease 项不能抬中脉");
  }
}

// CardVertexCount：消费方（烘焙器逐卡步进顶点、风绑定）必须靠它，不能假设恒等于 4。
void TestCardVertexCount() {
  LeafBend bent;
  bent.crease = 0.1;
  LeafCard card;

  Shape flat_shape = MakeCardShape("bottom_left", LeafBend(), &card);
  Expect(CardVertexCount(flat_shape, flat_shape.cards[0]) == 4,
         "平面卡 CardVertexCount 必须是 4");

  Shape bent_shape = MakeCardShape("bottom_left", bent, &card);
  Expect(CardVertexCount(bent_shape, bent_shape.cards[0]) == 25,
         "弯曲卡 CardVertexCount 必须是 25");
  Expect(BuildLeafCards(bent_shape).VertexCount() == 25,
         "CardVertexCount 必须与 BuildLeafCards 的实际顶点数一致");
}

// 确定性：同一份 shape 两次构建必须逐字节相同（gold / 资产可复现的前提）。
void TestBendDeterministic() {
  LeafBend bend;
  bend.along = -0.25;
  bend.across = 0.15;
  bend.crease = 0.2;
  LeafCard card;
  Shape shape = MakeCardShape("top_center", bend, &card);

  const MeshData a = BuildLeafCards(shape);
  const MeshData b = BuildLeafCards(shape);
  Expect(a.positions == b.positions && a.normals == b.normals && a.uvs == b.uvs &&
             a.tangents == b.tangents && a.indices == b.indices,
         "同一份 shape 两次 BuildLeafCards 必须完全一致");
}

}  // namespace
}  // namespace vegetation
}  // namespace jpov

int main() {
  jpov::vegetation::TestCylinderUV();
  jpov::vegetation::TestTaper();
  jpov::vegetation::TestUvVScale();
  jpov::vegetation::TestNonCircularProfile();
  jpov::vegetation::TestJunctionVContinuity();
  jpov::vegetation::TestCardFrame();
  jpov::vegetation::TestLeafCards();
  jpov::vegetation::TestBentCardGeometry();
  jpov::vegetation::TestBentCardDirections();
  jpov::vegetation::TestCardVertexCount();
  jpov::vegetation::TestBendDeterministic();
  if (jpov::vegetation::g_failures != 0) {
    std::printf("%d failure(s)\n", jpov::vegetation::g_failures);
    return 1;
  }
  std::printf("all vegetation sweep tests passed\n");
  return 0;
}
