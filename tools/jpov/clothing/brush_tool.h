// JPOV 穿衣工具 — 3D 画笔（选区）工具（pure header-only，GL-free，可单测）。
//
// 概念：一个「画笔激活态」下的**二值选区**——衣服模型上被涂抹到的顶点集合。
//   - 选区状态 = 各 primitive 的顶点 index 集合（本类唯一存的状态）。
//   - 只有激活态存在「选区」概念：SetActive(false) 会**清空**选区（生命周期结束）。
//   - 涂抹只**增不减**（同一次激活里，被涂到的点持续保持选中，直到生命周期结束）。
//
// 选点逻辑（"世界射线 + 距离"）：鼠标像素 → 世界射线（见 camera_projection.h），
// 衣物顶点到射线**垂距**足够近即选中；半径按深度换算：
//     世界半径 r(t) = radius_px · 2 · tan_half_fov · t / screen_h
//   （t = 顶点沿射线的深度）—— 即「屏幕上 radius_px 像素的圆」在该深度处的世界半径。
//   该换算下，射线垂距 ≤ r(t) ⟺ 顶点在屏幕上的投影落在以鼠标为中心、radius_px 的圆内
//   （见 brush_tool_test.cc 的往返断言）。
//
// 与 3D 笔刷家族的关系（本次只做「选区」这一层，具体编辑动作后续各 PR 接）：
//   - 平移/旋转/缩放（clothing tool 左侧）→ 可理解为「选区内」变换；
//   - LAG 副运动 → 对裙摆选区整体设系数；
//   - alpha 调整 → 对选区对应纹理区域整体去 alpha。

#ifndef JPOV_CLOTHING_BRUSH_TOOL_H_
#define JPOV_CLOTHING_BRUSH_TOOL_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/interface/camera.h"  // jpov::Vec3f

namespace jpov {
namespace clothing {

// 顶点 v 是否落在世界射线（origin + t·dir，t>0，dir 单位）的画笔锥内。
//   radius_px：画笔半径（屏幕像素）；screen_h：窗口高（像素）；
//   tan_half_fov：tan(垂直 fov / 2)。
// 返回 false 亦覆盖：顶点在相机后方（t ≤ 0）。
// Pre-condition: radius_px>0、screen_h>0、tan_half_fov>0、|dir|≈1。
inline bool VertexInBrush(const Vec3f& v, const Vec3f& ray_o, const Vec3f& ray_d,
                          float radius_px, float screen_h, double tan_half_fov) {
    CHECK_GT(radius_px, 0.0f);
    CHECK_GT(screen_h, 0.0f);
    CHECK_GT(tan_half_fov, 0.0);
    const double wx = static_cast<double>(v.x()) - ray_o.x();
    const double wy = static_cast<double>(v.y()) - ray_o.y();
    const double wz = static_cast<double>(v.z()) - ray_o.z();
    // 沿射线的深度（dir 单位 ⇒ 即投影长度）。
    const double t = wx * ray_d.x() + wy * ray_d.y() + wz * ray_d.z();
    if (t <= 0.0) {
        return false;  // 相机后方
    }
    // 垂距 = |w − t·dir|。
    const double px_ = wx - t * ray_d.x();
    const double py_ = wy - t * ray_d.y();
    const double pz_ = wz - t * ray_d.z();
    const double perp = std::sqrt(px_ * px_ + py_ * py_ + pz_ * pz_);
    const double world_r =
        static_cast<double>(radius_px) * 2.0 * tan_half_fov * t /
        static_cast<double>(screen_h);
    return perp <= world_r;
}

// 3D 画笔（选区）工具。状态 = 激活态 + 半径（像素）+ 选区（各 primitive 的顶点 index）。
//
// 选区按 primitive 分组（衣服是多 primitive glTF）；每组内 index **升序去重**（确定性）。
class BrushTool {
public:
    static constexpr float kMinRadiusPx     = 5.0f;
    static constexpr float kMaxRadiusPx     = 200.0f;
    static constexpr float kDefaultRadiusPx = 100.0f;

    // ---- 激活态（生命周期）----
    bool active() const { return active_; }
    // 设置激活态。置 false = 生命周期结束 → **清空选区**。
    void SetActive(bool on) {
        active_ = on;
        if (!on) {
            ClearSelection();
        }
    }
    void Toggle() { SetActive(!active_); }

    // ---- 画笔半径（屏幕像素）----
    float radius_px() const { return radius_px_; }
    void SetRadiusPx(float px) {
        radius_px_ = std::clamp(px, kMinRadiusPx, kMaxRadiusPx);
    }
    // UI 滑条写入用（值域由滑条 min/max 限制；如需严格 clamp 请用 SetRadiusPx）。
    float* radius_px_mutable() { return &radius_px_; }

    // ---- 选区 ----
    // 调整每个 primitive 的选区容器数量（衣物 primitive 数变化时调用；保留已有选区）。
    void EnsurePrimitives(size_t prim_count) { selected_.resize(prim_count); }

    void ClearSelection() {
        for (std::vector<uint32_t>& s : selected_) {
            s.clear();
        }
    }
    bool HasSelection() const {
        for (const std::vector<uint32_t>& s : selected_) {
            if (!s.empty()) {
                return true;
            }
        }
        return false;
    }
    size_t selected_count() const {
        size_t n = 0;
        for (const std::vector<uint32_t>& s : selected_) {
            n += s.size();
        }
        return n;
    }
    size_t primitive_count() const { return selected_.size(); }
    // 某 primitive 的选中顶点 index（升序、去重；只读）。
    const std::vector<uint32_t>& selected(size_t prim) const {
        CHECK_LT(prim, selected_.size());
        return selected_[prim];
    }
    bool IsSelected(size_t prim, uint32_t vertex_index) const {
        if (prim >= selected_.size()) {
            return false;
        }
        const std::vector<uint32_t>& s = selected_[prim];
        return std::binary_search(s.begin(), s.end(), vertex_index);
    }

    // 沿世界射线涂抹 prim 号 primitive：把 positions 中落在画笔内的顶点选入选区。
    // 返回本次**新增**选中的顶点数（已选中的不重复计）。
    // Pre-condition: prim < primitive_count()（先 EnsurePrimitives）。
    size_t PaintAlongRay(size_t prim, const std::vector<Vec3f>& positions,
                         const Vec3f& ray_o, const Vec3f& ray_d, float radius_px,
                         float screen_h, double tan_half_fov) {
        CHECK_LT(prim, selected_.size())
            << "PaintAlongRay: prim 越界，需先 EnsurePrimitives";
        std::vector<uint32_t>& sel = selected_[prim];
        size_t added = 0;
        for (size_t i = 0; i < positions.size(); ++i) {
            if (!VertexInBrush(positions[i], ray_o, ray_d, radius_px, screen_h,
                               tan_half_fov)) {
                continue;
            }
            const uint32_t idx = static_cast<uint32_t>(i);
            const std::vector<uint32_t>::iterator it =
                std::lower_bound(sel.begin(), sel.end(), idx);
            if (it == sel.end() || *it != idx) {
                sel.insert(it, idx);  // 保持升序去重
                ++added;
            }
        }
        return added;
    }

private:
    bool active_ = false;
    float radius_px_ = kDefaultRadiusPx;
    std::vector<std::vector<uint32_t>> selected_;  // 各 primitive：升序、去重
};

}  // namespace clothing
}  // namespace jpov

#endif  // JPOV_CLOTHING_BRUSH_TOOL_H_
