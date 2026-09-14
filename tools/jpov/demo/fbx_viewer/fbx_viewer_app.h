// JPOV FBX 观察器 — 渲染核心 App（FbxViewerApp）
//
// 用途：把一段 FBX 动画（外部动作素材）以**火柴人**形式播放出来，肉眼验证
// "从 FBX 的 Lcl Rotation 构造出的 skeleton pose 合不合法" —— 动作看起来是不是
// 一个能辨认的人形在动（详见 docs/jpov_fbx_viewer_design.md）。
//
// 与模型查看器（ViewerApp）的关系：同为"标准晴天布景 + 右键环绕相机"的姊妹工具，
// 但被观察对象与面板都不同 —— viewer 看静态 glTF + 光照滑条，本工具看 FBX 动作 +
// 播放控制。真正共享的是【场景构造/相机交互】（view_config.h）与【火柴人几何】
// （interface/skeleton_mesh.h）、【时间采样】（interface/animation_sampler.h），
// 外壳各自独立（同 editor_app.h 的判断：不共享的面板/输入不要硬塞进一个类）。
//
// 渲染链路（每帧）：
//   anim_time_seconds_ --(SampleClipPose, 逐骨 Slerp)--> SkeletonPose（源骨架语义）
//     --(BuildBoneMeshInBoneSpace)--> MeshData --(UpdateMesh)--> DrawObject3D
// 即"位姿 → 几何"直通，不引入 GPU 蒙皮中间层：本工具要验的是**位姿本身**对不对，
// 链路上每步都可见（骨杆朝向直接反映每骨位姿），出问题好定位。骨数少（Mixamo 全身
// 65 骨 × 每骨 24 顶点），每帧重建 mesh 的开销可忽略；且只在"位姿真的变了"时重建
// （暂停 / rest 模式下零重建）。
//
// ── 可选「对照组」：指定一个 glb 后，同时能看 glb rest 骨架被同一份 pose 驱动 ──
// Danis 2026-09-14 需求：验证"不加重定向把 fbx 的 lcl rotation 直接搬到 glb rest 骨架上
// 会不对"这一个判断。做法（本质是**消融实验/对照组**，不是重定向）：
//   · 源（红）= fbx rest 骨架 + fbx 自己的动画  → 正确答案的参照；
//   · 目标（蓝）= glb rest 骨架 + **同一份 pose 数值直搬**（骨名匹配，见
//     skeleton_pose_transfer.h，函数名即警示：NoRetarget）→ 预期"四肢绕错轴"；
//   · 面板「mesh 来源」combo 选看哪个（/两者并列，红左蓝右）。
// 蓝骨与红骨是**两套骨架各自的 rest 形状**，都只被 pose（旋转）驱动：于是"动作对不对"
// 与"mesh 蒙皮对不对"解耦，正是 retarget 设计文档 §6.4「先上骨人」的用法。
//
// ⚠️ 骨架与动画**分两个 loader 入口取**（都是既有能力，本 PR 不新增读取路径）：
//   - skeleton_ 走 jpov::LoadFbxSkeleton —— rest_offset 已归一到**米**；
//   - clip_     走 jpov::LoadFbxAnimation —— 给 帧频 + 全帧；帧里存的是**相对 bind 的
//     增量旋转**（纯旋转量，与单位无关）。
//   两者骨序同源（都按 node 树 DFS 收 bone 节点），叠加起来即"米制骨架 + 增量旋转"，
//   LoadFbx 里 CHECK 住骨数一致，防止未来某侧改了收集顺序而静默错位。

#ifndef JPOV_DEMO_FBX_VIEWER_FBX_VIEWER_APP_H_
#define JPOV_DEMO_FBX_VIEWER_FBX_VIEWER_APP_H_

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/fbx_viewer/skeleton_pose_transfer.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/interface/animation_sampler.h"
#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/interface/skeleton_mesh.h"
#include "tools/jpov/interface/skeleton_types.h"
#include "tools/jpov/interface/ui.h"
#include "tools/jpov/src/fbx_loader.h"
#include "tools/jpov/src/gltf_loader.h"

namespace jpov_fbx_viewer {

// 渲染/窗口分辨率（与查看器/编辑器同规格：1280×720，不可 resize）。
inline constexpr int kViewerWidth  = 1280;
inline constexpr int kViewerHeight = 720;

// 交互帧率（Hz）。也是动画时间的推进步长（每帧推进 1/kViewerFps 秒；见 AdvancePlayback）。
inline constexpr float kViewerFps = 60.0f;

// UI 文本字体别名 = CJK（面板显中文；拉丁由渲染层自动回退）。
// 与 viewer_app/editor_app 同值不同名：三个工具各自单点定义，互不牵连。
inline constexpr const char* kViewerFontAlias = jpov::kFontBuiltinCJK;

// 光照固定为 sunny day 同款（与查看器 MakeLighting 的晴天正午配置同参数）：
// 太阳仰角 90°（天顶正午）、浊度 2（大晴）、中性色温。
inline constexpr float kSunElevDeg = 90.0f;
inline constexpr float kTurbidity  = 2.0f;
inline constexpr float kSeasonR    = 1.0f;

// 地面板：浅灰实心板，中心在 y = -kGroundHalfThickness、顶面精确落在 y = 0，
// 于是骨架空间原点（脚底）正好踩在板上（同火柴人 gold 的布景约定）。
inline constexpr float kGroundHalfSize      = 30.0f;
inline constexpr float kGroundHalfThickness = 0.05f;

// 骨杆半宽（米）：直径 ≈ 4cm，1.75m 身高下肉眼可辨（与火柴人 gold 同款取值）。
inline constexpr float kBoneRadius = 0.02f;

// 面板字号（px）。
inline constexpr float kPanelFontSize = 16.0f;

// 顶点包围盒（逐分量 min/max）：初始机位适配 + 「两者并列」摆放用。
struct Bounds {
    jpov::Vec3f min;
    jpov::Vec3f max;

    float SizeX() const { return max.x() - min.x(); }
    float CenterX() const { return 0.5f * (min.x() + max.x()); }
};

// 由 mesh 顶点算包围盒。
// Pre-condition: mesh.positions 非空（空 → LOG(FATAL)；火柴人 mesh 为空的骨架无意义）。
inline Bounds ComputeMeshBounds(const jpov::MeshData& mesh) {
    CHECK(!mesh.positions.empty()) << "ComputeMeshBounds: mesh 顶点为空";
    Bounds b{mesh.positions[0], mesh.positions[0]};
    for (const jpov::Vec3f& p : mesh.positions) {
        b.min = jpov::Vec3f(std::min(b.min.x(), p.x()), std::min(b.min.y(), p.y()),
                            std::min(b.min.z(), p.z()));
        b.max = jpov::Vec3f(std::max(b.max.x(), p.x()), std::max(b.max.y(), p.y()),
                            std::max(b.max.z(), p.z()));
    }
    return b;
}

// 「mesh 来源」选项（面板 combo 的下标即本枚举值，顺序须一致）。
//   kFbxOnly —— 只看源（红）：fbx rest 骨架 + fbx 自己的动画（正确参照）；
//   kGlbOnly —— 只看目标（蓝）：glb rest 骨架 + 同一份 pose 数值直搬（无重定向）；
//   kBoth    —— 两者并列同屏（红左蓝右，方便一眼对比）。
enum MeshSource {
    kFbxOnly = 0,
    kGlbOnly = 1,
    kBoth    = 2,
};
// combo 下拉项文本（顺序 == 上面的枚举）。
inline const char* const kMeshSourceItems[] = {
    "fbx rest（红，源）",
    "glb rest（蓝，无重定向）",
    "两者并列（红左/蓝右）",
};
inline constexpr int kMeshSourceItemCount = 3;

// combo 项列表（Ui::Combo 要 vector<const char*>）。
inline std::vector<const char*> MeshSourceItems() {
    return std::vector<const char*>(kMeshSourceItems,
                                    kMeshSourceItems + kMeshSourceItemCount);
}

// 「两者并列」时两骨人的横向间距（米）：取各自 rest 包围盒宽 + 该间隙，
// 使两根骨人不重叠且对称于世界 x=0（相机环绕中心 = 原点，故对称才不偏）。
inline constexpr float kPairGapMeters = 0.4f;

// FBX 观察器渲染核心 App。
class FbxViewerApp : public JPOV {
public:
    using JPOV::JPOV;

    // ── 场景状态（LoadFbx / LoadGltfSkeleton 装配后由 OneIteration 消费）──
    jpov::SkeletonType skeleton_;   // 源骨架（米制 rest_offset + bind 朝向）
    jpov::FBXClip clip_;            // 源动画（帧频 + 全帧增量旋转）
    uint32_t ground_mesh_ = 0;      // 地面板 GPU handle
    uint32_t bone_mesh_   = 0;      // 源火柴人 mesh GPU handle（红；每帧按位姿更新）
    jpov_viewer::ViewConfig view_;  // 相机（右键 drag / 滚轮，与查看器共用同一套）

    // ── 可选目标骨架（glb；"无重定向对照组"）──
    // has_glb_ == true 时：glb_skeleton_ 为从 glb 读出的 rest 骨架（通常 23 骨），
    // glb_bone_mesh_ 为它的火柴人 mesh（蓝），面板多出「mesh 来源」combo。
    jpov::SkeletonType glb_skeleton_;
    bool has_glb_ = false;
    uint32_t glb_bone_mesh_ = 0;
    int glb_mapped_bones_ = 0;      // 源/目标骨名命中数（面板显示；未命中骨保持 rest）
    int mesh_source_ = kFbxOnly;    // 本帧看哪个（/哪些）骨架，见 MeshSource

    // ── 播放状态 ──
    double anim_time_seconds_ = 0.0;  // 动画时间（秒）；循环语义由 SampleClipPose 承担
    bool paused_          = false;    // 暂停：时间停止推进（"主频的时间停止更新"）
    bool rest_pose_mode_  = false;    // 固定 pose = identity 观察 rest（T-pose）模式

    // 装配场景：读 fbx 的骨架 + 动画，建地面板与首帧火柴人 mesh，按包围盒定初始机位。
    //   返回 false = 加载/装配失败（路径不存在 / 读不了 / 无 bone / 无动画），
    //   失败原因已 LOG(ERROR)，不静默、不 fallback。
    bool LoadFbx(const std::string& path);

    // 可选装配：读 glb 的 rest 骨架当**目标骨架**（蓝，无重定向对照组）。
    //   返回 false = 读不了 / 无 skin，失败原因已 LOG(ERROR)。
    //   须在 LoadFbx 之后调（骨名命中数依赖源骨架）；调用后 mesh_source_ 默认切到
    //   kBoth（传了 glb 就是想对比），初始机位重新按两者并列的包围盒适配。
    bool LoadGltf(const std::string& path);

    // 渲染/交互是否绘制顶部面板。
    //   true  = 交互窗口（OneIteration 末尾画面板）
    //   false = headless 单帧出图：截图是纯 3D（不画面板、不消费输入）
    void SetShowPanel(bool show) { show_panel_ = show; }

    // 注入真实字体文本测量回调（UI 内部用；交互与 headless 都装，装了无副作用）。
    void InstallTextMeasure() {
        ui_.SetTextMeasure(&FbxViewerApp::ViewerTextWidth, this);
    }

    // 唯一的渲染体：交互（Run 循环）与 headless 单帧（RunOnce）共用，零分叉。
    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;

        // 渲染分辨率 = 窗口分辨率（单点定义，与出图尺寸解耦）。
        cmds->camera.fbo_3d_width_  = kViewerWidth;
        cmds->camera.fbo_3d_height_ = kViewerHeight;

        // 交互输入 → 相机（仅可见窗口消费输入；headless 出图时相机由外部设定）。
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
            jpov_viewer::ApplyInput(&view_, dx, dy, scroll,
                                   static_cast<int>(winfo.width),
                                   static_cast<int>(winfo.height));
        }
        cmds->camera.position = view_.Position();
        cmds->camera.target   = {0.0f, camera_target_height_, 0.0f};
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov      = 60.0f;
        cmds->camera.near     = 0.05f;
        cmds->camera.far      = 1000.0f;

        // 光照：sunny day（天光 + 太阳 + 环境光，全由 sky 推导；同查看器晴天配置）。
        const jpov_viewer::NoonLighting light =
            jpov_viewer::MakeLighting(kSunElevDeg, kTurbidity, kSeasonR);
        cmds->sky     = light.sky;
        cmds->sun     = light.sun;
        cmds->ambient = light.ambient;
        cmds->tone_mapping = true;

        // 地面板：中心下沉半厚 → 顶面 y=0，脚踩地。
        cmds->DrawObject3D(ground_mesh_, jpov_viewer::GroundMaterial(),
                           /*center*/ {0.0f, -kGroundHalfThickness, 0.0f},
                           /*up*/     {0.0f, 1.0f, 0.0f},
                           /*front*/  {0.0f, 0.0f, 1.0f},
                           /*scale*/  1.0f,
                           /*highlight*/ false,
                           /*picking_id*/ 0);

        // 火柴人（红）：本帧位姿 → 几何（位姿变了才重建），单看时 identity 摆放
        // （模型系 ≡ 世界系，骨架空间原点即世界原点 → 脚落在地面板顶面）。
        UpdateFramePose();
        UpdateBoneMeshIfPoseChanged();
        // 目标火柴人（蓝）：同一份位姿按骨名**数值直搬**到 glb 骨架（无重定向）后重建。
        UpdateGlbBoneMeshIfNeeded();

        // 摆放：单看时各自在原点；“两者并列”时以 x=0 对称分开（相机环绕中心在原点，
        // 对称才不偏）；横向位置再各自减去自身包围盒中心的 x（使两根骨人不重叠）。
        const bool show_fbx = (mesh_source_ == kFbxOnly || mesh_source_ == kBoth);
        const bool show_glb =
            (has_glb_ && (mesh_source_ == kGlbOnly || mesh_source_ == kBoth));
        const bool side_by_side = (mesh_source_ == kBoth);
        const float red_x = side_by_side ? (-0.5f * pair_sep_m_ - fbx_center_x_) : 0.0f;
        const float blue_x = side_by_side ? (0.5f * pair_sep_m_ - glb_center_x_) : 0.0f;
        if (show_fbx) {
            cmds->DrawObject3D(bone_mesh_, jpov::PBRMaterial::SolidColor(jpov::kColorRed),
                               /*center*/ {red_x, 0.0f, 0.0f},
                               /*up*/     {0.0f, 1.0f, 0.0f},
                               /*front*/  {0.0f, 0.0f, 1.0f},
                               /*scale*/  1.0f,
                               /*highlight*/ false,
                               /*picking_id*/ 0);
        }
        if (show_glb) {
            cmds->DrawObject3D(glb_bone_mesh_,
                               jpov::PBRMaterial::SolidColor(jpov::kColorBlue),
                               /*center*/ {blue_x, 0.0f, 0.0f},
                               /*up*/     {0.0f, 1.0f, 0.0f},
                               /*front*/  {0.0f, 0.0f, 1.0f},
                               /*scale*/  1.0f,
                               /*highlight*/ false,
                               /*picking_id*/ 0);
        }

        // 面板（仅交互窗口）。时间推进放在最后：本帧画的是"推进前"的时刻，
        // 于是 headless 出图时先设 anim_time_seconds_ 再 RunOnce 即得到该时刻的帧。
        if (show_panel_) {
            DrawPanel(input, winfo);
            ui_.End();
            ui_.Emit(cmds);
        }
        AdvancePlayback(1.0 / static_cast<double>(kViewerFps));
    }

    // ── 白盒测试入口（friend FbxViewerPlaybackTest；ForTest 后缀明示用途，非公共 API）──
    void AdvancePlaybackForTest(double dt_seconds) { AdvancePlayback(dt_seconds); }
    void UpdateFramePoseForTest() { UpdateFramePose(); }
    const jpov::SkeletonPose& frame_pose_for_test() const { return frame_pose_; }
    int frame_index_for_test() const { return frame_index_; }

private:
    // 单测 friend（白盒回归，见 fbx_viewer_playback_test.cc）。
    friend class FbxViewerPlaybackTest;

    // 本帧位姿（唯一真相）：rest 模式 = identity；动画模式 = 按 anim_time_seconds_ 采样。
    // 取帧结果（帧号）一并记在 frame_index_，供面板显示/测试读取。
    // Pre-condition: skeleton_.bone_count() > 0 且 clip_ 已加载（LoadFbx 保证）。
    void UpdateFramePose() {
        if (rest_pose_mode_) {
            frame_pose_ = jpov::SkeletonPose::Identity(skeleton_.bone_count());
            frame_index_ = kRestFrameIndex;
            return;
        }
        frame_index_ = jpov::SampleClipPose(clip_, anim_time_seconds_, &frame_pose_);
    }

    // 推进动画时间（"主频"步进 = 1/kViewerFps 秒）。
    //   暂停 → 不推进（面板按下的暂停键让时间停住，画面定格在当前帧）；
    //   rest 模式 → 动画不参与显示，同样不推进（切回动画模式从停住的时刻继续）。
    // Pre-condition: dt_seconds > 0。
    void AdvancePlayback(double dt_seconds) {
        CHECK_GT(dt_seconds, 0.0) << "AdvancePlayback: dt 必须 > 0";
        if (paused_ || rest_pose_mode_) {
            return;
        }
        anim_time_seconds_ += dt_seconds;
    }

    // 位姿变了才重建源（红）mesh；目标（蓝）mesh 同款（但位姿要先直搬过去）。
    // 位姿由 (rest 模式, 动画时刻) 唯一决定，故用它作"是否变化"的键（两份各持一份）。
    void UpdateBoneMeshIfPoseChanged() {
        if (red_drawn_.Matches(rest_pose_mode_, anim_time_seconds_)) {
            return;
        }
        UpdateMesh(bone_mesh_,
                   jpov::BuildBoneMeshInBoneSpace(skeleton_, frame_pose_,
                                                  kBoneRadius));
        red_drawn_ = {true, rest_pose_mode_, anim_time_seconds_};
    }

    // 目标（蓝）mesh：把源位姿按骨名**数值直搬**到 glb 骨架（无重定向，见
    // skeleton_pose_transfer.h）后重建。没 glb / 本帧不看蓝 → 标 key 失效
    // （本帧没算蓝位姿，下次切回蓝模式必须重建）。
    void UpdateGlbBoneMeshIfNeeded() {
        if (!has_glb_ || mesh_source_ == kFbxOnly) {
            blue_drawn_.valid = false;
            return;
        }
        TransferPoseByNameNoRetarget(skeleton_, frame_pose_, glb_skeleton_,
                                     &glb_pose_);
        if (blue_drawn_.Matches(rest_pose_mode_, anim_time_seconds_)) {
            return;
        }
        UpdateMesh(glb_bone_mesh_,
                   jpov::BuildBoneMeshInBoneSpace(glb_skeleton_, glb_pose_,
                                                  kBoneRadius));
        blue_drawn_ = {true, rest_pose_mode_, anim_time_seconds_};
    }

        // 顶部面板：一行控件（mesh 来源 combo + 暂停按钮 + rest 复选框）+ 一行状态文本。
    // ⚠️ 面板贴**顶部**而非底部（2026-09-14 Danis 定）：`Ui::Combo` 的下拉列表是**向下**
    //    画的（list_top = 框底、高 = 项数×行高），贴底时 3 个选项会伸出屏幕外。
    // ⚠️ 布局用**本帧渲染分辨率（winfo）**而非编译期常量：2D 指令的坐标空间就是当帧
    //    渲染分辨率（editor_app.h 记过这条坑 —— 写死尺寸会在窗口/出图尺寸变化后飘位）。
    void DrawPanel(const jpov::InputSnapshot& input,
                   const jpov::WindowInfo& winfo) {
        const float w = winfo.width;
        const float h = winfo.height;
        jpov::UiTheme theme = jpov::UiTheme::Default(kPanelFontSize);
        theme.font_alias = kViewerFontAlias;
        ui_.Begin(input, theme, w, h, 1000.0f / kViewerFps);

        const float kRowH    = 30.0f;
        const float kButtonW = 120.0f;
        const float kCheckW  = 300.0f;
        const float kComboW  = 320.0f;
        const float kGap     = 16.0f;
        const float kTop     = 16.0f;  // 到屏顶留白
        const float kTextGap = 8.0f;   // 控件行与状态行之间
        const float kTextRow = 26.0f;

        // 两行整体贴顶居中：控件行在最顶（下拉展开才有地方），状态文本行紧随其下。
        // 没传 glb 时不画 combo（没有目标骨架可选，多余的控件只会误导）。
        float ctrl_w = kButtonW + kGap + kCheckW;
        if (has_glb_) {
            ctrl_w += kGap + kComboW;
        }
        const float ctrl_left = (w - ctrl_w) * 0.5f;
        const float ctrl_top = kTop;
        const float text_top = ctrl_top + kRowH + kTextGap;

        float x = ctrl_left;
        if (has_glb_) {
            // mesh 来源：看源（红）/ 目标（蓝）/ 两者并列（见 MeshSource 注释）。
            ui_.Combo("mesh 来源", &mesh_source_, MeshSourceItems(),
                      jpov::UiRect{{x, ctrl_top}, {kComboW, kRowH}});
            x += kComboW + kGap;
        }
        // 暂停按钮：按下切换暂停态（文案随状态变，一眼看出当前是停是放）。
        if (ui_.Button(paused_ ? "继续播放" : "暂停",
                       jpov::UiRect{{x, ctrl_top}, {kButtonW, kRowH}})) {
            paused_ = !paused_;
        }
        x += kButtonW + kGap;
        // rest 模式复选框：勾上 = 固定 pose=identity，观察骨架 rest（T-pose）形态。
        ui_.Checkbox("固定 rest 位姿(identity)", &rest_pose_mode_,
                     jpov::UiRect{{x, ctrl_top}, {kCheckW, kRowH}});

        // 状态文本：帧号 / 总帧 / 源帧频 / 动画时刻 / 播放态（+ glb 对照信息）。
        char head[192];
        if (rest_pose_mode_) {
            std::snprintf(head, sizeof(head),
                          "rest 模式：pose = identity（%d 骨 T-pose）",
                          skeleton_.bone_count());
        } else {
            std::snprintf(head, sizeof(head),
                          "帧 %d / %zu    fps %.1f    t = %.3f s    %s",
                          frame_index_,
                          clip_.frames.size(),
                          clip_.frames_per_second,
                          anim_time_seconds_,
                          paused_ ? "已暂停" : "播放中");
        }
        // glb 对照信息（仅传了 glb 时）：说清蓝骨是什么、“无重定向”这件事、命中多少骨。
        char mode_tag[160] = "";
        if (has_glb_) {
            if (mesh_source_ == kFbxOnly) {
                std::snprintf(mode_tag, sizeof(mode_tag), "  | 蓝：关（只看红）");
            } else if (mesh_source_ == kGlbOnly) {
                std::snprintf(mode_tag, sizeof(mode_tag),
                              "  | 蓝=glb rest 直驱（无重定向，命中 %d/%d）",
                              glb_mapped_bones_, glb_skeleton_.bone_count());
            } else {
                std::snprintf(mode_tag, sizeof(mode_tag),
                              "  | 红=fbx 源，蓝=glb 直驱（无重定向，命中 %d/%d）",
                              glb_mapped_bones_, glb_skeleton_.bone_count());
            }
        }
        const std::string line = std::string(head) + mode_tag;
        ui_.Text(line.c_str(), jpov::UiRect{{ctrl_left, text_top},
                                            {w - 2.0f * ctrl_left, kTextRow}});
    }

    // 文本测量回调 → JPOV::MeasureTextWidth（空别名 = 首个注册字体，同查看器接线）。
    static float ViewerTextWidth(const char* text, float font_size,
                                 const char* /*font_alias*/, void* userdata) {
        FbxViewerApp* app = static_cast<FbxViewerApp*>(userdata);
        return app->MeasureTextWidth(/*alias=*/std::string(),
                                     /*text=*/text ? text : "", font_size);
    }

    // rest 模式下"没有取帧"的哨兵帧号（面板据此不显示帧号）。
    static constexpr int kRestFrameIndex = -1;

    // 「已按哪个位姿建过 mesh」的键（位姿只由 (rest 模式, 动画时刻) 决定）。
    // 红/蓝两个 mesh 各持一份：各自记"我按哪个键建过"。
    struct DrawnPoseKey {
        bool valid = false;  // 是否已建过
        bool rest  = false;  // 建时的 rest 模式
        double time = 0.0;   // 建时的动画时刻
        bool Matches(bool rest_mode, double t) const {
            return valid && rest == rest_mode && time == t;
        }
    };

    bool show_panel_ = true;          // 是否画面板/消费输入（headless 出图为 false）
    jpov::SkeletonPose frame_pose_;    // 本帧源位姿（UpdateFramePose 写入）
    jpov::SkeletonPose glb_pose_;      // 目标位姿（源位姿直搬，UpdateGlbBoneMeshIfNeeded 写入）
    int frame_index_ = kRestFrameIndex;  // 本帧采样到的源帧下标
    float camera_target_height_ = 0.0f;  // 相机注视高度（按骨架包围盒算，见 FitInitialView）
    // rest 火柴人 mesh 的包围盒（装配时算）：初始机位适配 + 「两者并列」摆位用。
    Bounds fbx_bounds_{};
    Bounds glb_bounds_{};
    // 「两者并列」摆放用：两骨人 rest 包围盒中心的 x 与对称间距（LoadFbx/LoadGltf 算）。
    float fbx_center_x_ = 0.0f;
    float glb_center_x_ = 0.0f;
    float pair_sep_m_ = 0.0f;

    DrawnPoseKey red_drawn_;   // 源（红）mesh 已按哪个键建过
    DrawnPoseKey blue_drawn_;  // 目标（蓝）mesh 已按哪个键建过

    // 初始机位：按**当前场景全部骨人**（含并列的蓝）的包围盒自适应距离
    // （同查看器用 FitRadius 的做法），注视高度取包围盒中高（约胸口）。
    // ⚠️ 一律按“最宽”（两者并列）适配：切 mesh 来源时相机**不跳**，且任何模式都不裁切。
    void FitInitialView() {
        const float red_x =
            has_glb_ ? (-0.5f * pair_sep_m_ - fbx_center_x_) : 0.0f;
        const float blue_x =
            has_glb_ ? (0.5f * pair_sep_m_ - glb_center_x_) : 0.0f;
        float bmin[3] = {fbx_bounds_.min.x() + red_x, fbx_bounds_.min.y(),
                         fbx_bounds_.min.z()};
        float bmax[3] = {fbx_bounds_.max.x() + red_x, fbx_bounds_.max.y(),
                         fbx_bounds_.max.z()};
        if (has_glb_) {
            bmin[0] = std::min(bmin[0], glb_bounds_.min.x() + blue_x);
            bmin[1] = std::min(bmin[1], glb_bounds_.min.y());
            bmin[2] = std::min(bmin[2], glb_bounds_.min.z());
            bmax[0] = std::max(bmax[0], glb_bounds_.max.x() + blue_x);
            bmax[1] = std::max(bmax[1], glb_bounds_.max.y());
            bmax[2] = std::max(bmax[2], glb_bounds_.max.z());
        }
        view_ = jpov_viewer::DefaultView();
        view_.R = jpov_viewer::ViewConfig::FitRadius(bmin, bmax, /*fov_deg*/ 60.0);
        camera_target_height_ = 0.5f * (bmin[1] + bmax[1]);
        LOG(INFO) << "初始机位: 包围盒 x[" << bmin[0] << "," << bmax[0] << "] y["
                  << bmin[1] << "," << bmax[1] << "] 初始 R=" << view_.R;
    }

    jpov::Ui ui_;  // 跨帧持有（复选框/按钮/combo 内部状态记忆）
};

// ── LoadFbx 实现（放类外，避免头文件里塞大段实现；仅本头文件的唯一实现点）──

inline bool FbxViewerApp::LoadFbx(const std::string& path) {
    if (!jpov::LoadFbxSkeleton(path, &skeleton_)) {
        LOG(ERROR) << "LoadFbx: 读骨架失败: " << path;
        return false;
    }
    if (!jpov::LoadFbxAnimation(path, &clip_)) {
        LOG(ERROR) << "LoadFbx: 读动画失败（观察器需要带动画的 FBX）: " << path;
        return false;
    }
    // 两个 loader 入口必须给出同一种骨架（同骨序）：动画帧的 joint_rotation 是按
    // clip_.skeleton 的下标排的，若与 skeleton_ 不同源就会静默错位（把 A 骨的旋转
    // 套到 B 骨上），这里直接挡住。
    CHECK_EQ(clip_.skeleton.joints.size(), skeleton_.joints.size())
        << "LoadFbx: 两个 loader 入口的骨数不一致（动画帧与骨架会错位）: "
        << clip_.skeleton.joints.size() << " vs " << skeleton_.joints.size();
    for (size_t i = 0; i < skeleton_.joints.size(); ++i) {
        CHECK_EQ(clip_.skeleton.joints[i].name, skeleton_.joints[i].name)
            << "LoadFbx: joint[" << i << "] 骨名不一致（两个 loader 的收集顺序分叉）";
    }

    // 地面板：扁 box，顶面落在 y=0。
    ground_mesh_ = RegisterMesh(jpov::MeshData::MakeBox(
        /*front_half_width*/ kGroundHalfSize,
        /*up_half_width*/    kGroundHalfThickness,
        /*left_half_width*/  kGroundHalfSize));

    // 火柴人：先按 identity（rest/T-pose）注册一次，之后每帧按位姿 UpdateMesh。
    const jpov::MeshData rest_mesh = jpov::BuildBoneMeshInBoneSpace(
        skeleton_,
        jpov::SkeletonPose::Identity(skeleton_.bone_count()), kBoneRadius);
    CHECK(!rest_mesh.positions.empty())
        << "LoadFbx: 火柴人 mesh 为空（骨架所有骨长均为 0？）—— 无法定机位";
    bone_mesh_ = RegisterMesh(rest_mesh);

    // 初始机位：按 rest 火柴人的包围盒自适应距离（见 FitInitialView）；此时还没 glb，
    // 若之后传了 glb，LoadGltf 会带着并列摆放再适配一次。
    fbx_bounds_ = ComputeMeshBounds(rest_mesh);
    fbx_center_x_ = fbx_bounds_.CenterX();
    pair_sep_m_ = 0.5f * fbx_bounds_.SizeX() + kPairGapMeters;  // 单骨人时的占位值
    FitInitialView();

    LOG(INFO) << "FBX 观察器装配完成: bones=" << skeleton_.bone_count()
              << " frames=" << clip_.frames.size()
              << " fps=" << clip_.frames_per_second
              << " 包围盒 y[" << fbx_bounds_.min.y() << "," << fbx_bounds_.max.y()
              << "]"
              << " 初始 R=" << view_.R;
    return true;
}

inline bool FbxViewerApp::LoadGltf(const std::string& path) {
    CHECK(!skeleton_.joints.empty())
        << "LoadGltf: 请先调 LoadFbx（目标骨架的骨名命中数依赖源骨架）";

    // glb 可能带多个 skin：取第一个（单骨架资产是常态；需要挑时另开参数）。
    std::vector<jpov::SkeletonType> skins;
    if (!jpov::LoadGltfSkeleton(path, &skins) || skins.empty()) {
        LOG(ERROR) << "LoadGltf: 读骨架失败或无 skin: " << path;
        return false;
    }
    glb_skeleton_ = skins[0];
    glb_skeleton_.Validate();

    // 蓝 mesh：glb 骨架按 identity（其 rest/T-pose）注册一次，之后每帧按直搬位姿 UpdateMesh。
    const jpov::MeshData rest_mesh = jpov::BuildBoneMeshInBoneSpace(
        glb_skeleton_,
        jpov::SkeletonPose::Identity(glb_skeleton_.bone_count()), kBoneRadius);
    CHECK(!rest_mesh.positions.empty())
        << "LoadGltf: glb 火柴人 mesh 为空（骨架所有骨长均为 0？）";
    glb_bone_mesh_ = RegisterMesh(rest_mesh);
    glb_bounds_ = ComputeMeshBounds(rest_mesh);
    glb_center_x_ = glb_bounds_.CenterX();

    // 骨名命中数（面板显示）：直接跑一次直搬（用源骨架的零 pose）拿统计。
    jpov::SkeletonPose probe;
    glb_mapped_bones_ = TransferPoseByNameNoRetarget(
                            skeleton_, jpov::SkeletonPose{}, glb_skeleton_, &probe)
                            .mapped;

    // 传了 glb 就是想对比：默认「两者并列」，并把间距按两骨人宽度算好（对称于 x=0）。
    has_glb_ = true;
    pair_sep_m_ =
        0.5f * (fbx_bounds_.SizeX() + glb_bounds_.SizeX()) + kPairGapMeters;
    mesh_source_ = kBoth;
    FitInitialView();

    LOG(INFO) << "glb 目标骨架装配完成: " << path
              << " bones=" << glb_skeleton_.bone_count()
              << " 骨名命中=" << glb_mapped_bones_ << "/" << glb_skeleton_.bone_count()
              << " 并列间距=" << pair_sep_m_ << "m";
    return true;
}

}  // namespace jpov_fbx_viewer

#endif  // JPOV_DEMO_FBX_VIEWER_FBX_VIEWER_APP_H_
