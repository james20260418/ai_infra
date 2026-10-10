// JPOV 静态批量实例（DrawInstancedObject / InstancedObjectCommand）gold —— 共享场景构建
//
// 验证「同一 mesh 摆 N 份 = 一次 instanced draw」这条新路径，并与 Object3D 对齐：
//   - 逐实例摆放（center / up / front / scale）各自独立；
//   - 复用同一条 PBR + alpha cutout（MASK）+ 双面渲染链路（材质来自被载入的 glTF）；
//   - 阴影 pass 的实例化（instanced shadow）。
//
// 场景（y-up，右手系）：
//   - 大片地面：大 box（MakeBox），纯白，顶面贴 y=0。
//   - 太阳：斜上方平行光，给地面/裙子投影子。
//   - 3 份 lace_skirt 实例：沿 X 一字排开，各带不同 Y 轴朝向 + 缩放
//     ⇒ 一眼看出「同 mesh 摆了 3 份、各摆各的」，且镂空（投影透空）。
//
// 本头文件被 generator / test 共用，保证两边场景几何/光照/摆放逐字一致。
// 被实例化的网格 + 材质由调用方（generator/test）载入 glb 后填入。

#ifndef JPOV_TEST_OBJECT3D_JPOV_INSTANCED_OBJECT_GOLD_COMMON_H_
#define JPOV_TEST_OBJECT3D_JPOV_INSTANCED_OBJECT_GOLD_COMMON_H_

#include <vector>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/gltf_object.h"

namespace jpov_instanced_object {

// 3 份实例的摆放：沿 X 一字排开，绕 Y 各转不同角度，中间那份放大。
// 三份 center 的 y 抬高，使裙子底摆大致落在 y=0（裙子局部 Y 约 [0.08, 0.86]）。
inline std::vector<jpov::InstanceState> MakeInstances() {
    jpov::InstanceState left;
    left.transform.center = {-0.85f, -0.08f, 0.0f};
    left.transform.up     = {0.0f, 1.0f, 0.0f};
    left.transform.front  = {0.0f, 0.0f, 1.0f};   // 正面朝 +Z
    left.transform.scale  = 1.0f;

    jpov::InstanceState mid;
    mid.transform.center = {0.0f, -0.08f, 0.0f};
    mid.transform.up     = {0.0f, 1.0f, 0.0f};
    mid.transform.front  = {1.0f, 0.0f, 0.0f};    // 绕 Y 转 90°
    mid.transform.scale  = 1.5f;                  // 放大，区别于另两份

    jpov::InstanceState right;
    right.transform.center = {0.95f, -0.08f, 0.0f};
    right.transform.up     = {0.0f, 1.0f, 0.0f};
    right.transform.front  = {0.7071f, 0.0f, 0.7071f};  // 绕 Y 转 45°
    right.transform.scale  = 1.0f;

    return {left, mid, right};
}

class InstancedObjectApp : public JPOV {
public:
    using JPOV::JPOV;

    uint32_t ground_mesh_ = 0;             // 地面 quad（MakeBox 扁盒）
    uint32_t cloth_mesh_ = 0;              // 被实例化的静态网格（lace_skirt 单 primitive）
    jpov::PBRMaterial cloth_material_;     // 该网格的材质（含 cutout/doubleSided）

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count; (void)input; (void)winfo;

        const float kResW = 1280.0f;
        const float kResH = 720.0f;
        cmds->camera.fbo_3d_width_  = kResW;
        cmds->camera.fbo_3d_height_ = kResH;

        // 相机：前方略俯，三份实例都在画面里。
        cmds->camera.position = {0.2f, 1.7f, 4.0f};
        cmds->camera.target   = {0.0f, 0.45f, 0.0f};
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};
        cmds->camera.near     = 0.05f;
        cmds->camera.far      = 1000.0f;

        // 太阳：左前上斜照，投影落到地面（偏右后方）。
        cmds->sun = jpov::DirectionalLight{
            /*direction*/ {-0.55f, -1.0f, -0.35f},
            /*color*/     {1.0f, 1.0f, 1.0f, 1.0f},
            /*intensity*/ 3.0f,
        };
        cmds->ambient = jpov::AmbientLight{
            .color = {1.0f, 1.0f, 1.0f, 1.0f},
            .intensity = 0.35f,
        };
        cmds->tone_mapping = true;

        // 地面：扁 box，白色，顶面贴 y=0。
        cmds->DrawObject3D(
            ground_mesh_,
            jpov::PBRMaterial::SolidColor(jpov::kColorWhite),
            /*center*/ {0.0f, -0.1f, 0.0f},
            /*up*/     {0.0f, 1.0f, 0.0f},
            /*front*/  {0.0f, 0.0f, 1.0f});

        // 3 份实例：一次 DrawInstancedObject = 一次 instanced draw。
        cmds->DrawInstancedObject(cloth_mesh_, cloth_material_, MakeInstances());
    }
};

}  // namespace jpov_instanced_object

#endif  // JPOV_TEST_OBJECT3D_JPOV_INSTANCED_OBJECT_GOLD_COMMON_H_
