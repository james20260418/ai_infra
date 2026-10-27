// JPOV WaterRenderer —— 全局水面（水/海）
//
// 两个 pass，都与三大 renderer / 粒子 / 体积火 / 条带**平级**（独立子渲染器）：
//
//   ① DrawSurface  —— 水面本身（**全屏三角形 + 每像素射线求交**，无任何几何）
//        · 反射：天光 cubemap 探针（第一版唯一反射源）
//        · 透射：抓帧副本 + 逐通道 Beer-Lambert 吸收/散射（这就是"水下遮蔽感"）
//        · 写 **MRT 双附件**（颜色 + 场景深度）+ 写硬件深度
//          ⇒ 后面的粒子/火/条带按场景深度做手动作深度测试时，才知道被水面挡住；
//          ⇒ 雾按 MRT 深度作用，远景水面才会融进地平线。
//
//   ② DrawUnderwaterMedium —— 相机沉到水下时的**整屏介质**
//        · 整条视线都在水里 ⇒ 对已成画面做一次逐通道吸收 + 散射
//        · 位置：所有 3D 内容之后、雾之前（见 Renderer::Render）
//
// ⚠️ 采样反馈环：两个 pass 都要读"已经有了的画面"，但写的目标正是同一个 HDR FBO
//   ⇒ 必须先 `Capture()` 把 color + MRT 深度 blit 到自己的副本，再对副本采样。
//   （这也是后面加折射时唯一还要复用的基础设施：折射采样 uv 会偏移，天然需要副本。）
//
// 形态选择（2026-10-27 Danis 定）：**全屏 + 法线**，不要几何。
//   水面在数学上就是 `y = level` 这个平面，命中点是解析解 ⇒ 几何一点用都没有；
//   而任何"网格/quad"都有边界（远平面切平面 = 屏幕上的曲线、quad 直边、超大三角形
//   的 varying 插值在 llvmpipe 上会退化），这些边界全是不必要的伪影来源。
//   巨浪将来是**可插拔**的：只换「命中点 + 法线」这一段（网格光栅化 / 高度场迭代求交），
//   菲涅耳、吸收、深度写入全部复用 —— 见 water_shader.h 顶部的"可插拔点"。

#ifndef JPOV_SRC_WATER_WATER_RENDERER_H_
#define JPOV_SRC_WATER_WATER_RENDERER_H_

#include "tools/jpov/interface/camera.h"
#include "tools/jpov/interface/render_command.h"   // WaterSurface / DirectionalLight
#include "tools/jpov/src/shader_manager.h"

namespace jpov {

class WaterRenderer {
public:
    WaterRenderer() = default;
    ~WaterRenderer() = default;   // 不在这里释放 GL：必须显式 Finalize（见 SOUL 对称生命周期）

    WaterRenderer(const WaterRenderer&) = delete;
    WaterRenderer& operator=(const WaterRenderer&) = delete;

    // 初始化（编译 program + 建网格 + 建抓帧 FBO）。必须在 GL context 存活时调用。
    void Init(ShaderManager* shader_mgr);

    // 释放全部 GL 资源。可重复调用。
    void Finalize();

    // 相机是否在水面之下（决定水面 shader 走"从下看"分支、以及要不要跑整屏介质）。
    static bool IsCameraUnderwater(const WaterSurface& water, const Camera& cam);

    // ---- ① 水面 pass ----
    // 前置：调用方已把 HDR FBO 画好（不透明 3D 全部完成）；fbo_hdr 是目标 FBO，
    //       color_tex / scene_depth_tex 是它的附件 0 / 1（本函数内部会 blit 成副本）。
    // 后置：GL 状态恢复到调用前（enable / depth / viewport），FBO 绑定**不保证**。
    // view_proj：相机 Proj*View（列主序 16 float，与主 3D pass 同一份 —— 水面必须和
    //            场景用同一套投影，否则深度/命中判断会错位）。
    void DrawSurface(const WaterSurface& water, const Camera& cam,
                     int fbo_w, int fbo_h,
                     unsigned int fbo_hdr,
                     unsigned int color_tex, unsigned int scene_depth_tex,
                     unsigned int env_cube, float env_max_lod,
                     const DirectionalLight* sun,
                     const float tir_color[3],   // 纯皮模式的 TIR 色（skylight 地色）
                     const float view_proj[16]);

    // ---- ② 整屏介质 pass（仅相机在水下时调用）----
    // 前置：所有 3D 内容（含水下看到的水面本身）已画完；fbo_hdr 是目标。
    // 后置：同 DrawSurface。
    // env_cube：天光探针（内散射要按"上方漫射光"缩放，否则夜里水也发光）。
    void DrawUnderwaterMedium(const WaterSurface& water, const Camera& cam,
                              int fbo_w, int fbo_h,
                              unsigned int fbo_hdr,
                              unsigned int color_tex, unsigned int scene_depth_tex,
                              unsigned int env_cube,
                              const DirectionalLight* sun,
                              const float view_proj[16]);

private:
    // 抓帧副本 FBO（颜色 RGBA16F + 场景深度 R32F），尺寸变化时重建。
    void EnsureCapture(int w, int h);
    void DestroyCapture();
    // 把 fbo_hdr 的附件 0 / 1 blit 到抓帧副本。
    void Capture(int w, int h, unsigned int fbo_hdr);

    unsigned int prog_surface_ = 0;   // ShaderManager 缓存的 program id（0 = 未用）
    unsigned int prog_medium_ = 0;

    unsigned int fs_vao_ = 0;         // 全屏三角形用的空 VAO（core profile 必需）

    unsigned int cap_fbo_ = 0;        // 抓帧副本
    unsigned int cap_color_tex_ = 0;
    unsigned int cap_depth_tex_ = 0;
    int cap_w_ = 0;
    int cap_h_ = 0;

    ShaderManager* shader_mgr_ = nullptr;
};

}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_RENDERER_H_
