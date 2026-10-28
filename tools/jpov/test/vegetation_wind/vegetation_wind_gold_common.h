// 植被风（单关节）gold —— generator 与 test 共用的同一份场景。
//
// 保护什么：**顶点风**在两条绘制路径上的行为必须一致且稳定
//   · instance pass：`DrawInstancedObject(..., wind)`（一 draw 画 N 份，逐实例吹倒）
//   · object3d 路径：`DrawObject3D(..., wind)` / `DrawGltfObject`
// 两条路径共用同一份 VS 风函数语义（单关节刚性链：joint_id + phase），
// 任何一侧的公式/属性布局改错，出图就会和 gold 分道扬镳。
//
// 场景（y-up，右手系）：
//   · 地面：大扁 box，顶面 y=0（阴影落上去，能看出"树动、影也动"）。
//   · 太阳：斜上方平行光（开阴影）+ 弱 ambient。
//   · 三棵树（instance 路径）/ 一棵树（object3d 路径）：都是同一份 v2 烘焙产物。
//
// 为什么黄金图里没有叶子：叶卡是 alpha cutout + 覆盖率 mip，llvmpipe 下 ROI 均值会跳
// 0~22（见 test/vegetation_v2/vegetation_v2_gold_common.h 的实测），当不了逐像素基准；
// 本测试只关心骨架形变，烘叶子纯属噪声源，所以关掉（`leaf_png` 留空）。
//
// 资产：用 v2 素材当**输入**现场烘一份 glb（无叶子、纯色树干）。烘焙没有随机源，
// 同一份 shape 的产物逐字节稳定，所以 generator 与 test 各烘各的不影响比对。

#ifndef JPOV_TEST_VEGETATION_WIND_VEGETATION_WIND_GOLD_COMMON_H_
#define JPOV_TEST_VEGETATION_WIND_VEGETATION_WIND_GOLD_COMMON_H_

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/common/utils.h"
#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/gltf_object.h"
#include "tools/jpov/tools/vegetation_tool_v2/bake/vegetation_glb_baker.h"
#include "tools/jpov/tools/vegetation_tool_v2/shape.h"

namespace vegwind {

// 一张被测图的配置。
struct Shot {
    // true  = instance pass（DrawInstancedObject，三份实例，一次 instanced draw）
    // false = object3d 路径（DrawObject3D，单棵树）
    bool instanced = true;
    // false = 不下发 WindCommand（同一相机/几何的对照组，用来证明风**确实**在动）。
    bool wind_on = true;
    // 吹倒比例：> 0 时才有「按路径长 L 的平移」这一项。
    //   instance 路径：逐实例（三份实例给不同比例，顺带守住"逐实例独立"）
    //   object3d 路径：逐命令（WindCommand::blow_ratio）
    float blow_ratio = 0.0f;
};

// 钉死时间的风场（gold 必须可复现，时间不能由 wall-clock / 帧号推）。
//
// 数值取 v2 查看器默认的一组（肉眼已验收）：振幅 0.1167、基频 0.5Hz、湍流 0.04；
// 时间固定 1.7s（相位落在摆幅较大处，形变足够明显）。
// 风向给斜前方 (−0.6, 0, 0.8)：既不与任一坐标轴平行，三条实例的朝向又各不相同，
// 能同时守住「世界风向 → 各实例模型系」这一步逆变换。
// ref_length = 树的模型系包围盒高（吹倒位移按 L/H 归一化）。
inline jpov::WindCommand MakeWind(float blow_ratio, float ref_length) {
    jpov::WindCommand w;
    w.time = 1.7f;
    w.amplitude = 0.1167f;
    w.frequency = 0.5f;
    w.direction = {-0.6f, 0.0f, 0.8f};
    w.turb_amplitude = 0.04f;
    w.turb_frequency = 2.5f;
    w.gust_frequency = 0.6f;
    w.gust_strength = 0.7f;
    w.blow_ratio = blow_ratio;          // 仅 object3d 路径读这里
    w.blow_ref_length = ref_length;
    w.blow_away_exponent = 2.0f;
    return w;
}

// 一张 gold 图的名字 + 它的 shot。
struct NamedShot {
    const char* name;   // gold 文件名（不含 .png），落在本目录下
    Shot shot;
};

// 全部被测图 —— **generator 与 test 共用这一份**（避免两边场景漂移）。
// 顺序 = 生成顺序；文件名带 `_1280x720` 与本仓库既有 gold 命名一致。
inline std::vector<NamedShot> AllShots() {
    return {
        // instance pass：三份实例、只吹风（不吹倒）── 守「单关节刚性链」主体
        {"vegetation_wind_instance_1280x720",
         Shot{.instanced = true, .wind_on = true, .blow_ratio = 0.0f}},
        // instance pass：同上 + 逐实例吹倒 0 / 0.05 / 0.10 ── 守「按路径长 L 的平移」
        {"vegetation_wind_instance_blow_1280x720",
         Shot{.instanced = true, .wind_on = true, .blow_ratio = 0.05f}},
        // object3d 路径：单棵树、只吹风 ── 守另一条 VS 的等价性
        {"vegetation_wind_object3d_1280x720",
         Shot{.instanced = false, .wind_on = true, .blow_ratio = 0.0f}},
    };
}

// bazel runfiles 根（`$TEST_SRCDIR/__main__`），本地 `bazel run` 时退化成仓库根。
inline std::string RunfilesRoot() {
    const char* srcdir = std::getenv("TEST_SRCDIR");
    if (srcdir != nullptr) {
        std::string p = srcdir;
        if (!p.empty() && p.back() != '/') {
            p.push_back('/');
        }
        return p + "__main__/";
    }
    return jpov::GetProjectRoot();
}

// v2 素材目录（shape / leaf / bark 都在这里）。
inline std::string SampleAssetDir() {
    return RunfilesRoot() + "tools/jpov/tools/vegetation_tool_v2";
}

// 从 v2 素材烘一份只有树干的 .glb（带关节表 + 逐顶点 (joint_id, phase)）。
//
// Pre-condition: `sample_dir` 下有 samples/willow.vegshape（二进制资产，PR #224 起
// 仓库不再放文本 .pbtxt）；`glb_path` 的父目录已存在。
// 返回 false 时 *error 写原因（不 crash：调用方自己决定怎么报）。
inline bool BakeSamplePlant(const std::string& sample_dir, const std::string& glb_path,
                           std::string* error) {
    jpov::vegetation::Shape shape;
    if (!jpov::vegetation::LoadShapeFile(sample_dir + "/samples/willow.vegshape", &shape,
                                         error)) {
        return false;
    }
    jpov::vegetation::GlbBakeOptions opts;   // 默认 = 查看器默认；leaf_png 留空 = 不烘叶子
    jpov::vegetation::GlbBakeStats stats;
    if (!jpov::vegetation::BakeVegetationGlb(shape, glb_path, opts, &stats, error)) {
        return false;
    }
    LOG(INFO) << "风 gold 资产烘焙完成: joints=" << stats.joints
              << " trunk_vertices=" << stats.trunk_vertices
              << " triangles=" << stats.triangles
              << " path=" << glb_path;
    return true;
}

// 被测场景：地面 + 太阳 + 一棵/三棵树。
//
// 用法（generator / test 相同）：
//   WindSceneApp app(cfg);
//   app.Init();
//   app.ground_mesh_ = app.RegisterMesh(MeshData::MakeBox(60, 0.1, 60));
//   app.LoadPlant(glb_path);          // 内部记下包围盒，相机/摆位据此算
//   app.SetShot(shot);
//   app.RunOnce(input, winfo, png_path);
class WindSceneApp : public JPOV {
public:
    using JPOV::JPOV;

    // 地面（MakeBox 扁盒，纯白）
    uint32_t ground_mesh_ = 0;
    // 烘焙产物（独占 GPU 资源；析构前必须 ReleasePlant()）
    jpov::GltfObject plant_;

    void SetShot(const Shot& shot) { shot_ = shot; }

    // 载入烘焙产物并记下包围盒（相机取景 / 贴地 / 吹倒参考长度都用它）。
    // 返回 false = 读不到或空对象（调用方负责报错）。
    bool LoadPlant(const std::string& glb_path) {
        ReleasePlant();
        plant_ = LoadGltf(glb_path);
        if (plant_.empty()) {
            LOG(ERROR) << "LoadPlant: glb 载入失败或为空: " << glb_path;
            return false;
        }
        height_ = 1.0f;
        if (plant_.bounds_valid) {
            height_ = plant_.bounds_max[1] - plant_.bounds_min[1];
            base_y_ = -plant_.bounds_min[1];   // 把模型最低点摆到 y=0
        }
        if (height_ < 1.0e-3f) {
            LOG(WARNING) << "LoadPlant: 包围盒高≈0，退回 1m（相机/吹倒参考长度会不准）";
            height_ = 1.0f;
        }
        LOG(INFO) << "LoadPlant: primitives=" << plant_.size() << " height=" << height_;
        return true;
    }

    // 释放 glb 的 GPU 资源。**必须在 Finalize() 之前调**（之后 GL 上下文已销毁）。
    void ReleasePlant() {
        if (!plant_.empty()) {
            ReleaseGltf(plant_);
            plant_ = jpov::GltfObject();
        }
    }

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        CHECK(cmds != nullptr);

        // 3D FBO 尺寸跟窗口走（gold 都是 1280x720；写成常量会让"换个窗口跑"静默出歪比例）。
        cmds->camera.fbo_3d_width_ = winfo.width;
        cmds->camera.fbo_3d_height_ = winfo.height;
        // 相机按树高取景：前方略俯，能同时看到树冠形变与地面投影。
        cmds->camera.position = {0.0f, 0.55f * height_, 1.75f * height_};
        cmds->camera.target = {0.0f, 0.50f * height_, 0.0f};
        cmds->camera.up = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov = 60.0f;
        cmds->camera.near = 0.05f;
        cmds->camera.far = 200.0f;

        cmds->sun = jpov::DirectionalLight{
            /*direction*/ {-0.45f, -1.0f, -0.35f},
            /*color*/     {1.0f, 1.0f, 1.0f, 1.0f},
            /*intensity*/ 3.0f,
        };
        cmds->ambient = jpov::AmbientLight{
            .color = {1.0f, 1.0f, 1.0f, 1.0f},
            .intensity = 0.35f,
        };
        cmds->tone_mapping = true;

        // 地面：扁 box，顶面贴 y=0。
        cmds->DrawObject3D(ground_mesh_, jpov::PBRMaterial::SolidColor(jpov::kColorWhite),
                           /*center*/ {0.0f, -0.1f, 0.0f},
                           /*up*/     {0.0f, 1.0f, 0.0f},
                           /*front*/  {0.0f, 0.0f, 1.0f});

        const std::optional<jpov::WindCommand> wind =
            shot_.wind_on ? std::optional<jpov::WindCommand>(
                                MakeWind(shot_.blow_ratio, height_))
                          : std::nullopt;

        if (shot_.instanced) {
            const std::vector<jpov::InstanceState> instances = MakeInstances();
            for (const jpov::GltfPrimitive& prim : plant_.primitives) {
                cmds->DrawInstancedObject(prim.mesh_id, prim.material, instances,
                                          /*highlight*/ false, wind);
            }
            return;
        }

        // object3d 路径：单棵树摆在正中。
        for (const jpov::GltfPrimitive& prim : plant_.primitives) {
            cmds->DrawObject3D(prim.mesh_id, prim.material,
                               /*center*/ {0.0f, base_y_, 0.0f},
                               /*up*/     {0.0f, 1.0f, 0.0f},
                               /*front*/  {0.0f, 0.0f, 1.0f},
                               /*scale*/  1.0f,
                               /*highlight*/ false,
                               /*picking_id*/ 0,
                               wind);
        }
    }

private:
    // 三份实例：沿 X 排开、绕 Y 各转一个角度（黄金角 ⇒ 互不相同的模型系朝向）。
    // 吹倒比例逐实例递增（0 / 1× / 2×）：既守住"逐实例独立"，也让 L 项在图上可辨。
    std::vector<jpov::InstanceState> MakeInstances() const {
        const float kSpacing = 0.95f * height_;
        std::vector<jpov::InstanceState> instances;
        instances.reserve(3);
        for (int k = 0; k < 3; ++k) {
            const float yaw = static_cast<float>(k) * 2.39996323f;   // 黄金角
            jpov::InstanceState st;
            st.transform.center = {static_cast<float>(k - 1) * kSpacing, base_y_, 0.0f};
            st.transform.up = {0.0f, 1.0f, 0.0f};
            st.transform.front = {std::sin(yaw), 0.0f, std::cos(yaw)};
            st.transform.scale = 1.0f;
            st.blow_away_ratio = shot_.blow_ratio * static_cast<float>(k);
            instances.push_back(st);
        }
        return instances;
    }

    Shot shot_;
    float height_ = 1.0f;   // 模型系包围盒高（米）
    float base_y_ = 0.0f;   // 把模型最低点抬到 y=0 所需的平移
};

}  // namespace vegwind

#endif  // JPOV_TEST_VEGETATION_WIND_VEGETATION_WIND_GOLD_COMMON_H_
