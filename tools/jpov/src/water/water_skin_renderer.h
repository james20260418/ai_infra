// JPOV WaterSkinRenderer —— 无限水平平面的「水皮」层（WBO 覆盖集 + over 合成）
//
// 定位（Danis 2026-11-15 定）：
//   水皮只画**界面**：菲涅尔反射 + 透射（水上）/ Snell 窗 + 全内反射（水下）。
//   所以本 pass 输出一层带代表深度的 WBO 覆盖集，而不是把颜色直接拌进画面：
//     accum = 反射色 × F、reveal 槽写入 α = F（混合后纹理里是 1 − F）、depth(att3) = z_view × F。
//   将来雾接入时，这一层连同 att3 一起喂给 FogComposite；本版先自己 Composite。
//
// 画法：全屏三角形 + FS 里解析求交（见 water_skin_shader.h 顶部为什么不用网格）。
// 不采样场景颜色（背景由合成时的 dst 提供），也不需要抓帧副本。
//
// 链路位置：粒子/体积火/条带之后、froxel 之前（见 renderer.cc）。

#ifndef JPOV_SRC_WATER_WATER_SKIN_RENDERER_H_
#define JPOV_SRC_WATER_WATER_SKIN_RENDERER_H_

#include "tools/jpov/interface/camera.h"
#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/water/water_skin_math.h"
#include "tools/jpov/src/wbo/wbo_target.h"

namespace jpov {

class WaterSkinRenderer {
public:
    // 一次水面 pass 的全部输入（POD，值语义，便于 GPU 测试直接构造）。
    struct Inputs {
        Camera camera;                  // 位置/朝向/近远/fov（fbo_3d_* 需已填好）
        int full_w = 0;                 // 3D FBO 宽（= 水面 pass 的目标分辨率）
        int full_h = 0;
        unsigned int scene_depth_tex = 0;   // MRT#1 场景深度（R32F，1.0 = 背景）
        unsigned int env_cube_tex = 0;      // 天光探针（SkyOutput::kEnvProbe 渲的 cubemap）
        float env_scale = 1.0f;             // 天光反射消费量（= EnvSpecular::sky_scale）
        Vec3f tir_color = {0.0f, 0.0f, 0.0f};  // 水下全内反射色（= SkyCommand::ground_color）
        float level_y = 0.0f;               // 水面世界高度（米）
        float ior = water::kWaterIorDefault;  // 水的折射率（> 0）
    };

    WaterSkinRenderer();
    ~WaterSkinRenderer();

    WaterSkinRenderer(const WaterSkinRenderer&) = delete;
    WaterSkinRenderer& operator=(const WaterSkinRenderer&) = delete;

    // Pre-condition: shader_mgr != nullptr；GL context 已激活。
    void Init(ShaderManager* shader_mgr);

    // 释放 WBO 附件 / VAO。可重复调用（须在 GL context 存活时）。
    void Finalize();

    // 建/清本帧的 WBO 目标（尺寸变化会重建）。不画东西。
    // Pre-condition: 已 Init；full_w/full_h > 0。
    void Begin(int full_w, int full_h);

    // 画水面到 WBO 目标。
    // Pre-condition: Begin 已调用；inputs.scene_depth_tex / env_cube_tex 非 0；
    //               在 Begin 与 Draw 之间相机不改变；camera.fbo_3d_* 与 full_w/h 一致。
    void Draw(const Inputs& inputs);

    // 把 WBO 覆盖集 over 合成到**当前绑定的 FBO**（调用方负责绑定 + 设置
    // glDrawBuffers 只留 COLOR_ATTACHMENT0）。没有内容时不画。
    void Composite(int full_w, int full_h);

    // 本帧这一层的三张附件（供下一步交给 FogComposite 使用）。
    // 未 Begin / 未 Ensure 时返回 0。
    unsigned int accum_tex() const { return wbo_.accum_tex(); }
    unsigned int reveal_tex() const { return wbo_.reveal_tex(); }
    unsigned int depth_tex() const { return wbo_.depth_tex(); }

private:
    void EnsureTarget(int w, int h);
    void UploadCommonUniforms(unsigned int prog, const Inputs& inputs);

    ShaderManager* shader_mgr_ = nullptr;
    unsigned int prog_draw_ = 0;
    unsigned int prog_comp_ = 0;
    unsigned int vao_ = 0;
    WboTarget wbo_;
    bool has_content_ = false;
    bool initialized_ = false;
};

}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_SKIN_RENDERER_H_
