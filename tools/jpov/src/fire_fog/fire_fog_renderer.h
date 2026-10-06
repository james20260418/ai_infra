// JPOV FireFogRenderer — 雾火（体积雾 + 火）统一子渲染器
//
// 与 object3d / horizon_fog 平级的**单一自包含**渲染器。它消费 RenderCommandList 里的
// 点状雾体（PointFog），把一切效果降维成**统一的团**（FogBody），做屏幕 tile 剪枝 +
// 逐像素 ZDist 累加 + 末端积分，**就地**合成到调用方当前绑定的 3D HDR FBO
//（与 HorizonFogRenderer 同为「其它 3D 之后、HDR 后处理之前」的一次全屏 pass）。
//
// 设计见 tools/jpov/docs/jpov_fire_fog_design.md。本文件（首个 PR）只搭**头文件与框架**：
// 数据模型见 fog_body.h，本类先落方法签名；pass 实现在设计文档 §10.6 的 M1a。
//
// 明确不做（v1）：3D 体积纹理、froxel、TAA（见设计文档 §7）。

#ifndef JPOV_SRC_FIRE_FOG_FIRE_FOG_RENDERER_H_
#define JPOV_SRC_FIRE_FOG_FIRE_FOG_RENDERER_H_

#include <vector>

#include "tools/jpov/interface/camera.h"
#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/src/fire_fog/fog_body.h"
#include "tools/jpov/src/shader_manager.h"

namespace jpov {

class FireFogRenderer {
public:
    FireFogRenderer() = default;
    ~FireFogRenderer();
    FireFogRenderer(const FireFogRenderer&) = delete;
    FireFogRenderer& operator=(const FireFogRenderer&) = delete;

    // 初始化：编译雾火 shader、建 tile 剪枝资源等。要求 GL context 已激活。
    // Pre-condition: shader_mgr != nullptr
    void Init(ShaderManager* shader_mgr);

    // 释放 Init 申请的全部 GPU 资源（与 Init 对称，可重复调用；shader program 归
    // ShaderManager 统一释放）。
    void Finalize();

    // 全屏雾火 pass：把 fogs 全部降维成统一团（FogBody）→ 屏幕 tile 剪枝 → 逐像素
    // ZDist 累加 → 末端积分 →**就地**合成到当前绑定的 3D HDR FBO（不持有自己的 FBO）。
    //   fogs            : 本帧的点状雾体（命令层 PointFog）。
    //   cam             : 相机（视图/投影；供剪枝与光线重建）。
    //   view_proj       : Proj*View（列主序 16 float）。
    //   viewport_w/h    : 当前 3D FBO 像素尺寸（= 剪枝 tile 网格的依据）。
    //   scene_depth_tex : MRT#1 场景深度（R32F，单采样），用于 z 裁剪；0 = 不裁剪。
    // Pre-conditions: Init 已调用；调用方已绑定 HDR FBO 并设好 viewport；
    //                 tone_mapping=true（线性 HDR 域，否则 Renderer 侧 CHECK）。
    // 说明：fogs 为空时零开销直接返回。
    // ⚠️ 本 PR 为骨架：Draw 暂为空实现（不产生任何绘制），M1a 落地真正的 pass。
    void Draw(const std::vector<PointFog>& fogs,
              const Camera& cam,
              const float view_proj[16],
              int viewport_w,
              int viewport_h,
              unsigned int scene_depth_tex);

private:
    ShaderManager* shader_mgr_ = nullptr;

    // 每帧把命令层点雾降维后的统一团列表（tile 剪枝与 pass 的输入）。M1a 起使用。
    std::vector<FogBody> bodies_;
};

}  // namespace jpov

#endif  // JPOV_SRC_FIRE_FOG_FIRE_FOG_RENDERER_H_
