// JPOV RenderCommandList 成员函数实现
//
// 所有 RenderCommandList 的辅助方法在此定义。
// 见 render_command.h 中的声明。

#include "tools/jpov/interface/render_command.h"

#include <utility>

#include "tools/jpov/interface/gltf_object.h"

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
    skinned_mesh.clear();
    fires.clear();
    burnings.clear();
    particles.clear();
    point_lights.clear();
    object_use_default_color = false;
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

void RenderCommandList::DrawText3D(const std::string& text,
                                    const Vec3f& anchor,
                                    const Vec3f& face_direction,
                                    const Vec3f& up_direction,
                                    float font_height_world,
                                    const Color& color,
                                    const std::string& font_alias,
                                    TextAlignment alignment) {
    CHECK_GT(font_height_world, 0.0f)
        << "DrawText3D: font_height_world 必须 > 0（世界米；想按像素配字号请先用 "
           "PixelsPerMeterAt() 换算）";

    // 零向量检查：非零才能在渲染期正交化（共线检查在渲染期做，因为那里需要
    // 同时看两个向量）。
    const float f_len = std::sqrt(face_direction.x()*face_direction.x() +
                                  face_direction.y()*face_direction.y() +
                                  face_direction.z()*face_direction.z());
    const float u_len = std::sqrt(up_direction.x()*up_direction.x() +
                                  up_direction.y()*up_direction.y() +
                                  up_direction.z()*up_direction.z());
    CHECK_GT(f_len, 1e-8f) << "DrawText3D: face_direction 不能为零向量";
    CHECK_GT(u_len, 1e-8f) << "DrawText3D: up_direction 不能为零向量";

    Text3DCommand cmd;
    cmd.text = text;
    cmd.anchor = anchor;
    cmd.face_direction = face_direction;
    cmd.up_direction = up_direction;
    cmd.alignment = alignment;
    cmd.font_height_world = font_height_world;
    cmd.color = color;
    cmd.font_alias = font_alias;

    int idx = static_cast<int>(text3d.size());
    text3d.push_back(std::move(cmd));
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

void RenderCommandList::DrawObject3D(uint32_t mesh_id, const PBRMaterial& mat,
                                      const Vec3f& center,
                                      const Vec3f& up, const Vec3f& front,
                                      float scale,
                                      bool highlight,
                                      uint32_t picking_id) {
    CHECK_GT(mesh_id, 0u);
    // 纹理着色要求 mesh 含 UV，运行期在 Renderer 中校验（此处不知 mesh flags）。
    int idx = static_cast<int>(object3d.size());
    Object3DCommand cmd;
    cmd.mesh_id = mesh_id;
    cmd.material = mat;
    cmd.center = center;
    cmd.up = up;
    cmd.front = front;
    cmd.scale = scale;
    cmd.picking_id = picking_id;
    cmd.highlight = highlight;
    object3d.push_back(cmd);
    order.emplace_back(DrawCommandType::kObject3D, idx);
}

void RenderCommandList::DrawGltfObject(const GltfObject& obj,
                                       const Vec3f& center,
                                       const Vec3f& up, const Vec3f& front,
                                       float scale,
                                       bool highlight,
                                       uint32_t picking_id) {
    // 内部就是多个 Object3DCommand，无新命令体。
    // picking_id/highlight/scale 透传给每个 primitive（整模型统一拾取/高亮/缩放）。
    for (const GltfPrimitive& prim : obj.primitives) {
        DrawObject3D(prim.mesh_id, prim.material, center, up, front,
                     /*scale=*/scale, /*highlight=*/highlight,
                     /*picking_id=*/picking_id);
    }
}

void RenderCommandList::DrawMeshWithSkeleton(
    uint32_t mesh_id, uint32_t skeleton_id,
    const jpov::PBRMaterial& material,
    std::vector<SkinnedInstanceState> instances) {
    CHECK_GT(mesh_id, 0u) << "DrawMeshWithSkeleton: mesh_id 必须 > 0";
    CHECK_GT(skeleton_id, 0u) << "DrawMeshWithSkeleton: skeleton_id 必须 > 0"
        << "（先经 JPOV::RegisterSkeleton(SkeletonType, poses) 创建骨架拿到 id）";
    CHECK(!instances.empty()) << "DrawMeshWithSkeleton: instances 不能为空";
    int idx = static_cast<int>(skinned_mesh.size());
    SkinnedMeshCommand cmd;
    cmd.mesh_id = mesh_id;
    cmd.skeleton_id = skeleton_id;
    cmd.material = material;                 // 蒙皮网格材质（同 Object3DCommand.material）
    cmd.instances = std::move(instances);  // 每实例自己的插值 pose(pose_a/pose_b/ratio)在 instances 里(见 skeleton_types.h)
    skinned_mesh.push_back(cmd);
    order.emplace_back(DrawCommandType::kSkinnedMesh, idx);
}

void RenderCommandList::DrawFire(const Vec3f& base, float radius, float height,
                                 const Color& color_core,
                                 const Color& color_outer,
                                 float intensity, float speed,
                                 float noise_scale, ParticleBlend blend,
                                 float time_offset) {
    CHECK_GT(radius, 0.0f) << "DrawFire: radius 必须 > 0";
    CHECK_GT(height, 0.0f) << "DrawFire: height 必须 > 0";
    CHECK_GE(intensity, 0.0f) << "DrawFire: intensity 必须 >= 0";
    CHECK_GE(speed, 0.0f) << "DrawFire: speed 必须 >= 0";
    CHECK_GT(noise_scale, 0.0f) << "DrawFire: noise_scale 必须 > 0";
    int idx = static_cast<int>(fires.size());
    FireCommand cmd;
    cmd.base = base;
    cmd.radius = radius;
    cmd.height = height;
    cmd.color_core = color_core;
    cmd.color_outer = color_outer;
    cmd.intensity = intensity;
    cmd.speed = speed;
    cmd.noise_scale = noise_scale;
    cmd.blend = blend;
    cmd.time_offset = time_offset;
    fires.push_back(cmd);
    order.emplace_back(DrawCommandType::kFire, idx);
}

void RenderCommandList::DrawBurning(const Vec3f& center, const Vec3f& up,
                                    const Vec3f& front,
                                    const Vec3f& half_extents, float strength,
                                    uint32_t seed) {
    CHECK_GT(half_extents.x(), 0.0f) << "DrawBurning: half_extents.x 必须 > 0";
    CHECK_GT(half_extents.y(), 0.0f) << "DrawBurning: half_extents.y 必须 > 0";
    CHECK_GT(half_extents.z(), 0.0f) << "DrawBurning: half_extents.z 必须 > 0";
    CHECK_GE(strength, 0.0f) << "DrawBurning: strength 必须 >= 0";
    if (strength <= 0.0f) {
        return;   // 火势为 0：不画（无命令）
    }
    int idx = static_cast<int>(burnings.size());
    BurningCommand cmd;
    cmd.center = center;
    cmd.up = up;
    cmd.front = front;
    cmd.half_extents = half_extents;
    cmd.strength = strength;
    cmd.seed = seed;
    burnings.push_back(cmd);
    order.emplace_back(DrawCommandType::kBurning, idx);
}

void RenderCommandList::DrawParticle(const Vec3f& position, float size,
                                     float aspect, float heat, float alpha,
                                     float time_offset, ParticleStyle style,
                                     ParticleBlend blend) {
    CHECK_GT(size, 0.0f) << "DrawParticle: size 必须 > 0";
    CHECK_GT(aspect, 0.0f) << "DrawParticle: aspect 必须 > 0";
    CHECK_GE(alpha, 0.0f) << "DrawParticle: alpha 必须 >= 0";
    int idx = static_cast<int>(particles.size());
    ParticleCommand cmd;
    cmd.position = position;
    cmd.size = size;
    cmd.aspect = aspect;
    cmd.heat = heat;
    cmd.alpha = alpha;
    cmd.time_offset = time_offset;
    cmd.style = style;
    cmd.blend = blend;
    particles.push_back(cmd);
    order.emplace_back(DrawCommandType::kParticle, idx);
}

}  // namespace jpov
