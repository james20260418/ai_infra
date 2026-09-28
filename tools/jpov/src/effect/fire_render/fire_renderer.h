// JPOV FireRenderer — 程序化火焰特效渲染（MVP）
//
// 与 Primitives3DRenderer / Object3DRenderer 同构的**静态工具集**：所有方法为
// static，不含实例状态；GL 资源（stream_vbo / prog / mvp）由 Renderer 持有并传入。
//
// 定位：并列于 primitives3d 的一个「特效 pass」。渲染在 3D 不透明内容之后、tone map
// 之前；**参与深度测试（被前景遮挡）、不写深度（半透明）**，与 Object3D 共享同一张
// depth buffer。见 docs/jpov_effect_pass_design.md。
//
// 实现取向（MVP）：**程序化 shader 火焰**——不用纹理、不做粒子模拟。
//   一团火 = 一个面向相机的竖直 quad（**圆柱 billboard**，仅绕 Y 轴朝相机，
//   保证「火向上」永远正确），fragment 里用「形状梯度 × 滚动 fbm 噪声」实时生成火焰。
//   优点：1 个 quad / 1 次 draw、参数化（颜色/速度/频率）、无资源依赖、无 LOD 负担。
//
// 归并约束（将来加烟/雨/雪时的公共约束，与设计文档一致）：同一批 draw 只能有一个
// 混合模式；本 MVP 只画火焰（恒加法），故每个 FireCommand 一次 draw。

#ifndef JPOV_FIRE_RENDERER_H_
#define JPOV_FIRE_RENDERER_H_

#include "tools/jpov/interface/camera.h"
#include "tools/jpov/interface/render_command.h"

namespace jpov {

class FireRenderer {
public:
    // ---- GLSL shader 源码 ----
    //
    // 调用方用 ShaderManager 编译：
    //   shader_mgr.GetOrCreate("fire", {kFireVs, kFireFs});
    //
    // kFireVs：顶点已在 CPU 端展开为**世界坐标**（billboard 在 CPU 算，见 DrawFire），
    //          顶点着色器只做 MVP 变换 + 透传 UV。
    //          顶点布局：loc0 = vec3 世界坐标；loc1 = vec2 UV
    //          （UV 约定：u∈[0,1] 左→右；v∈[0,1] 底→顶）。
    static constexpr const char* kFireVs = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
uniform mat4 uMVP;
out vec2 vUv;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vUv = aUv;
}
)glsl";

    // kFireFs：程序化火焰。
    //   形状：底部宽、顶部收窄的三角剪影（形状梯度）。
    //   动态：fbm 噪声沿 +v 方向滚动（火向上蹿）。
    //   颜色：低热处 = 外焰色，高热处 = 核心色（热色阶）。
    //   alpha：剪影覆盖率（加法混合下即发光强度的一部分）。
    // 全部 uniform 由 CPU 每帧上传（时间/颜色/强度/噪声频率）。
    static constexpr const char* kFireFs = R"glsl(
#version 330 core
in vec2 vUv;
out vec4 FragColor;
uniform float uTime;        // 特效时钟（秒）
uniform float uSpeed;       // 动画速度倍率
uniform float uNoiseScale;  // 噪声频率（焰舌粗细）
uniform vec3  uColorCore;   // 核心色（热）
uniform vec3  uColorOuter;  // 外焰色（冷）
uniform float uIntensity;   // 发光强度（HDR 乘子）

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float fbm(vec2 p) {
    float v = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; ++i) {
        v += amp * vnoise(p);
        p *= 2.0;
        amp *= 0.5;
    }
    return v;
}

void main() {
    vec2 uv = vUv;                                  // u:0→1 左→右; v:0→1 底→顶
    float t = uTime * uSpeed;

    // 噪声：三个频率叠加（大块 + 细节 + 细纹），均向上滚动（火往上蹿）。
    float n1 = fbm(vec2(uv.x * uNoiseScale, uv.y * uNoiseScale * 0.5 - t));
    float n2 = fbm(vec2(uv.x * uNoiseScale * 2.4 + 3.0,
                        uv.y * uNoiseScale * 1.1 - t * 1.6));
    float n3 = fbm(vec2(uv.x * uNoiseScale * 3.7 - t * 2.4,
                        uv.y * uNoiseScale * 2.1 - t * 2.2));
    float n = mix(mix(n1, n2, 0.4), n3, 0.22);
    n = smoothstep(0.22, 0.85, n);                  // 提高对比度 → 更像“火舌”而非雾

    // 宽度包络：底部宽、顶部收窄；边缘被噪声侵蚀 → 不规则火舌。
    float w = mix(1.0, 0.10, uv.y);
    float dx = abs(uv.x - 0.5) * 2.0;               // 0(中) .. 1(边)
    float mask = 1.0 - smoothstep(w * 0.35, w * (0.55 + 0.6 * n), dx);
    mask *= smoothstep(1.0, 0.68, uv.y);            // 顶部渐隐
    mask *= smoothstep(0.0, 0.06, uv.y);            // 底部不硬贴

    // 热度：底热顶冷 × 噪声（给火焰内部纹理）。
    float grad = pow(max(1.0 - uv.y, 0.0), 1.4) * 1.9;
    float heat = clamp(n * grad * mask, 0.0, 1.0);

    // 热色阶：低热 = 外焰色（橙红），高热 = 核心色（亮黄）。
    // 亮度用 pow(heat,0.6) 拉开（否则颜色被 heat 乘暗、不鲜艳）。
    vec3 tone = mix(uColorOuter, uColorCore, smoothstep(0.12, 0.85, heat));
    float bright = pow(heat, 0.6);
    // alpha：剪影覆盖率；热处更实（原子透明的焰心更亮、更不透明）。
    float a = clamp(mask * (0.45 + 0.75 * heat), 0.0, 1.0);
    FragColor = vec4(tone * bright * uIntensity, a);
}
)glsl";

    // ---- 绘制 ----
    //
    // 画一条火焰命令：CPU 端构造圆柱 billboard 的 6 个三角形顶点（世界坐标 + UV），
    // 上传到 stream_vbo 后一次 glDrawArrays。
    //
    // cmd:       火焰命令（底部中心 / 半宽 / 高度 / 颜色 / 强度 / 速度 / 噪声频率）。
    // cam:       当前相机（billboard 右向量 = cross(worldUp, 水平视线)）。
    // mvp:       当前相机 MVP（CPU 预算）。
    // stream_vbo:渲染器持有的流式 VBO。
    // prog:      已编译的 fire shader program。
    // time:      特效时钟（秒），来自 RenderCommandList::effect_time。
    //
    // Pre-condition: cmd.radius > 0 且 cmd.height > 0（DrawFire 已 CHECK）
    // Pre-condition: prog 已注册（FireProg()）；stream_vbo 非 0
    static void DrawFire(const FireCommand& cmd, const Camera& cam,
                         unsigned int stream_vbo, unsigned int prog,
                         const float mvp[16], float time);
};

}  // namespace jpov

#endif  // JPOV_FIRE_RENDERER_H_
