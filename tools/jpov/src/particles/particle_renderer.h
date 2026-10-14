// JPOV 广告牌粒子渲染器 —— instanced 广告牌面片 + flipbook + 加法/半透明混合
//
// 与 object3d / skeleton / fire_fog 平级、单一自包含。输入一批已解算好的
// ParticleInstance（见 interface/particle_types.h），一次 instanced draw 画完：
//   · **无需顶点 VBO**：面片 4 角在 VS 里由 gl_VertexID 生成（TRIANGLE_STRIP，
//     每实例 4 顶点），故 VAO 只挂 per-instance 属性（divisor=1）。
//   · per-instance 属性：pos(3) / misc(4: 宽,高,滚转角,帧号) / color(4) / vel(3)。
//   · 朝向调制（view/vertical/facing/velocity）+ 滚转 + 拉伸 在 VS 里算。
//   · flipbook 在 FS 里按 (tiles_x,tiles_y,frame) 裁 UV。
//   · 混合：加法 (SRC_ALPHA,ONE) / 半透明 (SRC_ALPHA,ONE_MINUS_SRC_ALPHA)；深度测试开、
//     深度写关、关面剔除。半透明层可先按视深排序（SortMode::kByDepth）。
//
// ⚠️ 渲染进带 MRT 的 HDR 3D FBO 时，**不可**写第二附件（场景深度）—— 本渲染器在
//   mrt=true 时把 draw buffer 临时收成 {COLOR_ATTACHMENT0}，画完恢复 {0,1}，避免把
//   透明粒子的深度写进「不透明场景深度」（那会污染后续体积雾的 z 裁剪）。
//
// GL 状态：内部 glPushAttrib/glPopAttrib 兜底（ENABLE|DEPTH_BUFFER|COLOR_BUFFER）。
//
// 设计：tools/jpov/docs/jpov_particle_fire_design.md。

#ifndef JPOV_SRC_PARTICLES_PARTICLE_RENDERER_H_
#define JPOV_SRC_PARTICLES_PARTICLE_RENDERER_H_

#include "tools/jpov/interface/camera.h"
#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/src/instance_buffer.h"
#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/texture_manager.h"

namespace jpov {

// per-instance 属性布局（本渲染器独立拥有；与蒙皮/静态实例布局解耦）。
inline constexpr InstanceAttrSpec kParticlePosSpec{/*base_loc*/ 0, /*slot_count*/ 1,
                                                   /*slot_components*/ 3, /*stride*/ 3};
inline constexpr InstanceAttrSpec kParticleMiscSpec{/*base_loc*/ 1, /*slot_count*/ 1,
                                                    /*slot_components*/ 4, /*stride*/ 4};
inline constexpr InstanceAttrSpec kParticleColorSpec{/*base_loc*/ 2, /*slot_count*/ 1,
                                                     /*slot_components*/ 4, /*stride*/ 4};
inline constexpr InstanceAttrSpec kParticleVelSpec{/*base_loc*/ 3, /*slot_count*/ 1,
                                                   /*slot_components*/ 3, /*stride*/ 3};

class ParticleRenderer {
public:
    ParticleRenderer();
    ~ParticleRenderer();

    ParticleRenderer(const ParticleRenderer&) = delete;
    ParticleRenderer& operator=(const ParticleRenderer&) = delete;

    // 顶点着色器：gl_VertexID 生成面片四角 + 广告牌朝向调制 + 逐粒子滚转/拉伸。
    static constexpr const char* kParticleVs = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aMisc;   // (width, height, rotation, frame)
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec3 aVel;
uniform mat4 uViewProj;
uniform vec3 uCamRight;
uniform vec3 uCamUp;
uniform vec3 uCamPos;
uniform vec3 uWorldUp;
uniform int  uAlign;         // 0 view, 1 vertical, 2 facing, 3 velocity
uniform int  uStretched;     // 0/1
uniform float uStretchScale;
out vec2 vUV;
out vec4 vColor;
flat out float vFrame;
void main() {
    vec2 corner = vec2((gl_VertexID == 1 || gl_VertexID == 3) ? 1.0 : -1.0,
                       (gl_VertexID >= 2) ? 1.0 : -1.0);
    vUV = corner * 0.5 + 0.5;
    vColor = aColor;
    vFrame = aMisc.w;
    vec3 toCam = uCamPos - aPos;
    vec3 right;
    vec3 up;
    if (uAlign == 0) {                 // 屏幕/view-plane 对齐
        right = uCamRight;
        up = uCamUp;
    } else if (uAlign == 1) {          // 竖直/roll 锁定（只绕 Y 轴转向相机）
        vec3 d = normalize(toCam);
        vec3 r = cross(uWorldUp, d);
        if (length(r) < 1e-4) {
            r = uCamRight;
        }
        right = normalize(r);
        up = uWorldUp;
    } else if (uAlign == 2) {          // 面向相机位置（球面 facing）
        vec3 n = normalize(toCam);
        vec3 r = cross(uWorldUp, n);
        if (length(r) < 1e-4) {
            r = uCamRight;
        }
        right = normalize(r);
        up = normalize(cross(n, right));
    } else {                           // 沿速度对齐（火花/余烬）
        float sp = length(aVel);
        vec3 u = (sp > 1e-4) ? (aVel / sp) : uCamUp;
        vec3 r = cross(u, normalize(toCam));
        if (length(r) < 1e-4) {
            r = uCamRight;
        }
        right = normalize(r);
        up = normalize(cross(right, u));
    }
    float c = cos(aMisc.z);
    float s = sin(aMisc.z);
    vec3 rr = right * c + up * s;
    vec3 uu = -right * s + up * c;
    vec2 size = aMisc.xy;
    if (uStretched == 1) {
        size.y *= (1.0 + length(aVel) * uStretchScale);
    }
    vec3 world = aPos + (corner.x * size.x) * rr + (corner.y * size.y) * uu;
    gl_Position = uViewProj * vec4(world, 1.0);
}
)glsl";

    // 片元着色器：flipbook UV 裁剪 + RGBA 乘。
    static constexpr const char* kParticleFs = R"glsl(
#version 330 core
in vec2 vUV;
in vec4 vColor;
flat in float vFrame;
uniform sampler2D uTex;
uniform int uHasTex;
uniform int uTilesX;
uniform int uTilesY;
uniform int uFlipbook;
out vec4 FragColor;
void main() {
    vec2 uv = vUV;
    if (uFlipbook == 1 && uTilesX >= 1 && uTilesY >= 1) {
        float f = floor(vFrame + 0.5);
        float col = mod(f, float(uTilesX));
        float row = floor(f / float(uTilesX));
        float vrow = float(uTilesY - 1) - row;   // 第 0 帧在顶行
        uv = vec2((uv.x + col) / float(uTilesX), (uv.y + vrow) / float(uTilesY));
    }
    vec4 tex = (uHasTex == 1) ? texture(uTex, uv) : vec4(1.0);
    FragColor = vColor * tex;
}
)glsl";

    // 初始化：编译 program + 建空 VAO。
    // Pre-condition: GL context 已激活；shader_mgr 非空。
    void Init(ShaderManager* shader_mgr);

    // 释放本渲染器拥有的 GL 对象（VAO；InstanceBuffer 自管其 VBO）。
    void Finalize();

    // 画一批广告牌粒子到**当前绑定的 3D FBO**（viewport 已设好）。
    // Pre-condition: Init 已调用；cmd.particles 非空（空则直接返回）。
    // mrt=true 表示目标 FBO 带第二颜色附件（场景深度），绘制期间会临时收 draw buffer。
    void Draw(const ParticleRenderCommand& cmd,
              TextureManager& texture_mgr,
              const Camera& camera,
              const float view_proj[16],
              bool mrt);

private:
    ShaderManager* shader_mgr_ = nullptr;
    unsigned int prog_ = 0;
    unsigned int vao_ = 0;
    InstanceBuffer pos_buf_{kParticlePosSpec};
    InstanceBuffer misc_buf_{kParticleMiscSpec};
    InstanceBuffer color_buf_{kParticleColorSpec};
    InstanceBuffer vel_buf_{kParticleVelSpec};
    bool initialized_ = false;
};

}  // namespace jpov

#endif  // JPOV_SRC_PARTICLES_PARTICLE_RENDERER_H_
