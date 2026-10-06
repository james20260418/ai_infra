// JPOV FireFogRenderer — 雾火（体积雾 + 火）统一子渲染器
//
// 与 object3d / horizon_fog 平级的**单一自包含**渲染器。它消费 RenderCommandList 里的
// 点状雾体（PointFog），把一切效果降维成**统一的团**（FogBody），做屏幕 tile 剪枝 +
// 逐像素 ZDist 累加 + 末端积分，**就地**合成到调用方当前绑定的 3D HDR FBO
//（与 HorizonFogRenderer 同为「其它 3D 之后、HDR 后处理之前」的一次全屏 pass）。
//
// 设计见 tools/jpov/docs/jpov_fire_fog_design.md。当前进度：
//   ✅ L1 屏幕 tile 剪枝（本 PR）+ 调试可视化（验证用）
//   ⏳ L2 逐像素 ZDist 累加 + 末端积分（M1a）
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
    // L1 tile 剪枝参数（与点光源 culling 同构）。
    static constexpr int kTileSize = 16;        // tile 边长（像素）
    static constexpr int kMaxFogsPerTile = 16;  // 每 tile 候选雾体上限 K

    FireFogRenderer() = default;
    ~FireFogRenderer();
    FireFogRenderer(const FireFogRenderer&) = delete;
    FireFogRenderer& operator=(const FireFogRenderer&) = delete;

    // 初始化：缓存 ShaderManager 指针（shader 首次用到时惰性编译）。GL context 须已激活。
    // Pre-condition: shader_mgr != nullptr
    void Init(ShaderManager* shader_mgr);

    // 释放 Init 申请的全部 GPU 资源（与 Init 对称，可重复调用；shader program 归
    // ShaderManager 统一释放）。
    void Finalize();

    // 全屏雾火 pass。
    //   fogs                  : 本帧的点状雾体（命令层 PointFog）。
    //   debug_visualize_tiles : 调试开关 —— true 时只把「有雾体覆盖的 tile」涂一层
    //                           color blend（验证 L1 tile 剪枝），不画真实体积效果。
    //   cam                   : 相机（M1a 用；本 PR 的 tile 调试不需要）。
    //   view_proj             : Proj*View（列主序 16 float），供雾体包围盒投影剪枝。
    //   viewport_w/h          : 当前 3D FBO 像素尺寸（= 剪枝 tile 网格依据）。
    //   scene_depth_tex       : MRT#1 场景深度（R32F，单采样），用于 z 裁剪；0 = 不裁剪。
    //                           （本 PR 仅剪枝/调试，暂未使用。）
    // Pre-conditions: Init 已调用；调用方已绑定 HDR FBO 并设好 viewport；
    //                 tone_mapping=true（线性 HDR 域，否则 Renderer 侧 CHECK）。
    // 说明：fogs 为空、或视口非正时零开销直接返回。
    void Draw(const std::vector<PointFog>& fogs,
              bool debug_visualize_tiles,
              const Camera& cam,
              const float view_proj[16],
              int viewport_w,
              int viewport_h,
              unsigned int scene_depth_tex);

private:
    // 命令层 PointFog → 后端统一团 FogBody（球 → 外接立方 Obb）。
    void LowerToBodies(const std::vector<PointFog>& fogs);

    // 按视口尺寸确保 tile 索引纹理存在（网格尺寸变化时才重建）。
    void EnsureTileTable(int viewport_w, int viewport_h);

    // L1：把 bodies_ 投影到屏幕 tile 网格，填 tile 索引表（每 tile ≤ K，先到先得），
    // 上传到 tile 索引纹理。
    void BuildTileIndices(const float view_proj[16], int viewport_w, int viewport_h);

    // 调试：把有雾体覆盖的 tile 涂一层 color blend（本 PR 的验证输出，就地合成）。
    void DrawTileDebugOverlay();

    // 取（惰性编译/缓存的）tile 调试 program；shader_mgr_ 为空时返回 0。
    unsigned int TileDebugProg();

    ShaderManager* shader_mgr_ = nullptr;

    std::vector<FogBody> bodies_;  // 每帧降维后的统一团（tile 剪枝的输入）

    // tile 索引纹理：R32F，(grid_cols * K) × grid_rows。每个 tile 占 K 个连续列；
    // 槽值 = 雾 index + 1（0 = 空槽），先到先得按槽序填。
    unsigned int tile_index_tex_ = 0;
    int tile_grid_w_ = 0;   // tile 列数（= ceil(viewport_w / kTileSize)）
    int tile_grid_h_ = 0;   // tile 行数（= ceil(viewport_h / kTileSize)）
    int tile_tex_w_ = 0;    // 纹理宽 = tile_grid_w_ * kMaxFogsPerTile
    int tile_tex_h_ = 0;    // 纹理高 = tile_grid_h_
};

}  // namespace jpov

#endif  // JPOV_SRC_FIRE_FOG_FIRE_FOG_RENDERER_H_
