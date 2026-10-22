// 扫管的单元测试：几何 + UV 的**数值**判据。
//
// 这里刻意不比对像素或截图，只查可证伪的几何不变量：
//   * 等距性：同一个三角形里，两条边的「UV 长度 / 世界长度」必须相等（格子保持方形）
//   * v 沿枝单调、且 dv/ds = 1/tile（非方贴图时再乘 uv_v_scale）
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

void TestLeafCards() {
  Shape shape = StraightBranch(2, 0.1, 1.0, 8);
  LeafType type;
  type.type = 0;
  type.description = "测试叶";
  type.anchor_mode = "bottom_center";
  shape.leaf_types.push_back(type);

  const double width = 0.1;   // 现在存在 shape 里（装配时 = height × 格子宽/高）

  LeafCard card;
  card.anchor = Vec3d{0.0, 1.0, 0.0};
  card.up = Vec3d{0, 1, 0};
  card.normal = Vec3d{0, 0, 1};
  card.height = 0.2;
  card.width = width;
  card.type = 0;
  card.branch = 0;
  card.s = 0.5;
  shape.cards.push_back(card);
  // 顶点顺序固定：0=(u0,v0) 1=(u1,v0) 2=(u1,v1) 3=(u0,v1)。用双线性插值算
  // 「参数 (au, av) 处的世界坐标」，再要求它等于 anchor —— 这才是 anchor 的定义。
  const auto point_at = [](const MeshData& m, double u, double v) {
    const P3 c0 = GetPos(m, 0);
    const P3 c1 = GetPos(m, 1);
    const P3 c2 = GetPos(m, 2);
    const P3 c3 = GetPos(m, 3);
    const double w00 = (1 - u) * (1 - v);
    const double w10 = u * (1 - v);
    const double w11 = u * v;
    const double w01 = (1 - u) * v;
    return P3{c0.x * w00 + c1.x * w10 + c2.x * w11 + c3.x * w01,
              c0.y * w00 + c1.y * w10 + c2.y * w11 + c3.y * w01,
              c0.z * w00 + c1.z * w10 + c2.z * w11 + c3.z * w01};
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
    Expect(mesh.VertexCount() == 4, "一张卡应 4 个顶点");
    Expect(mesh.TriangleCount() == 2, "一张卡应 2 个三角形");

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
    }
    Expect(Near(max_x - min_x, width, 1e-6), "宽度必须来自材质");
    Expect(Near(max_y - min_y, card.height, 1e-6), "高度必须沿 up 且等于 card.height");

    // UV 四角（格内 [0,1]；atlas 的格子子矩形由调用方再映射）
    const float expected_uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (uint32_t v = 0; v < 4; ++v) {
      const P2 uv = GetUV(mesh, v);
      Expect(Near(uv.u, expected_uv[v][0], 1e-6) && Near(uv.v, expected_uv[v][1], 1e-6),
             "叶卡 UV 必须是整格四角");
    }
  }
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
  if (jpov::vegetation::g_failures != 0) {
    std::printf("%d failure(s)\n", jpov::vegetation::g_failures);
    return 1;
  }
  std::printf("all vegetation sweep tests passed\n");
  return 0;
}
