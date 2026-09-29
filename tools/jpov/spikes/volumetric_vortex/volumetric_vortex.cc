// JPOV 预实验 spike —— 「每颗粒子 = 一个涡流体」的体积渲染（验证后删除本目录）
//
// 目的：验证涡流（vortex）用**体积场**渲染时的质量上限，重点看**俯视会不会
// 穿帮**（billboard 的纸片感）。暂不考虑性能。
//
// 方案（对应讨论结论）：
//   1. 先渲染场景（地面 + 立柱）到一张 FBO，并**把线性距离写进 RGBA32F 颜色
//      纹理**（不是深度纹理！llvmpipe 采样 depth texture 不可靠，见 renderer.cc
//      shadow 的踩坑注释）。这张 float 深度图 = "其它物体的深度场"。
//   2. 每个存活粒子 = 一个**涡流体**：相机朝向包围 quad + 逐像素 ray-sphere
//      求交，沿射线 marching 采样**程序化 3D 密度场**（绕轴旋涡 + 域扭曲 fbm）。
//      密度定义在世界空间 ⇒ 俯视/侧视一致，不穿帮。
//   3. 射线用场景 float 深度裁剪：被立柱挡住的部分不画（软裁剪的前提）。
//   4. 火是发光体 ⇒ 加法混合 ⇒ 天然免排序。
//
// 纹理 vs 纯模拟：**纯程序化**，无资产纹理。噪声在 shader 里实时算；
// 唯一用到的纹理是渲染管线自带的**场景深度**（float 颜色纹理）。
//
// 运行：bazel run //tools/jpov/spikes/volumetric_vortex -- <out_dir>
// 输出：<out_dir>/vol_side.png、<out_dir>/vol_top.png（+ 帧时不报，spike 而已）

#define GL_GLEXT_PROTOTYPES

#include <GLFW/glfw3.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include <fcntl.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <glog/logging.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

// ============ headless GL context 引导（同 pbr_tbn_probe） ============
GLFWwindow* BootGL(int w, int h) {
    if (!glfwInit()) {
        int pid = fork();
        if (pid == 0) {
            // 子进程把 stdout/stderr 接到 /dev/null，否则 exec 后的 Xvfb 会
            // 继承管道 → 调用方的 `| tail` 永远等不到 EOF（假死）。
            const int nul = open("/dev/null", O_WRONLY);
            if (nul >= 0) {
                dup2(nul, 1);
                dup2(nul, 2);
            }
            execlp("Xvfb", "Xvfb", ":97", "-ac", "-screen", "0", "1280x720x24",
                   "-noreset", "+extension", "GLX", "+iglx", nullptr);
            _exit(1);
        }
        if (pid < 0) {
            LOG(FATAL) << "fork() failed";
        }
        setenv("DISPLAY", ":97", 1);
        sleep(1);
        if (!glfwInit()) {
            LOG(FATAL) << "glfwInit() failed after Xvfb launch";
        }
    }
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* win = glfwCreateWindow(w, h, "volumetric_vortex", nullptr, nullptr);
    CHECK(win != nullptr) << "glfwCreateWindow() failed";
    glfwMakeContextCurrent(win);
    return win;
}

unsigned int CompileShader(GLenum type, const char* src) {
    unsigned int s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOG(FATAL) << "shader compile failed: " << log;
    }
    return s;
}

unsigned int BuildProgram(const char* vs, const char* fs) {
    unsigned int p = glCreateProgram();
    unsigned int v = CompileShader(GL_VERTEX_SHADER, vs);
    unsigned int f = CompileShader(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    int ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        LOG(FATAL) << "program link failed: " << log;
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

// ============ 极简 mat4（列主序，GL 风格） ============
struct Mat4 {
    float m[16];  // column-major
};

Mat4 Identity() {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

Mat4 Mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) {
                s += a.m[k * 4 + row] * b.m[c * 4 + k];
            }
            r.m[c * 4 + row] = s;
        }
    }
    return r;
}

Mat4 Perspective(float fovy_rad, float aspect, float zn, float zf) {
    Mat4 r{};
    const float f = 1.0f / std::tan(fovy_rad * 0.5f);
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = (zf + zn) / (zn - zf);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * zf * zn) / (zn - zf);
    return r;
}

struct V3 {
    float x, y, z;
};
V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 Cross(V3 a, V3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Norm(V3 a) {
    float l = std::sqrt(Dot(a, a));
    return {a.x / l, a.y / l, a.z / l};
}

Mat4 LookAt(V3 eye, V3 center, V3 up) {
    V3 f = Norm(Sub(center, eye));
    V3 s = Norm(Cross(f, up));
    V3 u = Cross(s, f);
    Mat4 r = Identity();
    r.m[0] = s.x;  r.m[4] = s.y;  r.m[8]  = s.z;
    r.m[1] = u.x;  r.m[5] = u.y;  r.m[9]  = u.z;
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
    r.m[12] = -Dot(s, eye);
    r.m[13] = -Dot(u, eye);
    r.m[14] = Dot(f, eye);
    return r;
}

// ============ shaders ============
const char* kSceneVs = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
uniform mat4 uMVP;
out vec3 vWorld;
out vec3 vNormal;
void main() {
    vWorld = aPos;
    vNormal = aNormal;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)glsl";

// 场景颜色（简单 lambert 的暖灰）。
const char* kSceneFs = R"glsl(
#version 330 core
in vec3 vWorld;
in vec3 vNormal;
uniform vec3 uCamPos;
uniform vec3 uLightDir;
uniform vec3 uBaseColor;
out vec4 FragColor;
void main() {
    vec3 N = normalize(vNormal);
    float ndl = max(dot(N, normalize(uLightDir)), 0.0);
    vec3 c = uBaseColor * (0.25 + 0.75 * ndl);
    FragColor = vec4(c, 1.0);
}
)glsl";

// 场景线性深度：把「相机到该像素的距离」写进 RGBA32F 颜色纹理。
// （llvmpipe 下不能采样 depth texture，故用 float 颜色纹理代替。）
const char* kDepthVs = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
out vec3 vWorld;
void main() {
    vWorld = aPos;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)glsl";
const char* kDepthFs = R"glsl(
#version 330 core
in vec3 vWorld;
uniform vec3 uCamPos;
out vec4 FragColor;
void main() {
    FragColor = vec4(length(vWorld - uCamPos), 0.0, 0.0, 1.0);
}
)glsl";

const char* kVolVs = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uVP;
out vec3 vWorld;
void main() {
    vWorld = aPos;
    gl_Position = uVP * vec4(aPos, 1.0);
}
)glsl";

// 体积涡流碎片着色器：逐像素 ray-sphere + marching 程序化 3D 密度场。
const char* kVolFs = R"glsl(
#version 330 core
in vec3 vWorld;
out vec4 FragColor;

uniform vec3  uCamPos;
uniform vec3  uCenter;
uniform float uRadius;
uniform float uHeat;
uniform float uTime;
uniform float uSwirl;
uniform vec3  uColorCore;
uniform vec3  uColorOuter;
uniform vec2  uResolution;
uniform sampler2D uSceneDepth;   // .r = 线性相机距离

// ---- 3D value noise + fbm ----
float hash31(vec3 p) {
    p = fract(p * 0.3183099 + vec3(0.1, 0.2, 0.3));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float vnoise3(vec3 x) {
    vec3 i = floor(x), f = fract(x);
    vec3 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash31(i + vec3(0,0,0)), hash31(i + vec3(1,0,0)), u.x),
                   mix(hash31(i + vec3(0,1,0)), hash31(i + vec3(1,1,0)), u.x), u.y),
               mix(mix(hash31(i + vec3(0,0,1)), hash31(i + vec3(1,0,1)), u.x),
                   mix(hash31(i + vec3(0,1,1)), hash31(i + vec3(1,1,1)), u.x), u.y), u.z);
}
float fbm3(vec3 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 3; ++i) {
        v += a * vnoise3(p);
        p = p * 2.03 + vec3(1.7, 9.2, 3.1);
        a *= 0.5;
    }
    return v;
}

// ---- 涡流体密度（世界空间）：球包络 × 绕轴旋涡扭转 × 域扭曲 fbm ----
float density(vec3 p) {
    vec3 q = p - uCenter;
    float R = uRadius;
    float env = smoothstep(1.0, 0.15, length(q) / R);
    if (env <= 0.0) {
        return 0.0;
    }
    // 绕 Y 轴扭转：越靠底越强 → 涡卷。
    float h = clamp(q.y / R * 0.5 + 0.5, 0.0, 1.0);
    float ang = uSwirl * (1.0 - h) / (length(q.xz) / R + 0.22) + uTime * 0.9;
    float s = sin(ang), c = cos(ang);
    vec3 qq = vec3(q.x * c - q.z * s, q.y, q.x * s + q.z * c);
    vec3 pw = qq * (3.4 / R) + vec3(0.0, -uTime * 1.1, 0.0);
    // 域扭曲（curl-like）：先算一个 warp 向量再采密度。
    vec3 warp = vec3(fbm3(pw), fbm3(pw + vec3(5.2, 1.3, 2.7)),
                     fbm3(pw + vec3(9.1, 4.4, 7.7))) - 0.5;
    float d = fbm3(pw + warp * 1.5);
    return env * max(0.0, d - 0.34) * (0.55 + 0.9 * uHeat);
}

void main() {
    vec3 ro = uCamPos;
    vec3 rd = normalize(vWorld - uCamPos);
    vec3 oc = ro - uCenter;
    float R = uRadius;
    float b = dot(oc, rd);
    float cc = dot(oc, oc) - R * R;
    float disc = b * b - cc;
    if (disc <= 0.0) {
        discard;
    }
    float sq = sqrt(disc);
    float t0 = max(0.0, -b - sq);
    float t1 = -b + sq;
    if (t1 <= t0) {
        discard;
    }
    // 场景深度裁剪：立柱挡住的部分不画。
    vec2 uv = gl_FragCoord.xy / uResolution;
    float sceneT = texture(uSceneDepth, uv).r;
    float tmax = min(t1, sceneT);
    if (tmax <= t0) {
        discard;
    }

    const int N = 40;
    float dt = (tmax - t0) / float(N);
    vec3 acc = vec3(0.0);
    for (int i = 0; i < N; ++i) {
        float t = t0 + (float(i) + 0.5) * dt;
        vec3 p = ro + rd * t;
        float d = density(p);
        if (d > 0.0) {
            float heat = clamp(uHeat * (1.0 - 0.5 * length(p - uCenter) / R) * 1.4, 0.0, 1.0);
            vec3 tone = mix(uColorOuter, uColorCore, smoothstep(0.1, 0.8, heat));
            acc += tone * (d * dt * 7.0);
        }
    }
    FragColor = vec4(acc, 1.0);   // 加法混合（GL_ONE, GL_ONE）
}
)glsl";

// ---- 对照：旧的「Y 轴圆柱 billboard」+ 2D 软团（当前 JPOV 粒子就是这么画的）----
// 它只绕 Y 轴朝相机 → 从正上方看是**侧立**的，被看成一条纸片边缘。
const char* kBillVs = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
uniform mat4 uVP;
out vec2 vUv;
void main() {
    vUv = aUv;
    gl_Position = uVP * vec4(aPos, 1.0);
}
)glsl";
const char* kBillFs = R"glsl(
#version 330 core
in vec2 vUv;
out vec4 FragColor;
uniform float uHeat;
uniform vec3  uColorCore;
uniform vec3  uColorOuter;
void main() {
    vec2 d = (vUv - 0.5) * 2.0;
    float r = length(d);
    float m = 1.0 - smoothstep(0.25, 1.0, r);
    vec3 tone = mix(uColorOuter, uColorCore, smoothstep(0.1, 0.8, uHeat));
    FragColor = vec4(tone * m * 0.8, 1.0);
}
)glsl";

// ============ 几何 ============
// 一个轴对齐盒（位置 + 法线），返回 36 顶点 * 6 float。
std::vector<float> MakeBox(float hx, float hy, float hz, V3 center) {
    const V3 c = center;
    const float x0 = c.x - hx, x1 = c.x + hx;
    const float y0 = c.y - hy, y1 = c.y + hy;
    const float z0 = c.z - hz, z1 = c.z + hz;
    struct Face { V3 n; V3 a, b, cc, d; };
    const Face faces[6] = {
        {{0, 0, 1}, {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}},
        {{0, 0, -1}, {x1, y0, z0}, {x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}},
        {{1, 0, 0}, {x1, y0, z1}, {x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}},
        {{-1, 0, 0}, {x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}},
        {{0, 1, 0}, {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, {x0, y1, z0}},
        {{0, -1, 0}, {x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}},
    };
    std::vector<float> out;
    for (const Face& f : faces) {
        const V3 tri[6] = {f.a, f.b, f.cc, f.a, f.cc, f.d};
        for (const V3& v : tri) {
            out.insert(out.end(), {v.x, v.y, v.z, f.n.x, f.n.y, f.n.z});
        }
    }
    return out;
}

std::vector<float> MakeGround(float half, int seg) {
    std::vector<float> out;
    const float step = 2.0f * half / static_cast<float>(seg);
    for (int i = 0; i < seg; ++i) {
        for (int j = 0; j < seg; ++j) {
            const float x0 = -half + i * step, x1 = x0 + step;
            const float z0 = -half + j * step, z1 = z0 + step;
            const float quad[6][3] = {
                {x0, 0, z1}, {x1, 0, z1}, {x1, 0, z0},
                {x0, 0, z1}, {x1, 0, z0}, {x0, 0, z0}};
            for (const auto& v : quad) {
                out.insert(out.end(), {v[0], v[1], v[2], 0.0f, 1.0f, 0.0f});
            }
        }
    }
    return out;
}

// ============ 粒子（涡流）模拟（CPU，确定性） ============
struct Particle {
    V3 pos, vel;
    float age = 0.0f, life = 1.0f, size = 0.2f, grow = 1.6f, phase = 0.0f;
    bool alive = false;
};

uint32_t HashMix(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
float Hash2Float(uint32_t a, uint32_t b) {
    const uint32_t h = HashMix(a * 2654435761u + b * 40503u);
    return static_cast<float>(h >> 8) / 16777216.0f;
}

std::vector<Particle> g_particles;
uint32_t g_seed = 1;

void SpawnOne() {
    for (Particle& p : g_particles) {
        if (p.alive) {
            continue;
        }
        const uint32_t s = g_seed++;
        const float r1 = Hash2Float(s, 1), r2 = Hash2Float(s, 2);
        const float r3 = Hash2Float(s, 3), r4 = Hash2Float(s, 4);
        const float theta = 6.2831853f * r1;
        const float rad = 0.28f + 0.14f * r3;
        p.pos = {rad * std::cos(theta), 0.08f, rad * std::sin(theta)};
        p.vel = {0.18f * (2 * r1 - 1), 0.55f + 0.45f * r2, 0.18f * (2 * r4 - 1)};
        p.age = 0.0f;
        p.life = 1.4f + 0.8f * r2;
        p.size = 0.16f + 0.07f * r3;
        p.grow = 1.5f + 0.9f * r1;
        p.phase = 20.0f * r4;
        p.alive = true;
        return;
    }
}

void Step(float dt) {
    static float acc = 0.0f;
    acc += 130.0f * dt;
    while (acc >= 1.0f) {
        acc -= 1.0f;
        SpawnOne();
    }
    for (Particle& p : g_particles) {
        if (!p.alive) {
            continue;
        }
        p.age += dt;
        if (p.age >= p.life) {
            p.alive = false;
            continue;
        }
        const float t = p.age / p.life;
        p.vel.y += 1.6f * (1.0f - t) * dt;
        p.pos = {p.pos.x + p.vel.x * dt, p.pos.y + p.vel.y * dt, p.pos.z + p.vel.z * dt};
    }
}

// ============ 渲染辅助 ============
unsigned int MakeVB() {
    unsigned int v = 0;
    glGenBuffers(1, &v);
    return v;
}

void Upload(float vbo, const std::vector<float>& data) {
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, data.size() * sizeof(float), data.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                          (void*)(3 * sizeof(float)));
}

// 每颗粒子的包围 quad（完整朝向相机：右 = cam right，上 = cam up）。
std::vector<float> BlobQuad(V3 center, float R, V3 right, V3 up) {
    const float e = R * 1.25f;
    const V3 bl{center.x - right.x * e - up.x * e, center.y - right.y * e - up.y * e,
                center.z - right.z * e - up.z * e};
    const V3 br{center.x + right.x * e - up.x * e, center.y + right.y * e - up.y * e,
                center.z + right.z * e - up.z * e};
    const V3 tr{center.x + right.x * e + up.x * e, center.y + right.y * e + up.y * e,
                center.z + right.z * e + up.z * e};
    const V3 tl{center.x - right.x * e + up.x * e, center.y - right.y * e + up.y * e,
                center.z - right.z * e + up.z * e};
    std::vector<float> out;
    const V3 tri[6] = {bl, br, tr, bl, tr, tl};
    for (const V3& v : tri) {
        out.insert(out.end(), {v.x, v.y, v.z, 0.0f, 0.0f, 0.0f});
    }
    return out;
}

// 旧版「直立圆柱 billboard」：底边中点在 base，右向量在水平面内垂直于视线。
// 顶点带 uv（供 2D 软团）。
std::vector<float> BillQuadUp(V3 base, V3 center, float R, float aspect) {
    const V3 to_p{center.x - base.x, 0.0f, center.z - base.z};
    const float hlen = std::sqrt(to_p.x * to_p.x + to_p.z * to_p.z);
    V3 right{1.0f, 0.0f, 0.0f};
    if (hlen > 1e-6f) {
        right = {to_p.z / hlen, 0.0f, -to_p.x / hlen};
    }
    const float hw = R;
    const float h = R * aspect;
    const V3 rx{right.x * hw, 0.0f, right.z * hw};
    const V3 bl{base.x - rx.x, base.y, base.z - rx.z};
    const V3 br{base.x + rx.x, base.y, base.z + rx.z};
    const V3 tr{br.x, br.y + h, br.z};
    const V3 tl{bl.x, bl.y + h, bl.z};
    const float uv[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
    const V3 tri[6] = {bl, br, tr, bl, tr, tl};
    std::vector<float> out;
    for (int i = 0; i < 6; ++i) {
        out.insert(out.end(), {tri[i].x, tri[i].y, tri[i].z, uv[i][0], uv[i][1]});
    }
    return out;
}

struct Fbo {
    unsigned int fbo = 0, color = 0, depth_rb = 0, depth_f = 0;
};

Fbo MakeSceneFbo(int w, int h, bool with_float_depth) {
    Fbo f;
    glGenFramebuffers(1, &f.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, f.fbo);
    glGenTextures(1, &f.color);
    glBindTexture(GL_TEXTURE_2D, f.color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f.color, 0);
    glGenRenderbuffers(1, &f.depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, f.depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, f.depth_rb);
    if (with_float_depth) {
        glGenTextures(1, &f.depth_f);
        glBindTexture(GL_TEXTURE_2D, f.depth_f);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D,
                               f.depth_f, 0);
    }
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);
    return f;
}

void SavePngFlipped(const std::string& path, int w, int h, const std::vector<float>& rgb) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t si = (static_cast<size_t>(y) * w + x) * 3;
            const size_t di = (static_cast<size_t>(h - 1 - y) * w + x) * 3;
            for (int c = 0; c < 3; ++c) {
                float v = rgb[si + c];
                v = v / (1.0f + v);                       // Reinhard
                v = std::pow(std::max(0.0f, v), 1.0f / 2.2f);  // 粗 sRGB
                px[di + c] = static_cast<uint8_t>(std::min(255.0f, v * 255.0f + 0.5f));
            }
        }
    }
    stbi_write_png(path.c_str(), w, h, 3, px.data(), w * 3);
}

}  // namespace

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    std::string out_dir = "/tmp/vol_vortex";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--out") == 0) {
            out_dir = argv[i + 1];
        }
    }
    if (argc >= 2 && argv[1][0] != '-') {
        out_dir = argv[1];
    }

    const int W = 960, H = 540;
    GLFWwindow* win = BootGL(W, H);
    (void)win;
    LOG(INFO) << "GL: " << glGetString(GL_VERSION);
    // core profile 必须有 VAO 才能设置顶点属性 / draw。
    unsigned int vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    const unsigned int scene_prog = BuildProgram(kSceneVs, kSceneFs);
    const unsigned int depth_prog = BuildProgram(kDepthVs, kDepthFs);
    const unsigned int vol_prog   = BuildProgram(kVolVs, kVolFs);
    const unsigned int bill_prog  = BuildProgram(kBillVs, kBillFs);

    const unsigned int ground_vbo = MakeVB();
    const unsigned int box_vbo = MakeVB();
    Upload(ground_vbo, MakeGround(12.0f, 24));
    Upload(box_vbo, MakeBox(0.30f, 1.50f, 0.30f, {0.0f, 1.50f, 0.0f}));
    const int ground_verts = 24 * 24 * 6;
    const int box_verts = 36;

    const unsigned int quad_vbo = MakeVB();

    Fbo scene = MakeSceneFbo(W, H, /*with_float_depth=*/false);
    // 线性距离写在 color（RGBA16F）里，充当"场景深度场"供体积 pass 采样
    //（llvmpipe 下不能采样 depth texture）。
    Fbo depthbuf = MakeSceneFbo(W, H, /*with_float_depth=*/false);

    // 涡流粒子池 + 预热到稳态。
    g_particles.resize(400);
    for (int i = 0; i < 360; ++i) {   // 6 秒 @ 60Hz
        Step(1.0f / 60.0f);
    }
    size_t alive = 0;
    for (const Particle& p : g_particles) {
        if (p.alive) {
            ++alive;
        }
    }
    LOG(INFO) << "alive vortex blobs = " << alive;

    auto draw_scene = [&](const Mat4& vp, const Mat4& view, V3 cam) {
        (void)view;
        // 颜色 pass
        glBindFramebuffer(GL_FRAMEBUFFER, scene.fbo);
        glViewport(0, 0, W, H);
        glClearColor(0.02f, 0.03f, 0.05f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glUseProgram(scene_prog);
        glUniformMatrix4fv(glGetUniformLocation(scene_prog, "uMVP"), 1, GL_FALSE, vp.m);
        glUniform3f(glGetUniformLocation(scene_prog, "uCamPos"), cam.x, cam.y, cam.z);
        glUniform3f(glGetUniformLocation(scene_prog, "uLightDir"), 0.4f, 0.8f, 0.45f);
        glBindBuffer(GL_ARRAY_BUFFER, ground_vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                              (void*)(3 * sizeof(float)));
        glUniform3f(glGetUniformLocation(scene_prog, "uBaseColor"), 0.32f, 0.33f, 0.35f);
        glDrawArrays(GL_TRIANGLES, 0, ground_verts);
        glBindBuffer(GL_ARRAY_BUFFER, box_vbo);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                              (void*)(3 * sizeof(float)));
        glUniform3f(glGetUniformLocation(scene_prog, "uBaseColor"), 0.45f, 0.34f, 0.22f);
        glDrawArrays(GL_TRIANGLES, 0, box_verts);
    };

    auto draw_depth = [&](const Mat4& vp, V3 cam) {
        glBindFramebuffer(GL_FRAMEBUFFER, depthbuf.fbo);
        glViewport(0, 0, W, H);
        glClearColor(1000.0f, 0.0f, 0.0f, 1.0f);   // 远处 = 大值（无遮挡）
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_DEPTH_TEST);
        glUseProgram(depth_prog);
        glUniformMatrix4fv(glGetUniformLocation(depth_prog, "uMVP"), 1, GL_FALSE, vp.m);
        glUniform3f(glGetUniformLocation(depth_prog, "uCamPos"), cam.x, cam.y, cam.z);
        glBindBuffer(GL_ARRAY_BUFFER, ground_vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
        glDrawArrays(GL_TRIANGLES, 0, ground_verts);
        glBindBuffer(GL_ARRAY_BUFFER, box_vbo);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
        glDrawArrays(GL_TRIANGLES, 0, box_verts);
    };

    auto draw_volumetric = [&](const Mat4& vp, V3 cam, V3 right, V3 up, float t_sec) {
        glBindFramebuffer(GL_FRAMEBUFFER, scene.fbo);
        glViewport(0, 0, W, H);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);        // 加法（火是发光体 → 免排序）
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glUseProgram(vol_prog);
        glUniformMatrix4fv(glGetUniformLocation(vol_prog, "uVP"), 1, GL_FALSE, vp.m);
        glUniform3f(glGetUniformLocation(vol_prog, "uCamPos"), cam.x, cam.y, cam.z);
        glUniform2f(glGetUniformLocation(vol_prog, "uResolution"), (float)W, (float)H);
        glUniform3f(glGetUniformLocation(vol_prog, "uColorCore"), 1.0f, 0.88f, 0.52f);
        glUniform3f(glGetUniformLocation(vol_prog, "uColorOuter"), 0.80f, 0.12f, 0.02f);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, depthbuf.color);
        glUniform1i(glGetUniformLocation(vol_prog, "uSceneDepth"), 0);
        for (const Particle& p : g_particles) {
            if (!p.alive) {
                continue;
            }
            const float tt = p.age / p.life;
            const float size_mul = 1.0f + (p.grow - 1.0f) * std::sin(tt * 3.14159f);
            const float R = p.size * size_mul;
            const float heat = std::max(0.0f, 1.0f - 0.95f * tt);
            std::vector<float> q = BlobQuad(p.pos, R, right, up);
            glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
            glBufferData(GL_ARRAY_BUFFER, q.size() * sizeof(float), q.data(), GL_DYNAMIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
            glUniform3f(glGetUniformLocation(vol_prog, "uCenter"), p.pos.x, p.pos.y, p.pos.z);
            glUniform1f(glGetUniformLocation(vol_prog, "uRadius"), R);
            glUniform1f(glGetUniformLocation(vol_prog, "uHeat"), heat);
            glUniform1f(glGetUniformLocation(vol_prog, "uTime"), t_sec + p.phase);
            glUniform1f(glGetUniformLocation(vol_prog, "uSwirl"), 2.2f);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }
        glDisable(GL_BLEND);
    };

    // 对照组：旧 Y 轴 billboard（同一批粒子、同一时刻）。
    auto draw_billboards = [&](const Mat4& vp, V3 cam, float t_sec) {
        glBindFramebuffer(GL_FRAMEBUFFER, scene.fbo);
        glViewport(0, 0, W, H);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glUseProgram(bill_prog);
        glUniformMatrix4fv(glGetUniformLocation(bill_prog, "uVP"), 1, GL_FALSE, vp.m);
        glUniform3f(glGetUniformLocation(bill_prog, "uColorCore"), 1.0f, 0.88f, 0.52f);
        glUniform3f(glGetUniformLocation(bill_prog, "uColorOuter"), 0.80f, 0.12f, 0.02f);
        (void)t_sec;
        for (const Particle& p : g_particles) {
            if (!p.alive) {
                continue;
            }
            const float tt = p.age / p.life;
            const float size_mul = 1.0f + (p.grow - 1.0f) * std::sin(tt * 3.14159f);
            const float R = p.size * size_mul;
            const float heat = std::max(0.0f, 1.0f - 0.95f * tt);
            std::vector<float> q = BillQuadUp(p.pos, cam, R, 2.2f);
            glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
            glBufferData(GL_ARRAY_BUFFER, q.size() * sizeof(float), q.data(), GL_DYNAMIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                                  (void*)(3 * sizeof(float)));
            glUniform1f(glGetUniformLocation(bill_prog, "uHeat"), heat);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }
        glDisable(GL_BLEND);
    };

    auto capture = [&](const char* name, V3 eye, V3 center, V3 up) {
        const Mat4 view = LookAt(eye, center, up);
        const Mat4 proj = Perspective(60.0f * 3.14159265f / 180.0f,
                                      (float)W / (float)H, 0.05f, 100.0f);
        const Mat4 vp = Mul(proj, view);
        // 相机右/上（world 空间）：从 view 矩阵第一行/第二行取。
        V3 right{view.m[0], view.m[4], view.m[8]};
        V3 camup{view.m[1], view.m[5], view.m[9]};
        draw_scene(vp, view, eye);
        draw_depth(vp, eye);
        const bool want_vol = (std::strstr(name, "vol") != nullptr);
        if (want_vol) {
            draw_volumetric(vp, eye, right, camup, 2.7f);
        } else {
            draw_billboards(vp, eye, 2.7f);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, scene.fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        std::vector<float> rgb(static_cast<size_t>(W) * H * 3);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, W, H, GL_RGB, GL_FLOAT, rgb.data());
        const std::string path = out_dir + "/" + name + ".png";
        SavePngFlipped(path, W, H, rgb);
        LOG(INFO) << "saved " << path;
    };

    capture("vol_side", {4.6f, 2.2f, 4.6f}, {0.0f, 1.3f, 0.0f}, {0.0f, 1.0f, 0.0f});
    capture("vol_top", {0.0f, 8.5f, 0.35f}, {0.0f, 0.9f, 0.0f}, {0.0f, 0.0f, 1.0f});
    // 对照组（旧 Y 轴 billboard，同样两个机位）。
    capture("bill_side", {4.6f, 2.2f, 4.6f}, {0.0f, 1.3f, 0.0f}, {0.0f, 1.0f, 0.0f});
    capture("bill_top", {0.0f, 8.5f, 0.35f}, {0.0f, 0.9f, 0.0f}, {0.0f, 0.0f, 1.0f});

    glfwTerminate();
    LOG(INFO) << "done";
    return 0;
}
