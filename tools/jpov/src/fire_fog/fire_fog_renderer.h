// JPOV FireFogRenderer — 雾火（体积雾 + 火）统一子渲染器（froxel 版）
//
// 与 object3d / horizon_fog 平级的**单一自包含**渲染器。它消费 RenderCommandList 里的
// 点状雾体（PointFog），把一切效果降维成**统一的团**（FogBody），用 **froxel 屏幕空间
// 摊销 Z 轴采样**（设计见 tools/jpov/docs/jpov_froxel_design.md）算出体积内散射，
// **就地**合成到调用方当前绑定的 3D HDR FBO（与 HorizonFogRenderer 同为「其它 3D 之后、
// HDR 后处理之前」的一次全屏 pass）。
//
// 趟（inject/reduce/scatter 跑在 froxel 纹理上；composite 跑在主 FBO）：
//   froxel 网格：每个 Nxy 单元占 froxel 纹理上一块 sblock×sblock texel（sblock=√nz，
//   承载 nz 个 z 切片，指数分布）：
//   1. inject （fire_fog_inject） 逐 texel = 某 froxel 的**局部** (τ, S)：tile 中心视线 ×
//                              [z_k, z_{k+1}) 段内累加候选雾团的 Δτ 与 S_leaf。用每 tile 的
//                              保守 z 范围做**短路**（切片完全在候选范围外 ⇒ 恒等）。
//   2. reduce （fire_fog_reduce） 金字塔每一级：由上一级沿 z **4 合 1**（over 合成）。
//   3. scatter（fire_fog_scatter）逐 texel = 某 froxel 的**累积** (τ, S)：沿 z 有序前缀，
//                              用金字塔 + **base-4 分解** ⇒ **O(log nz)** 次合成。
//   4. composite（fire_fog_composite）全屏：4 邻 tile 双线性 + 像素深度 z 向插值 →
//                              vec4(S.rgb, T)，配 GL_ONE/GL_SRC_ALPHA 就地混合（S + dst·T）。
//
// 分步验收（Danis 2026-10-09 定）：
//   step1：FireFogParams::sun_enable=false ⇒ 只输出 base 发射（不采样任何光源），看雾团形状；
//   step2：sun_enable=true ⇒ 叠加 ambient + 太阳×CSM 阴影（god ray）。
//
// 约束：nz 必须是 **4 的幂**（sblock=√nz 为 2 的幂，金字塔才能整齐 4 合 1）。
//
// ── GL 状态机契约（谁保存 / 谁复原）────────────────────────────────────────────
// Draw() 自行保存并复原（不依赖调用方）：
//   · GL_FRAMEBUFFER_BINDING / GL_VIEWPORT / GL_ACTIVE_TEXTURE（进入时取出，结束前回写）；
//   · 使用的纹理单元（0..(3+kMaxCascades)）的 GL_TEXTURE_2D 绑定（进入时逐个取出，结束前回写）；
//   · GL_BLEND_SRC_ALPHA / GL_BLEND_DST_ALPHA（本趟用 GL_ONE/GL_SRC_ALPHA，必须回写，否则污染 2D/字体）。
// Draw() 不保存、由**调用方的 glPushAttrib 兜底**（JPOV 的 3D 段用 GL_ENABLE_BIT|GL_VIEWPORT_BIT）：
//   · GL_BLEND / GL_DEPTH_TEST / GL_CULL_FACE 的 **enable 位**（本趟统一关掉）；
//   · 当前 program / VAO（本趟结束置 0 / 不动，调用方后续各自重设）。
// Draw() **不触碰**当前 FBO 的 draw buffer（由调用方设置；本渲染器不调 glDrawBuffers 于调用方 FBO）。
// 即：调用方**必须**在 3D 段内用 glPushAttrib(GL_ENABLE_BIT|GL_VIEWPORT_BIT) 包住对 Draw 的调用。
//
// GLSL 见 fire_fog_shader.h；数据模型见 fog_body.h；GL-free 工具见 fire_fog_lower.h。

#ifndef JPOV_SRC_FIRE_FOG_FIRE_FOG_RENDERER_H_
#define JPOV_SRC_FIRE_FOG_FIRE_FOG_RENDERER_H_

#include <optional>
#include <string>
#include <vector>

#include "tools/jpov/interface/camera.h"
#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/src/fire_fog/fog_body.h"
#include "tools/jpov/src/shader_manager.h"

namespace jpov {

// 雾火的**物理光照输入**（与 object3d 共用同一套 CSM 资源，由调用方填）。
//
// 内散射源：L_in = fog_tint · ( ambient_color·ambient_intensity
//                                + sun_color·sun_intensity·csm_shadow )。
// 前者来自天光单色 ambient 推导（SkyCommand::AmbientColor()/AmbientIntensity()），
// 后者是太阳直射经 CSM 阴影调制 —— god ray 正是来自后者。
struct FireFogLighting {
    Color ambient_color{0.9f, 0.9f, 0.9f, 1.0f};   // SkyCommand::AmbientColor()
    float ambient_intensity = 1.0f;                // SkyCommand::AmbientIntensity()
    std::optional<DirectionalLight> sun;           // 无 ⇒ 无 god ray
    ShadowConfig shadow_cfg{};                      // 级联范围 / 淡出 / 偏置
    std::vector<CascadeFBO> shadow_fbos;            // 长度 = cascade_count（仅 sun 时有效）
    float shadow_vp[ShadowConfig::kMaxCascades][16] = {};
    float shadow_depth_vp[ShadowConfig::kMaxCascades][16] = {};
    float shadow_texel_world[ShadowConfig::kMaxCascades] = {};
};

class FireFogRenderer {
public:
    // ── 常量 ──
    // froxel 网格分辨率由 FireFogParams 的 nz / tile_px 给出（运行期），下列为其**合法范围**，
    // Draw() 会 CHECK（不合法直接 crash，不静默回退）。
    static constexpr int kMinNz = 4;             // nz 下界（4=4^1，sblock=2）
    static constexpr int kMaxNz = 1024;          // nz 上界（1024=4^5，sblock=32）
    static constexpr int kMinTilePx = 1;         // tile_px 下界（屏幕像素）
    static constexpr int kMaxTilePx = 256;       // tile_px 上界（屏幕像素）
    static constexpr int kMaxFroxelDim = 16384;  // 派生 froxel 纹理单轴上限（GL 常见上限）
    static constexpr int kMaxLevels = 5;         // 金字塔缩减级上限（nz=4^5）
    static constexpr int kMaxFogsPerTile = 8;          // K：每 tile 团上限
    static constexpr int kTexelsPerTile = kMaxFogsPerTile / 4;  // 每 tile 的 RGBA8 texel 数 = 2
    static constexpr int kMaxTotalFogs = 255;          // 全局团上限（uint8 索引，sentinel=255）
    static constexpr uint8_t kFogIndexSentinel = 255;  // 空槽哨兵

    FireFogRenderer() = default;
    ~FireFogRenderer();
    FireFogRenderer(const FireFogRenderer&) = delete;
    FireFogRenderer& operator=(const FireFogRenderer&) = delete;

    // 初始化：编译各趟 shader（program 归 ShaderManager 释放）。要求 GL context 已激活。
    // Pre-condition: shader_mgr != nullptr
    void Init(ShaderManager* shader_mgr);

    // 释放 Init 申请的全部 GPU 资源（与 Init 对称，可重复调用）。
    void Finalize();

    // froxel 雾火管线（见头文件顶部）：inject/reduce/scatter 在自建 FBO 上跑，composite 回到
    // 调用方当前绑定的 3D HDR FBO **就地**混合（GL_ONE/GL_SRC_ALPHA）。
    //   fogs            : 本帧的点状雾体（命令层 PointFog）。
    //   cam             : 相机（取世界位置做光线原点 + 每 tile z 范围）。
    //   view_proj       : Proj*View（列主序 16 float）。
    //   viewport_w/h    : 当前 3D FBO 像素尺寸（= froxel 纹理尺寸）。
    //   scene_depth_tex : MRT#1 场景深度（R32F，单采样），composite 按像素深度裁剪；0 = 不裁剪。
    //   params          : 光照开关 / 相位 / 增益 + froxel z 分布区间（z_near/z_far）。
    //   light           : 物理光照 + CSM 资源（仅 sun_enable=true 时使用）。
    // Pre-conditions: Init 已调用；调用方已绑定 HDR FBO 并设好 viewport；场景深度纹理
    //                 **不**是当前 draw FBO 的附件（否则采样↔写入反馈环）。
    // 说明：fogs 为空时零开销直接返回；本函数自行保存/复原调用方 FBO 绑定 / viewport /
    //       活动纹理单元 / 使用的纹理单元绑定 / blend func（见头文件顶部「GL 状态机契约」）。
    void Draw(const std::vector<PointFog>& fogs,
              const Camera& cam,
              const float view_proj[16],
              int viewport_w,
              int viewport_h,
              unsigned int scene_depth_tex,
              const FireFogParams& params,
              const FireFogLighting& light);

private:
    // 按 (grid_cols, grid_rows, sblock, num_levels)（= Nxy 列/行数 + 每柱边长 + 金字塔级数）
    // 确保 froxel 目标（inject / scatter / 各级金字塔）存在且尺寸匹配（变化则重建）。
    void EnsureTargets(int grid_cols, int grid_rows, int sblock, int num_levels);
    // 按 (grid_cols, grid_rows) 确保 tile 索引纹理 + z 范围纹理 + tile 射线纹理存在且尺寸匹配。
    void EnsureTileTexture(int grid_cols, int grid_rows);
    // 确保 z 切片边界表纹理（R32F，(nz+1)×1）存在且长度匹配。
    void EnsureZSlices(int nz);

    ShaderManager* shader_mgr_ = nullptr;

    // 各趟 program（Init 编译；归 ShaderManager 释放）。
    unsigned int prog_inject_ = 0;
    unsigned int prog_reduce_ = 0;
    unsigned int prog_scatter_ = 0;
    unsigned int prog_composite_ = 0;
    std::string fs_inject_;      // 组装后的 FS 源码（须存活到编译完成）
    std::string fs_reduce_;
    std::string fs_scatter_;
    std::string fs_composite_;

    // froxel 目标（主 FBO 尺寸，各 1 个 RGBA32F 附件）。
    unsigned int inject_tex_ = 0;    // L1：局部 (τ, S)
    unsigned int scatter_tex_ = 0;   // 累积 (τ, S)
    unsigned int inject_fbo_ = 0;
    unsigned int scatter_fbo_ = 0;
    int froxel_w_ = 0;
    int froxel_h_ = 0;

    // 金字塔缩减级（每级 1 个 RGBA32F 附件；级 j 每柱边长 = sblock>>j，块大小 4^j）。
    // 仅前 num_levels_ 个有效；数组下标 = 级号-1。
    unsigned int level_tex_[kMaxLevels] = {};
    unsigned int level_fbo_[kMaxLevels] = {};
    int level_w_[kMaxLevels] = {};
    int level_h_[kMaxLevels] = {};
    int num_levels_ = 0;             // 缩减级数 = log4(nz)
    int level_sblock_ = 0;           // 建这些级时的每柱边长（= √nz），用于失效判断
    int level_grid_w_ = 0;
    int level_grid_h_ = 0;

    // tile 索引纹理（RGBA8；宽 = grid_w*kTexelsPerTile，高 = grid_h）。
    unsigned int tile_index_tex_ = 0;
    int tile_tex_w_ = 0;
    int tile_tex_h_ = 0;
    int grid_w_ = 0;
    int grid_h_ = 0;

    // 每 tile 保守 z 范围纹理（RG32F；宽 = grid_w，高 = grid_h）。
    // 尺寸随 Nxy 栅格（tile_px）变化，故与 tile_index_tex_ 一样记录尺寸、变化即重建
    //（否则从粗档切到细档时旧尺寸残留 → inject 短路读到错位数据 → 只有旧范围区域有雾）。
    unsigned int tile_zrange_tex_ = 0;
    int tile_zrange_tex_w_ = 0;
    int tile_zrange_tex_h_ = 0;

    // 每 tile 中心视线方向纹理（RGBA32F；宽 = grid_w，高 = grid_h；xyz=单位方向, w=1）。
    // CPU 预算（每 tile 一次）→ inject 不必逐 texel 做两次 MVP 逆乘 + normalize。
    // 尺寸随 Nxy 栅格变化 ⇒ 同样记录尺寸、变化即重建。
    unsigned int tile_ray_tex_ = 0;
    int tile_ray_tex_w_ = 0;
    int tile_ray_tex_h_ = 0;

    // z 切片边界表 z_k（R32F；(nz+1)×1）→ inject 不必逐 texel pow(uR, k)。
    unsigned int z_slices_tex_ = 0;
    int z_slices_count_ = 0;   // 表长度 = nz+1

    // 团属性纹理（RGBA32F；宽 = kMaxTotalFogs*3，高 = 1；每团 3 texel）。
    unsigned int fog_body_tex_ = 0;

    // 1×1 R32F 哑元深度纹理（值 = 1.0）：调用方传 scene_depth_tex==0 时使用，
    // 使 composite 退化为「不打裁剪」（像素深度取远平面）。
    unsigned int dummy_depth_tex_ = 0;

    // 每帧把命令层点雾降维后的统一团列表（烘焙到 fog_body_tex_）。
    std::vector<FogBody> bodies_;
};

}  // namespace jpov

#endif  // JPOV_SRC_FIRE_FOG_FIRE_FOG_RENDERER_H_
