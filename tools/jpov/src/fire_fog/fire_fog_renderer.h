// JPOV FireFogRenderer — 雾火（体积雾 + 火）统一子渲染器
//
// 与 object3d / horizon_fog 平级的**单一自包含**渲染器。它消费 RenderCommandList 里的
// 点状雾体（PointFog），把一切效果降维成**统一的团**（FogBody），做屏幕 tile 剪枝 +
// 逐像素 ZDist 累加 + 降维到 ≤8 + 末端积分，**就地**合成到调用方当前绑定的 3D HDR FBO
//（与 HorizonFogRenderer 同为「其它 3D 之后、HDR 后处理之前」）。
//
// 设计见 tools/jpov/docs/jpov_fire_fog_design.md §3（ZDist）/ §14（屏幕空间高斯）。
// GLSL 见 fire_fog_shader.h；数据模型见 fog_body.h；GL-free 工具见 fire_fog_lower.h；
// ZDist 纹理布局见 zdist_texture_layout.h。
//
// 三趟管线（2026-10-09 Danis 定「简单加权」版）：ZDist 全部在**低分辨率**域（÷N，可配）：
//   A 累加（fire_fog_zdist）  低分辨率：tile 剪枝 + 逐团 M 段采样 → ZDist 累加 → 降维 ≤8
//                            → 打包成 6 个 RGBA32F（ZDist 纹理组）。
//   B 合并（fire_fog_gauss）  低分辨率：屏幕空间深度敏感高斯（权重 g·ρ，非重叠段 Z0 补全，
//                            标量分母）→ 降维 ≤8 → 打包（ping-pong 目标）。
//   C 合成（fire_fog_compose）主分辨率：双线性重建 ZDist → 场景深度裁剪 → 末端积分 →
//                            就地混合到调用方 3D HDR FBO（GL_ONE / GL_SRC_ALPHA）。
//
// 其余 MVP 约定：只有点状雾（kAnalyticProfile）；L_in 不采样任何光源，用常量发射色；
// M=2 段/团，K=8 团/tile，tile=16。开关/参数见 FireFogParams。

#ifndef JPOV_SRC_FIRE_FOG_FIRE_FOG_RENDERER_H_
#define JPOV_SRC_FIRE_FOG_FIRE_FOG_RENDERER_H_

#include <cstdint>
#include <string>
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
    static constexpr int kZDistTexelsPerPixel = 6;    // ZDist 每像素 RGBA32F texel 数（见 zdist_texture_layout.h）
    static constexpr int kMaxDownsample = 8;          // ZDist 降采样倍数上限
    static constexpr int kMaxKernelRadius = 3;        // 高斯核半径上限（低分辨率像素）

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

    // 全屏雾火管线（见头文件顶部的三趟说明）：趟 A/B 在自身低分辨率 FBO 上跑，趟 C 回到
    // 调用方当前绑定的 3D HDR FBO **就地**混合（靠 GL_ONE/GL_SRC_ALPHA 与帧缓冲已有颜色合成）。
    //   fogs            : 本帧的点状雾体（命令层 PointFog）。
    //   cam             : 相机（取世界位置做光线原点）。
    //   view_proj       : Proj*View（列主序 16 float）。
    //   viewport_w/h    : 当前 3D FBO 像素尺寸（低分辨率 = 其 ÷ params.downsample）。
    //   scene_depth_tex : MRT#1 场景深度（R32F，单采样），供 z 裁剪；0 = 不裁剪（退化为远平面）。
    //   params          : 高斯核 / 降采样倍数 / 抖动开关（见 FireFogParams）。
    // Pre-conditions: Init 已调用；调用方已绑定 HDR FBO 并设好 viewport；场景深度纹理
    //                 **不**是当前 draw FBO 的附件（否则采样↔写入反馈环）。
    // 说明：fogs 为空时零开销直接返回；本函数会自行保存/复原调用方的 FBO 绑定与 viewport。
    void Draw(const std::vector<PointFog>& fogs,
              const Camera& cam,
              const float view_proj[16],
              int viewport_w,
              int viewport_h,
              unsigned int scene_depth_tex,
              const FireFogParams& params);

private:
    // 按 (low_w, low_h) 确保 tile 索引纹理存在且尺寸匹配；变化则重建（低分辨率域）。
    void EnsureTileTexture(int low_w, int low_h);

    // 按 (low_w, low_h) 确保两组 ZDist FBO 存在且尺寸匹配（各 6 个 RGBA32F 附件）。
    void EnsureZDistTargets(int low_w, int low_h);

    ShaderManager* shader_mgr_ = nullptr;

    // 三趟的 program（Init 编译；归 ShaderManager 释放）。
    unsigned int prog_zdst_ = 0;      // 趟 A：ZDist 累加
    unsigned int prog_gauss_ = 0;     // 趟 B：高斯合并
    unsigned int prog_compose_ = 0;   // 趟 C：合成
    std::string fs_zdst_;             // 组装后的 FS 源码（须存活到编译完成）
    std::string fs_gauss_;
    std::string fs_compose_;

    // ZDist 低分辨率目标（双缓冲：A→[0]，B→[1]，C 读 [1] 或 [0]）。
    unsigned int zdist_fbo_[2] = {0, 0};
    unsigned int zdist_tex_[2][kZDistTexelsPerPixel] = {{0}};
    int zdist_w_ = 0;
    int zdist_h_ = 0;

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
