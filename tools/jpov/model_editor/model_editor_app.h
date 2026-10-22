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
#include "tools/jpov/interface/axis_frame.h"
#include "tools/jpov/interface/skeleton_types.h"
#include "tools/jpov/interface/ui.h"
#include "tools/jpov/model_editor/mesh_clip.h"
#include "tools/jpov/model_editor/mesh_seam.h"
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
// 缝合面用另一个色（绿），和裁剪面区分开。
inline constexpr jpov::Color kSeamPlaneColor{0.25f, 1.0f, 0.45f, 0.25f};
// 半透明面相对 target AABB 的外扩系数（留一点边，别让模型从面里穿出去看不见）。
inline constexpr float kPlaneQuadMargin = 1.05f;
// 平面坐标步进默认步长（米）/ 合法范围。
inline constexpr float kDefaultPlaneStep = 0.01f;
inline constexpr float kPlaneStepMin = 1e-4f;
inline constexpr float kPlaneStepMax = 10.0f;

// ==================== 4x4 矩阵 / HUD ====================

// 行主序 4x4（列向量约定 p' = M·p）。
struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

inline Mat4 Mat4Mul(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float acc = 0.0f;
            for (int k = 0; k < 4; ++k) {
                acc += a.m[i * 4 + k] * b.m[k * 4 + j];
            }
            r.m[i * 4 + j] = acc;
        }
    }
    return r;
}

inline Mat4 Mat4Translate(const jpov::Vec3f& t) {
    Mat4 r;
    r.m[3] = t.x();
    r.m[7] = t.y();
    r.m[11] = t.z();
    return r;
}

inline Mat4 Mat4ScaleUniform(float s) {
    Mat4 r;
    r.m[0] = s;
    r.m[5] = s;
    r.m[10] = s;
    return r;
}

// 与世界轴 axis 的旋转（与 model_transform.h::RotateMesh 同一套右手约定）。
inline Mat4 Mat4RotateAxis(int axis, float deg) {
    constexpr float kPi = 3.14159265358979323846f;
    const float rad = deg * kPi / 180.0f;
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    Mat4 r;
    switch (axis) {
        case 0:  // 绕 X
            r.m[5] = c;
            r.m[6] = -s;
            r.m[9] = s;
            r.m[10] = c;
            break;
        case 1:  // 绕 Y
            r.m[0] = c;
            r.m[2] = s;
            r.m[8] = -s;
            r.m[10] = c;
            break;
        default:  // 绕 Z
            r.m[0] = c;
            r.m[1] = -s;
            r.m[4] = s;
            r.m[5] = c;
            break;
    }
    return r;
}

// 绕 pivot 的旋转 / 缩放（= T(p)·R·T(-p)）。
inline Mat4 Mat4RotateAbout(int axis, float deg, const jpov::Vec3f& pivot) {
    return Mat4Mul(Mat4Mul(Mat4Translate(pivot), Mat4RotateAxis(axis, deg)),
                   Mat4Translate(jpov::Vec3f(-pivot.x(), -pivot.y(), -pivot.z())));
}
inline Mat4 Mat4ScaleAbout(float factor, const jpov::Vec3f& pivot) {
    return Mat4Mul(Mat4Mul(Mat4Translate(pivot), Mat4ScaleUniform(factor)),
                   Mat4Translate(jpov::Vec3f(-pivot.x(), -pivot.y(), -pivot.z())));
}

inline Mat4 Mat4LookAt(const jpov::Vec3f& eye, const jpov::Vec3f& center,
                       const jpov::Vec3f& up) {
    const jpov::Vec3f f = (center - eye).Unit();
    const jpov::Vec3f s = f.Cross(up).Unit();
    const jpov::Vec3f u = s.Cross(f);
    Mat4 r;
    r.m[0] = s.x();  r.m[1] = s.y();  r.m[2] = s.z();   r.m[3] = -s.Dot(eye);
    r.m[4] = u.x();  r.m[5] = u.y();  r.m[6] = u.z();   r.m[7] = -u.Dot(eye);
    r.m[8] = -f.x(); r.m[9] = -f.y(); r.m[10] = -f.z(); r.m[11] = f.Dot(eye);
    r.m[12] = 0.0f;  r.m[13] = 0.0f;  r.m[14] = 0.0f;   r.m[15] = 1.0f;
    return r;
}

inline Mat4 Mat4Perspective(float fov_deg, float aspect, float znear,
                            float zfar) {
    constexpr float kPi = 3.14159265358979323846f;
    const float f = 1.0f / std::tan(fov_deg * kPi / 360.0f);
    Mat4 r;
    for (float& v : r.m) {
        v = 0.0f;
    }
    r.m[0] = f / std::max(aspect, 1e-6f);
    r.m[5] = f;
    r.m[10] = (zfar + znear) / (znear - zfar);
    r.m[11] = 2.0f * zfar * znear / (znear - zfar);
    r.m[14] = -1.0f;
    return r;
}


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

    // 源文件长度单位 → 米（2026-10-22：glb 里的单位要消费，进 JPOV 一律米）。
    // <= 0 = 自动（读 asset.extras.jpov_unit / node scale）；> 0 = 显式覆盖（cm=0.01）。
    // target 与 reference 是**两个独立资产**，单位可以不同（例：cm 模型焊到米制人体），
    // 所以分两个系数，互不影响。
    float unit_meters_ = 0.0f;           // target
    float reference_unit_meters_ = 0.0f;  // reference

    uint32_t ground_mesh_ = 0;
    jpov::PBRMaterial ground_mat_;

    // 坐标架：三根轴杆共用一份网格（见 interface/axis_frame.h）。
    uint32_t axis_frame_mesh_ = 0;

    ViewConfig view_;
    // 中键纵向 pan：相机注视点世界 Y 偏移（相机 position 与 target 同步平移，朝向不变）。
    // 半屏 pan = 1m（jpov_viewer::VerticalPanDeltaY，与穿衣工具一致）。
    float cam_pan_y_ = 0.0f;

    bool show_reference_ = true;
    bool show_target_ = true;
    bool show_axis_frame_ = true;

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

    // 累计「用户变换」矩阵（行主序，列向量约定 p' = M·p）：把所有平移/旋转/缩放按烘焙
    // 顺序左乘累积起来。用途 = HUD 诊断：截图里能读出「这个结果是哪一组变换做出来的」。
    // 注意裁剪/缝合是就地改几何，不改这个矩阵（它只描述用户摆位）。
    Mat4 target_xform_;
    // 用户层累计平移（= 所有平移按钮 / --target_move 的和）。矩阵里的平移项是
    // 「原点被搬到哪」，绕 pivot 缩放/旋转也会改变它，看代码时容易误读，所以单独记一份。
    jpov::Vec3f user_move_;

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
    float clip_step_ = kDefaultPlaneStep;              // 裁剪面坐标步长（米）
    NumberField clip_step_field_ = NumberField(kDefaultPlaneStep);
    std::string clip_msg_;     // 裁剪结果 / 错误提示

    // ══════════════ 缝合（2026-10-22）══════════════
    // reference 的 CPU 几何（bind pose，只读）：断面的形状来源。GPU 侧 reference_ 不动。
    jpov::MeshData reference_cpu_;
    // reference 各 primitive 注册到 GPU 时的 flags：UpdateMesh 要求新数据 flags 与注册时
    // 完全一致（否则 CHECK abort）。切断面做显示前先比对，避免崩溃。
    std::vector<jpov::MeshVertexFlags> reference_gpu_flags_;
    float seam_plane_ = 0.0f;        // 同一个水平面（y）同时切 target 与 reference
    bool show_seam_plane_ = true;    // 显示缝合面（半透明 quad）
    float seam_step_ = kDefaultPlaneStep;              // 缝合面坐标步长（米）
    NumberField seam_step_field_ = NumberField(kDefaultPlaneStep);
    int seam_algo_ = 0;              // 0=harmonic / 1=arap / 2=tps
    float seam_band_scale_ = 2.0f;   // 影响带高度 = 该系数 × 断面等效半径
    float seam_y_span_ = 0.0f;       // 最大形变区高度（米；0 = 自动）。>0 时在
                                     // plane+y_span 插一圈顶点当硬边界，环以上完全隔离。
    float seam_offset_deg_ = 0.0f;   // 环对应旋转偏移（度）
    bool show_reference_cut_ = true;  // 显示时把 reference 也切到缝合面（否则接头被挡住）
    bool reference_is_cut_ = false;   // reference 的 GPU 几何当前是否是切过的版本
    std::string seam_msg_;           // 缝合结果 / 错误提示
    jpov::model_editor::SeamStats seam_stats_;

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
        // 单位：每个资产各自换算到米（进 JPOV 后同处米制空间）。
        jpov::GltfLoadOptions load_options;
        load_options.unit_meters = unit_meters_;
        jpov::GltfLoadOptions ref_load_options;
        ref_load_options.unit_meters = reference_unit_meters_;

        // target GPU 资产。
        target_ = LoadGltf(target_path_, load_options);
        CHECK(!target_.empty()) << "target 加载失败或为空: " << target_path_;

        // reference GPU 资产（可选）。
        if (has_reference_) {
            reference_ = LoadGltf(reference_path_, ref_load_options);
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
        CHECK(jpov::LoadGltfScene(target_path_, cb, &collector, load_options))
            << "target CPU 几何加载失败: " << target_path_;
        CHECK_EQ(startup_geometry.size(), target_.size())
            << "target CPU 几何与 GPU primitive 数量不一致："
            << startup_geometry.size() << " vs " << target_.size();

        target_startup_ = startup_geometry;
        target_current_ = startup_geometry;
        target_prim_enabled_.assign(target_current_.size(), true);

        // reference 的 CPU 几何（缝合时取断面形状）。reference 只读，多 primitive 直接合并。
        if (has_reference_) {
            std::vector<jpov::GltfSaveMesh> ref_geometry;
            struct RefCollector {
                std::vector<jpov::GltfSaveMesh>* out;
            } ref_collector{&ref_geometry};
            const auto ref_cb = [](const jpov::GltfMeshEntry* entry, void* user) {
                RefCollector* c = static_cast<RefCollector*>(user);
                jpov::GltfSaveMesh sm;
                sm.mesh = entry->mesh;
                sm.material = entry->material;
                c->out->push_back(std::move(sm));
            };
            if (jpov::LoadGltfScene(reference_path_, ref_cb, &ref_collector)) {
                reference_cpu_ = MergePrimitivesForSeam(ref_geometry);
            }
        }

        // target 自带骨架（可选）。
        std::vector<jpov::SkeletonType> skins;
        if (jpov::LoadGltfSkeleton(target_path_, &skins, load_options) &&
            !skins.empty()) {
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
            // 缝合面初值 = 同一个高度（用户再按步进取值；不要默认 0 让他从底下一路点上来）。
            seam_plane_ = (clip_min_[1] + clip_max_[1]) * 0.5f;
        }

        // 地面。
        ground_mat_ = GroundMaterial();
        ground_mesh_ = RegisterMesh(MakeGroundQuad(ground_y_));
        ground_y_last_built_ = ground_y_;

        // 坐标架轴杆（三根轴共用一个 MakeBox 网格）。
        axis_frame_mesh_ = RegisterMesh(jpov::axis_frame::MakeAxisFrameBarMesh());

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

    // ==================== 缝合（2026-10-22）====================

    // 把多个 primitive 合成一个 MeshData（reference 只读，合并安全）。
    // 属性只在「所有非空 primitive 都有」时才带上（否则 Validate 会因长度不一致 crash）。
    static jpov::MeshData MergePrimitivesForSeam(
        const std::vector<jpov::GltfSaveMesh>& prims) {
        jpov::MeshData out;
        bool has_normal = true;
        bool has_uv = true;
        bool has_joints = true;
        bool has_tangent = true;
        size_t non_empty = 0;
        for (const auto& p : prims) {
            if (p.mesh.positions.empty()) {
                continue;
            }
            ++non_empty;
            has_normal = has_normal && !p.mesh.normals.empty();
            has_uv = has_uv && !p.mesh.uvs.empty();
            has_joints = has_joints && !p.mesh.joint_indices.empty();
            has_tangent = has_tangent && !p.mesh.tangents.empty();
        }
        if (non_empty == 0) {
            return out;
        }
        uint8_t flags = static_cast<uint8_t>(jpov::MeshVertexFlags::kPosition);
        if (has_normal) {
            flags = static_cast<uint8_t>(flags |
                                         static_cast<uint8_t>(jpov::MeshVertexFlags::kNormal));
        }
        if (has_uv) {
            flags = static_cast<uint8_t>(flags |
                                         static_cast<uint8_t>(jpov::MeshVertexFlags::kUV));
        }
        if (has_joints) {
            flags = static_cast<uint8_t>(flags |
                                         static_cast<uint8_t>(jpov::MeshVertexFlags::kJoints));
        }
        if (has_tangent) {
            flags = static_cast<uint8_t>(flags |
                                         static_cast<uint8_t>(jpov::MeshVertexFlags::kTangent));
        }
        out.flags = static_cast<jpov::MeshVertexFlags>(flags);
        for (const auto& p : prims) {
            if (p.mesh.positions.empty()) {
                continue;
            }
            const uint32_t base = static_cast<uint32_t>(out.positions.size());
            out.positions.insert(out.positions.end(), p.mesh.positions.begin(),
                                 p.mesh.positions.end());
            if (has_normal) {
                out.normals.insert(out.normals.end(), p.mesh.normals.begin(),
                                   p.mesh.normals.end());
            }
            if (has_uv) {
                out.uvs.insert(out.uvs.end(), p.mesh.uvs.begin(), p.mesh.uvs.end());
            }
            if (has_joints) {
                out.joint_indices.insert(out.joint_indices.end(),
                                         p.mesh.joint_indices.begin(),
                                         p.mesh.joint_indices.end());
                out.joint_weights.insert(out.joint_weights.end(),
                                         p.mesh.joint_weights.begin(),
                                         p.mesh.joint_weights.end());
            }
            if (has_tangent) {
                out.tangents.insert(out.tangents.end(), p.mesh.tangents.begin(),
                                    p.mesh.tangents.end());
            }
            if (p.mesh.indices.empty()) {
                for (uint32_t i = 0; i < p.mesh.positions.size(); ++i) {
                    out.indices.push_back(base + i);
                }
            } else {
                for (uint32_t idx : p.mesh.indices) {
                    out.indices.push_back(base + idx);
                }
            }
        }
        out.Validate();
        return out;
    }

    // headless 用：target 绕包围盒中心缩放，再整体平移（便于脚本摆位）。
    void ScaleAndMoveTarget(float scale, const jpov::Vec3f& offset) {
        if (scale > 0.0f && scale != 1.0f) {
            StepScale(scale);
        }
        if (offset.Norm() > 0.0f) {
            for (size_t i = 0; i < target_current_.size(); ++i) {
                if (target_prim_enabled_[i]) {
                    TranslateMesh(&target_current_[i].mesh, offset);
                }
            }
            target_xform_ = Mat4Mul(Mat4Translate(offset), target_xform_);
            user_move_ = user_move_ + offset;
            SyncTargetToGpu();
        }
        FitViewToScene();  // 摆位后重新取景（否则 headless 图里模型在画面外/太小）
    }

    // 显示层：reference 也按缝合面切一刀（保下）。不切的话，接缝会被 reference 自己
    // 的几何挡住（例：蓝人的头），根本看不出"焊上去"的效果。
    // 只支持单 primitive 的 reference（多 primitive 合并后的几何与 GPU primitive 不是一对一）。
    void ShowReferenceCut(bool cut) {
        if (!has_reference_ || reference_cpu_.positions.empty() ||
            reference_.size() != 1) {
            LOG_FIRST_N(WARNING, 1)
                << "reference 断面显示：只支持单 primitive 的 reference，已跳过";
            return;
        }
        if (cut) {
            jpov::MeshData ref_cut;
            ClipStats cs;
            if (!ClipMeshByAxis(reference_cpu_, 1, seam_plane_, ClipKeepSide::kLess,
                                &ref_cut, &cs)) {
                return;
            }
            // UpdateMesh 要求 flags 与注册时一致（否则 CHECK abort）——先比对。
            if (reference_gpu_flags_.size() == 1 &&
                ref_cut.flags != reference_gpu_flags_[0]) {
                LOG_FIRST_N(WARNING, 1)
                    << "reference 断面显示：切后 flags 与注册时不符，已跳过（仅影响显示）";
                return;
            }
            UpdateMesh(reference_.primitives[0].mesh_id, ref_cut);
            reference_is_cut_ = true;
        } else {
            UpdateMesh(reference_.primitives[0].mesh_id, reference_cpu_);
            reference_is_cut_ = false;
        }
    }

    // 执行缝合：同一个 y 平面切 target（保上）与 reference（保下），
    // 再把 target 的底口雕成 reference 的顶口。
    // 只支持「恰好一个 primitive 有断面」：多个 primitive 同时被切到时断面可能跨部件，
    // 暂不处理（拒绝并提示）。
    void ApplyStitch() {
        seam_msg_.clear();
        seam_stats_ = jpov::model_editor::SeamStats{};
        if (!has_reference_ || reference_cpu_.positions.empty()) {
            seam_msg_ = "缝合需要 reference（--reference_path）";
            return;
        }
        const MeshBounds tb = TargetBounds();
        if (tb.valid) {
            LOG(INFO) << "缝合：plane y=" << seam_plane_ << "，target 包围盒 y=["
                      << tb.min.y() << "," << tb.max.y() << "] x=["
                      << tb.min.x() << "," << tb.max.x() << "] z=["
                      << tb.min.z() << "," << tb.max.z() << "]";
        }
        int prim = -1;
        for (size_t i = 0; i < target_current_.size(); ++i) {
            if (!target_prim_enabled_[i]) {
                continue;
            }
            jpov::MeshData cut;
            ClipStats cs;
            const bool cut_ok = ClipMeshByAxis(target_current_[i].mesh, 1, seam_plane_,
                                               ClipKeepSide::kGreater, &cut, &cs);
            if (!cut_ok) {
                LOG(INFO) << "  prim " << i << "：切空（完全在平面下方）";
                continue;
            }
            jpov::model_editor::SeamLoop loop;
            std::string loop_err;
            if (!jpov::model_editor::seam_internal::ExtractCutLoop(
                    cut, 1, seam_plane_, 1e-5f, &loop, &loop_err)) {
                LOG(INFO) << "  prim " << i << "：无断面 — " << loop_err;
                continue;  // 没有断面（整个在平面上方 / 断面不合法）
            }
            LOG(INFO) << "  prim " << i << "：断面 " << loop.verts.size() << " 顶点";
            if (prim >= 0) {
                seam_msg_ = Format("有多个 primitive 的断面（%d 与 %zu），暂不支持", prim, i);
                return;
            }
            prim = static_cast<int>(i);
        }
        if (prim < 0) {
            seam_msg_ = "切平面没有切出合法断面（检查 y 是否穿过模型）";
            return;
        }

        jpov::model_editor::SeamOptions opt;
        opt.axis = 1;
        opt.plane = seam_plane_;
        opt.algo = static_cast<jpov::model_editor::SeamAlgo>(seam_algo_);
        opt.band_scale = seam_band_scale_;
        opt.y_span = seam_y_span_;
        opt.offset_deg = seam_offset_deg_;
        jpov::MeshData out;
        jpov::model_editor::SeamStats st;
        std::string err;
        const size_t open_before =
            jpov::model_editor::CountOpenEdges(target_current_[prim].mesh);
        if (!jpov::model_editor::StitchSeam(target_current_[prim].mesh,
                                           reference_cpu_, opt, &out, &st, &err)) {
            seam_msg_ = "缝合失败：" + err;
            LOG(WARNING) << "缝合被拒绝：" << err;
            return;
        }
        const size_t open_after = jpov::model_editor::CountOpenEdges(out);
        LOG(INFO) << "缝合前后开口边：" << open_before << " → " << open_after
                  << "（delta=" << static_cast<long long>(open_after) -
                                         static_cast<long long>(open_before)
                  << "）";
        target_current_[prim].mesh = std::move(out);
        seam_stats_ = st;
        if (show_reference_cut_) {
            ShowReferenceCut(true);  // 让接缝露出来
        }
        if (!st.message.empty()) {
            LOG(INFO) << "缝合细节：" << st.message;
        }
        SyncTargetToGpu();
        seam_msg_ = Format(
            "缝合完成（%s%s）：带 %zu 顶点，断面误差 %.3fmm，法线均值 %.1f°/最大 %.1f°，"
            "翻面 %zu，额外开口边 %zu",
            jpov::model_editor::SeamAlgoName(opt.algo),
            st.loop_rebuilt ? "，环已重建" : "，环未重建",
            st.band_vertices, st.loop_hausdorff_m * 1000.0f, st.normal_angle_mean_deg,
            st.normal_angle_max_deg, st.flipped_triangles, st.unexpected_open_edges);
        LOG(INFO) << seam_msg_;
    }

    // 供 headless / CLI 用：阻塞等待上一次保存完成。
    void WaitForSave() {
        while (save_ctrl_.state() == ModelSaveState::kSaving) {
            save_ctrl_.Tick();
        }
    }
    // 供 headless / CLI 用：把当前 target 存到指定格式文件（同步等待完成）。
    void SaveTargetToSourceSync(ModelOutputFormat format) {
        StartSaveTarget(format);
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
            // 中键纵向 pan（同穿衣工具）：平移注视点，相机 position 同步跟随。
            if (input.middle.IsDrag()) {
                cam_pan_y_ += jpov_viewer::VerticalPanDeltaY(input.mouse_dy, winfo.height);
            }
        }

        // 相机（叠加中键 pan 的 Y 偏移：position 与 target 同步平移，朝向不变）。
        const jpov::Vec3f pan(0.0f, cam_pan_y_, 0.0f);
        cmds->camera.position = view_.Position() + pan;
        cmds->camera.target = ViewConfig::Target() + pan;
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

        // 坐标架（轴杆 + X/Y/Z 标签）。
        if (show_axis_frame_) {
            jpov::axis_frame::DrawAxisFrame(cmds, axis_frame_mesh_,
                                /*origin*/ {0.0f, 0.0f, 0.0f},
                                jpov::kFontBuiltinLatin);
        }

        // 裁剪面（半透明，画在最后；双面绕序以应对背面剔除）。
        if (show_clip_plane_) {
            DrawClipPlane(cmds);
        }
        if (show_seam_plane_) {
            DrawSeamPlane(cmds);
        }

        // 面板。
        if (show_panel_) {
            DrawPanels(input, winfo, cmds);
            ui_.End();
            ui_.Emit(cmds);
        }

        // 诊断 HUD（相机 / 配置 / 累积变换 / MVP）：始终画，截图就能复现配置。
        DrawHud(cmds, winfo);
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
        // HUD：记录累积变换（左乘，与烘焙顺序一致）。
        target_xform_ = Mat4Mul(Mat4Translate(delta), target_xform_);
        user_move_ = user_move_ + delta;
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
        target_xform_ = Mat4Mul(Mat4RotateAbout(axis, deg, pivot), target_xform_);
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
        target_xform_ = Mat4Mul(Mat4ScaleAbout(applied, pivot), target_xform_);
        model_scale_ = target;
        SyncTargetToGpu();
    }

    // 「重置 target」：几何回到启动态（撤销所有变换 / 裁剪），重新启用全部 primitive。
    void ResetTarget() {
        target_current_ = target_startup_;
        target_prim_enabled_.assign(target_current_.size(), true);
        model_scale_ = 1.0f;
        target_xform_ = Mat4{};
        user_move_ = jpov::Vec3f(0.0f, 0.0f, 0.0f);
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

    // 画一块半透明平面 quad。尺寸 = **本帧实时求出的 target AABB**（面内两轴） × margin，
    // 中心 = 该 AABB 的面内中心 —— 所以 target 平移 / 缩放 / 裁剪 / 缝合之后，
    // 平面会跟着一起变大小和位置（2026-10-22 Danis：之前没联动）。
    void DrawPlaneQuad(jpov::RenderCommandList* cmds, int axis, float coord,
                       const jpov::Color& color) {
        const MeshBounds b = TargetBounds();  // 每帧现算（当前 CPU 几何）
        if (!b.valid) {
            return;
        }
        const int a = axis;
        const int u = (a + 1) % 3;  // 面内两轴
        const int v = (a + 2) % 3;
        const float bmin[3] = {b.min.x(), b.min.y(), b.min.z()};
        const float bmax[3] = {b.max.x(), b.max.y(), b.max.z()};
        const float cu = (bmin[u] + bmax[u]) * 0.5f;
        const float cv = (bmin[v] + bmax[v]) * 0.5f;
        // 退化轴（零厚度）给极小兜底，避免 quad 变成一条线。
        const float hu = std::max((bmax[u] - bmin[u]) * 0.5f * kPlaneQuadMargin, 1e-3f);
        const float hv = std::max((bmax[v] - bmin[v]) * 0.5f * kPlaneQuadMargin, 1e-3f);
        const auto point = [&](float du, float dv) {
            float p[3] = {0.0f, 0.0f, 0.0f};
            p[a] = coord;
            p[u] = cu + du;
            p[v] = cv + dv;
            return jpov::Vec3f(p[0], p[1], p[2]);
        };
        const jpov::Vec3f p00 = point(-hu, -hv);
        const jpov::Vec3f p01 = point(-hu, +hv);
        const jpov::Vec3f p10 = point(+hu, -hv);
        const jpov::Vec3f p11 = point(+hu, +hv);
        // 一个 quad = 两条 4 顶点 strip（相反绕序），保证从两侧都可见。
        cmds->DrawStrip3D({p00, p01, p10, p11}, color);
        cmds->DrawStrip3D({p00, p10, p01, p11}, color);
    }

    // 裁剪面（沿 clip_axis_，位置 clip_coord_）。
    void DrawClipPlane(jpov::RenderCommandList* cmds) {
        DrawPlaneQuad(cmds, clip_axis_, clip_coord_, kClipPlaneColor);
    }

    // 缝合面（恒为 Y 平面，位置 seam_plane_）。
    void DrawSeamPlane(jpov::RenderCommandList* cmds) {
        DrawPlaneQuad(cmds, 1, seam_plane_, kSeamPlaneColor);
    }

    // 步进平面坐标（不夹在启动包围盒里 —— 这正是换掉滑条的原因）。
    void StepClipCoord(float direction) {
        clip_coord_ += direction * clip_step_;
    }

    void StepSeamPlane(float direction) {
        seam_plane_ += direction * seam_step_;
        // 已经显示 reference 断面的话，跟着刷新（否则面动了、断面还停在旧位置）。
        if (reference_is_cut_) {
            ShowReferenceCut(true);
        }
    }

    // ==================== 保存 ====================

    void StartSaveTarget(ModelOutputFormat format) {
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
        save_ctrl_.Start(std::move(meshes), target_skeleton_, target_path_,
                         "target", format);
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

        // 保存按钮行：glb（内嵌单文件）/ gltf（外置纹理，可外部编辑贴图）。
        const bool saving = save_ctrl_.state() == ModelSaveState::kSaving;
        const float save_btn_w = (row_w - 8.0f) * 0.5f;
        const float save_btn2_x = left + save_btn_w + 8.0f;
        const char* glb_label = saving ? "保存中..." : "保存 glb";
        if (ui_.Button(glb_label,
                       jpov::UiRect{{left, row_y}, {save_btn_w, kRowH}})) {
            StartSaveTarget(ModelOutputFormat::kGlb);
        }
        const char* gltf_label = saving ? "保存中..." : "导出 gltf(外置)";
        if (ui_.Button(gltf_label,
                       jpov::UiRect{{save_btn2_x, row_y}, {save_btn_w, kRowH}})) {
            StartSaveTarget(ModelOutputFormat::kGltfExternal);
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

        // 显示开关：三个 toggle 同一行。
        //
        // 每格宽度 = 方框 + 标签实测宽 + Ui padding（再留 24px 间距），各格按此
        // 排 —— 等分会让「显示 reference」溢出到下一格；而 Checkbox 在
        // 「理想宽 < box」时整体居中，box 给宽了同样是标签右溢。故 box 直接
        // 给「理想宽」，且不拉伸（拉伸会把方框推到格子中间）。
        // 前提：一行总宽 ≈ 370px，默认 1280 窗口的左边栏（390px）可容纳；
        // 窗口更窄时这一行会变挤（Ui 不缩放字号）。
        const jpov::UiTheme toggle_theme = jpov::UiTheme::Default(kFontSize);
        const auto toggle_width = [&](const char* label) {
            return kRowH +
                   MeasureTextWidth(/*alias=*/std::string(), label, kFontSize) +
                   toggle_theme.padding_px + 24.0f;
        };
        const float kToggleRefW = toggle_width("显示 reference");
        const float kToggleTargetW = toggle_width("显示 target");
        const float kToggleAxisW = toggle_width("显示坐标轴");
        ui_.Checkbox("显示 reference", &show_reference_,
                     jpov::UiRect{{left, row_y}, {kToggleRefW, kRowH}},
                     /*stretch_w=*/false, /*stretch_h=*/false);
        ui_.Checkbox("显示 target", &show_target_,
                     jpov::UiRect{{left + kToggleRefW, row_y},
                                  {kToggleTargetW, kRowH}},
                     /*stretch_w=*/false, /*stretch_h=*/false);
        ui_.Checkbox("显示坐标轴", &show_axis_frame_,
                     jpov::UiRect{{left + kToggleRefW + kToggleTargetW, row_y},
                                  {kToggleAxisW, kRowH}},
                     /*stretch_w=*/false, /*stretch_h=*/false);
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
        // 裁剪：标题1 + 轴1 + 坐标滑条1 + 按钮行1 + 显示面1 + 消息1 + 只读1 = 7；
        // 缝合：标题1 + 算法1 + y 步进1 + 影响带1 + 形变区1 + 旋转偏移1 + 按钮1
        //       + 显示 ref 断面1 + 显示缝合面1 + 消息1 = 10；合计 7 + 10 = 17。
        constexpr int kRows = 17;
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

        {
            const std::string clip_value = Format("%.3f", static_cast<double>(clip_coord_));
            const std::string clip_label = Format("裁剪面 %c(米)", AxisTag());
            const int dir = DrawPlaneStepRow(clip_label.c_str(), clip_value.c_str(),
                                             &clip_step_, &clip_step_field_, left, row_w,
                                             row_y);
            if (dir != 0) {
                StepClipCoord(static_cast<float>(dir));
            }
        }
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

        // ---- 缝合 ----
        DrawLabel("缝合（同一个 y 平面切两个模型）", left, row_w, row_y);
        row_y += step_y;

        {
            static const std::vector<const char*> kAlgoItems = {"harmonic", "arap",
                                                                "tps"};
            ui_.Combo("算法", &seam_algo_, kAlgoItems,
                      jpov::UiRect{{left, row_y}, {row_w, kRowH}});
        }
        row_y += step_y;

        {
            const std::string seam_value = Format("%.3f", static_cast<double>(seam_plane_));
            const int dir = DrawPlaneStepRow("缝合面 y(米)", seam_value.c_str(), &seam_step_,
                                             &seam_step_field_, left, row_w, row_y);
            if (dir != 0) {
                StepSeamPlane(static_cast<float>(dir));
            }
        }
        row_y += step_y;

        ui_.SliderFloat("影响带 ×半径", &seam_band_scale_,
                        jpov::UiRect{{left, row_y}, {row_w, kRowH}}, 0.2f, 6.0f,
                        /*decimal_places*/ 1);
        row_y += step_y;

        ui_.SliderFloat("形变区 y_span(米, 0=自动)", &seam_y_span_,
                        jpov::UiRect{{left, row_y}, {row_w, kRowH}}, 0.0f, 1.0f,
                        /*decimal_places*/ 2);
        row_y += step_y;

        ui_.SliderFloat("对应旋转 (°)", &seam_offset_deg_,
                        jpov::UiRect{{left, row_y}, {row_w, kRowH}}, -180.0f, 180.0f,
                        /*decimal_places*/ 0);
        row_y += step_y;

        if (ui_.Button("执行缝合（底口 → reference 顶口）",
                       jpov::UiRect{{left, row_y}, {row_w, kRowH}})) {
            ApplyStitch();
        }
        row_y += step_y;

        if (ui_.Checkbox("reference 也切到缝合面", &show_reference_cut_,
                         jpov::UiRect{{left, row_y}, {row_w, kRowH}})) {
            ShowReferenceCut(show_reference_cut_);
        }
        row_y += step_y;

        ui_.Checkbox("显示缝合面", &show_seam_plane_,
                     jpov::UiRect{{left, row_y}, {row_w, kRowH}});
        row_y += step_y;

        if (!seam_msg_.empty()) {
            cmds->DrawText(seam_msg_, {left, row_y + (kRowH - kFontSize) * 0.5f},
                           kFontSize, jpov::Color{0.92f, 0.93f, 0.95f, 1.0f},
                           jpov::TextAlignment::kTopLeft, kViewerFontAlias);
        }
        row_y += step_y;
    }

    // ==================== HUD ====================

    // 把累积矩阵拆成 (T, 欧拉角°, 均匀缩放)，只用于 HUD 显示。
    void DecomposeXform(jpov::Vec3f* t, jpov::Vec3f* euler_deg, float* scale) const {
        const Mat4& M = target_xform_;
        *t = jpov::Vec3f(M.m[3], M.m[7], M.m[11]);
        const jpov::Vec3f c0(M.m[0], M.m[4], M.m[8]);
        const jpov::Vec3f c1(M.m[1], M.m[5], M.m[9]);
        const jpov::Vec3f c2(M.m[2], M.m[6], M.m[10]);
        const float s0 = std::max(c0.Norm(), 1e-9f);
        const float s1 = std::max(c1.Norm(), 1e-9f);
        const float s2 = std::max(c2.Norm(), 1e-9f);
        *scale = (s0 + s1 + s2) / 3.0f;
        const float r00 = M.m[0] / s0, r10 = M.m[4] / s0, r20 = M.m[8] / s0;
        const float r01 = M.m[1] / s1, r11 = M.m[5] / s1, r21 = M.m[9] / s1;
        const float r02 = M.m[2] / s2, r12 = M.m[6] / s2, r22 = M.m[10] / s2;
        // R = Rz·Ry·Rx（显示用）。
        const float sy = std::sqrt(r00 * r00 + r10 * r10);
        float rx = 0.0f, ry = 0.0f, rz = 0.0f;
        if (sy > 1e-5f) {
            rx = std::atan2(r21, r22);
            ry = std::atan2(-r20, sy);
            rz = std::atan2(r10, r00);
        } else {
            rx = std::atan2(-r12, r11);
            ry = std::atan2(-r20, sy);
        }
        constexpr float kRad2Deg = 180.0f / 3.14159265358979323846f;
        *euler_deg = jpov::Vec3f(rx * kRad2Deg, ry * kRad2Deg, rz * kRad2Deg);
    }

    // 诊断 HUD：相机姿态 + 当前配置 + 用户累计变换 + 模型矩阵 M + MVP。
    // 放在视口上方中间（左右面板之间），截图里一定能读到 —— 用来复现"出问题的配置"。
    void DrawHud(jpov::RenderCommandList* cmds, const jpov::WindowInfo& winfo) {
        constexpr float kPi = 3.14159265358979323846f;
        const float kFont = 11.0f;
        const float kLineH = 13.0f;
        const float kPad = 8.0f;
        const float x0 = 0.305f * winfo.width;
        const float y0 = 8.0f;

        const jpov::Vec3f cam = view_.Position();
        const jpov::Vec3f tgt = ViewConfig::Target();
        const Mat4 view_m = Mat4LookAt(cam, tgt, jpov::Vec3f(0.0f, 1.0f, 0.0f));
        const Mat4 proj_m =
            Mat4Perspective(60.0f, winfo.width / std::max(winfo.height, 1.0f), 0.05f,
                            1000.0f);
        const Mat4 mvp = Mat4Mul(Mat4Mul(proj_m, view_m), target_xform_);

        std::vector<std::string> lines;
        lines.push_back(Format(
            "CAMERA pos(%.3f,%.3f,%.3f) R=%.3f phi=%.1fdeg theta=%.1fdeg panY=%.3f",
            static_cast<double>(cam.x()), static_cast<double>(cam.y()),
            static_cast<double>(cam.z()), static_cast<double>(view_.R),
            static_cast<double>(view_.phi * 180.0 / kPi),
            static_cast<double>(view_.theta * 180.0 / kPi),
            static_cast<double>(cam_pan_y_)));
        lines.push_back(Format(
            "  target(%.3f,%.3f,%.3f) fov=60 near=0.05 far=1000 aspect=%.3f",
            static_cast<double>(tgt.x()), static_cast<double>(tgt.y()),
            static_cast<double>(tgt.z()),
            static_cast<double>(winfo.width / std::max(winfo.height, 1.0f))));
        lines.push_back(Format(
            "STATE clip=%c%.3f seamY=%.3f algo=%s band=%.2f off=%.1f refCut=%d",
            AxisTag(), static_cast<double>(clip_coord_),
            static_cast<double>(seam_plane_),
            jpov::model_editor::SeamAlgoName(
                static_cast<jpov::model_editor::SeamAlgo>(seam_algo_)),
            static_cast<double>(seam_band_scale_),
            static_cast<double>(seam_offset_deg_), reference_is_cut_ ? 1 : 0));
        lines.push_back(Format(
            "MOVE   t(%.3f,%.3f,%.3f)  scale=%.4f  (用户累计：平移 + 缩放系数)",
            static_cast<double>(user_move_.x()), static_cast<double>(user_move_.y()),
            static_cast<double>(user_move_.z()), static_cast<double>(model_scale_)));
        for (int row = 0; row < 4; ++row) {
            lines.push_back(Format(
                "%s[%9.4f %9.4f %9.4f %9.4f]", row == 0 ? "M    " : "     ",
                static_cast<double>(target_xform_.m[row * 4 + 0]),
                static_cast<double>(target_xform_.m[row * 4 + 1]),
                static_cast<double>(target_xform_.m[row * 4 + 2]),
                static_cast<double>(target_xform_.m[row * 4 + 3])));
        }
        for (int row = 0; row < 4; ++row) {
            lines.push_back(Format(
                "%s[%9.4f %9.4f %9.4f %9.4f]", row == 0 ? "MVP  " : "     ",
                static_cast<double>(mvp.m[row * 4 + 0]),
                static_cast<double>(mvp.m[row * 4 + 1]),
                static_cast<double>(mvp.m[row * 4 + 2]),
                static_cast<double>(mvp.m[row * 4 + 3])));
        }

        const float h = kPad * 2.0f + kLineH * static_cast<float>(lines.size());
        const float w = 452.0f;
        cmds->DrawRect({x0, y0}, {w, h}, jpov::Color{0.0f, 0.0f, 0.0f, 0.62f});
        float y = y0 + kPad;
        for (const std::string& line : lines) {
            cmds->DrawText(line, {x0 + kPad, y}, kFont,
                           jpov::Color{0.92f, 0.95f, 1.0f, 1.0f},
                           jpov::TextAlignment::kTopLeft, kViewerFontAlias);
            y += kLineH;
        }
    }

    // 「平面坐标」一行：[标签] [步长输入] [<] [>] [当前值]。
    // 返回 -1 / 0 / +1（用户点了 < / 没点 / 点了 >），由调用方真正应用步进
    // （裁剪面和缝合面各有自己的后处理）。
    int DrawPlaneStepRow(const char* label, const char* value_text, float* step,
                         NumberField* step_field, float left, float row_w,
                         float row_y) {
        const float kRowH = kPanelRowH;
        const float lbl_w = 96.0f;
        const float step_w = 64.0f;
        const float btn_w = 26.0f;
        const float gap = 6.0f;
        DrawLabel(label, left, lbl_w, row_y);
        const float sx = left + lbl_w;
        const bool focus = ui_.InputText("", step_field->text, kNumberInputCapacity,
                                         jpov::UiRect{{sx, row_y}, {step_w, kRowH}});
        if (step_field->focused_prev && !focus) {
            CommitNumberField(
                step_field,
                [](float v) { return std::clamp(v, kPlaneStepMin, kPlaneStepMax); },
                step);
        }
        step_field->focused_prev = focus;
        const float b1 = sx + step_w + gap;
        const float b2 = b1 + btn_w + gap;
        int dir = 0;
        if (ui_.Button("<", jpov::UiRect{{b1, row_y}, {btn_w, kRowH}})) {
            dir = -1;
        }
        if (ui_.Button(">", jpov::UiRect{{b2, row_y}, {btn_w, kRowH}})) {
            dir = +1;
        }
        const float vx = b2 + btn_w + gap;
        DrawLabel(value_text, vx, std::max(0.0f, left + row_w - vx), row_y);
        return dir;
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
        cam_pan_y_ = 0.0f;
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
