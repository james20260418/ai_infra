// JPOV RenderCommandList 成员函数实现
//
// 所有 RenderCommandList 的辅助方法在此定义。
// 见 render_command.h 中的声明。

#include "tools/jpov/interface/render_command.h"

namespace jpov {

void RenderCommandList::Clear() {
    polyline2d.clear();
    rect2d.clear();
    circle2d.clear();
    text2d.clear();
    line3d.clear();
    triangle3d.clear();
    strip3d.clear();
    text3d.clear();
    strip2d.clear();
    roundrect2d.clear();
    fillrect2d.clear();
    arc2d.clear();
    image2d.clear();
    object3d.clear();
    order.clear();
    // 注意：camera.fbo_3d_width_/height_ 不清零
}

void RenderCommandList::DrawPolyline(const std::vector<Vec2f>& vertices,
                                     const Color& color, float line_width) {
    CHECK_GT(line_width, 0.0f);
    int idx = static_cast<int>(polyline2d.size());
    polyline2d.push_back({vertices, color, line_width});
    order.emplace_back(DrawCommandType::kPolyline2D, idx);
}

void RenderCommandList::DrawRect(const Vec2f& pos, const Vec2f& size,
                                  const Color& color) {
    int idx = static_cast<int>(rect2d.size());
    rect2d.push_back({pos, size, color});
    order.emplace_back(DrawCommandType::kRect2D, idx);
}

void RenderCommandList::DrawCircle(const Vec2f& center, float radius,
                                    const Color& color) {
    CHECK_GT(radius, 0.0f);
    int idx = static_cast<int>(circle2d.size());
    circle2d.push_back({center, radius, color});
    order.emplace_back(DrawCommandType::kCircle2D, idx);
}

void RenderCommandList::DrawText(const std::string& text, const Vec2f& pos,
                                  float font_size, const Color& color,
                                  TextAlignment alignment,
                                  const std::string& font_alias) {
    CHECK_GT(font_size, 0.0f);
    int idx = static_cast<int>(text2d.size());
    text2d.push_back({text, pos, font_size, color, alignment, font_alias});
    order.emplace_back(DrawCommandType::kText2D, idx);
}

void RenderCommandList::DrawLine3D(const Vec3f& p1, const Vec3f& p2,
                                    const Color& color, float width) {
    CHECK_GT(width, 0.0f);
    int idx = static_cast<int>(line3d.size());
    line3d.push_back({p1, p2, color, width});
    order.emplace_back(DrawCommandType::kLine3D, idx);
}

void RenderCommandList::DrawTriangle3D(const Vec3f& p1, const Vec3f& p2,
                                        const Vec3f& p3, const Color& color) {
    int idx = static_cast<int>(triangle3d.size());
    triangle3d.push_back({p1, p2, p3, color});
    order.emplace_back(DrawCommandType::kTriangle3D, idx);
}

void RenderCommandList::DrawStrip3D(const std::vector<Vec3f>& vertices,
                                     const Color& color) {
    if (vertices.size() < 3) {
        LOG_EVERY_N(WARNING, 100) << "DrawStrip3D: less than 3 vertices, skipping";
        return;
    }
    int idx = static_cast<int>(strip3d.size());
    strip3d.push_back({vertices, color});
    order.emplace_back(DrawCommandType::kStrip3D, idx);
}

void RenderCommandList::DrawText3D(const std::string& text, const Vec3f& pos,
                                    float font_size, const Color& color,
                                    const std::string& font_alias) {
    CHECK_GT(font_size, 0.0f);
    int idx = static_cast<int>(text3d.size());
    text3d.push_back({text, pos, font_size, color, font_alias});
    order.emplace_back(DrawCommandType::kText3D, idx);
}

void RenderCommandList::DrawStrip2D(const std::vector<Vec2f>& vertices,
                                     const Color& color) {
    if (vertices.size() < 3) {
        LOG_EVERY_N(WARNING, 100) << "DrawStrip2D: less than 3 vertices, skipping";
        return;
    }
    int idx = static_cast<int>(strip2d.size());
    strip2d.push_back({vertices, color});
    order.emplace_back(DrawCommandType::kStrip2D, idx);
}

void RenderCommandList::DrawRoundRect(const Vec2f& pos, const Vec2f& size,
                                       float radius, const Color& color) {
    CHECK_GT(size.x(), 0.0f);
    CHECK_GT(size.y(), 0.0f);
    CHECK_GE(radius, 0.0f);
    float half_min = std::min(size.x(), size.y()) * 0.5f;
    CHECK_LE(radius, half_min)
        << "RoundRect radius " << radius << " exceeds half of min side " << half_min;

    int idx = static_cast<int>(roundrect2d.size());
    roundrect2d.push_back({pos, size, radius, color});
    order.emplace_back(DrawCommandType::kRoundRect2D, idx);
}

void RenderCommandList::DrawArc2D(const Vec2f& center, float radius,
                                     float start_angle, float span_angle,
                                     const Color& color) {
    CHECK_GT(radius, 0.0f);
    int idx = static_cast<int>(arc2d.size());
    arc2d.push_back({center, radius, start_angle, span_angle, color});
    order.emplace_back(DrawCommandType::kArc2D, idx);
}

void RenderCommandList::DrawFillRect(const Vec2f& pos, const Vec2f& size,
                                       const Color& fill_color,
                                       const Color& border_color,
                                       float border_width, float radius) {
    CHECK_GT(size.x(), 0.0f);
    CHECK_GT(size.y(), 0.0f);
    CHECK_GE(radius, 0.0f);
    float half_min = std::min(size.x(), size.y()) * 0.5f;
    CHECK_LE(radius, half_min)
        << "FillRect radius " << radius << " exceeds half of min side " << half_min;
    CHECK_GE(border_width, 0.0f);

    int idx = static_cast<int>(fillrect2d.size());
    fillrect2d.push_back({pos, size, fill_color, border_color, border_width, radius});
    order.emplace_back(DrawCommandType::kFillRect2D, idx);
}

void RenderCommandList::DrawImage(uint32_t texture_id, const Vec2f& pos,
                                   const Vec2f& size, const Color& tint) {
    CHECK_GT(texture_id, 0u);
    CHECK_GT(size.x(), 0.0f);
    CHECK_GT(size.y(), 0.0f);
    int idx = static_cast<int>(image2d.size());
    image2d.push_back({texture_id, pos, size, tint});
    order.emplace_back(DrawCommandType::kImage2D, idx);
}

void RenderCommandList::DrawObject3D(uint32_t mesh_id, uint32_t texture_id,
                                      const Color& color,
                                      const Vec3f& center,
                                      const Vec3f& up, const Vec3f& front) {
    CHECK_GT(mesh_id, 0u);
    // 纹理着色要求 mesh 含 UV，运行期在 Renderer 中校验（此处不知 mesh flags）。
    int idx = static_cast<int>(object3d.size());
    object3d.push_back({mesh_id, texture_id, color, center, up, front});
    order.emplace_back(DrawCommandType::kObject3D, idx);
}

}  // namespace jpov
