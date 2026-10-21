// 扫管实现：Shape -> 网格 + UV。

#include "tools/jpov/tools/vegetation_tool_v2/sweep.h"

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
BranchFrame BuildBranchFrame(const Branch& branch, const BranchFrame* parent) {
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
  if (parent != nullptr) {
    const V3 entry = FromVec3d(branch.samples[0].position);
    const double gap = Len(Sub(entry, parent->exit_position));
    const double offset = parent->exit_v_uv + gap / parent->exit_tile;
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
    f.v_uv[i] = f.v_uv[i - 1] + ds / tile_mid;
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

MeshData SweepBranches(const Shape& shape) {
  MeshData mesh;

  // 按 id 顺序处理（Validate 保证 parent < id），把出口状态记下来给子枝用。
  std::unordered_map<uint32_t, ExitState> exits;

  for (const Branch& branch : shape.branches) {
    const ExitState* parent = nullptr;
    std::unordered_map<uint32_t, ExitState>::const_iterator it;
    if (branch.parent >= 0) {
      it = exits.find(static_cast<uint32_t>(branch.parent));
      if (it != exits.end()) {
        parent = &it->second;
      }
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
        BuildBranchFrame(branch, parent == nullptr ? nullptr : &parent_frame);

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
    if (cursor + count <= mesh.VertexCount()) {
      for (size_t t = 0; t < mesh.indices.size(); t += 3) {
        const uint32_t i0 = mesh.indices[t];
        if (i0 < cursor || i0 >= cursor + count) {
          continue;
        }
        part.indices.push_back(i0 - cursor);
        part.indices.push_back(mesh.indices[t + 1] - cursor);
        part.indices.push_back(mesh.indices[t + 2] - cursor);
      }
    }
    out.push_back(std::move(part));
    cursor += count;
  }
  return out;
}

MeshData BuildLeafCards(const Shape& shape, const std::vector<double>& width_by_type) {
  MeshData mesh;
  for (const LeafCard& card : shape.cards) {
    if (card.type >= width_by_type.size()) {
      std::fprintf(stderr, "BuildLeafCards: card type %u 没有对应宽度\n", card.type);
      std::abort();
    }
    const double width = width_by_type[card.type];
    if (!(width > 0.0)) {
      std::fprintf(stderr, "BuildLeafCards: type %u 的宽度必须 > 0，实际 %g\n",
                   card.type, width);
      std::abort();
    }

    const V3 up = Normalize(V3{card.up.x, card.up.y, card.up.z});
    const V3 nrm = Normalize(V3{card.normal.x, card.normal.y, card.normal.z});
    const V3 right = Normalize(Cross(up, nrm));  // (right, up, normal) 右手系
    const V3 anchor{card.anchor.x, card.anchor.y, card.anchor.z};

    // anchor_mode 决定四边形相对锚点往哪边长：
    //   bottom_center → 从锚点向上长（叶/叶簇的根在下）
    //   top_center    → 从锚点向下长（花序挂在枝下，根在上）
    //   center        → 上下各半
    std::string mode = "bottom_center";
    for (const LeafType& t : shape.leaf_types) {
      if (t.type == card.type) {
        mode = t.anchor_mode;
        break;
      }
    }
    double base_offset = 0.0;  // 沿 up 的起点偏移（米）
    if (mode == "top_center") {
      base_offset = -card.height;
    } else if (mode == "center") {
      base_offset = -0.5 * card.height;
    }

    const V3 origin = Add(anchor, Mul(up, base_offset));
    const V3 half = Mul(right, 0.5 * width);
    const V3 corner[4] = {Sub(origin, half), Add(origin, half),
                          Add(Add(origin, half), Mul(up, card.height)),
                          Add(origin, Mul(up, card.height))};
    const float uv[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};

    const uint32_t base = static_cast<uint32_t>(mesh.VertexCount());
    for (int i = 0; i < 4; ++i) {
      mesh.positions.push_back(static_cast<float>(corner[i].x));
      mesh.positions.push_back(static_cast<float>(corner[i].y));
      mesh.positions.push_back(static_cast<float>(corner[i].z));
      mesh.normals.push_back(static_cast<float>(nrm.x));
      mesh.normals.push_back(static_cast<float>(nrm.y));
      mesh.normals.push_back(static_cast<float>(nrm.z));
      mesh.uvs.push_back(uv[i][0]);
      mesh.uvs.push_back(uv[i][1]);
    }
    mesh.indices.push_back(base + 0);
    mesh.indices.push_back(base + 1);
    mesh.indices.push_back(base + 2);
    mesh.indices.push_back(base + 0);
    mesh.indices.push_back(base + 2);
    mesh.indices.push_back(base + 3);
  }
  return mesh;
}

}  // namespace vegetation
}  // namespace jpov
