// JPOV 模型编辑器 — 渲染核心 App（加载 + 变换 + 裁剪 + 保存）
//
// 2026-10-06 Danis：从穿衣工具（tools/jpov/clothing/）**复制做减法**而来。丢弃软体
//   仿真 / 自动蒙皮 / 人体排斥 / 建最近邻三角形（本工具都用不上），只保留：
//     ① 加载：reference（可空）+ target 两个模型；reference 只显示，target 可编辑；
//     ② 左上角面板：target 的平移 / 旋转 / 缩放（步进式）+ 保存按钮（沿用穿衣工具）；
//     ③ 右侧面板：**裁剪**——沿坐标平面（X / Y / Z 可选）删掉一侧，裁剪面用
//        primitive3d（DrawStrip3D）半透明画出（尺寸 = target 包围盒大小，可 toggle 关闭显示）。
//
// 与穿衣工具的关键差异：
//   - **没有仿真器**：变换直接烘进 target 的 CPU 顶点（见 model_transform.h），
//     每次操作后 UpdateMesh 推上 GPU。
//   - **裁剪**是新能力：把跨裁剪面的三角形截断、按插值补出边界顶点（见 mesh_clip.h）。
//
// 坐标系（与穿衣工具同款）：2D 面板画在主 FBO，尺寸 = 本帧窗口尺寸 winfo；3D FBO
//   跟随窗口尺寸，resize 不拉伸。
//
// 命名空间 jpov::model_editor、文件夹 tools/jpov/model_editor/ 均为独立一整套。

#ifndef JPOV_MODEL_EDITOR_MODEL_EDITOR_APP_H_
#define JPOV_MODEL_EDITOR_MODEL_EDITOR_APP_H_

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/demo/skylight_scene.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/skeleton_types.h"
#include "tools/jpov/interface/ui.h"
#include "tools/jpov/model_editor/mesh_clip.h"
#include "tools/jpov/model_editor/model_save.h"
#include "tools/jpov/model_editor/model_transform.h"
#include "tools/jpov/model_editor/number_input.h"
#include "tools/jpov/src/gltf_loader.h"

namespace jpov {
namespace model_editor {

// 视角 / 光照 / 地面工具：复用姊妹查看器的纯函数。
using jpov_viewer::ApplyInput;
using jpov_viewer::DefaultView;
using jpov_viewer::GroundMaterial;
using jpov_viewer::MakeGroundQuad;
using jpov_viewer::ViewConfig;

// 默认窗口尺寸（= headless 出图尺寸）。运行时窗口可 resize，面板按每帧 winfo 推算。
inline constexpr int kDefaultWindowWidth = 1280;
inline constexpr int kDefaultWindowHeight = 720;

// 交互帧率（Hz）。
inline constexpr float kViewerFps = 60.0f;

// UI 文本默认字体 = CJK。
inline constexpr const char* kViewerFontAlias = jpov::kFontBuiltinCJK;

// 数值输入框文本容量。
inline constexpr size_t kNumberInputCapacity = 64;

// 步长 / 系数的初始默认值（与穿衣工具一致，Danis 2026-09-30 指定）。
inline constexpr float kDefaultTransStep = 0.1f;
inline constexpr float kDefaultRotStep = 45.0f;
inline constexpr float kDefaultScaleStep = 1.1f;

// 合法范围（与穿衣工具一致，Danis 2026-09-30 指定）。
inline constexpr float kTransStepAbsMax = 5.0f;
inline constexpr float kRotStepAbsMax = 90.0f;
inline constexpr float kScaleStepMin = 1.0f;
inline constexpr float kScaleStepMax = 2.0f;
inline constexpr float kModelScaleMin = 0.1f;
inline constexpr float kModelScaleMax = 10.0f;

// 裁剪面半透明颜色（浅蓝）。
inline constexpr jpov::Color kClipPlaneColor{0.20f, 0.60f, 1.0f, 0.28f};

// 一个数值输入框的跨帧状态（文本缓冲 + 上一帧聚焦态）。
struct NumberField {
    char text[kNumberInputCapacity];
    bool focused_prev = false;

    NumberField() { text[0] = '\0'; }
    explicit NumberField(float init) {
        snprintf(text, kNumberInputCapacity, "%g", static_cast<double>(init));
    }
};

// 模型编辑器渲染核心 App。
class ModelEditorApp : public JPOV {
public:
    using JPOV::JPOV;

    // ── 场景状态（main 在 Init() 后装配，再调 LoadScene）──
    jpov::GltfObject reference_;  // reference 模型（可空 = 无 reference）
    jpov::GltfObject target_;     // target 模型（被编辑对象）
    std::string reference_path_;  // reference 来源路径（空 = 无）
    std::string target_path_;     // target 来源路径（面板显示 + 保存定位）
    bool has_reference_ = false;

    uint32_t ground_mesh_ = 0;
    jpov::PBRMaterial ground_mat_;

    ViewConfig view_;

    bool show_reference_ = true;
    bool show_target_ = true;

    float ground_y_ = -3.0f;

    // ══════════════ target CPU 几何（当前态 / 启动态）══════════════
    //
    // 唯一事实源 = target_current_（CPU 顶点，已被平移/旋转/缩放/裁剪改写）。
    // 改动后 UpdateMesh 推上 GPU。target_startup_ 是「重置」用的原始快照。
    // target_prim_enabled_：某 primitive 被裁空后置 false（不画、不保存）。
    std::vector<jpov::GltfSaveMesh> target_current_;
    std::vector<jpov::GltfSaveMesh> target_startup_;
    std::vector<bool> target_prim_enabled_;

    // target 自带骨架（带骨模型才有）。保存时一并写入 glb 的 skin。
    std::optional<jpov::SkeletonType> target_skeleton_;

    // 各步长 / 系数（步进按钮使用；输入框提交时 clamp）。
    float trans_step_[3] = {kDefaultTransStep, kDefaultTransStep, kDefaultTransStep};
    float rot_step_[3] = {kDefaultRotStep, kDefaultRotStep, kDefaultRotStep};
    float scale_step_ = kDefaultScaleStep;
    float model_scale_ = 1.0f;  // 累计缩放系数，夹在 [kModelScaleMin, kModelScaleMax]。

    NumberField trans_step_field_[3] = {NumberField(kDefaultTransStep),
                                        NumberField(kDefaultTransStep),
                                        NumberField(kDefaultTransStep)};
    NumberField rot_step_field_[3] = {NumberField(kDefaultRotStep),
                                      NumberField(kDefaultRotStep),
                                      NumberField(kDefaultRotStep)};
    NumberField scale_step_field_ = NumberField(kDefaultScaleStep);

    // ══════════════ 裁剪 ══════════════
    int clip_axis_ = 1;        // 裁剪坐标轴：0=X / 1=Y / 2=Z（默认 Y = 水平面）
    float clip_coord_ = 0.0f;  // 裁剪面在该轴上的坐标（米）
    float clip_min_[3] = {-1.0f, -1.0f, -1.0f};  // 各轴滑块下界（= target 启动包围盒 min）
    float clip_max_[3] = {1.0f, 1.0f, 1.0f};     // 各轴滑块上界（= target 启动包围盒 max）
    bool show_clip_plane_ = true;
    std::string clip_msg_;     // 裁剪结果 / 错误提示

    ModelSaveController save_ctrl_;

    void InstallTextMeasure() {
        ui_.SetTextMeasure(&ModelEditorApp::ViewerTextWidth, this);
    }

    void SetShowPanel(bool show) { show_panel_ = show; }
    bool show_panel() const { return show_panel_; }

    // ⭐ 装配场景：加载 reference（可空）/ target 的 GPU 资产 + target 的 CPU 几何 /
    // 骨架，建地面，自适应相机。Init() 后调用一次（仅主线程 / GL 上下文）。
    // Pre-condition: target_path_ 非空，且指向可加载的 glb/gltf。
    void LoadScene() {
        // target GPU 资产。
        target_ = LoadGltf(target_path_);
        CHECK(!target_.empty()) << "target 加载失败或为空: " << target_path_;

        // reference GPU 资产（可选）。
        if (has_reference_) {
            reference_ = LoadGltf(reference_path_);
            CHECK(!reference_.empty()) << "reference 加载失败或为空: " << reference_path_;
        }

        // target 的 CPU 几何（供变换 / 裁剪 / 保存）。顺序与 LoadGltf 内部一致，可下标对齐。
        std::vector<jpov::GltfSaveMesh> startup_geometry;
        struct Collector {
            std::vector<jpov::GltfSaveMesh>* out;
        } collector{&startup_geometry};
        const auto cb = [](const jpov::GltfMeshEntry* entry, void* user) {
            Collector* c = static_cast<Collector*>(user);
            jpov::GltfSaveMesh sm;
            sm.mesh = entry->mesh;
            sm.material = entry->material;
            c->out->push_back(std::move(sm));
        };
        CHECK(jpov::LoadGltfScene(target_path_, cb, &collector))
            << "target CPU 几何加载失败: " << target_path_;
        CHECK_EQ(startup_geometry.size(), target_.size())
            << "target CPU 几何与 GPU primitive 数量不一致："
            << startup_geometry.size() << " vs " << target_.size();

        target_startup_ = startup_geometry;
        target_current_ = startup_geometry;
        target_prim_enabled_.assign(target_current_.size(), true);

        // target 自带骨架（可选）。
        std::vector<jpov::SkeletonType> skins;
        if (jpov::LoadGltfSkeleton(target_path_, &skins) && !skins.empty()) {
            skins[0].Validate();
            target_skeleton_ = skins[0];
        }

        // 裁剪滑块范围 = target 启动包围盒（逐轴，固定，不随裁剪跳动）。
        MeshBounds bounds = TargetBounds();
        if (bounds.valid) {
            const float bmin[3] = {bounds.min.x(), bounds.min.y(), bounds.min.z()};
            const float bmax[3] = {bounds.max.x(), bounds.max.y(), bounds.max.z()};
            for (int a = 0; a < 3; ++a) {
                clip_min_[a] = bmin[a];
                clip_max_[a] = bmax[a];
                if (clip_max_[a] - clip_min_[a] < 1e-4f) {
                    clip_max_[a] = clip_min_[a] + 1e-4f;  // 退化保护（Ui Slider 要求 min<max）
                }
            }
            clip_coord_ = (clip_min_[clip_axis_] + clip_max_[clip_axis_]) * 0.5f;
        }

        // 地面。
        ground_mat_ = GroundMaterial();
        ground_mesh_ = RegisterMesh(MakeGroundQuad(ground_y_));
        ground_y_last_built_ = ground_y_;

        // 初始视角（用命令行给的 phi）。
        view_ = DefaultView();
        view_.phi = view_phi_rad_;
        FitViewToScene();

        scene_loaded_ = true;
        LOG(INFO) << "模型编辑器就绪：target=" << target_path_
                  << "（" << target_.size() << " primitives）"
                  << (has_reference_ ? ("，reference=" + reference_path_) : "，无 reference");
    }

    // 交互窗口在 main 里用它设置初始俯视角（弧度）。
    void SetInitialViewPhi(double phi_rad) { view_phi_rad_ = phi_rad; }

    // 供 headless / CLI 复现裁剪用：设坐标轴（坐标重置到该轴范围中点）/ 坐标 / 按 keep 侧裁一刀。
    void SetClipAxis(int axis) {
        CHECK_GE(axis, 0);
        CHECK_LT(axis, 3);
        clip_axis_ = axis;
        clip_coord_ = (clip_min_[axis] + clip_max_[axis]) * 0.5f;
    }
    void SetClipCoord(float coord) { clip_coord_ = coord; }
    void ApplyClip(ClipKeepSide keep) { DoClip(keep); }
    // 供 headless / CLI 用：阻塞等待上一次保存完成。
    void WaitForSave() {
        while (save_ctrl_.state() == ModelSaveState::kSaving) {
            save_ctrl_.Tick();
        }
    }
    // 供 headless / CLI 用：把当前 target 存到指定 glb（同步等待完成）。
    void SaveTargetToSourceSync() {
        StartSaveTarget();
        WaitForSave();
    }

    // ⭐ 唯一的渲染体（交互循环与 headless 出图共用）。
    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        CHECK(scene_loaded_) << "LoadScene() 未调用";
        CHECK_GT(winfo.width, 0.0f);
        CHECK_GT(winfo.height, 0.0f);
        cmds->camera.fbo_3d_width_ = static_cast<int>(winfo.width);
        cmds->camera.fbo_3d_height_ = static_cast<int>(winfo.height);

        save_ctrl_.Tick();

        // 交互输入 → 视角（仅可见窗口消费输入）。
        if (show_panel_) {
            float dx = 0.0f;
            float dy = 0.0f;
            float scroll = 0.0f;
            if (input.right.IsDrag()) {
                dx = input.mouse_dx;
                dy = input.mouse_dy;
            }
            if (input.scroll_delta != 0.0f) {
                scroll = input.scroll_delta;
            }
            ApplyInput(&view_, dx, dy, scroll, static_cast<int>(winfo.width),
                       static_cast<int>(winfo.height));
        }

        // 相机。
        cmds->camera.position = view_.Position();
        cmds->camera.target = ViewConfig::Target();
        cmds->camera.up = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov = 60.0f;
        cmds->camera.near = 0.05f;
        cmds->camera.far = 1000.0f;

        // 光照：skylight 同款天光（太阳仰角 45° + 三色环境光）。
        const jpov_skylight::SkyDegrees sky_deg;
        const jpov_skylight::SkyLighting light =
            jpov_skylight::MakeSkyLighting(sky_deg, /*tricolor_ambient*/ true);
        cmds->sky = light.sky;
        cmds->sun = light.dir_light;
        cmds->ambient = light.ambient;
        cmds->tone_mapping = true;

        // 地面。
        if (ground_y_ != ground_y_last_built_) {
            UpdateMesh(ground_mesh_, MakeGroundQuad(ground_y_));
            ground_y_last_built_ = ground_y_;
        }
        cmds->DrawObject3D(ground_mesh_, ground_mat_,
                           /*center*/ {0.0f, 0.0f, 0.0f},
                           /*up*/ {0.0f, 1.0f, 0.0f},
                           /*front*/ {0.0f, 0.0f, 1.0f});

        // reference（固定画在原点）。
        if (show_reference_ && has_reference_) {
            cmds->DrawGltfObject(reference_,
                                 /*center*/ {0.0f, 0.0f, 0.0f},
                                 /*up*/ {0.0f, 1.0f, 0.0f},
                                 /*front*/ {0.0f, 0.0f, 1.0f});
        }
        // target（逐 primitive，跳过被裁空的）。
        if (show_target_) {
            for (size_t i = 0; i < target_.primitives.size(); ++i) {
                if (!target_prim_enabled_[i]) {
                    continue;
                }
                cmds->DrawObject3D(target_.primitives[i].mesh_id,
                                   target_.primitives[i].material,
                                   /*center*/ {0.0f, 0.0f, 0.0f},
                                   /*up*/ {0.0f, 1.0f, 0.0f},
                                   /*front*/ {0.0f, 0.0f, 1.0f});
            }
        }

        // 裁剪面（半透明，画在最后；双面绕序以应对背面剔除）。
        if (show_clip_plane_) {
            DrawClipPlane(cmds);
        }

        // 面板。
        if (show_panel_) {
            DrawPanels(input, winfo, cmds);
            ui_.End();
            ui_.Emit(cmds);
        }
    }

private:
    static float ViewerTextWidth(const char* text, float font_size,
                                 const char* /*font_alias*/, void* userdata) {
        ModelEditorApp* app = static_cast<ModelEditorApp*>(userdata);
        return app->MeasureTextWidth(/*alias=*/std::string(),
                                     /*text=*/text ? text : "", font_size);
    }

    // ==================== 变换（就地烘进 target 顶点） ====================

    // target 合并包围盒（当前态）。
    MeshBounds TargetBounds() const {
        MeshBounds b;
        for (size_t i = 0; i < target_current_.size(); ++i) {
            if (!target_prim_enabled_[i] || target_current_[i].mesh.positions.empty()) {
                continue;
            }
            const MeshBounds m = ComputeMeshBounds(target_current_[i].mesh);
            if (!b.valid) {
                b = m;
            } else {
                b.min = jpov::Vec3f(std::min(b.min.x(), m.min.x()),
                                    std::min(b.min.y(), m.min.y()),
                                    std::min(b.min.z(), m.min.z()));
                b.max = jpov::Vec3f(std::max(b.max.x(), m.max.x()),
                                    std::max(b.max.y(), m.max.y()),
                                    std::max(b.max.z(), m.max.z()));
            }
        }
        return b;
    }

    // 旋转 / 缩放的枢轴 = target 合并包围盒中心（空则原点）。
    jpov::Vec3f TargetPivot() const {
        const MeshBounds b = TargetBounds();
        if (!b.valid) {
            return jpov::Vec3f(0.0f, 0.0f, 0.0f);
        }
        return jpov::Vec3f((b.min.x() + b.max.x()) * 0.5f,
                           (b.min.y() + b.max.y()) * 0.5f,
                           (b.min.z() + b.max.z()) * 0.5f);
    }

    // 把当前 CPU 几何推上 GPU（跳过被裁空的 primitive）。
    void SyncTargetToGpu() {
        for (size_t i = 0; i < target_current_.size(); ++i) {
            if (!target_prim_enabled_[i]) {
                continue;
            }
            UpdateMesh(target_.primitives[i].mesh_id, target_current_[i].mesh);
        }
    }

    // 平移步进（沿 axis 轴）。
    void StepTranslation(int axis, float direction) {
        CHECK_GE(axis, 0);
        CHECK_LT(axis, 3);
        const float d = direction * trans_step_[axis];
        const jpov::Vec3f delta(axis == 0 ? d : 0.0f, axis == 1 ? d : 0.0f,
                                axis == 2 ? d : 0.0f);
        for (size_t i = 0; i < target_current_.size(); ++i) {
            if (target_prim_enabled_[i]) {
                TranslateMesh(&target_current_[i].mesh, delta);
            }
        }
        SyncTargetToGpu();
    }

    // 旋转步进（绕合并中心、绕 axis 轴逆时针 direction*rot_step 度）。
    void StepRotation(int axis, float direction) {
        CHECK_GE(axis, 0);
        CHECK_LT(axis, 3);
        const float deg = direction * rot_step_[axis];
        const jpov::Vec3f pivot = TargetPivot();
        for (size_t i = 0; i < target_current_.size(); ++i) {
            if (target_prim_enabled_[i]) {
                RotateMesh(&target_current_[i].mesh, axis, deg, pivot);
            }
        }
        SyncTargetToGpu();
    }

    // 缩放步进（绕合并中心乘 factor；累计系数超界则不动）。
    void StepScale(float factor) {
        CHECK_GT(factor, 0.0f);
        const float target = ClampModelScale(model_scale_ * factor);
        const float applied = target / model_scale_;
        if (applied == 1.0f) {
            return;  // 已到上下界。
        }
        const jpov::Vec3f pivot = TargetPivot();
        for (size_t i = 0; i < target_current_.size(); ++i) {
            if (target_prim_enabled_[i]) {
                ScaleMesh(&target_current_[i].mesh, applied, pivot);
            }
        }
        model_scale_ = target;
        SyncTargetToGpu();
    }

    // 「重置 target」：几何回到启动态（撤销所有变换 / 裁剪），重新启用全部 primitive。
    void ResetTarget() {
        target_current_ = target_startup_;
        target_prim_enabled_.assign(target_current_.size(), true);
        model_scale_ = 1.0f;
        clip_msg_.clear();
        SyncTargetToGpu();
        LOG(INFO) << "重置 target：几何回到加载时状态";
    }

    // ==================== 裁剪 ====================

    // 坐标轴字母（面板标签 / 日志用）。
    char AxisTag() const { return "XYZ"[clip_axis_]; }

    // 沿所选坐标轴（clip_axis_）在 clip_coord_ 处裁剪 target，保留 keep 侧。裁空则报错并不做。
    void DoClip(ClipKeepSide keep) {
        std::vector<jpov::GltfSaveMesh> new_meshes(target_current_.size());
        std::vector<bool> new_enabled(target_current_.size(), false);
        size_t total_out_tris = 0;
        size_t total_new_verts = 0;

        for (size_t i = 0; i < target_current_.size(); ++i) {
            if (!target_prim_enabled_[i]) {
                new_meshes[i] = target_current_[i];  // 已空的保持不动（仍禁用）
                continue;
            }
            jpov::MeshData out;
            ClipStats stats;
            const bool ok =
                ClipMeshByAxis(target_current_[i].mesh, clip_axis_, clip_coord_,
                               keep, &out, &stats);
            if (ok) {
                new_meshes[i].mesh = std::move(out);
                new_meshes[i].material = target_current_[i].material;
                new_enabled[i] = true;
                total_out_tris += stats.output_triangles;
                total_new_verts += stats.new_boundary_vertices;
            } else {
                new_enabled[i] = false;  // 本 primitive 被裁空。
            }
        }

        if (total_out_tris == 0) {
            clip_msg_ = Format("%c 轴裁剪会清空模型，已取消（未修改几何）", AxisTag());
            LOG(WARNING) << "裁剪被取消：结果为空。axis=" << clip_axis_
                         << " coord=" << clip_coord_;
            return;
        }

        size_t dropped_prims = 0;
        for (size_t i = 0; i < target_current_.size(); ++i) {
            if (new_enabled[i]) {
                target_current_[i] = std::move(new_meshes[i]);
                target_prim_enabled_[i] = true;
            } else if (target_prim_enabled_[i]) {
                // 本 primitive 变为空 → 禁用（不画、不保存）。
                target_current_[i] = jpov::GltfSaveMesh{};
                target_prim_enabled_[i] = false;
                ++dropped_prims;
            }
        }
        SyncTargetToGpu();
        clip_msg_ = Format("裁剪完成（%c 轴）：保留 %zu 三角形，新增边界顶点 %zu%s",
                           AxisTag(), total_out_tris, total_new_verts,
                           dropped_prims > 0 ? "（有 primitive 被裁空）" : "");
        LOG(INFO) << "裁剪完成（axis=" << clip_axis_ << " coord=" << clip_coord_
                  << " keep=" << (keep == ClipKeepSide::kGreater ? "greater" : "less")
                  << "）：" << clip_msg_;
    }

    // 画裁剪面：一块与 target 包围盒**同样大小**的半透明 quad，位于所选轴上 clip_coord_ 处。
    void DrawClipPlane(jpov::RenderCommandList* cmds) {
        const MeshBounds b = TargetBounds();
        if (!b.valid) {
            return;
        }
        const int a = clip_axis_;
        const int u = (a + 1) % 3;  // 面内两轴
        const int v = (a + 2) % 3;
        const float bmin[3] = {b.min.x(), b.min.y(), b.min.z()};
        const float bmax[3] = {b.max.x(), b.max.y(), b.max.z()};
        const float cu = (bmin[u] + bmax[u]) * 0.5f;
        const float cv = (bmin[v] + bmax[v]) * 0.5f;
        // quad 边长 = 包围盒在另两轴的尺寸（即「同 bounding box 大小」）；退化轴给极小兜底。
        const float hu = std::max((bmax[u] - bmin[u]) * 0.5f, 1e-3f);
        const float hv = std::max((bmax[v] - bmin[v]) * 0.5f, 1e-3f);
        const auto point = [&](float du, float dv) {
            float p[3] = {0.0f, 0.0f, 0.0f};
            p[a] = clip_coord_;
            p[u] = cu + du;
            p[v] = cv + dv;
            return jpov::Vec3f(p[0], p[1], p[2]);
        };
        const jpov::Vec3f p00 = point(-hu, -hv);
        const jpov::Vec3f p01 = point(-hu, +hv);
        const jpov::Vec3f p10 = point(+hu, -hv);
        const jpov::Vec3f p11 = point(+hu, +hv);
        // 一个 quad = 两条 4 顶点 strip（相反绕序），保证从两侧都可见。
        cmds->DrawStrip3D({p00, p01, p10, p11}, kClipPlaneColor);
        cmds->DrawStrip3D({p00, p10, p01, p11}, kClipPlaneColor);
    }

    // ==================== 保存 ====================

    void StartSaveTarget() {
        std::vector<jpov::GltfSaveMesh> meshes;
        for (size_t i = 0; i < target_current_.size(); ++i) {
            if (target_prim_enabled_[i]) {
                meshes.push_back(target_current_[i]);
            }
        }
        if (meshes.empty()) {
            LOG(WARNING) << "保存被忽略：target 几何为空";
            return;
        }
        save_ctrl_.Start(std::move(meshes), target_skeleton_, target_path_, "target");
    }

    // ==================== 面板 ====================

    template <typename ClampFn>
    static bool CommitNumberField(NumberField* field, ClampFn clamp_fn,
                                  float* target /*inout*/) {
        CHECK(field != nullptr);
        CHECK(target != nullptr);
        const float before = *target;
        float parsed = 0.0f;
        if (ParseNumber(field->text, &parsed)) {
            *target = clamp_fn(parsed);
        }
        snprintf(field->text, kNumberInputCapacity, "%g",
                 static_cast<double>(*target));
        return *target != before;
    }

    static float ClampTransStep(float v) {
        return std::clamp(v, -kTransStepAbsMax, kTransStepAbsMax);
    }
    static float ClampRotStep(float v) {
        return std::clamp(v, -kRotStepAbsMax, kRotStepAbsMax);
    }
    static float ClampScaleStep(float v) {
        return std::clamp(v, kScaleStepMin, kScaleStepMax);
    }
    static float ClampModelScale(float v) {
        return std::clamp(v, kModelScaleMin, kModelScaleMax);
    }

    void DrawPanels(const jpov::InputSnapshot& input,
                    const jpov::WindowInfo& winfo,
                    jpov::RenderCommandList* cmds) {
        jpov::UiTheme theme = jpov::UiTheme::Default(kFontSize);
        theme.font_alias = kViewerFontAlias;
        ui_.Begin(input, theme, winfo.width, winfo.height, 1000.0f / kViewerFps);
        DrawLeftPanel(cmds, winfo.width);
        DrawRightPanel(cmds, winfo.width);
    }

    // ---- 左上角：变换 / 保存 / 地面 / 显示 ----
    void DrawLeftPanel(jpov::RenderCommandList* cmds, float win_w) {
        const float kMargin = 12.0f;
        const float kPad = 10.0f;
        const float kRowH = kPanelRowH;
        const float kSpacing = 5.0f;
        const float panel_w = 0.32f * win_w;
        const float panel_x = kMargin;
        const float panel_y = kMargin;
        // 平移表头1 + 平移3 + 旋转表头1 + 旋转3 + 缩放1 + 保存1 + 状态1 + 地面1
        // + 勾选1 + 只读2 + 重置1 = 16。
        constexpr int kRows = 16;
        const float panel_h = kPad * 2.0f + kRows * kRowH + (kRows - 1) * kSpacing;
        cmds->DrawRect({panel_x, panel_y}, {panel_w, panel_h},
                       jpov::Color{0.0f, 0.0f, 0.0f, 0.5f});

        const float left = panel_x + kPad;
        const float top = panel_y + kPad;
        const float row_w = panel_w - kPad * 2.0f;
        const float step_y = kRowH + kSpacing;

        const float axis_w = 30.0f;
        const float stepbox_w = 72.0f;
        const float btn_w = 26.0f;
        const float gap = kSpacing;
        const float col_axis = left;
        const float col_stepbx = col_axis + axis_w + gap;
        const float col_btn_in = col_stepbx + stepbox_w + gap;
        const float col_btn_in2 = col_btn_in + btn_w + gap;

        float row_y = top;

        // 平移表头。
        DrawLabel("轴", col_axis, axis_w, row_y);
        DrawLabel("步长(米)", col_stepbx, stepbox_w, row_y);
        DrawLabel("步进", col_btn_in, col_btn_in2 + btn_w - col_btn_in, row_y);
        row_y += step_y;

        static const char* const kAxisTags[3] = {"X", "Y", "Z"};
        for (int axis = 0; axis < 3; ++axis) {
            DrawLabel(kAxisTags[axis], col_axis, axis_w, row_y);
            const bool focus = ui_.InputText(
                "", trans_step_field_[axis].text, kNumberInputCapacity,
                jpov::UiRect{{col_stepbx, row_y}, {stepbox_w, kRowH}});
            if (trans_step_field_[axis].focused_prev && !focus) {
                CommitNumberField(&trans_step_field_[axis], ClampTransStep,
                                  &trans_step_[axis]);
            }
            trans_step_field_[axis].focused_prev = focus;
            if (ui_.Button("<", jpov::UiRect{{col_btn_in, row_y}, {btn_w, kRowH}})) {
                StepTranslation(axis, -1.0f);
            }
            if (ui_.Button(">", jpov::UiRect{{col_btn_in2, row_y}, {btn_w, kRowH}})) {
                StepTranslation(axis, +1.0f);
            }
            row_y += step_y;
        }

        // 旋转表头。
        DrawLabel("轴", col_axis, axis_w, row_y);
        DrawLabel("步长(°)", col_stepbx, stepbox_w, row_y);
        DrawLabel("步进", col_btn_in, col_btn_in2 + btn_w - col_btn_in, row_y);
        row_y += step_y;

        static const char* const kRotTags[3] = {"RX", "RY", "RZ"};
        for (int axis = 0; axis < 3; ++axis) {
            DrawLabel(kRotTags[axis], col_axis, axis_w, row_y);
            const bool focus = ui_.InputText(
                "", rot_step_field_[axis].text, kNumberInputCapacity,
                jpov::UiRect{{col_stepbx, row_y}, {stepbox_w, kRowH}});
            if (rot_step_field_[axis].focused_prev && !focus) {
                CommitNumberField(&rot_step_field_[axis], ClampRotStep,
                                  &rot_step_[axis]);
            }
            rot_step_field_[axis].focused_prev = focus;
            if (ui_.Button("<", jpov::UiRect{{col_btn_in, row_y}, {btn_w, kRowH}})) {
                StepRotation(axis, -1.0f);
            }
            if (ui_.Button(">", jpov::UiRect{{col_btn_in2, row_y}, {btn_w, kRowH}})) {
                StepRotation(axis, +1.0f);
            }
            row_y += step_y;
        }

        // 缩放行。
        const float scale_lbl_w = 72.0f;
        DrawLabel("缩放系数", col_axis, scale_lbl_w, row_y);
        const float scale_bx = col_axis + scale_lbl_w + gap;
        const bool scale_focus = ui_.InputText(
            "", scale_step_field_.text, kNumberInputCapacity,
            jpov::UiRect{{scale_bx, row_y}, {stepbox_w, kRowH}});
        if (scale_step_field_.focused_prev && !scale_focus) {
            CommitNumberField(&scale_step_field_, ClampScaleStep, &scale_step_);
        }
        scale_step_field_.focused_prev = scale_focus;
        const float scale_minus_x = scale_bx + stepbox_w + gap;
        const float scale_plus_x = scale_minus_x + btn_w + gap;
        if (ui_.Button("-", jpov::UiRect{{scale_minus_x, row_y}, {btn_w, kRowH}})) {
            StepScale(1.0f / scale_step_);
        }
        if (ui_.Button("+", jpov::UiRect{{scale_plus_x, row_y}, {btn_w, kRowH}})) {
            StepScale(scale_step_);
        }
        const float scale_info_x = scale_plus_x + btn_w + gap;
        DrawLabel(Format("x%.3f", static_cast<double>(model_scale_)).c_str(),
                  scale_info_x, left + row_w - scale_info_x, row_y);
        row_y += step_y;

        // 保存按钮行。
        const float save_btn_w = 140.0f;
        const char* save_label =
            (save_ctrl_.state() == ModelSaveState::kSaving) ? "保存中..."
                                                            : "保存 target glb";
        if (ui_.Button(save_label, jpov::UiRect{{left, row_y}, {save_btn_w, kRowH}})) {
            StartSaveTarget();
        }
        row_y += step_y;

        // 保存状态行。
        if (!save_ctrl_.message().empty()) {
            cmds->DrawText(save_ctrl_.message(),
                           {left, row_y + (kRowH - kFontSize) * 0.5f}, kFontSize,
                           jpov::Color{0.92f, 0.93f, 0.95f, 1.0f},
                           jpov::TextAlignment::kTopLeft, kViewerFontAlias);
        }
        row_y += step_y;

        // 地面高度。
        ui_.SliderFloat("地面高度 y", &ground_y_,
                        jpov::UiRect{{left, row_y}, {row_w, kRowH}}, -3.0f, 3.0f,
                        /*decimal_places*/ 2);
        row_y += step_y;

        // 显示开关。
        const float kCheckW = row_w / 2.0f;
        ui_.Checkbox("显示 reference", &show_reference_,
                     jpov::UiRect{{left, row_y}, {kCheckW, kRowH}});
        ui_.Checkbox("显示 target", &show_target_,
                     jpov::UiRect{{left + kCheckW, row_y}, {kCheckW, kRowH}});
        row_y += step_y;

        // 只读来源。
        ui_.Text(Format("reference：%s",
                        has_reference_ ? BaseNameOf(reference_path_).c_str() : "（无）")
                     .c_str(),
                 jpov::UiRect{{left, row_y}, {row_w, kRowH}});
        row_y += step_y;
        ui_.Text(Format("target：%s（%zu primitives）",
                        BaseNameOf(target_path_).c_str(), target_.size())
                     .c_str(),
                 jpov::UiRect{{left, row_y}, {row_w, kRowH}});
        row_y += step_y;

        // 重置。
        if (ui_.Button("重置 target（撤销变换/裁剪）",
                       jpov::UiRect{{left, row_y}, {row_w, kRowH}})) {
            ResetTarget();
        }
        row_y += step_y;
    }

    // ---- 右侧：裁剪 ----
    void DrawRightPanel(jpov::RenderCommandList* cmds, float win_w) {
        const float kMargin = 12.0f;
        const float kPad = 10.0f;
        const float kRowH = kPanelRowH;
        const float kSpacing = 5.0f;
        const float panel_w = 0.30f * win_w;
        const float panel_x = win_w - panel_w - kMargin;
        const float panel_y = kMargin;
        // 标题1 + 轴选择1 + 坐标滑条1 + 按钮行1 + 显示面1 + 消息1 + 只读1 = 7。
        constexpr int kRows = 7;
        const float panel_h = kPad * 2.0f + kRows * kRowH + (kRows - 1) * kSpacing;
        cmds->DrawRect({panel_x, panel_y}, {panel_w, panel_h},
                       jpov::Color{0.0f, 0.0f, 0.0f, 0.5f});

        const float left = panel_x + kPad;
        const float top = panel_y + kPad;
        const float row_w = panel_w - kPad * 2.0f;
        const float step_y = kRowH + kSpacing;
        float row_y = top;

        DrawLabel("裁剪（沿坐标平面切一刀）", left, row_w, row_y);
        row_y += step_y;

        // 坐标轴选择（X/Y/Z）。切换轴时把坐标重置到该轴范围中点。
        {
            static const std::vector<const char*> kAxisItems = {"X", "Y", "Z"};
            if (ui_.Combo("轴", &clip_axis_, kAxisItems,
                          jpov::UiRect{{left, row_y}, {row_w, kRowH}})) {
                clip_coord_ = (clip_min_[clip_axis_] + clip_max_[clip_axis_]) * 0.5f;
            }
        }
        row_y += step_y;

        ui_.SliderFloat(Format("裁剪面 %c (米)", AxisTag()).c_str(), &clip_coord_,
                        jpov::UiRect{{left, row_y}, {row_w, kRowH}},
                        clip_min_[clip_axis_], clip_max_[clip_axis_],
                        /*decimal_places*/ 3);
        row_y += step_y;

        // 两个裁剪按钮：删除「坐标小的一侧」/「坐标大的一侧」。
        const float kBtnW = (row_w - kSpacing) * 0.5f;
        if (ui_.Button(Format("删除 %c < 侧", AxisTag()).c_str(),
                       jpov::UiRect{{left, row_y}, {kBtnW, kRowH}})) {
            DoClip(ClipKeepSide::kGreater);
        }
        if (ui_.Button(Format("删除 %c > 侧", AxisTag()).c_str(),
                       jpov::UiRect{{left + kBtnW + kSpacing, row_y}, {kBtnW, kRowH}})) {
            DoClip(ClipKeepSide::kLess);
        }
        row_y += step_y;

        ui_.Checkbox("显示裁剪面", &show_clip_plane_,
                     jpov::UiRect{{left, row_y}, {row_w, kRowH}});
        row_y += step_y;

        // 消息行。
        if (!clip_msg_.empty()) {
            cmds->DrawText(clip_msg_, {left, row_y + (kRowH - kFontSize) * 0.5f},
                           kFontSize, jpov::Color{0.92f, 0.93f, 0.95f, 1.0f},
                           jpov::TextAlignment::kTopLeft, kViewerFontAlias);
        }
        row_y += step_y;

        // 只读：当前 target 三角形数 + y 范围。
        size_t tri_count = 0;
        for (size_t i = 0; i < target_current_.size(); ++i) {
            if (target_prim_enabled_[i]) {
                const jpov::MeshData& m = target_current_[i].mesh;
                tri_count += m.indices.empty() ? (m.positions.size() / 3)
                                               : (m.indices.size() / 3);
            }
        }
        ui_.Text(Format("target 三角形：%zu", tri_count).c_str(),
                 jpov::UiRect{{left, row_y}, {row_w, kRowH}});
        row_y += step_y;
    }

    void DrawLabel(const char* text, float x, float width, float y) {
        ui_.Text(text, jpov::UiRect{{x, y}, {width, kPanelRowH}}, false, false);
    }

    // ==================== 相机自适应 ====================

    void FitViewToScene() {
        // 把 target 与 reference（若有）的包围盒并入取景范围。
        float bmin[3] = {0.0f, 0.0f, 0.0f};
        float bmax[3] = {0.0f, 0.0f, 0.0f};
        bool valid = false;
        const MeshBounds tb = TargetBounds();
        if (tb.valid) {
            bmin[0] = tb.min.x(); bmin[1] = tb.min.y(); bmin[2] = tb.min.z();
            bmax[0] = tb.max.x(); bmax[1] = tb.max.y(); bmax[2] = tb.max.z();
            valid = true;
        }
        if (has_reference_ && reference_.bounds_valid) {
            if (!valid) {
                for (int i = 0; i < 3; ++i) {
                    bmin[i] = reference_.bounds_min[i];
                    bmax[i] = reference_.bounds_max[i];
                }
                valid = true;
            } else {
                for (int i = 0; i < 3; ++i) {
                    bmin[i] = std::min(bmin[i], reference_.bounds_min[i]);
                    bmax[i] = std::max(bmax[i], reference_.bounds_max[i]);
                }
            }
        }
        if (!valid) {
            return;
        }
        view_.R = ViewConfig::FitRadius(bmin, bmax, /*fov_deg*/ 60.0);
    }

    static std::string BaseNameOf(const std::string& path) {
        const size_t slash = path.find_last_of("/\\");
        return (slash == std::string::npos) ? path : path.substr(slash + 1);
    }

    static std::string Format(const char* fmt, ...) {
        char buf[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        return std::string(buf);
    }

    bool show_panel_ = true;
    bool scene_loaded_ = false;
    float ground_y_last_built_ = -3.0f;
    double view_phi_rad_ = 0.35;  // 初始俯视角（main 可覆盖）

    jpov::Ui ui_;

    static constexpr float kFontSize = 16.0f;
    static constexpr float kPanelRowH = 26.0f;
};

}  // namespace model_editor
}  // namespace jpov

#endif  // JPOV_MODEL_EDITOR_MODEL_EDITOR_APP_H_
