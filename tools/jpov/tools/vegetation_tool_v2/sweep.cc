// 扫管实现：Shape -> 网格 + UV。

#include "tools/jpov/tools/vegetation_tool_v2/sweep.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace jpov {
namespace vegetation {

namespace {

constexpr double kPi = 3.14159265358979323846;

// ── 极简向量运算（只在本文件用，不值得抽公共库）──
struct V3 {
  double x, y, z;
};

V3 FromVec3d(const Vec3d& v) { return V3{v.x, v.y, v.z}; }

V3 Sub(const V3& a, const V3& b) { return V3{a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 Add(const V3& a, const V3& b) { return V3{a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 Mul(const V3& a, double s) { return V3{a.x * s, a.y * s, a.z * s}; }
double Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(const V3& a, const V3& b) {
  return V3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double Len(const V3& a) { return std::sqrt(Dot(a, a)); }

V3 Normalize(const V3& a) {
  const double l = Len(a);
  if (l < 1e-12) {
    return V3{0.0, 1.0, 0.0};  // 退化切线：给个确定值，别产生 NaN 污染全网格
  }
  return Mul(a, 1.0 / l);
}

// 把向量投影到与 axis 垂直的平面上并归一化；投影退化时返回 fallback。
V3 ProjectPerpendicular(const V3& v, const V3& axis, const V3& fallback) {
  const V3 proj = Sub(v, Mul(axis, Dot(v, axis)));
  if (Len(proj) < 1e-9) {
    return fallback;
  }
  return Normalize(proj);
}

// 任意一个与 t 垂直的单位向量（确定性选择：优先世界 +Y，退到 +X）。
V3 AnyPerpendicular(const V3& t) {
  const V3 seed = (std::fabs(Dot(t, V3{0, 1, 0})) < 0.9) ? V3{0, 1, 0} : V3{1, 0, 0};
  return ProjectPerpendicular(seed, t, V3{1, 0, 0});
}

// 把 v 绕单位轴 axis 旋转 angle（Rodrigues）。
V3 RotateAround(const V3& v, const V3& axis, double angle) {
  const double c = std::cos(angle);
  const double s = std::sin(angle);
  return Add(Add(Mul(v, c), Mul(Cross(axis, v), s)),
             Mul(axis, Dot(axis, v) * (1.0 - c)));
}

// 把 from 旋到 to 的最小旋转，作用在 v 上（平行传输）。
V3 Transport(const V3& v, const V3& from, const V3& to) {
  const V3 axis = Cross(from, to);
  const double sin_a = Len(axis);
  const double cos_a = Dot(from, to);
  if (sin_a < 1e-9) {
    return v;  // 切线几乎没转（或 180°，此时任何法向都"对"，保持原样最稳定）
  }
  return RotateAround(v, Mul(axis, 1.0 / sin_a), std::atan2(sin_a, cos_a));
}

double UnitPerimeter(const std::vector<ProfilePoint>& pts) {
  double sum = 0.0;
  for (size_t i = 0; i < pts.size(); ++i) {
    const ProfilePoint& a = pts[i];
    const ProfilePoint& b = pts[(i + 1) % pts.size()];
    sum += std::hypot(b.x - a.x, b.y - a.y);
  }
  return sum;
}

// 一根枝在求解过程中的完整状态（子枝要用父枝的出口帧接续）。
struct BranchFrame {
  std::vector<V3> tangent;
  std::vector<V3> axis_u;
  std::vector<V3> axis_v;
  std::vector<double> tile;     // 每个采样点的 tile（米）：radius × 单位周长
  std::vector<double> v_uv;     // 每个采样点的 v（UV 单位，沿枝累计）
  V3 exit_position;
  V3 exit_u;
  double exit_tile = 1.0;
  double exit_v_uv = 0.0;
};

// 计算一根枝的逐点帧、tile 与 v。parent 为空表示根枝。
BranchFrame BuildBranchFrame(const Branch& branch, const BranchFrame* parent,
                            const SweepOptions& opts) {
  BranchFrame f;
  const size_t n = branch.samples.size();
  const uint32_t ring = branch.EffectiveRingCount();
  if (n < 2 || ring < 3) {
    std::fprintf(stderr, "BuildBranchFrame: branch %u 前置条件不满足\n", branch.id);
    std::abort();
  }

  // 切线：中心差分（端点单边），保证截面平面不随采样密度抖动。
  f.tangent.resize(n);
  for (size_t i = 0; i < n; ++i) {
    const V3 p_prev = FromVec3d((i == 0) ? branch.samples[0].position
                                         : branch.samples[i - 1].position);
    const V3 p_next = FromVec3d((i + 1 == n) ? branch.samples[n - 1].position
                                             : branch.samples[i + 1].position);
    f.tangent[i] = Normalize(Sub(p_next, p_prev));
  }

  // 截面基：起点继承父枝出口的 U（保证接口处纹路不拧转），然后逐点平行传输，
  // 最后叠加该点的 tilt（tilt 是绝对值，不累加）。
  f.axis_u.resize(n);
  f.axis_v.resize(n);
  if (parent != nullptr) {
    f.axis_u[0] = ProjectPerpendicular(parent->exit_u, f.tangent[0],
                                       AnyPerpendicular(f.tangent[0]));
  } else {
    f.axis_u[0] = AnyPerpendicular(f.tangent[0]);
  }
  f.axis_u[0] = RotateAround(f.axis_u[0], f.tangent[0], branch.samples[0].tilt);
  f.axis_v[0] = Cross(f.tangent[0], f.axis_u[0]);

  for (size_t i = 1; i < n; ++i) {
    const V3 transported =
        Transport(f.axis_u[i - 1], f.tangent[i - 1], f.tangent[i]);
    f.axis_u[i] = ProjectPerpendicular(transported, f.tangent[i],
                                       AnyPerpendicular(f.tangent[i]));
    f.axis_u[i] = RotateAround(f.axis_u[i], f.tangent[i], branch.samples[i].tilt);
    f.axis_v[i] = Cross(f.tangent[i], f.axis_u[i]);
  }

  // tile 与 v。
  f.tile.resize(n);
  f.v_uv.resize(n);
  std::vector<std::vector<ProfilePoint>> profiles(n);
  for (size_t i = 0; i < n; ++i) {
    const BranchSample& s = branch.samples[i];
    profiles[i] = s.profile.empty() ? UnitCircleProfile(ring) : s.profile;
    if (static_cast<uint32_t>(profiles[i].size()) != ring) {
      std::fprintf(stderr, "BuildBranchFrame: branch %u sample %zu 环点数不一致\n",
                   branch.id, i);
      std::abort();
    }
    f.tile[i] = s.radius * UnitPerimeter(profiles[i]);
  }

  // 子枝的 v 从父枝出口接续（+ 接口间距），否则每根枝都从贴图第 0 行开始，
  // 接口处会出现一条横向重复线。取模因为贴图可平铺。
  //
  // 非方贴图：v 整体乘 uv_v_scale（等价于把 tile 除以它）。这里必须一起缩，
  // 否则父子接续项与枝内累计项不在同一个尺度上，接口处会错行。
  const double vs = opts.uv_v_scale;
  if (parent != nullptr) {
    const V3 entry = FromVec3d(branch.samples[0].position);
    const double gap = Len(Sub(entry, parent->exit_position));
    const double offset = parent->exit_v_uv + gap * vs / parent->exit_tile;
    f.v_uv[0] = std::fmod(offset, 1.0);
    if (f.v_uv[0] < 0.0) {
      f.v_uv[0] += 1.0;
    }
  } else {
    f.v_uv[0] = 0.0;
  }

  for (size_t i = 1; i < n; ++i) {
    const V3 a = FromVec3d(branch.samples[i - 1].position);
    const V3 b = FromVec3d(branch.samples[i].position);
    const double ds = Len(Sub(b, a));
    const double tile_mid = 0.5 * (f.tile[i - 1] + f.tile[i]);
    f.v_uv[i] = f.v_uv[i - 1] + ds * vs / tile_mid;
  }

  const V3 exit = FromVec3d(branch.samples[n - 1].position);
  f.exit_position = exit;
  f.exit_u = f.axis_u[n - 1];
  f.exit_tile = f.tile[n - 1];
  f.exit_v_uv = f.v_uv[n - 1];
  return f;
}

struct ExitState {
  V3 position;
  V3 axis_u;
  double tile = 1.0;
  double v_uv = 0.0;
};

}  // namespace

MeshData SweepBranches(const Shape& shape, const SweepOptions& opts) {
  if (!(opts.uv_v_scale > 0.0)) {
    std::fprintf(stderr, "SweepBranches: uv_v_scale 必须 > 0（0 会让 v 恒定）\n");
    std::abort();
  }
  MeshData mesh;

  // 按 id 顺序处理（Validate 保证 parent < id），把出口状态记下来给子枝用。
  std::unordered_map<uint32_t, ExitState> exits;

  for (const Branch& branch : shape.branches) {
    const ExitState* parent = nullptr;
    std::unordered_map<uint32_t, ExitState>::const_iterator it;
    if (branch.parent >= 0) {
      it = exits.find(static_cast<uint32_t>(branch.parent));
      if (it == exits.end()) {
        // Validate 保证 parent < id（父枝一定先处理过），走到这里说明资产/调用顺序坏了。
        // 静默当根枝会让子枝丢掉 UV 接续与相位继承（接口处出重复线），所以直接中止。
        std::fprintf(stderr, "SweepBranches: branch %u 找不到父枝 %d 的出口状态\n",
                     branch.id, branch.parent);
        std::abort();
      }
      parent = &it->second;
    }

    // 复用 BuildBranchFrame 需要 BranchFrame 形状的父状态：拷贝一份即可。
    BranchFrame parent_frame;
    if (parent != nullptr) {
      parent_frame.exit_position = parent->position;
      parent_frame.exit_u = parent->axis_u;
      parent_frame.exit_tile = parent->tile;
      parent_frame.exit_v_uv = parent->v_uv;
    }
    const BranchFrame f =
        BuildBranchFrame(branch, parent == nullptr ? nullptr : &parent_frame, opts);

    const size_t n = branch.samples.size();
    const uint32_t ring = branch.EffectiveRingCount();
    const uint32_t base = static_cast<uint32_t>(mesh.VertexCount());

    // 每圈 ring+1 个顶点：最后一个与第一个位置相同、u = 1.0。
    // 这样接缝处那一圈四边形用 u∈[0,1] 的两个端点，不需要按面改 UV，
    // 也不会出现"接缝处横跨整张图"的拉丝。
    for (size_t i = 0; i < n; ++i) {
      const BranchSample& s = branch.samples[i];
      const V3 c = FromVec3d(s.position);
      const std::vector<ProfilePoint> profile =
          s.profile.empty() ? UnitCircleProfile(ring) : s.profile;
      const double unit_perimeter = UnitPerimeter(profile);
      const uint32_t seam = branch.seam_index % ring;

      // 从 seam 顶点起绕一圈，累计弧长 → u = arc / 单位周长 ∈ [0,1)
      double arc = 0.0;
      double prev_x = profile[seam].x;
      double prev_y = profile[seam].y;
      for (uint32_t j = 0; j <= ring; ++j) {
        const uint32_t idx = (seam + j) % ring;
        if (j > 0) {
          arc += std::hypot(profile[idx].x - prev_x, profile[idx].y - prev_y);
          prev_x = profile[idx].x;
          prev_y = profile[idx].y;
        }
        const double px = profile[idx].x;
        const double py = profile[idx].y;
        const V3 pos = Add(c, Mul(Add(Mul(f.axis_u[i], px), Mul(f.axis_v[i], py)),
                                  s.radius));
        mesh.positions.push_back(static_cast<float>(pos.x));
        mesh.positions.push_back(static_cast<float>(pos.y));
        mesh.positions.push_back(static_cast<float>(pos.z));
        mesh.uvs.push_back(static_cast<float>(j == ring ? 1.0 : arc / unit_perimeter));
        mesh.uvs.push_back(static_cast<float>(f.v_uv[i]));
        // 法线/切线稍后统一算（解析法向在非圆截面下不对），先占位
        mesh.normals.push_back(0.0f);
        mesh.normals.push_back(0.0f);
        mesh.normals.push_back(0.0f);
        mesh.tangents.push_back(0.0f);
        mesh.tangents.push_back(0.0f);
        mesh.tangents.push_back(0.0f);
      }
    }

    // 三角形：相邻两圈之间铺四边形。绕序 (a,b,c)/(a,c,d) 给出朝外法线
    // （U×V=T 的右手系下，b-a 是逆时针切向，c-a 沿 +T）。
    const uint32_t stride = ring + 1;
    for (size_t i = 0; i + 1 < n; ++i) {
      for (uint32_t j = 0; j < ring; ++j) {
        const uint32_t a = base + static_cast<uint32_t>(i) * stride + j;
        const uint32_t b = a + 1;
        const uint32_t d = a + stride;
        const uint32_t c = d + 1;
        mesh.indices.push_back(a);
        mesh.indices.push_back(b);
        mesh.indices.push_back(c);
        mesh.indices.push_back(a);
        mesh.indices.push_back(c);
        mesh.indices.push_back(d);
      }
    }

    // 法线：面法线累加到顶点上再归一化。只处理本枝新增的索引区间——
    // 每次从 0 开始遍历会把前面所有枝重算一遍（O(n²)）。
    const size_t first_index = mesh.indices.size() - static_cast<size_t>(ring) * 6 * (n - 1);
    for (size_t t = first_index; t < mesh.indices.size(); t += 3) {
      const uint32_t ia = mesh.indices[t];
      const uint32_t ib = mesh.indices[t + 1];
      const uint32_t ic = mesh.indices[t + 2];
      const V3 pa{mesh.positions[ia * 3], mesh.positions[ia * 3 + 1],
                  mesh.positions[ia * 3 + 2]};
      const V3 pb{mesh.positions[ib * 3], mesh.positions[ib * 3 + 1],
                  mesh.positions[ib * 3 + 2]};
      const V3 pc{mesh.positions[ic * 3], mesh.positions[ic * 3 + 1],
                  mesh.positions[ic * 3 + 2]};
      const V3 fn = Cross(Sub(pb, pa), Sub(pc, pa));
      const uint32_t ids[3] = {ia, ib, ic};
      for (int k = 0; k < 3; ++k) {
        mesh.normals[ids[k] * 3 + 0] += static_cast<float>(fn.x);
        mesh.normals[ids[k] * 3 + 1] += static_cast<float>(fn.y);
        mesh.normals[ids[k] * 3 + 2] += static_cast<float>(fn.z);
      }
    }
    // 接缝重复顶点：把环首的累加值复制过去
    for (size_t i = 0; i < n; ++i) {
      const uint32_t first = base + static_cast<uint32_t>(i) * stride;
      const uint32_t last = first + ring;
      for (int k = 0; k < 3; ++k) {
        mesh.normals[last * 3 + k] = mesh.normals[first * 3 + k];
      }
    }
    for (size_t v = base; v < mesh.VertexCount(); ++v) {
      V3 nrm{mesh.normals[v * 3], mesh.normals[v * 3 + 1], mesh.normals[v * 3 + 2]};
      nrm = Normalize(nrm);
      mesh.normals[v * 3 + 0] = static_cast<float>(nrm.x);
      mesh.normals[v * 3 + 1] = static_cast<float>(nrm.y);
      mesh.normals[v * 3 + 2] = static_cast<float>(nrm.z);
    }

    // 切线 = 沿环方向的差分（就是 ∂P/∂u）。接缝处用环形邻居，避免端点退化。
    for (size_t i = 0; i < n; ++i) {
      const uint32_t ring_base = base + static_cast<uint32_t>(i) * stride;
      for (uint32_t j = 0; j <= ring; ++j) {
        const uint32_t jm = (j == 0) ? ring - 1 : j - 1;
        const uint32_t jp = (j == ring) ? 1 : (j == ring - 1 ? 0 : j + 1);
        const uint32_t vm = ring_base + jm;
        const uint32_t vp = ring_base + jp;
        const uint32_t v = ring_base + j;
        V3 a{mesh.positions[vm * 3], mesh.positions[vm * 3 + 1], mesh.positions[vm * 3 + 2]};
        V3 b{mesh.positions[vp * 3], mesh.positions[vp * 3 + 1], mesh.positions[vp * 3 + 2]};
        V3 t = Sub(b, a);
        V3 nrm{mesh.normals[v * 3], mesh.normals[v * 3 + 1], mesh.normals[v * 3 + 2]};
        t = Sub(t, Mul(nrm, Dot(t, nrm)));   // 去法线分量，保证 T ⟂ N
        t = Normalize(t);
        mesh.tangents[v * 3] = static_cast<float>(t.x);
        mesh.tangents[v * 3 + 1] = static_cast<float>(t.y);
        mesh.tangents[v * 3 + 2] = static_cast<float>(t.z);
      }
    }

    ExitState state;
    state.position = f.exit_position;
    state.axis_u = f.exit_u;
    state.tile = f.exit_tile;
    state.v_uv = f.exit_v_uv;
    exits[branch.id] = state;
  }

  return mesh;
}

std::vector<MeshData> SplitMeshByBranch(const Shape& shape, const MeshData& mesh) {
  std::vector<MeshData> out;
  out.reserve(shape.branches.size());
  uint32_t cursor = 0;  // 当前枝的起始顶点
  for (const Branch& branch : shape.branches) {
    const uint32_t count = static_cast<uint32_t>(branch.samples.size()) *
                           (branch.EffectiveRingCount() + 1);
    if (cursor + count > mesh.VertexCount()) {
      // 越界必须在 assign 之前拦：assign 越界是 UB（旧代码只挡了索引拷贝）。
      std::fprintf(stderr,
                   "SplitMeshByBranch: 顶点布局与 shape 不一致（branch %u 需要到 %u，"
                   "mesh 只有 %zu）\n",
                   branch.id, cursor + count, mesh.VertexCount());
      std::abort();
    }
    MeshData part;
    part.positions.assign(mesh.positions.begin() + cursor * 3,
                          mesh.positions.begin() + (cursor + count) * 3);
    part.normals.assign(mesh.normals.begin() + cursor * 3,
                        mesh.normals.begin() + (cursor + count) * 3);
    part.uvs.assign(mesh.uvs.begin() + cursor * 2,
                    mesh.uvs.begin() + (cursor + count) * 2);
    // 注意：MeshData 新增逐顶点属性时必须同步这里，否则切分出来的子网格该属性为空，
    // 下游按下标读会越界（本次加切线时就漏了，直接 SIGSEGV）。
    if (!mesh.tangents.empty()) {
      part.tangents.assign(mesh.tangents.begin() + cursor * 3,
                           mesh.tangents.begin() + (cursor + count) * 3);
    }
    for (size_t t = 0; t < mesh.indices.size(); t += 3) {
      const uint32_t i0 = mesh.indices[t];
      if (i0 < cursor || i0 >= cursor + count) {
        continue;
      }
      part.indices.push_back(i0 - cursor);
      part.indices.push_back(mesh.indices[t + 1] - cursor);
      part.indices.push_back(mesh.indices[t + 2] - cursor);
    }
    out.push_back(std::move(part));
    cursor += count;
  }
  return out;
}

Vec3d OrthogonalizeTangent(const Vec3d& t, const Vec3d& n, const Vec3d& fallback) {
  const double dot = t.x * n.x + t.y * n.y + t.z * n.z;
  Vec3d o{t.x - n.x * dot, t.y - n.y * dot, t.z - n.z * dot};
  const double l = std::sqrt(o.x * o.x + o.y * o.y + o.z * o.z);
  if (l < 1e-6) {
    return fallback;
  }
  o.x /= l;
  o.y /= l;
  o.z /= l;
  return o;
}

CardFrame ComputeCardFrame(const Shape& shape, const LeafCard& card) {
  const LeafType* type = nullptr;
  for (const LeafType& t : shape.leaf_types) {
    if (t.type == card.type) {
      type = &t;
      break;
    }
  }
  if (type == nullptr) {
    std::fprintf(stderr,
                 "ComputeCardFrame: card type %u 不在 leaf_types 里"
                 "（Shape 必须先过 Validate）\n",
                 card.type);
    std::abort();
  }
  CardFrame f;
  const V3 up = Normalize(V3{card.up.x, card.up.y, card.up.z});
  const V3 nrm = Normalize(V3{card.normal.x, card.normal.y, card.normal.z});
  const V3 right = Normalize(Cross(up, nrm));  // (right, up, normal) 右手系
  if (!AnchorModeToUv(type->anchor_mode, &f.au, &f.av)) {
    std::fprintf(stderr, "ComputeCardFrame: card type %u 的 anchor_mode \"%s\" 非法\n",
                 card.type, type->anchor_mode.c_str());
    std::abort();
  }
  f.up = Vec3d{up.x, up.y, up.z};
  f.normal = Vec3d{nrm.x, nrm.y, nrm.z};
  f.right = Vec3d{right.x, right.y, right.z};
  f.bend = type->bend;
  return f;
}

namespace {

// ── 叶卡解析式弯曲（语义见 shape.h 的 LeafBend 与 proto/shape.proto 的 LeafBendProto）

// 中脉「脊」的圆角宽度（s 归一化到 [−1,1] 之后的量）。
// 0.25 ≈ 叶宽的 1/8 是脊的过渡带：再小，圆角比 4 段网格的步长还细 ⇒ 网格上出现锯齿
// （正是要避免的折线感）；再大，V 折痕会被糊成抛物线，跟 across 项分不出来了。
constexpr double kCardRidgeRound = 0.25;

// 弯曲卡的细分段数：4 段 ⇒ 12cm 的卡 3cm 一段。法线是解析的（与细分无关），
// 这里只决定**曲面轮廓**与二次项的形状误差 —— 4 段看不出折线，再多只是浪费顶点。
constexpr uint32_t kBentCardSegments = 4;

uint32_t CardSegments(const LeafBend& bend) {
  return bend.IsFlat() ? 1u : kBentCardSegments;
}

// 叶片局部坐标基：e 沿「anchor → 格子中心」（= jpov_leaf_card 摆正后的叶片长轴），
// n 垂直它（中脉线就在 s = 0 上）。
//
// 归一化分母 T/S = 四个角在 e/n 上的最大投影 ⇒ t ∈ [0,1]、s ∈ [−1,1]，
// 于是三个弯曲系数与卡的绝对尺寸无关（位移 = 系数 × height）。
struct CardAxes {
  double e_u = 0.0;
  double e_v = 1.0;
  double n_u = -1.0;
  double n_v = 0.0;
  double t_scale = 1.0;
  double s_scale = 1.0;
};

CardAxes MakeCardAxes(double au, double av) {
  CardAxes axes;
  const double to_centre_u = 0.5 - au;
  const double to_centre_v = 0.5 - av;
  const double length = std::hypot(to_centre_u, to_centre_v);
  if (length > 1e-9) {
    axes.e_u = to_centre_u / length;
    axes.e_v = to_centre_v / length;
  }
  // 退化（anchor 恰好在格子中心，4 种 anchor_mode 都不会）：取 +v 当长轴。
  axes.n_u = -axes.e_v;
  axes.n_v = axes.e_u;

  const double corners[4][2] = {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}};
  double t_max = 0.0;
  double s_max = 0.0;
  for (const auto& corner : corners) {
    const double du = corner[0] - au;
    const double dv = corner[1] - av;
    t_max = std::max(t_max, du * axes.e_u + dv * axes.e_v);
    s_max = std::max(s_max, std::fabs(du * axes.n_u + dv * axes.n_v));
  }
  axes.t_scale = std::max(t_max, 1e-9);
  axes.s_scale = std::max(s_max, 1e-9);
  return axes;
}

// 曲面上的一个点：位置 + 解析法线 + ∂P/∂u（TBN 的切线，平面时就是 card 的 +u 方向）。
struct CardSurfacePoint {
  V3 position;
  V3 normal;
  V3 tangent_u;
};

// 一张卡的曲面求值器：基与归一化只算一次，逐顶点复用。
//
// 曲面定义（p = (u,v) 是格子坐标，a = (au, av)）：
//   z(p) = height · ( along·t² + across·s² + crease·ridge(s) )
//   ridge(s) = sqrt(s² + r²) − r
//   P(p) = anchor + right·((u−au)·width) + up·((v−av)·height) + normal·z(p)
// z(a) ≡ 0（t = s = 0）⇒ **anchor 点永远不动**，叶柄不会因为弯曲离开枝条。
class CardEvaluator {
 public:
  CardEvaluator(const Vec3d& anchor, const CardFrame& frame, double width, double height)
      : anchor_(FromVec3d(anchor)),
        right_(FromVec3d(frame.right)),
        up_(FromVec3d(frame.up)),
        normal_(FromVec3d(frame.normal)),
        width_(width),
        height_(height),
        au_(frame.au),
        av_(frame.av),
        bend_(frame.bend),
        axes_(MakeCardAxes(frame.au, frame.av)) {}

  CardSurfacePoint At(double u, double v) const {
    const double du = u - au_;
    const double dv = v - av_;
    const double t = (du * axes_.e_u + dv * axes_.e_v) / axes_.t_scale;
    const double s = (du * axes_.n_u + dv * axes_.n_v) / axes_.s_scale;
    const double ridge_root = std::sqrt(s * s + kCardRidgeRound * kCardRidgeRound);
    const double ridge = ridge_root - kCardRidgeRound;
    // ridge 对 s 的偏导：s = 0 处 = 0（脊顶是圆的，法线在这里连续 —— 硬棱会出折线感）。
    const double ridge_ds = s / ridge_root;

    const double z = height_ * (bend_.along * t * t + bend_.across * s * s +
                                bend_.crease * ridge);
    // 链式法则：t、s 都是 (u,v) 的线性函数，所以偏导只有这几项。
    const double dz_dt = height_ * (2.0 * bend_.along * t);
    const double dz_ds = height_ * (2.0 * bend_.across * s + bend_.crease * ridge_ds);
    const double dz_du =
        dz_dt * axes_.e_u / axes_.t_scale + dz_ds * axes_.n_u / axes_.s_scale;
    const double dz_dv =
        dz_dt * axes_.e_v / axes_.t_scale + dz_ds * axes_.n_v / axes_.s_scale;

    CardSurfacePoint out;
    out.position = Add(Add(Add(anchor_, Mul(right_, du * width_)), Mul(up_, dv * height_)),
                       Mul(normal_, z));
    const V3 dpos_du = Add(Mul(right_, width_), Mul(normal_, dz_du));
    const V3 dpos_dv = Add(Mul(up_, height_), Mul(normal_, dz_dv));
    // cross(∂u, ∂v) 在平面上 = width·height·(right × up) = +normal（见 sweep.h 的右手系）。
    out.normal = Normalize(Cross(dpos_du, dpos_dv));
    out.tangent_u = Normalize(dpos_du);
    return out;
  }

 private:
  V3 anchor_;
  V3 right_;
  V3 up_;
  V3 normal_;
  double width_;
  double height_;
  double au_;
  double av_;
  LeafBend bend_;
  CardAxes axes_;
};

}  // namespace

uint32_t CardVertexCount(const Shape& shape, const LeafCard& card) {
  const CardFrame frame = ComputeCardFrame(shape, card);
  const uint32_t stride = CardSegments(frame.bend) + 1;
  return stride * stride;
}

MeshData BuildLeafCards(const Shape& shape) {
  MeshData mesh;
  for (const LeafCard& card : shape.cards) {
    if (!(card.width > 0.0)) {
      std::fprintf(stderr, "BuildLeafCards: card type %u 的 width 必须 > 0，实际 %g\n",
                   card.type, card.width);
      std::abort();
    }
    if (!(card.height > 0.0)) {
      // height 是弯曲位移的基准（z = 系数 × height），<= 0 会让整张卡退化成一个点。
      std::fprintf(stderr, "BuildLeafCards: card type %u 的 height 必须 > 0，实际 %g\n",
                   card.type, card.height);
      std::abort();
    }

    // 姿态 + UV 锚点 + 弯曲只有一份来源（ComputeCardFrame）：绿卡、角标、真实叶卡都用它，
    // 否则三者会各自漂移（历史上「绿卡和角标对不上」就是这么来的）。
    const CardFrame frame = ComputeCardFrame(shape, card);
    const CardEvaluator evaluator(card.anchor, frame, card.width, card.height);
    const uint32_t segments = CardSegments(frame.bend);
    const uint32_t stride = segments + 1;
    const uint32_t base = static_cast<uint32_t>(mesh.VertexCount());

    // 顶点：先沿 u 后沿 v（下标 = i·stride + j，u = j/seg、v = i/seg）。
    // 每张卡独立（不共享顶点）：CU 裁切/双面/风绑定都按卡走，共享只会增加耦合。
    for (uint32_t i = 0; i <= segments; ++i) {
      for (uint32_t j = 0; j <= segments; ++j) {
        const double u = static_cast<double>(j) / static_cast<double>(segments);
        const double v = static_cast<double>(i) / static_cast<double>(segments);
        const CardSurfacePoint point = evaluator.At(u, v);
        mesh.positions.push_back(static_cast<float>(point.position.x));
        mesh.positions.push_back(static_cast<float>(point.position.y));
        mesh.positions.push_back(static_cast<float>(point.position.z));
        mesh.normals.push_back(static_cast<float>(point.normal.x));
        mesh.normals.push_back(static_cast<float>(point.normal.y));
        mesh.normals.push_back(static_cast<float>(point.normal.z));
        mesh.uvs.push_back(static_cast<float>(u));
        mesh.uvs.push_back(static_cast<float>(v));
        mesh.tangents.push_back(static_cast<float>(point.tangent_u.x));
        mesh.tangents.push_back(static_cast<float>(point.tangent_u.y));
        mesh.tangents.push_back(static_cast<float>(point.tangent_u.z));
      }
    }

    // 三角形：每格两个，绕序 (a,b,c)/(a,c,d) —— 与扫管同一套约定（b-a 沿 +u、c-a 沿 +v）。
    for (uint32_t i = 0; i < segments; ++i) {
      for (uint32_t j = 0; j < segments; ++j) {
        const uint32_t a = base + i * stride + j;
        const uint32_t b = a + 1;
        const uint32_t d = a + stride;
        const uint32_t c = d + 1;
        mesh.indices.push_back(a);
        mesh.indices.push_back(b);
        mesh.indices.push_back(c);
        mesh.indices.push_back(a);
        mesh.indices.push_back(c);
        mesh.indices.push_back(d);
      }
    }
  }
  return mesh;
}

}  // namespace vegetation
}  // namespace jpov
