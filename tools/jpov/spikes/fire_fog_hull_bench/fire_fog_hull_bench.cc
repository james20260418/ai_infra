// Fire-Fog tile culling 三法基准（临时 spike，验证后可删）
//
// 对比「给定 box 的屏幕 8 点，求它覆盖哪些 tile」的三种做法：
//   A) AABB      —— 8 点屏幕 AABB 矩形 → 标矩形内所有 tile（廉价保守，会多标四角）
//   B) SAT       —— 凸包(单调链) + 逐 tile 矩形 vs 凸多边形 分离轴测试（精确但每 tile 一次测试）
//   C) 扫描线    —— ScreenHull(PWL 上下壳) + 扫描线（geom::math::ConvexHullCoveredTiles）
//
// 「随机覆盖构造」：随机 3D box（随机中心 + 随机尺寸，覆盖从几像素到几百像素）→
// 透视投影 8 角点 → 屏幕 8 点。三者吃同样的 8 点，苹果对苹果。
//
// 运行：bazel run //tools/jpov/spikes/fire_fog_hull_bench:fire_fog_hull_bench [N]

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "geom/math/convex_hull_tiles.h"

namespace {

using geom::math::ConvexHullCoveredTiles;
using geom::math::TileCoord;
using geom::math::TileGrid;
using geom::math::Vec2d;

struct V3 { double x, y, z; };

// 相机在 (0,0,25) 看向原点，up=+y，fov 60 → 列主序 mvp。
void BuildMvp(double mvp[16]) {
  const double cam[3] = {0, 0, 25};
  const double fwd[3] = {0, 0, -1};
  const double right[3] = {1, 0, 0};
  const double up[3] = {0, 1, 0};
  const double view[16] = {
      right[0], up[0], -fwd[0], 0,
      right[1], up[1], -fwd[1], 0,
      right[2], up[2], -fwd[2], 0,
      -(right[0]*cam[0]+right[1]*cam[1]+right[2]*cam[2]),
      -(up[0]*cam[0]+up[1]*cam[1]+up[2]*cam[2]),
       (fwd[0]*cam[0]+fwd[1]*cam[1]+fwd[2]*cam[2]), 1};
  const double t = 1.0 / std::tan(60.0 * M_PI / 360.0);
  const double n = 0.05, f = 1000.0;
  const double proj[16] = {t, 0, 0, 0, 0, t, 0, 0, 0, 0, (f+n)/(n-f), -1,
                           0, 0, 2*f*n/(n-f), 0};
  for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) {
    double s = 0;
    for (int k = 0; k < 4; ++k) s += proj[k*4+r] * view[c*4+k];
    mvp[c*4+r] = s;
  }
}

bool ProjectPoint(const double m[16], const V3& p, int W, int H, double* ox, double* oy) {
  const double w = m[3]*p.x + m[7]*p.y + m[11]*p.z + m[15];
  if (w <= 0) return false;
  const double inv = 1.0 / w;
  *ox = ((m[0]*p.x + m[4]*p.y + m[8]*p.z + m[12]) * inv * 0.5 + 0.5) * W;
  *oy = ((m[1]*p.x + m[5]*p.y + m[9]*p.z + m[13]) * inv * 0.5 + 0.5) * H;
  return true;
}

// ---------- 方法 A：AABB 矩形 → tile 计数 ----------
long long CountAabb(const Vec2d* pts, double cell, int cols, int rows, int W, int H) {
  double mnx = 1e300, mxx = -1e300, mny = 1e300, mxy = -1e300;
  for (int i = 0; i < 8; ++i) {
    mnx = std::min(mnx, pts[i].x); mxx = std::max(mxx, pts[i].x);
    mny = std::min(mny, pts[i].y); mxy = std::max(mxy, pts[i].y);
  }
  auto cl = [](double v, double cell, int n) {
    double f = std::floor(v / cell);
    if (f < 0) return 0;
    if (f > n - 1) return n - 1;
    return static_cast<int>(f);
  };
  const int c0 = cl(mnx, cell, cols), c1 = cl(mxx, cell, cols);
  const int r0 = cl(mny, cell, rows), r1 = cl(mxy, cell, rows);
  (void)W; (void)H;
  if (c0 > c1 || r0 > r1) return 0;
  return static_cast<long long>(c1 - c0 + 1) * (r1 - r0 + 1);
}

// ---------- 方法 B：凸包 + 逐 tile SAT ----------
std::vector<Vec2d> HullPoly(std::vector<Vec2d> p) {
  std::sort(p.begin(), p.end(), [](const Vec2d& a, const Vec2d& b) {
    return a.x != b.x ? a.x < b.x : a.y < b.y;
  });
  p.erase(std::unique(p.begin(), p.end(), [](const Vec2d& a, const Vec2d& b) {
            return a.x == b.x && a.y == b.y; }), p.end());
  if (p.size() < 3) return p;
  auto cr = [](const Vec2d& o, const Vec2d& a, const Vec2d& b) {
    return (a.x-o.x)*(b.y-o.y) - (a.y-o.y)*(b.x-o.x); };
  std::vector<Vec2d> h;
  for (const Vec2d& q : p) {
    while (h.size() >= 2 && cr(h[h.size()-2], h.back(), q) <= 0) h.pop_back();
    h.push_back(q);
  }
  const size_t lower = h.size() + 1;
  for (size_t i = p.size() - 1; i-- > 0;) {
    while (h.size() >= lower && cr(h[h.size()-2], h.back(), p[i]) <= 0) h.pop_back();
    h.push_back(p[i]);
  }
  h.pop_back();
  return h;
}

bool RectHullSat(double x0, double y0, double x1, double y1, const std::vector<Vec2d>& h) {
  auto sep = [&](double ax, double ay) -> bool {
    const double rx[4] = {x0, x1, x0, x1};
    const double ry[4] = {y0, y0, y1, y1};
    double rmin = 1e300, rmax = -1e300;
    for (int i = 0; i < 4; ++i) { double q = ax*rx[i]+ay*ry[i]; rmin=std::min(rmin,q); rmax=std::max(rmax,q); }
    double hmin = 1e300, hmax = -1e300;
    for (const Vec2d& v : h) { double q = ax*v.x+ay*v.y; hmin=std::min(hmin,q); hmax=std::max(hmax,q); }
    return (rmax < hmin) || (hmax < rmin);
  };
  if (sep(1,0) || sep(0,1)) return false;
  for (size_t i = 0; i < h.size(); ++i) {
    const Vec2d& a = h[i]; const Vec2d& b = h[(i+1)%h.size()];
    if (sep(-(b.y-a.y), (b.x-a.x))) return false;
  }
  return true;
}

long long CountSat(const Vec2d* pts, double cell, int cols, int rows, int W, int H) {
  std::vector<Vec2d> p(pts, pts + 8);
  const std::vector<Vec2d> h = HullPoly(p);
  if (h.size() < 3) return 0;
  double mnx = 1e300, mxx = -1e300, mny = 1e300, mxy = -1e300;
  for (const Vec2d& v : h) {
    mnx = std::min(mnx, v.x); mxx = std::max(mxx, v.x);
    mny = std::min(mny, v.y); mxy = std::max(mxy, v.y);
  }
  auto cl = [](double v, double cell, int n) {
    double f = std::floor(v / cell);
    if (f < 0) return 0;
    if (f > n - 1) return n - 1;
    return static_cast<int>(f);
  };
  const int c0 = cl(mnx, cell, cols), c1 = cl(mxx, cell, cols);
  const int r0 = cl(mny, cell, rows), r1 = cl(mxy, cell, rows);
  (void)W; (void)H;
  long long cnt = 0;
  for (int r = r0; r <= r1; ++r) {
    for (int c = c0; c <= c1; ++c) {
      if (RectHullSat(c*cell, r*cell, (c+1)*cell, (r+1)*cell, h)) ++cnt;
    }
  }
  return cnt;
}

}  // namespace

int main(int argc, char** argv) {
  int N = 20000;
  for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], "--n") && i+1 < argc) N = atoi(argv[i+1]);

  const int W = 1920, H = 1080;
  const double cell = 16.0;
  TileGrid g; g.cell = cell; g.cols = W/16; g.rows = H/16;
  double mvp[16]; BuildMvp(mvp);

  // 随机覆盖构造：随机 box（随机中心 + 随机半尺寸，跨数量级）→ 投影 8 点。
  std::mt19937_64 rng(20261007);
  std::uniform_real_distribution<double> Uc(-18.0, 18.0);
  std::uniform_real_distribution<double> Uz(-9.0, 9.0);
  std::uniform_real_distribution<double> logh(std::log(0.15), std::log(5.0));
  std::vector<std::array<Vec2d, 8>> cases(N);
  int skipped = 0;
  for (int i = 0; i < N; ++i) {
    const double cx = Uc(rng), cy = Uc(rng), cz = Uz(rng);
    const double hx = std::exp(logh(rng)), hy = std::exp(logh(rng)), hz = std::exp(logh(rng));
    int k = 0;
    bool ok = true;
    for (int sx = -1; sx <= 1; sx += 2)
      for (int sy = -1; sy <= 1; sy += 2)
        for (int sz = -1; sz <= 1; sz += 2) {
          double px, py;
          if (!ProjectPoint(mvp, V3{cx+sx*hx, cy+sy*hy, cz+sz*hz}, W, H, &px, &py)) { ok = false; break; }
          cases[i][k++] = Vec2d{px, py};
        }
    if (!ok) { ++skipped; }
  }

  auto run = [&](const char* name, auto fn) {
    // 预热
    long long warm = 0;
    for (int i = 0; i < 200; ++i) warm += fn(cases[i].data());
    double best = 1e30;
    long long total = 0;
    for (int rep = 0; rep < 5; ++rep) {
      long long tiles = 0;
      auto t0 = std::chrono::high_resolution_clock::now();
      for (int i = 0; i < N; ++i) tiles += fn(cases[i].data());
      auto t1 = std::chrono::high_resolution_clock::now();
      double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
      if (ms < best) { best = ms; total = tiles; }
    }
    (void)warm;
    printf("%-28s | %8.2f ms | total_tiles=%9lld | tiles/box=%7.1f\n",
           name, best, total, static_cast<double>(total) / N);
  };

  printf("grid %dx%d tile=%.0f  N=%d (skipped=%d)\n", g.cols, g.rows, cell, N, skipped);
  printf("%-28s | %11s | %s\n", "method", "time", "tiles");
  printf("--------------------------------------------------------------------\n");

  run("A) AABB (rect)", [&](const Vec2d* pts) {
    return CountAabb(pts, cell, g.cols, g.rows, W, H);
  });
  run("B) SAT (hull + per-tile)", [&](const Vec2d* pts) {
    return CountSat(pts, cell, g.cols, g.rows, W, H);
  });
  run("C) scanline (ConvexHullTiles)", [&](const Vec2d* pts) {
    std::vector<Vec2d> p(pts, pts + 8);
    return static_cast<long long>(ConvexHullCoveredTiles(g, p).size());
  });
  printf("\n");
  return 0;
}
