// 扫管的单元测试：几何 + UV 的**数值**判据。
//
// 这里刻意不比对像素或截图，只查可证伪的几何不变量：
//   * 等距性：同一个三角形里，两条边的「UV 长度 / 世界长度」必须相等（格子保持方形）
//   * v 沿枝单调、且 dv/ds = 1/tile
//   * 每圈的 u 恰好覆盖 [0,1]（一圈一张图，接缝在 u=1 处闭合）
//   * 子枝入口 v 从父枝出口接续（接口处没有横向重复线）
//   * 叶卡：4 顶点 / 2 三角形 / UV 四角 / 宽度来自材质

#include <cmath>
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
  // 父：竖直 2m，半径 0.15；子：从父的末端附近长出去，半径 0.05。
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
  child.samples.push_back(BranchSample{Vec3d{0, 2, 0}, 0.05, 0.0, {}});
  child.samples.push_back(BranchSample{Vec3d{1, 2, 0}, 0.05, 0.0, {}});

  shape.branches.push_back(parent);
  shape.branches.push_back(child);

  const MeshData mesh = SweepBranches(shape);

  const double parent_tile = 0.15 * 12 * 2.0 * std::sin(kPi / 12);
  const uint32_t parent_verts = 2 * (12 + 1);
  // 父枝出口 v 在最后一圈的第 0 个顶点
  const double parent_v_exit = GetUV(mesh, parent_verts - 1).v;
  // 子枝入口 v 在紧接着的第一圈
  const double child_v_entry = GetUV(mesh, parent_verts).v;

  const double expected = std::fmod(parent_v_exit + 0.0 / parent_tile, 1.0);  // 间距 0
  Expect(Near(child_v_entry, expected, 1e-6),
         "子枝入口 v 必须从父枝出口接续（间距为 0 时应相等）");
  Expect(child_v_entry < 1.0 && child_v_entry >= 0.0, "接续后的 v 必须在 [0,1)");
}

void TestLeafCards() {
  Shape shape = StraightBranch(2, 0.1, 1.0, 8);
  LeafType type;
  type.type = 0;
  type.description = "测试叶";
  type.anchor_mode = "bottom_center";
  shape.leaf_types.push_back(type);

  LeafCard card;
  card.anchor = Vec3d{0.0, 1.0, 0.0};
  card.up = Vec3d{0, 1, 0};
  card.normal = Vec3d{0, 0, 1};
  card.height = 0.2;
  card.type = 0;
  card.branch = 0;
  card.s = 0.5;
  shape.cards.push_back(card);

  const MeshData mesh = BuildLeafCards(shape, {0.1});
  Expect(mesh.VertexCount() == 4, "一张卡应 4 个顶点");
  Expect(mesh.TriangleCount() == 2, "一张卡应 2 个三角形");

  // bottom_center：底边中点在 anchor，顶点向上长 height
  double min_y = 1e9;
  double max_y = -1e9;
  double min_x = 1e9;
  double max_x = -1e9;
  for (uint32_t v = 0; v < 4; ++v) {
    const P3 p = GetPos(mesh, v);
    min_y = std::fmin(min_y, p.y);
    max_y = std::fmax(max_y, p.y);
    min_x = std::fmin(min_x, p.x);
    max_x = std::fmax(max_x, p.x);
  }
  Expect(Near(min_y, 1.0, 1e-6) && Near(max_y, 1.2, 1e-6),
         "bottom_center：底边贴 anchor，向上长 height");
  Expect(Near(max_x - min_x, 0.1, 1e-6), "宽度必须来自材质（0.1）");

  // UV 四角
  const float expected_uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  for (uint32_t v = 0; v < 4; ++v) {
    const P2 uv = GetUV(mesh, v);
    Expect(Near(uv.u, expected_uv[v][0], 1e-6) && Near(uv.v, expected_uv[v][1], 1e-6),
           "叶卡 UV 必须是整格四角");
  }

  // top_center：整张卡挂在锚点下方
  shape.leaf_types[0].anchor_mode = "top_center";
  const MeshData flipped = BuildLeafCards(shape, {0.1});
  double fy_min = 1e9;
  double fy_max = -1e9;
  for (uint32_t v = 0; v < 4; ++v) {
    const P3 p = GetPos(flipped, v);
    fy_min = std::fmin(fy_min, p.y);
    fy_max = std::fmax(fy_max, p.y);
  }
  Expect(Near(fy_max, 1.0, 1e-6) && Near(fy_min, 0.8, 1e-6),
         "top_center：顶边贴 anchor，向下长 height（花序垂挂）");
}

}  // namespace
}  // namespace vegetation
}  // namespace jpov

int main() {
  jpov::vegetation::TestCylinderUV();
  jpov::vegetation::TestTaper();
  jpov::vegetation::TestNonCircularProfile();
  jpov::vegetation::TestJunctionVContinuity();
  jpov::vegetation::TestLeafCards();
  if (jpov::vegetation::g_failures != 0) {
    std::printf("%d failure(s)\n", jpov::vegetation::g_failures);
    return 1;
  }
  std::printf("all vegetation sweep tests passed\n");
  return 0;
}
