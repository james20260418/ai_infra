// JPOV 空间殖民（Space Colonization）—— 实现（GL-free、确定性）。
//
// 结构：
//   §1 吸引点云采样（按包络形状）
//   §2 殖民主循环（认领 → 生长 → 吞噬）
//   §3 后处理：管道模型粗细 + 拆枝链 + 叶位
//   §4 内置树种（4 种常见植物）
//
// 见 space_colonization.h 的算法说明与输出约定。

#include "tools/jpov/src/vegetation/space_colonization.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <glog/logging.h>

namespace jpov {
namespace vegetation {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

using Vec3f = geom::Vec3<float>;

inline Vec3f VAdd(const Vec3f& a, const Vec3f& b) {
    return Vec3f(a.x() + b.x(), a.y() + b.y(), a.z() + b.z());
}
inline Vec3f VSub(const Vec3f& a, const Vec3f& b) {
    return Vec3f(a.x() - b.x(), a.y() - b.y(), a.z() - b.z());
}
inline Vec3f VScale(const Vec3f& a, float s) {
    return Vec3f(a.x() * s, a.y() * s, a.z() * s);
}
inline float VDot(const Vec3f& a, const Vec3f& b) {
    return a.x() * b.x() + a.y() * b.y() + a.z() * b.z();
}
inline Vec3f VCross(const Vec3f& a, const Vec3f& b) {
    return Vec3f(a.y() * b.z() - a.z() * b.y(),
                 a.z() * b.x() - a.x() * b.z(),
                 a.x() * b.y() - a.y() * b.x());
}
inline float VLen(const Vec3f& a) { return std::sqrt(VDot(a, a)); }
inline float VDist2(const Vec3f& a, const Vec3f& b) {
    const Vec3f d = VSub(a, b);
    return VDot(d, d);
}
// 归一化；零向量（长度 < eps）返回 false 且不改 out。
inline bool VNormTo(const Vec3f& a, float eps, Vec3f* out /*output*/) {
    const float l = VLen(a);
    if (l < eps) {
        return false;
    }
    *out = VScale(a, 1.0f / l);
    return true;
}

// ============================ §1 吸引点云采样 ============================

// 给定「冠内高度占比」t ∈ [0,1]，返回该高度处的冠半径（包络形状函数）。
float CrownRadiusAt(CrownEnvelope env, float t, float base_radius,
                    float top_radius) {
    switch (env) {
        case CrownEnvelope::kCone:
            return base_radius + (top_radius - base_radius) * t;
        case CrownEnvelope::kDome: {
            // 椭球穹顶：底最宽、顶收圆。
            const float s = 1.0f - t * t;
            return base_radius * std::sqrt(std::max(0.0f, s));
        }
        case CrownEnvelope::kEllipsoid: {
            // 竖椭球：中部最胖、上下收。
            const float u = 2.0f * t - 1.0f;
            const float s = 1.0f - u * u;
            return base_radius * std::sqrt(std::max(0.0f, s));
        }
    }
    LOG(FATAL) << "未知包络形状";
    return 0.0f;
}

// 按包络形状撒 attractor_count 个吸引点（确定性：消耗给定的 rng）。
// 用**边界盒拒绝采样**保证点云**体积均匀**（关键：若按高度均匀采样，锥顶半径小 ⇒ 顶部点
// 密度 ∝ 1/r² 反而高，会把生长全往上拉 ⇒ "树枝往上冒"）。
std::vector<Vec3f> SampleAttractors(const SpaceColonizationConfig& cfg,
                                    Rng* rng) {
    std::vector<Vec3f> pts;
    pts.reserve(static_cast<size_t>(cfg.attractor_count));
    const float rmax = cfg.crown_radius;
    if (rmax <= 1.0e-6f) {   // 退化为纯竖直轴（无冠幅）
        for (int i = 0; i < cfg.attractor_count; ++i) {
            pts.push_back(Vec3f(0.0f,
                cfg.trunk_height + rng->NextFloat01() * cfg.crown_height, 0.0f));
        }
        return pts;
    }
    while (static_cast<int>(pts.size()) < cfg.attractor_count) {
        const float x = (rng->NextFloat01() * 2.0f - 1.0f) * rmax;
        const float z = (rng->NextFloat01() * 2.0f - 1.0f) * rmax;
        const float t = rng->NextFloat01();   // 冠内高度占比
        const float r_lim = CrownRadiusAt(cfg.envelope, t, cfg.crown_radius,
                                          cfg.crown_top_radius);
        if (x * x + z * z <= r_lim * r_lim) {   // 在包络内 → 接受
            pts.push_back(
                Vec3f(x, cfg.trunk_height + t * cfg.crown_height, z));
        }
    }
    return pts;
}

}  // namespace

// ============================ §2+3 殖民主循环 + 后处理 ============================

Skeleton Colonize(const SpaceColonizationConfig& cfg) {
    CHECK_GT(cfg.attractor_count, 0) << "attractor_count 必须 > 0";
    CHECK_GE(cfg.trunk_height, 0.0f) << "trunk_height 必须 >= 0";
    CHECK_GT(cfg.crown_height, 0.0f) << "crown_height 必须 > 0";
    CHECK_GE(cfg.crown_radius, 0.0f) << "crown_radius 必须 >= 0";
    CHECK_GE(cfg.crown_top_radius, 0.0f) << "crown_top_radius 必须 >= 0";
    CHECK_GT(cfg.influence_radius, 0.0f) << "influence_radius 必须 > 0";
    CHECK_GT(cfg.kill_radius, 0.0f) << "kill_radius 必须 > 0";
    CHECK_LT(cfg.kill_radius, cfg.influence_radius)
        << "kill_radius 必须 < influence_radius";
    CHECK_GT(cfg.step, 0.0f) << "step 必须 > 0";
    CHECK_GE(cfg.max_nodes, 1) << "max_nodes 必须 >= 1";
    CHECK_GT(cfg.base_radius, 0.0f) << "base_radius 必须 > 0";
    CHECK_GT(cfg.min_radius, 0.0f) << "min_radius 必须 > 0";
    CHECK_LE(cfg.min_radius, cfg.base_radius)
        << "min_radius 必须 <= base_radius";
    CHECK_GT(cfg.radius_power, 0.0f) << "radius_power 必须 > 0";

    Rng rng(cfg.seed);
    std::vector<Vec3f> attractors = SampleAttractors(cfg, &rng);

    // 节点图：位置 + 母节点下标（-1 = 根）。父下标恒小于子下标（方便逆序递推）。
    struct Node {
        Vec3f pos;
        int   parent;
    };
    std::vector<Node> nodes;
    nodes.push_back({Vec3f(0.0f, 0.0f, 0.0f), -1});

    // 裸干：从根沿 +Y 预置一段直干到 trunk_height。否则（如松/龙爪槐）树冠吸引点
    // 离根远于影响距离、根认领不到任何吸引点 ⇒ 树长不起来。trunk_height==0 → 无裸干。
    if (cfg.trunk_height > 0.0f) {
        const int trunk_steps = std::max(
            1, static_cast<int>(std::lround(cfg.trunk_height / cfg.step)));
        for (int i = 1; i <= trunk_steps; ++i) {
            nodes.push_back({Vec3f(0.0f, cfg.step * static_cast<float>(i), 0.0f),
                             static_cast<int>(nodes.size()) - 1});
        }
    }

    const float d2 = cfg.influence_radius * cfg.influence_radius;
    const float dk2 = cfg.kill_radius * cfg.kill_radius;

    // 把落在 p 的 kill 半径内的吸引点删掉（erase-remove）。
    auto sweep_kill = [&](const Vec3f& p) {
        attractors.erase(
            std::remove_if(attractors.begin(), attractors.end(),
                           [&](const Vec3f& a) { return VDist2(a, p) < dk2; }),
            attractors.end());
    };
    // 预置的裸干节点先清一遍（它们一开始就存在，周围的吸引点要吃掉）。
    for (const Node& nd : nodes) {
        sweep_kill(nd.pos);
    }

    // 节点数上限对应的生长轮数上限（每轮至少长 1 个节点）；再给一点余量防病态。
    const int kMaxIterations = cfg.max_nodes + 64;
    for (int iter = 0; iter < kMaxIterations; ++iter) {
        if (attractors.empty()) {
            break;
        }
        if (static_cast<int>(nodes.size()) >= cfg.max_nodes) {
            break;
        }

        // ① 认领：每个吸引点找最近的、且在影响距离内的节点。
        std::vector<int> owner(attractors.size(), -1);
        for (size_t ai = 0; ai < attractors.size(); ++ai) {
            int best = -1;
            float best_d2 = d2;
            for (size_t ni = 0; ni < nodes.size(); ++ni) {
                const float dd = VDist2(attractors[ai], nodes[ni].pos);
                if (dd < best_d2) {
                    best_d2 = dd;
                    best = static_cast<int>(ni);
                }
            }
            owner[ai] = best;
        }

        // ② 累加每个节点的吸引方向。
        std::vector<Vec3f> dir_sum(nodes.size(), Vec3f(0.0f, 0.0f, 0.0f));
        std::vector<int>   dir_count(nodes.size(), 0);
        for (size_t ai = 0; ai < attractors.size(); ++ai) {
            const int o = owner[ai];
            if (o < 0) {
                continue;
            }
            dir_sum[o] = VAdd(dir_sum[o], VSub(attractors[ai], nodes[o].pos));
            dir_count[o] += 1;
        }

        // ③ 生长：每个有吸引点的节点，朝平均方向（叠加向性）长一个新节点。
        Vec3f tropism_unit;
        const bool has_tropism =
            (cfg.tropism_strength > 0.0f) &&
            VNormTo(cfg.tropism, 1.0e-9f, &tropism_unit);
        std::vector<Node> born;
        for (size_t ni = 0; ni < nodes.size(); ++ni) {
            if (dir_count[ni] == 0) {
                continue;
            }
            Vec3f g;
            if (!VNormTo(dir_sum[ni], 1.0e-12f, &g)) {
                continue;   // 吸引方向抵消为零 → 本轮不长
            }
            Vec3f dir = g;
            if (has_tropism) {
                const Vec3f biased =
                    VAdd(g, VScale(tropism_unit, cfg.tropism_strength));
                Vec3f dir_n;
                if (VNormTo(biased, 1.0e-9f, &dir_n)) {
                    dir = dir_n;
                }
            }
            born.push_back({VAdd(nodes[ni].pos, VScale(dir, cfg.step)),
                            static_cast<int>(ni)});
        }
        if (born.empty()) {
            break;   // 有吸引点但都够不着（不应发生）→ 收尾
        }

        // ④ 追加新节点，并"吃掉"新节点周围（kill 半径内）的吸引点。
        std::vector<Vec3f> born_pos;
        born_pos.reserve(born.size());
        for (const Node& nd : born) {
            born_pos.push_back(nd.pos);
            nodes.push_back(nd);
        }
        attractors.erase(
            std::remove_if(
                attractors.begin(), attractors.end(),
                [&](const Vec3f& a) {
                    for (const Vec3f& p : born_pos) {
                        if (VDist2(a, p) < dk2) {
                            return true;
                        }
                    }
                    return false;
                }),
            attractors.end());
    }

    // ── 后处理 A：子节点表 + 子树**枝长**（管道模型粗细）──
    //    用"子树枝长"而非"末梢数"：单子链内末梢数恒定 ⇒ 半径恒定 ⇒ 每段都是圆柱；
    //    按枝长则沿枝单调递减 ⇒ **枝链自然收细**，不再像水管。
    const size_t n_nodes = nodes.size();
    std::vector<std::vector<int>> children(n_nodes);
    for (size_t i = 1; i < n_nodes; ++i) {
        children[nodes[i].parent].push_back(static_cast<int>(i));
    }
    // 逆序递推：sub_len[node] = 本段长 + Σ sub_len[child]（父下标恒小于子 ⇒ 逆序安全）。
    std::vector<float> sub_len(n_nodes, 0.0f);
    for (size_t i = n_nodes; i-- > 0;) {
        float s = 0.0f;
        for (int c : children[i]) {
            s += sub_len[c];
        }
        if (i != 0) {   // 加上"父→本节点"这段的长度（根没有入边）
            s += VLen(VSub(nodes[i].pos, nodes[nodes[i].parent].pos));
        }
        sub_len[i] = s;
    }
    const float sub_len_root = std::max(1.0e-6f, sub_len[0]);
    std::vector<float> radius(n_nodes, cfg.min_radius);
    for (size_t i = 0; i < n_nodes; ++i) {
        const float frac = sub_len[i] / sub_len_root;
        radius[i] = cfg.min_radius +
                    (cfg.base_radius - cfg.min_radius) *
                        std::pow(frac, 1.0f / cfg.radius_power);
    }

    // ── 后处理 B：组装 Skeleton ──
    Skeleton skel;

    // segments：每个非根节点一段。
    for (size_t i = 1; i < n_nodes; ++i) {
        Segment seg;
        seg.start = nodes[nodes[i].parent].pos;
        seg.end = nodes[i].pos;
        seg.radius = radius[i];
        skel.segments.push_back(seg);
    }

    // polylines：从每个分叉点（含根）出发，沿单子链走到下一分叉点/末梢。
    for (size_t i = 0; i < n_nodes; ++i) {
        const bool is_root = (i == 0);
        if (!is_root && children[i].size() < 2) {
            continue;   // 只从根与分叉点起链
        }
        for (int c : children[i]) {
            Polyline pl;
            // 领口缝合：非根分叉点的子链首点半径 = 母枝半径（根链不缝合）。
            pl.attach_radius = is_root ? 0.0f : radius[i];
            pl.points.push_back(nodes[i].pos);
            pl.radii.push_back(radius[i]);
            int cur = c;
            while (true) {
                pl.points.push_back(nodes[cur].pos);
                pl.radii.push_back(radius[cur]);
                if (children[cur].size() != 1) {
                    break;   // 分叉点或末梢 → 链到此为止
                }
                cur = children[cur][0];
            }
            skel.polylines.push_back(std::move(pl));
        }
    }

    // leaf_sites：末梢枝（未分叉的外围细枝）的每个节点放一个叶位，让外围铺满叶。
    // 定义：`bears_leaf[i]` = 子树无分叉（本节点入 1 个子且该子树也无分叉，或本节点即枝尖）。
    // 这样针叶树的主梢（顶端无分叉段）也会挂叶，不像光杆。
    if (cfg.leaves_on_terminal_twigs) {
        std::vector<char> bears_leaf(n_nodes, 0);
        for (size_t i = n_nodes; i-- > 0;) {
            if (children[i].empty()) {
                bears_leaf[i] = 1;
            } else if (children[i].size() == 1) {
                bears_leaf[i] = bears_leaf[children[i][0]];
            } else {
                bears_leaf[i] = 0;
            }
        }
        for (size_t i = 1; i < n_nodes; ++i) {   // 跳过根（根不是枝叶）
            if (!bears_leaf[i]) {
                continue;
            }
            Vec3f axis;
            if (!VNormTo(VSub(nodes[i].pos, nodes[nodes[i].parent].pos),
                         1.0e-9f, &axis)) {
                continue;
            }
            const Vec3f ref = (std::abs(axis.y()) > 0.9f)
                                  ? Vec3f(1.0f, 0.0f, 0.0f)
                                  : Vec3f(0.0f, 1.0f, 0.0f);
            Vec3f normal;
            VNormTo(VCross(axis, ref), 1.0e-9f, &normal);
            LeafSite site;
            site.position = nodes[i].pos;
            site.axis = axis;
            site.normal = normal;
            skel.leaf_sites.push_back(site);
        }
    }
    return skel;
}

// ============================ §4 内置树种 ============================

SpaceColonizationConfig FirConfig() {
    // 杉：高窄尖锥 —— 锥形包络（底宽顶尖）、微向光上挺保持塔形。
    SpaceColonizationConfig c;
    c.envelope = CrownEnvelope::kCone;
    c.attractor_count = 4000;
    c.trunk_height = 0.75f;
    c.crown_height = 2.1f;
    c.crown_radius = 0.80f;
    c.crown_top_radius = 0.10f;
    c.influence_radius = 0.30f;
    c.kill_radius = 0.15f;
    c.step = 0.09f;
    c.max_nodes = 4500;
    c.tropism = Vec3f(0.0f, -1.0f, 0.0f);   // 轻微重力：侧枝平展/微垂（不要向光上挺—那会让枝"往上冒"）
    c.tropism_strength = 0.05f;
    c.base_radius = 0.05f;
    c.min_radius = 0.003f;
    c.radius_power = 2.0f;
    c.leaves_on_terminal_twigs = true;
    c.seed = 7;
    return c;
}

SpaceColonizationConfig PineConfig() {
    // 松：下部裸干 + 上部圆冠 —— 穹顶包络、抬高裸干段。
    SpaceColonizationConfig c;
    c.envelope = CrownEnvelope::kDome;
    c.attractor_count = 2800;
    c.trunk_height = 0.7f;
    c.crown_height = 1.5f;
    c.crown_radius = 0.95f;
    c.crown_top_radius = 0.0f;   // 穹顶不用
    c.influence_radius = 0.38f;
    c.kill_radius = 0.18f;
    c.step = 0.11f;
    c.max_nodes = 4500;
    c.tropism = Vec3f(0.0f, 1.0f, 0.0f);
    c.tropism_strength = 0.03f;
    c.base_radius = 0.06f;
    c.min_radius = 0.003f;
    c.radius_power = 2.0f;
    c.leaves_on_terminal_twigs = true;
    c.seed = 11;
    return c;
}

SpaceColonizationConfig ShrubConfig() {
    // 灌木：几乎无干 + 矮宽丛 —— 竖椭球包络（中部最胖）、小步长多枝。
    SpaceColonizationConfig c;
    c.envelope = CrownEnvelope::kEllipsoid;
    c.attractor_count = 2200;
    c.trunk_height = 0.05f;
    c.crown_height = 1.0f;
    c.crown_radius = 0.85f;
    c.crown_top_radius = 0.0f;   // 椭球不用
    c.influence_radius = 0.36f;
    c.kill_radius = 0.20f;
    c.step = 0.10f;
    c.max_nodes = 4500;
    c.tropism = Vec3f(0.0f, 1.0f, 0.0f);
    c.tropism_strength = 0.02f;
    c.base_radius = 0.03f;
    c.min_radius = 0.003f;
    c.radius_power = 2.0f;
    c.leaves_on_terminal_twigs = true;
    c.seed = 3;
    return c;
}

SpaceColonizationConfig DragonClawConfig() {
    // 龙爪槐：矮干 + 宽扁伞盖 + 枝下垂 —— 穹顶包络（宽而矮）+ 重力向性下垂。
    SpaceColonizationConfig c;
    c.envelope = CrownEnvelope::kDome;
    c.attractor_count = 3200;
    c.trunk_height = 0.7f;
    c.crown_height = 1.2f;
    c.crown_radius = 1.2f;
    c.crown_top_radius = 0.0f;   // 穹顶不用
    c.influence_radius = 0.38f;
    c.kill_radius = 0.17f;
    c.step = 0.10f;
    c.max_nodes = 4500;
    c.tropism = Vec3f(0.0f, -1.0f, 0.0f);   // 重力向下
    c.tropism_strength = 0.18f;
    c.base_radius = 0.05f;
    c.min_radius = 0.003f;
    c.radius_power = 2.0f;
    c.leaves_on_terminal_twigs = true;
    c.seed = 5;
    return c;
}

}  // namespace vegetation
}  // namespace jpov
