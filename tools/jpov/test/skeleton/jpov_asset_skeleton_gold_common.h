// JPOV 资产骨架 gold 公共场景 ——「skeleton from glb」+「skeleton from fbx」两条读取链路
//
// 目的（Danis 2026-09-14 定）：把两个【从资产文件读出】的骨架都按"无旋转 / 无缩放 / 无位移"
// （identity 摆放）画进同一张图，供肉眼直接对比两侧骨人（"先瞅瞅这俩长啥样"的底图）。
//
// 本 gold 覆盖的两个函数（定义同步维护在其源码处）：
//   1) skeleton from glb:  jpov::LoadGltfSkeleton（src/gltf_loader.h）
//        读 glTF skin：rest_offset = node.translation（相对父，米）；
//        bind_rotation = node.rotation（相对父的 bind 朝向）。
//        identity pose 下 jointWorld = JW_bind ≡ 资产里 mesh 的 bind 姿势（严格一致）。
//   2) skeleton from fbx:  jpov::LoadFbxSkeleton（src/fbx_loader.h）
//        读 FBX 骨节点：rest_offset = Lcl Translation × unit_meters（归一为米）；
//        bind_rotation = 骨静止 local 旋转（即 PreRotation 的合成——FBX 的 Lcl Rotation
//        是动画通道、默认≈0，静止朝向不在它里面）。
//        **identity pose 下的骨架 ≡ 源文件在「Lcl Rotation = 0（无动画）」时求出的姿势**
//        （Mixamo 官方源为 T-pose）。
//
// 场景与「bone_mesh_stickman_1280x720.png 对应 gold」相同（浅灰纯色地面板 + 标准晴天
// 天光/太阳/阴影 + 屏幕角红字 z/x/y 轴标签 + 同机位相机），差异只有两处：
//   - 火柴人来源从 Mixamo23Skeleton 换成"两个资产读出的骨架"各一根；
//   - fbx 那根用蓝色材质，与红 glb 区分（glb 保持红色，与既有火柴人 gold 同款风格）。
//
// 摆放："无旋转 / 无缩放 / 无位移"—— 两根骨人都以 identity DrawObject3D
// （center=0 / up=+Y / front=+Z / scale=1）画在骨架空间原点（两者重叠，第一时间直接对比
// 原始差异；如需左右并排另出改版，属后续迭代，不在此 gold 内）。
//
// 本头文件被 generator（写仓库 gold image）和 test（渲染 + 门禁）共用，保证两者帧一致。

#ifndef JPOV_TEST_SKELETON_JPOV_ASSET_SKELETON_GOLD_COMMON_H_
#define JPOV_TEST_SKELETON_JPOV_ASSET_SKELETON_GOLD_COMMON_H_

#include <cstdint>
#include <string>
#include <vector>

#include "glog/logging.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/interface/skeleton_mesh.h"
#include "tools/jpov/interface/skeleton_types.h"
#include "tools/jpov/src/fbx_loader.h"
#include "tools/jpov/src/gltf_loader.h"
#include "tools/jpov/test/test_utils.h"

namespace jpov_asset_skeleton_gold {

// gold image 仓库相对路径（generator 写入、test 读取比对）。
inline std::string GetGoldRelPath() {
    return "/skeleton/skeleton_from_glb_fbx_1280x720.png";
}

// 骨杆半宽（米）—— 与 bone_mesh_stickman gold 同款配置。
inline constexpr float kBoneRadius = 0.02f;

// 屏幕空间坐标轴提示标签："z"左下 / "x"右下 / "y"上方居中（同 bone_mesh gold）。
inline constexpr float kAxisLabelFontSize = 28.0f;
// 地面板半厚（米）：板中心下沉该值，使**顶面精确落在 y=0**（根关节正好踩地）。
inline constexpr float kGroundHalfThickness = 0.05f;
// 地面板半宽（米）—— 足够大以铺满视野。
inline constexpr float kGroundHalfSize = 30.0f;

// 两个资产路径（经 Bazel data 依赖提供；GetTestDataDir 兼容 bazel test/run 两态）。
inline std::string GlbSkeletonPath() {
    return jpov::GetTestDataDir() + "/object3d/mixamo_male/mixamo_male.glb";
}
inline std::string FbxSkeletonPath() {
    return jpov::GetTestDataDir() + "/animations/hip_hop_dance.fbx";
}

// 渲染应用：浅灰纯色地面 + 天光天色 + 两根 identity 摆放的骨人（glb 红 / fbx 蓝）。
class AssetSkeletonGoldApp : public JPOV {
public:
    using JPOV::JPOV;

    uint32_t ground_mesh_ = 0;         // 浅灰纯色地面板
    uint32_t glb_skeleton_mesh_ = 0;   // 从 glb 读出的骨架 → 火柴人 mesh（红）
    uint32_t fbx_skeleton_mesh_ = 0;   // 从 fbx 读出的骨架 → 火柴人 mesh（蓝）

    void OneIteration(int64_t, const jpov::InputSnapshot&,
                      const jpov::WindowInfo& winfo, jpov::RenderCommandList* cmds) override {
        constexpr float kResW = 1280.0f, kResH = 720.0f;
        cmds->camera.fbo_3d_width_  = kResW;
        cmds->camera.fbo_3d_height_ = kResH;
        // 相机与 bone_mesh gold 相同（对准原点附近，斜俯视角）。
        cmds->camera.position = {4.0f, 2.5f, 4.0f};
        cmds->camera.target   = {0.0f, 1.0f, 0.0f};
        cmds->camera.near     = 0.05f;

        // 太阳（斜上）+ 天光（Preetham 正午晴天）+ 环境光 + HDR tone map
        // —— 与 standard_sunny_day 一致的照明基准（同 bone_mesh gold）。
        const jpov::Vec3f sun_l = {0.0f, -1.0f, -1.0f};
        cmds->sun = jpov::DirectionalLight{{sun_l}, {1, 1, 1, 1}, 3.0f};
        cmds->ambient = jpov::AmbientLight{.color = {1, 1, 1, 1}, .intensity = 0.3f};
        cmds->sky = jpov::SkyCommand{
            jpov::Vec3f(-sun_l.x(), -sun_l.y(), -sun_l.z()),
            /*turbidity*/ 2.0f, /*daylight_season*/ {1, 1, 1, 1}, /*intensity*/ 1.0f,
            /*ground_color*/ {0.05f, 0.06f, 0.08f, 1.0f},
            /*sun_radius*/ 0.02, /*sun_brightness*/ 1e3, /*sun_glow*/ 1.0};
        cmds->tone_mapping = true;

        // 地面：浅灰纯色板，中心下沉 kGroundHalfThickness 使顶面贴 y=0。
        if (ground_mesh_ != 0) {
            cmds->DrawObject3D(
                ground_mesh_, jpov::PBRMaterial::SolidColor({0.75f, 0.75f, 0.75f, 1.0f}),
                /*center*/ {0, -kGroundHalfThickness, 0},
                /*up*/     {0, 1, 0},
                /*front*/  {0, 0, 1},
                /*scale*/  1.0f,
                /*highlight*/ false,
                /*picking_id*/ 0);
        }

        // 两根骨人：**无旋转 / 无缩放 / 无位移**（identity 摆放，见文件头）。
        // glb 骨人（红）—— 与 bone_mesh gold 的火柴人同款材质风格。
        if (glb_skeleton_mesh_ != 0) {
            cmds->DrawObject3D(glb_skeleton_mesh_,
                               jpov::PBRMaterial::SolidColor(jpov::kColorRed),
                               /*center*/ {0, 0, 0},
                               /*up*/     {0, 1, 0},
                               /*front*/  {0, 0, 1},
                               /*scale*/  1.0f,
                               /*highlight*/ false,
                               /*picking_id*/ 0);
        }
        // fbx 骨人（蓝）—— 与 glb 骨人在原点重叠，直接对比原始差异。
        if (fbx_skeleton_mesh_ != 0) {
            cmds->DrawObject3D(fbx_skeleton_mesh_,
                               jpov::PBRMaterial::SolidColor(jpov::kColorBlue),
                               /*center*/ {0, 0, 0},
                               /*up*/     {0, 1, 0},
                               /*front*/  {0, 0, 1},
                               /*scale*/  1.0f,
                               /*highlight*/ false,
                               /*picking_id*/ 0);
        }

        // ── 屏幕空间坐标轴标签（2D，不参与 3D 变换）──
        // ⚠️ pos 是**输出帧**（winfo）像素坐标，不是 3D FBO 分辨率 —— 两者在本
        //    gold 里不同（3D=1280×720，输出=640×360），用错会画到画面外。
        const float out_w = winfo.width;
        const float out_h = winfo.height;
        const jpov::Vec2f kMargin{6.0f, 6.0f};
        cmds->DrawText("z", /*pos*/ {kMargin.x(), out_h - kMargin.y()},
                       kAxisLabelFontSize, jpov::kColorRed,
                       jpov::TextAlignment::kBottomLeft, jpov::kFontBuiltinLatin);
        cmds->DrawText("x", /*pos*/ {out_w - kMargin.x(), out_h - kMargin.y()},
                       kAxisLabelFontSize, jpov::kColorRed,
                       jpov::TextAlignment::kBottomRight, jpov::kFontBuiltinLatin);
        cmds->DrawText("y", /*pos*/ {out_w * 0.5f, kMargin.y()},
                       kAxisLabelFontSize, jpov::kColorRed,
                       jpov::TextAlignment::kMidTop, jpov::kFontBuiltinLatin);
    }
};

// 搭建场景（须 app 已 Init()）：浅灰地面板 + 两个资产骨架各自生成骨人 mesh 并注册。
inline void BuildScene(AssetSkeletonGoldApp* app) {
    CHECK(app != nullptr) << "BuildScene: app 不能为空";

    // 地面：一块扁 box（局部 +Z=front, +Y=up, +X=left），用 Draw 的 center 下沉使顶面贴 y=0。
    app->ground_mesh_ = app->RegisterMesh(jpov::MeshData::MakeBox(
        /*front_half_width*/ kGroundHalfSize,
        /*up_half_width*/    kGroundHalfThickness,
        /*left_half_width*/  kGroundHalfSize));

    // ---- skeleton from glb（红色）----
    std::vector<jpov::SkeletonType> glb_skels;
    CHECK(jpov::LoadGltfSkeleton(GlbSkeletonPath(), &glb_skels))
        << "LoadGltfSkeleton 失败: " << GlbSkeletonPath();
    CHECK(!glb_skels.empty()) << "glb 资产应含至少一个 skin";
    const jpov::SkeletonType& glb_skel = glb_skels[0];
    const jpov::SkeletonPose glb_pose =
        jpov::SkeletonPose::Identity(glb_skel.bone_count());
    const jpov::MeshData glb_bone_mesh =
        jpov::BuildBoneMeshInBoneSpace(glb_skel, glb_pose, kBoneRadius);
    app->glb_skeleton_mesh_ = app->RegisterMesh(glb_bone_mesh);
    LOG(INFO) << "glb 骨人: bones=" << glb_skel.bone_count()
              << " verts=" << glb_bone_mesh.positions.size();

    // ---- skeleton from fbx（蓝色）----
    jpov::SkeletonType fbx_skel;
    CHECK(jpov::LoadFbxSkeleton(FbxSkeletonPath(), &fbx_skel))
        << "LoadFbxSkeleton 失败: " << FbxSkeletonPath();
    const jpov::SkeletonPose fbx_pose =
        jpov::SkeletonPose::Identity(fbx_skel.bone_count());
    const jpov::MeshData fbx_bone_mesh =
        jpov::BuildBoneMeshInBoneSpace(fbx_skel, fbx_pose, kBoneRadius);
    app->fbx_skeleton_mesh_ = app->RegisterMesh(fbx_bone_mesh);
    LOG(INFO) << "fbx 骨人: bones=" << fbx_skel.bone_count()
              << " verts=" << fbx_bone_mesh.positions.size();
}

}  // namespace jpov_asset_skeleton_gold

#endif  // JPOV_TEST_SKELETON_JPOV_ASSET_SKELETON_GOLD_COMMON_H_
