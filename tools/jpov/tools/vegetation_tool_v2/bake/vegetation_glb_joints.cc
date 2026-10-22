// 植被风关节表的实现。不变式与语义见头文件。

#include "tools/jpov/tools/vegetation_tool_v2/bake/vegetation_glb_joints.h"

#include <algorithm>
#include <cmath>

#include <glog/logging.h>

namespace jpov {
namespace vegetation {
namespace {

double Distance(const Vec3d& a, const Vec3d& b) {
  const double dx = a.x - b.x;
  const double dy = a.y - b.y;
  const double dz = a.z - b.z;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

int AddJoint(std::vector<jpov::MeshData::WindJoint>* joints, const Vec3d& pivot, int parent,
             double level, double phase, double soft) {
  jpov::MeshData::WindJoint j;
  j.pivot[0] = static_cast<float>(pivot.x);
  j.pivot[1] = static_cast<float>(pivot.y);
  j.pivot[2] = static_cast<float>(pivot.z);
  j.parent = static_cast<float>(parent);
  j.level = static_cast<float>(level);
  j.phase = static_cast<float>(phase);
  j.soft = static_cast<float>(soft);
  joints->push_back(j);
  return static_cast<int>(joints->size()) - 1;
}

}  // namespace

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

int FindNearestSampleIndex(const Branch& b, const Vec3d& p) {
  CHECK(!b.samples.empty()) << "FindNearestSampleIndex: 枝没有采样点";
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

BranchJoints BuildBranchJoints(const Branch& b, const GlbBakeOptions& opts) {
  CHECK(!b.samples.empty()) << "BuildBranchJoints: 枝没有采样点";
  BranchJoints out;
  const size_t n = b.samples.size();
  out.cum.assign(n, 0.0);
  for (size_t i = 1; i < n; ++i) {
    out.cum[i] = out.cum[i - 1] + Distance(b.samples[i - 1].position, b.samples[i].position);
  }
  const double len = out.cum.back();
  const bool thick = static_cast<int>(b.level) <= opts.thick_level_max;
  const double spacing = thick ? opts.thick_spacing : opts.thin_spacing;
  CHECK_GT(spacing, 1e-6) << "BuildBranchJoints: 关节间距必须 > 0（粗枝 " << opts.thick_spacing
                          << "、细枝 " << opts.thin_spacing << "）";
  CHECK_GE(opts.max_joints_per_branch, 1) << "BuildBranchJoints: max_joints_per_branch 必须 >= 1";

  int m = static_cast<int>(std::lround(len / spacing));
  m = std::max(1, std::min(opts.max_joints_per_branch, m));

  // 按下标均匀取 m+1 个保留点。首尾一定在内（k=0 → 0，k=m → n-1），
  // 中间的重复下标（枝很短、m 相对采样数偏大时）顺手去重。
  const size_t last = n - 1;
  std::vector<uint32_t> kept;
  kept.reserve(static_cast<size_t>(m) + 1);
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
  // 判据用**平均半径**（一根枝整体是"细条"还是"树干"），不是逐点半径。
  const double droop = b.samples.front().position.y - b.samples.back().position.y;
  double rsum = 0.0;
  for (const BranchSample& s : b.samples) {
    rsum += s.radius;
  }
  const double mean_radius = rsum / static_cast<double>(n);
  double strand_soft = 0.0;
  if (len > 1e-6 && droop > opts.droop_min && mean_radius < opts.strand_radius_max) {
    CHECK_GT(opts.droop_scale, 1e-6) << "BuildBranchJoints: droop_scale 必须 > 0";
    strand_soft = std::max(0.0, std::min(1.0, droop / opts.droop_scale));
  }
  out.soft_at.resize(kept.size());
  for (size_t k = 0; k < kept.size(); ++k) {
    out.soft_at[k] = (len > 1e-6) ? strand_soft * (out.arc[k] / len) : 0.0;
  }
  return out;
}

JointLayout BuildJointLayout(const Shape& shape, const GlbBakeOptions& opts) {
  CHECK(!shape.branches.empty()) << "BuildJointLayout: shape 没有枝条";
  JointLayout out;
  out.per_branch.resize(shape.branches.size());
  out.joint_of_kept.resize(shape.branches.size());

  // 枝 id → 下标（Validate 保证 id 拓扑序，但这里仍按 id 查，避免假设下标==id）。
  uint32_t max_id = 0;
  for (const Branch& b : shape.branches) {
    max_id = std::max(max_id, b.id);
  }
  std::vector<int> branch_index_by_id(max_id + 1, -1);
  for (size_t i = 0; i < shape.branches.size(); ++i) {
    branch_index_by_id[shape.branches[i].id] = static_cast<int>(i);
  }

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
        const BranchJoints& pbj = out.per_branch[static_cast<size_t>(pi)];
        const std::vector<int>& pjoint_of_kept = out.joint_of_kept[static_cast<size_t>(pi)];
        CHECK(!pbj.kept.empty()) << "BuildJointLayout: 父枝还没有关节（枝序必须是拓扑序）";
        CHECK_EQ(pjoint_of_kept.size(), pbj.kept.size())
            << "BuildJointLayout: 父枝映射与关节数不一致";
        const int nearest =
            FindNearestSampleIndex(shape.branches[static_cast<size_t>(pi)], pivot);
        int q = 0;
        for (size_t kk = 0; kk < pbj.kept.size(); ++kk) {
          if (static_cast<int>(pbj.kept[kk]) <= nearest) {
            q = static_cast<int>(kk);
          } else {
            break;
          }
        }
        parent = pjoint_of_kept[static_cast<size_t>(q)];
        CHECK_GE(parent, 0) << "BuildJointLayout: 父枝关节下标未初始化";
      }
      // level = 距根的**关节数**（shader 端 clamp 到 [0,3]，这里如实写）。
      double level = 0.0;
      if (parent >= 0) {
        level = out.joints[static_cast<size_t>(parent)].level + 1.0;
      }
      const double phase = SmoothPhase01(pivot.x, pivot.y, pivot.z, opts.phase_scale);
      joint_of_kept[k] = AddJoint(&out.joints, pivot, parent, level, phase, bj.soft_at[k]);
    }
    out.joint_of_kept[bi] = std::move(joint_of_kept);
    out.per_branch[bi] = std::move(bj);
  }
  return out;
}

int JointCoveringSample(const BranchJoints& bj, const std::vector<int>& joint_of_kept,
                        uint32_t sample_index) {
  CHECK_EQ(bj.kept.size(), joint_of_kept.size())
      << "JointCoveringSample: 保留点与关节映射长度不一致";
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

}  // namespace vegetation
}  // namespace jpov
