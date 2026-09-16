// JPOV skeleton gold 公共场景（M1：蒙皮 T-rest 蓝人 + A/B object3d 对照）
//
// generator(写仓库 gold PNG) 与 test(渲一帧与 gold 逐像素 diff) 共用同一场景，
// 保证两者帧完全一致 —— M1 验证：左=真蒙皮(bind pose)渲, 右=object3d 直画同 mesh+材质。
// 若蒙皮链路正确(bind 下 M_i=I), 两人渲染应几乎相同 → 像素级门禁。
#ifndef JPOV_TEST_SKELETON_JPOV_SKELETON_GOLD_COMMON_H_
#define JPOV_TEST_SKELETON_JPOV_SKELETON_GOLD_COMMON_H_

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "glog/logging.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/gltf_object.h"
#include "tools/jpov/src/gltf_loader.h"

namespace jpov_skeleton_gold {

inline std::string GetGoldRelPath() { return "/skeleton/human_skeleton_gold_t_rest_1280x720.png"; }

// test/generator 输出帧格式常量（与 RunOnce winfo 一致：640x360）。
constexpr int kOutW = 640;
constexpr int kOutH = 360;

// 蒙皮渲染场景 app：把「同一画布」画成两人(左 skinned, 右 object3d) + sunny-day base。
class SkeletonGoldApp : public JPOV {
public:
    using JPOV::JPOV;

    struct Slot { jpov::GltfObject obj; jpov::Vec3f center, up, front; };
    std::vector<Slot> slots_;   // 地面 tiles + 人物

    uint32_t mesh_id_ = 0;   // 蓝人 mesh(mixamo_male) 已注册
    uint32_t skel_id_ = 0;   // 蓝人骨架 SkeletonManager id
    jpov::PBRMaterial mat_;  // 蓝人材质(baseColor 贴图等)

    // 多帧 pose 集（下标 = 该骨架 atlas 里的 pose 序号）。第 0 个 = identity（gold 用的
    //   静态退化门基准，与改动前逐字节一致）；其余帧给若干骨明显旋转，供多帧一致性
    //   测试（jpov_skinned_multipose_test）覆盖 atlas 的**多行**取址。
    //   注意：gold(generator/test) 只用 poses_[0]，故 gold 图不受此处新增帧影响。
    std::vector<jpov::SkeletonPose> poses_;
    // 多帧一致性测试用：本帧蒙皮人用 poses_ 的哪一帧（默认 0 = 与 gold 完全一致）。
    int pose_sel_ = 0;
    // 多帧一致性测试用：左人走哪条渲染路径（默认 kSkinned = 与 gold 语义一致）。
    enum class DrawWhich { kSkinned, kDirect, kCpuSkinned };
    DrawWhich draw_which_ = DrawWhich::kSkinned;
    // CPU 真值通道：glTF 原始网格（带 joints/weights）+ 同源骨架 + 其 mesh 句柄。
    uint32_t cpu_mesh_id_ = 0;
    jpov::MeshData rest_mesh_;
    jpov::SkeletonType raw_skel_;

    void AddSlot(jpov::GltfObject obj, jpov::Vec3f c, jpov::Vec3f u, jpov::Vec3f f) {
        slots_.push_back({std::move(obj), c, u, f});
    }

    void OneIteration(int64_t, const jpov::InputSnapshot&,
                      const jpov::WindowInfo&, jpov::RenderCommandList* cmds) override {
        constexpr float kResW = 1280.0f, kResH = 720.0f;
        cmds->camera.fbo_3d_width_  = kResW;
        cmds->camera.fbo_3d_height_ = kResH;
        const jpov::Vec3f center{0.0f, 1.0f, 0.0f};
        cmds->camera.position = {4.0f, 2.0f, 4.0f};
        cmds->camera.target   = center;
        cmds->camera.near     = 0.05f;

        // sunny-day：太阳(斜上) + 天光(Preetham) + 环境光 + tone_mapping。
        const jpov::Vec3f sun_l = {0.0f, -1.0f, -1.0f};
        cmds->sun = jpov::DirectionalLight{{sun_l}, {1,1,1,1}, 3.0f};
        cmds->ambient = jpov::AmbientLight{.color = {1,1,1,1}, .intensity = 0.3f};
        cmds->sky = jpov::DaySkyCommand{
            jpov::Vec3f(-sun_l.x(), -sun_l.y(), -sun_l.z()),
            2.0f, {1,1,1,1}, 1.0f, {0.05f,0.06f,0.08f,1}, 0.02, 1e3, 1.0};
        cmds->tone_mapping = true;

        // 地面（DrawGltfObject → object3d；让照明/tile 设置跑）+ 右 object3d 人。
        for (const Slot& s : slots_) cmds->DrawGltfObject(s.obj, s.center, s.up, s.front);
        // 右：object3d 直画同 mesh+材质（rest T-pose, 非蒙皮 —— 对照基准）
        if (mesh_id_ != 0)
            cmds->DrawObject3D(mesh_id_, mat_,
                               /*center*/ {2.0f,0,0}, /*up*/ {0,1,0}, /*front*/ {0,0,1},
                               /*scale*/ 4.0f, /*highlight*/ false, /*picking_id*/ 0);
        // 左：按 draw_which_ 选「真蒙皮」或「CPU 真值蒙皮」（两者必须同形）—— 两条路径
        //   都画在同一处（center/up/front/scale 完全一致），故可逐像素比对。
        //   kDirect 时左人也走右人的直画路径（保持旧 gold 语义不变）。
        //   注意 pose_a == pose_b == pose_sel_：单帧静态采样，ratio 无关。
        const bool left_skinned =
            (draw_which_ == DrawWhich::kSkinned || draw_which_ == DrawWhich::kDirect);
        if (left_skinned) {
            if (mesh_id_ != 0 && skel_id_ != 0) {
                jpov::SkinnedInstanceState inst;
                inst.center = {0,0,0}; inst.up = {0,1,0}; inst.front = {0,0,1};
                inst.scale = 4.0f; inst.pose_a = pose_sel_; inst.pose_b = pose_sel_;
                inst.ratio = 0.0f;
                std::vector<jpov::SkinnedInstanceState> instances{inst};
                cmds->DrawMeshWithSkeleton(mesh_id_, skel_id_, mat_, std::move(instances));
            }
        } else {  // kCpuSkinned
            if (cpu_mesh_id_ != 0)
                cmds->DrawObject3D(cpu_mesh_id_, mat_,
                                   /*center*/ {0,0,0}, /*up*/ {0,1,0}, /*front*/ {0,0,1},
                                   /*scale*/ 4.0f, /*highlight*/ false, /*picking_id*/ 0);
        }
    }
};

// 构造多帧 pose 集（下标即 pose_a/pose_b）。第 0 帧 = identity（与 gold 的静态退化门一致：
//   bind 姿态下肤矩阵 = I，gold 图逐字节不变），其余帧给若干骨**明显**旋转。
// 帧数刻意 **> 一行能装的 pose 数**（23 骨：pose_per_row = 2048/92 = 22）：这样才有 pose
//   落在 atlas 第二行及以后。atlas 布局的歧义只在跨行情形下暴露 —— 帧数 <= pose_per_row
//   时两种理解等价、测试抓不到。
inline std::vector<jpov::SkeletonPose> MakeMultiPoses(int bone_count) {
    CHECK_GT(bone_count, 4) << "MakeMultiPoses: 骨数太少，无法覆盖多骨旋转";
    constexpr int kNumTurns = 24;
    std::vector<jpov::SkeletonPose> poses;
    poses.reserve(kNumTurns + 1);
    poses.push_back(jpov::SkeletonPose::Identity(bone_count));  // 0: T-pose（基准）
    for (int k = 1; k <= kNumTurns; ++k) {
        jpov::SkeletonPose p = jpov::SkeletonPose::Identity(bone_count);
        const float a[3] = {37.0f * k, -23.0f * k, 61.0f * k};
        for (int b = 1; b <= 3 && b < bone_count; ++b) {
            float deg = std::fmod(a[b - 1], 360.0f);
            if (deg > 180.0f) deg -= 360.0f;
            if (deg <= -180.0f) deg += 360.0f;
            const float half = 0.5f * deg * 3.14159265358979323846f / 180.0f;
            p.joint_rotation[b] = geom::Quaternion<float>(0.0f, 0.0f,
                                                          std::sin(half),
                                                          std::cos(half));
        }
        poses.push_back(std::move(p));
    }
    return poses;
}

// CPU 端「真值」蒙皮：按与 SkeletonManager/shader 完全相同的公式，把 rest 顶点算成该 pose
//   下的骨架空间坐标，产出新 MeshData（只改 positions/normals，拓扑/UV/joints 原样保留）。
//
// 坐标系（2026-09-16 起）：loader **不做任何坐标旋转**（PR #103），网格顶点与骨架都是资产
//   原坐标系、**天然同帧** ⇒ 这里直接相乘，不需要任何映射。
//   （历史：曾需在两侧补一对 (x,-z,y)/(x,z,-y) 互逆映射，那是 loader 单方面旋转顶点造成的；
//     该 bug 修掉后这对补丁反而成为错误的来源，已删。）
//
// 公式（与 skeleton_manager.cc 烘焙 + skinning_shader.h 蒙皮一致）：
//   local(j) = T(rest_offset[j]) · R(bind_rotation[j]) · R(pose.joint_rotation[j])
//   jw(j)    = (根 ? I : jw(parent)) · local(j)
//   skinM(j) = jw(j) · inverseBind(j)      ← 方案甲折入
//   v'       = Σ_i w_i · skinM(j_i) · v    （4-bone）
inline jpov::MeshData SkinMeshOnCpuForTest(const jpov::SkeletonType& type,
                                           const jpov::SkeletonPose& pose,
                                           const jpov::MeshData& rest) {
    using geom::math::Mat4;
    type.Validate();
    const int bone = type.bone_count();
    CHECK_GT(bone, 0);

    const std::vector<std::array<float, 16>> ibm = type.ComputeInverseBind();
    std::vector<Mat4> inv(bone);
    for (int j = 0; j < bone; ++j) {
        for (int k = 0; k < 16; ++k) {
            inv[j].m[k] = ibm[static_cast<size_t>(j)][k];
        }
    }

    std::vector<Mat4> jw_all(bone);
    std::vector<Mat4> skin_m(bone);
    for (int j = 0; j < bone; ++j) {
        const geom::Quaternion<float> bind =
            type.bind_rotation.empty() ? geom::Quaternion<float>::Identity()
                                       : type.bind_rotation[j];
        const geom::Quaternion<float> pr =
            pose.joint_rotation.size() > static_cast<size_t>(j)
                ? pose.joint_rotation[j]
                : geom::Quaternion<float>::Identity();
        const Mat4 local = geom::math::JointLocal(type.joints[j].rest_offset, bind, pr);
        jw_all[j] = (type.joints[j].parent == jpov::kSkeletonNoParent)
                        ? local
                        : geom::math::Mat4Mul(
                              jw_all[static_cast<size_t>(type.joints[j].parent)], local);
        skin_m[j] = geom::math::Mat4Mul(jw_all[j], inv[j]);
    }

    jpov::MeshData out = rest;
    auto xf_pt = [](const Mat4& m, const jpov::Vec3f& v) {
        return jpov::Vec3f(m.m[0] * v.x() + m.m[4] * v.y() + m.m[8] * v.z() + m.m[12],
                           m.m[1] * v.x() + m.m[5] * v.y() + m.m[9] * v.z() + m.m[13],
                           m.m[2] * v.x() + m.m[6] * v.y() + m.m[10] * v.z() + m.m[14]);
    };
    auto xf_dir = [](const Mat4& m, const jpov::Vec3f& v) {
        return jpov::Vec3f(m.m[0] * v.x() + m.m[4] * v.y() + m.m[8] * v.z(),
                           m.m[1] * v.x() + m.m[5] * v.y() + m.m[9] * v.z(),
                           m.m[2] * v.x() + m.m[6] * v.y() + m.m[10] * v.z());
    };
    CHECK_EQ(rest.joint_indices.size(), rest.positions.size());
    CHECK_EQ(rest.joint_weights.size(), rest.positions.size());
    for (size_t i = 0; i < rest.positions.size(); ++i) {
        const jpov::Vec3f vg = rest.positions[i];
        const jpov::Vec3f ng =
            rest.normals.empty() ? jpov::Vec3f{0, 0, 0} : rest.normals[i];
        jpov::Vec3f sp{0, 0, 0}, sn{0, 0, 0};
        float wsum = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const float w = rest.joint_weights[i][static_cast<size_t>(k)];
            if (w <= 0.0f) {
                continue;
            }
            const int j = rest.joint_indices[i][static_cast<size_t>(k)];
            CHECK(j >= 0 && j < bone) << "CPU 蒙皮: joint 下标越界 " << j;
            sp = sp + xf_pt(skin_m[static_cast<size_t>(j)], vg) * w;
            if (!rest.normals.empty()) {
                sn = sn + xf_dir(skin_m[static_cast<size_t>(j)], ng) * w;
            }
            wsum += w;
        }
        if (wsum <= 0.0f) {
            sp = vg;
        }
        out.positions[i] = sp;
        if (!rest.normals.empty()) {
            const float n = std::sqrt(sn.x() * sn.x() + sn.y() * sn.y() + sn.z() * sn.z());
            out.normals[i] = (n > 1e-8f)
                                 ? jpov::Vec3f(sn.x() / n, sn.y() / n, sn.z() / n)
                                 : rest.normals[i];
        }
    }
    return out;
}

// 搭建整个场景（须 app 已 Init()）：地面 tiles + 蒙皮人(A/B 共用 mixamo_male mesh/材质/skeleton)。
inline void BuildScene(SkeletonGoldApp* app, const std::string& scene_assets_dir,
                       const std::string& male_glb) {
    const auto load = [app](const std::string& p) {
        jpov::GltfObject o = app->LoadGltf(p);
        CHECK(!o.empty()) << "LoadGltf failed: " << p;
        return o;
    };
    // 地面 5x5 tiles
    for (int iz = 0; iz < 5; ++iz) {
        for (int ix = 0; ix < 5; ++ix) {
            const float gx = -12.0f + ix * 6.0f, gz = -12.0f + iz * 6.0f;
            const bool brick = (gx < 0.0f);
            auto ground = load(scene_assets_dir + (brick ? "ground/ground_brick.gltf"
                                                         : "ground/ground_dirt.gltf"));
            app->AddSlot(std::move(ground), {gx, -0.5f, gz}, {0,0,1}, {0,-1,0});
        }
    }
    // 蓝人(蒙皮 rest mesh + 材质) + 骨架(bind pose)
    jpov::GltfObject male = load(male_glb);
    CHECK(!male.primitives.empty()) << male_glb << " 应含蒙皮 primitive";
    std::vector<jpov::SkeletonType> skels;
    CHECK(jpov::LoadGltfSkeleton(male_glb, &skels));
    CHECK(!skels.empty()) << "LoadGltfSkeleton 失败";
    // 静态退化门：第 0 帧 = 全恒等 pose（bind 姿态）。
    // （2026-09-11 起 bind 朝向已存在骨架的 bind_rotation 里，不再需要硬编码 bind pose 表；
    //   identity pose 下 jointWorld == JW_bind，×自算 inverse_bind = I。）
    // 第 1..N 帧给若干骨明显旋转，供多帧一致性测试覆盖 atlas 多行。
    app->poses_ = MakeMultiPoses(skels[0].bone_count());
    app->skel_id_ = app->RegisterSkeleton(skels[0], app->poses_);
    app->raw_skel_ = skels[0];
    app->mesh_id_ = male.primitives[0].mesh_id;
    app->mat_ = male.primitives[0].material;
    // CPU 真值通道：把 glTF 原始网格（带 joints/weights）也读一份，注册成独立 mesh；
    //   多帧一致性测试会按选中的 pose 在 CPU 端蒙皮后 UpdateMesh。
    {
        jpov::GltfMaterialInfo mi;
        CHECK(jpov::LoadGltf(male_glb, &app->rest_mesh_, &mi))
            << "CPU 真值通道需要读到原始网格: " << male_glb;
        app->cpu_mesh_id_ = app->RegisterMesh(app->rest_mesh_);
    }
}

}  // namespace jpov_skeleton_gold

#endif  // JPOV_TEST_SKELETON_JPOV_SKELETON_GOLD_COMMON_H_
