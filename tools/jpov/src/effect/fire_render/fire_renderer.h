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
    //   由两部分叠加：
    //     ① 根部：持续不断的一束小火苗（不消失，钉在底部）。
    //     ② 火舌串：竖直方向分成若干“团”，像烟囱的烟团一样不断向上冒出、
    //        各自摆动/淡出 —— 火焰主体由这些断续的火舌构成（不是连续柱）。
    //   水平摆动幅度随高度增大；噪声用低频（纹理疏，不做高频细密纹理）。
    static constexpr const char* kFireFs = R"glsl(
#version 330 core
in vec2 vUv;
out vec4 FragColor;
uniform float uTime;        // 特效时钟（秒）
uniform float uSpeed;       // 动画速度倍率
uniform float uNoiseScale;  // 噪声频率（焰舌粗细/纹理疏密）
uniform vec3  uColorCore;   // 核心色（热）
uniform vec3  uColorOuter;  // 外焰色（冷）
uniform float uIntensity;   // 发光强度（HDR 乘子）

float hash11(float p) {
    return fract(sin(p * 127.1) * 43758.5453123);
}

float hash12(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash12(i);
    float b = hash12(i + vec2(1.0, 0.0));
    float c = hash12(i + vec2(0.0, 1.0));
    float d = hash12(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float fbm(vec2 p) {
    float v = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 3; ++i) {          // 3 层，纹理更疏
        v += amp * vnoise(p);
        p *= 2.0;
        amp *= 0.5;
    }
    return v;
}

void main() {
    vec2 uv = vUv;                          // x:0→1 左→右; y:0→1 底→顶
    float t = uTime * uSpeed;
    float x = uv.x - 0.5;                   // -0.5 .. 0.5
    float y = uv.y;

    // ── 火舌串：竖直方向分格，每格是一团向上冒出、摆动、消失的火舌 ──
    const float kBands = 5.0;
    float sv  = y * kBands - t * 1.2;
    float ci  = floor(sv);
    float cf  = fract(sv);
    float rnd = hash11(ci);

    // 活跃区间：软收尾，相邻团略有重叠 → 连成一条上升的火焰，但仍能看出“一团团”。
    float life = smoothstep(0.0, 0.28, cf) * (1.0 - smoothstep(0.72, 1.0, cf));

    // 水平摆动：沿高度连续变化（关键：不能用每格随机的相，否则相邻团会水平错开、
    // 看起来是“分离的悬浮块”）。幅度随高度增大 → 顶部摆得更大。
    float swayAmp = 0.05 + 0.35 * y;
    float sway = 0.60 * sin(t * 1.5 + y * 3.0)
               + 0.40 * sin(t * 2.7 + y * 6.0 + 1.3);
    float cx = 0.5 + swayAmp * sway + 0.02 * (rnd - 0.5);

    // 火舌形状：先定一条连续的火柱宽度包络（底宽顶窄），再被噪声 + 每团的
    // “鼓包”调制 → 既连成一条火焰，又能看出一团团上升的火舌。
    float vp  = clamp(cf, 0.0, 1.0);
    float nW  = vnoise(vec2(uv.x * uNoiseScale * 1.6, uv.y * 3.6 - t * 2.0));
    float wCol = 0.27 * (1.0 - 0.62 * y);
    float w  = wCol * (0.60 + 0.60 * nW) * (0.80 + 0.50 * life);
    float dx = abs(uv.x - cx);
    float tongue = (1.0 - smoothstep(w * 0.35, w, dx)) * (0.55 + 0.45 * life);

    // ── 根部：持续不断的一小束小火苗（不随格子消失，钉在底部）──
    const float kRootH = 0.22;
    float nR = vnoise(vec2(uv.x * uNoiseScale * 2.2, uv.y * 5.0 - t * 2.6));
    float rootW = 0.13 * (1.0 - y / kRootH) * (0.60 + 0.90 * nR);
    float rootMask = (1.0 - smoothstep(0.0, kRootH, y)) *
                     (1.0 - smoothstep(rootW * 0.25, rootW, abs(x)));
    rootMask *= 0.85 + 0.15 * sin(t * 3.4 + y * 10.0);

    // 噪声侵蚀：让剪影不规则 + 断续（大块 + 细节两层）。
    float n1 = fbm(vec2(uv.x * uNoiseScale * 0.80, uv.y * 2.4 - t * 1.4));
    float n2 = vnoise(vec2(uv.x * uNoiseScale * 2.6 + 5.0, uv.y * 4.6 - t * 2.4));
    float erode = n1 * 0.65 + n2 * 0.35;

    float density = max(tongue, rootMask);
    float shape = density * smoothstep(0.28, 0.62, erode + 0.15 * density);

    // 热度由单层噪声驱动（分布宽）→ 颜色从橙红到黄白有层次。
    float nH  = vnoise(vec2(uv.x * uNoiseScale * 1.1, uv.y * 3.0 - t * 1.8));
    float heat = clamp(density * (0.25 + 1.35 * nH) * (1.0 - 0.25 * y), 0.0, 1.0);

    // 颜色与亮度解耦：亮度不再随 heat 归零（否则外缘橙红被压黑看不见）
    // → 外缘保留可见的橙红（外焰），核心趋黄白。
    vec3 tone = mix(uColorOuter, uColorCore, smoothstep(0.05, 0.70, heat));
    float bright = mix(0.55, 1.0, heat);
    float a = clamp(shape * (0.55 + 0.45 * heat), 0.0, 1.0);
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
