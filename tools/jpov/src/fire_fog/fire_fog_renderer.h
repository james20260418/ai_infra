// JPOV FireFogRenderer — 雾火（体积雾 + 火）统一子渲染器
//
// 与 object3d / horizon_fog 平级的**单一自包含**渲染器。它消费 RenderCommandList 里的
// 点状雾体（PointFog），把一切效果降维成**统一的团**（FogBody），做屏幕 tile 剪枝 +
// 逐像素 ZDist 累加 + 降维到 ≤8 + 末端积分，**就地**合成到调用方当前绑定的 3D HDR FBO
//（与 HorizonFogRenderer 同为「其它 3D 之后、HDR 后处理之前」的一次全屏 pass）。
//
// 设计见 tools/jpov/docs/jpov_fire_fog_design.md。GLSL 见 fire_fog_shader.h；
// 数据模型见 fog_body.h；GL-free 的降维/打包工具见 fire_fog_lower.h。
//
// MVP 范围（Danis 2026-10-08 定）：只有点状雾（kAnalyticProfile）；L_in 不采样任何光源，
// 用常量发射色；M=2 段/团，K=8 团/tile，tile=16；用场景深度裁剪；屏幕坐标哈希抖动；
// 二叉最小堆降维。不做屏幕空间深度敏感高斯（L3）、独立雾纹理/合成 pass。

#ifndef JPOV_SRC_FIRE_FOG_FIRE_FOG_RENDERER_H_
#define JPOV_SRC_FIRE_FOG_FIRE_FOG_RENDERER_H_

#include <cstdint>
#include <vector>

#include "tools/jpov/interface/camera.h"
#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/src/fire_fog/fog_body.h"
#include "tools/jpov/src/shader_manager.h"

namespace jpov {

class FireFogRenderer {
public:
    // ── 常量（shader 里同名 #define 与之逐字对应；改一处必须同步）──
    static constexpr int kTileSize = 16;              // tile 边长（像素）
    static constexpr int kMaxFogsPerTile = 8;         // K：每 tile 团上限
    static constexpr int kTexelsPerTile = kMaxFogsPerTile / 4;  // 每 tile 的 RGBA8 texel 数 = 2
    static constexpr int kMaxTotalFogs = 255;         // 全局团上限（uint8 索引，sentinel=255）
    static constexpr uint8_t kFogIndexSentinel = 255; // 空槽哨兵
    static constexpr int kSegmentsPerFog = 2;         // M：每团 z 段数

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

    // 全屏雾火 pass：把 fogs 全部降维成统一团（FogBody）→ 屏幕 tile 剪枝（L1）→ 逐像素
    // ZDist 累加（L2）→ 降维到 ≤8 → 末端积分 →**就地**合成到当前绑定的 3D HDR FBO
    //（不持有自己的 FBO；靠 GL_ONE/GL_SRC_ALPHA 混合与帧缓冲已有颜色合成）。
    //   fogs            : 本帧的点状雾体（命令层 PointFog）。
    //   cam             : 相机（取世界位置做光线原点）。
    //   view_proj       : Proj*View（列主序 16 float）。
    //   viewport_w/h    : 当前 3D FBO 像素尺寸（= 剪枝 tile 网格的依据）。
    //   scene_depth_tex : MRT#1 场景深度（R32F，单采样），供 z 裁剪；0 = 不裁剪（退化为远平面）。
    // Pre-conditions: Init 已调用；调用方已绑定 HDR FBO 并设好 viewport；场景深度纹理
    //                 **不**是当前 draw FBO 的附件（否则采样↔写入反馈环）。
    // 说明：fogs 为空时零开销直接返回。
    void Draw(const std::vector<PointFog>& fogs,
              const Camera& cam,
              const float view_proj[16],
              int viewport_w,
              int viewport_h,
              unsigned int scene_depth_tex);

private:
    // 按 (viewport_w, viewport_h) 确保 tile 索引纹理存在且尺寸匹配；变化则重建。
    void EnsureTileTexture(int viewport_w, int viewport_h);

    ShaderManager* shader_mgr_ = nullptr;

    // tile 索引纹理（RGBA8；宽 = grid_w*kTexelsPerTile，高 = grid_h）。
    unsigned int tile_index_tex_ = 0;
    int tile_tex_w_ = 0;
    int tile_tex_h_ = 0;
    int grid_w_ = 0;
    int grid_h_ = 0;

    // 团属性纹理（RGBA32F；宽 = kMaxTotalFogs*3，高 = 1；每团 3 texel）。
    unsigned int fog_body_tex_ = 0;

    // 每帧把命令层点雾降维后的统一团列表（烘焙到 uFogBodyTex 属性纹理）。
    std::vector<FogBody> bodies_;
};

}  // namespace jpov

#endif  // JPOV_SRC_FIRE_FOG_FIRE_FOG_RENDERER_H_
