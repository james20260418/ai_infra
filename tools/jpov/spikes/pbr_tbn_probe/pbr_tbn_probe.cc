// JPOV PBR — 阶段0 TBN 探针（临时 spike，验证后删除）
//
// 验证问题：当前 GL 环境（Xvfb + GLFW hidden window + GL 330 core）能否
// 运行 complete 的 normal map TBN 管线：
//   1. RGBA8 法线贴图能否被采样（GL_RGBA8 + GL_UNSIGNED_BYTE）
//   2. GLSL 能否构建 TBN 矩阵（mat3 / cross / normalize / transpose/inverse）
//   3. 用扰动法线做光照是否能产生与 flat normal 不同的、非退化的结果
//
// 本探针完全独立于 renderer.cc 的生产 shader / DrawObject3D，
// 自建 GL context + 自带 shader + 自带 mesh（position/normal/uv/tangent）。
// 渲染一块带 UV + tangent 的方块，采样 procedural RGBA8 法线贴图：
//   - 贴图左半 = flat normal (0.5,0.5,1.0)
//   - 贴图右半 = 凸起 bump（normal.z 偏斜）
// 用统一方向光做 Blinn-Phong 光照，比较 flat / bump 两区亮度差异，
// 若差异显著且输出非退化 → TBN 可行（VALIDATED）。
//
// 运行：bazel run //tools/jpov/spikes/pbr_tbn_probe
// 输出：渲染 PNG + 判定结果写到 stdout & verdict 文件。

#define GL_GLEXT_PROTOTYPES

#include <GLFW/glfw3.h>
#include <GL/gl.h>
#include <GL/glext.h>

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

// ============ 精简 headless GL context 引导（复用 jpov.cc 的模式） ============
GLFWwindow* BootGL(int w, int h) {
    if (!glfwInit()) {
        int pid = fork();
        if (pid == 0) {
            execlp("Xvfb", "Xvfb", ":99", "-ac", "-screen", "0", "1280x720x24",
                   "-noreset", "+extension", "GLX", "+iglx", nullptr);
            _exit(1);
        }
        if (pid < 0) {
            LOG(FATAL) << "fork() failed";
        }
        setenv("DISPLAY", ":99", 1);
        sleep(1);
        if (!glfwInit()) {
            LOG(FATAL) << "glfwInit() failed after Xvfb launch";
        }
        LOG(INFO) << "Xvfb 已启动 :99";
    } else {
        const char* d = getenv("DISPLAY");
        LOG(INFO) << "glfwInit() OK, DISPLAY=" << (d ? d : "(null)");
    }

    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* win = glfwCreateWindow(w, h, "pbr_tbn_probe", nullptr, nullptr);
    CHECK(win != nullptr) << "glfwCreateWindow() failed";
    glfwMakeContextCurrent(win);
    return win;
}

// ============ shader 编译 ============
unsigned int CompileShader(GLenum type, const char* src) {
    unsigned int s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
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
        char log[2048];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        LOG(FATAL) << "program link failed: " << log;
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

const char* kVs = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aTangent;

uniform mat4 uMVP;
uniform mat3 uNormalMat;   // 世界空间，标度一致时可用 model 的旋转部分

out vec3 vWorldPos;
out vec3 vWorldNormal;
out vec2 vUV;
out vec3 vTangent;

void main() {
    vWorldPos = aPos;
    vWorldNormal = normalize(uNormalMat * aNormal);
    vTangent = normalize(uNormalMat * aTangent);
    vUV = aUV;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)glsl";

const char* kFs = R"glsl(
#version 330 core
in vec3 vWorldPos;
in vec3 vWorldNormal;
in vec2 vUV;
in vec3 vTangent;

uniform sampler2D uNormalMap;   // RGBA8
uniform vec3 uLightDir;         // 世界空间，指向光源
uniform vec3 uViewPos;

out vec4 FragColor;

void main() {
    // 采样法线贴图（RGBA8 线性，范围 [0,1] → [-1,1]）
    vec3 texNormal = texture(uNormalMap, vUV).rgb * 2.0 - 1.0;
    texNormal = normalize(texNormal);

    // 构建 TBN（tangent 已归一化；bitangent = cross(N, T)，右手系）
    vec3 N = normalize(vWorldNormal);
    vec3 T = normalize(vTangent - dot(vTangent, N) * N);  // Gram-Schmidt 正交化
    vec3 B = normalize(cross(N, T));
    mat3 TBN = mat3(T, B, N);

    // TBN 变换：把切线空间法线转到世界空间
    vec3 worldN = normalize(TBN * texNormal);

    // 方向光 Blinn-Phong（仅验证法线方向影响，不做复杂 BRDF）
    vec3 L = normalize(uLightDir);
    vec3 V = normalize(uViewPos - vWorldPos);
    vec3 H = normalize(L + V);

    float NdotL = max(dot(worldN, L), 0.0);
    float NdotH = max(dot(worldN, H), 0.0);
    vec3 baseColor = vec3(0.6, 0.6, 0.6);
    vec3 diff = baseColor * NdotL;
    vec3 spec = vec3(1.0) * pow(NdotH, 64.0) * 0.5;
    vec3 amb  = vec3(0.08, 0.08, 0.1);

    FragColor = vec4(amb + diff + spec, 1.0);
}
)glsl";

// ============ 程序化 RGBA8 法线贴图 ============
// 左半 flat (0.5,0.5,1.0)，右半 bump（x 向左/右偏斜模拟凹坑/凸起）
// 返回 RGBA 像素（width*height*4）
std::vector<uint8_t> MakeNormalMap(int w, int h) {
    std::vector<uint8_t> px(w * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float u = static_cast<float>(x) / static_cast<float>(w);
            uint8_t nx, ny, nz;
            if (u < 0.5f) {
                // 左半：flat normal (0,0,1) → 存储 (0.5,0.5,1.0)
                nx = 128; ny = 128; nz = 255;
            } else {
                // 右半：bump — 围绕 y 轴倾斜，制造明显的法线扰动
                float t = (u - 0.5f) * 2.0f;  // [0,1]
                float ang = (t - 0.5f) * 1.2f; // 只在一个方向旋转出坡度突跳
                nx = static_cast<uint8_t>(128 + 120.0f * std::sin(ang + 0.0f));
                nz = static_cast<uint8_t>(128 + 120.0f * std::cos(ang + 0.0f));
                ny = 128;
            }
            size_t i = (static_cast<size_t>(y) * w + x) * 4;
            px[i + 0] = nx;
            px[i + 1] = ny;
            px[i + 2] = nz;
            px[i + 3] = 255;
        }
    }
    return px;
}

// ============ 带有 position/normal/uv/tangent 的方块 ============
struct Vtx {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    float tx, ty, tz;
};

// 画一个 +Z 面朝向相机的方块（XY 平面），法线 +Z，
// tangent 沿 +X，bitangent 沿 +Y。用 center 平移。
void BuildQuad(Vtx* out, float half, float center_x, float center_y,
               float center_z) {
    // 4 顶点 (顺时针 CCW 视向 +Z)
    const float N[3] = {0, 0, 1};
    const float T[3] = {1, 0, 0};
    const float c[3] = {center_x, center_y, center_z};
    const float h = half;
    float corners[4][2] = {
        {-h, -h},  // (0,0)
        { h, -h},  // (1,0)
        { h,  h},  // (1,1)
        {-h,  h},  // (0,1)
    };
    float uv[4][2] = {
        {0, 0}, {1, 0}, {1, 1}, {0, 1},
    };
    for (int i = 0; i < 4; ++i) {
        out[i].px = corners[i][0] + c[0];
        out[i].py = corners[i][1] + c[1];
        out[i].pz = 0.0f + c[2];
        out[i].nx = N[0]; out[i].ny = N[1]; out[i].nz = N[2];
        out[i].u = uv[i][0]; out[i].v = uv[i][1];
        out[i].tx = T[0]; out[i].ty = T[1]; out[i].tz = T[2];
    }
}

// 两个三角形（indexed）
const unsigned int kIdx[6] = {0, 1, 2, 0, 2, 3};

// ============ 简单列主序矩阵工具（够用即可） ============
void MulM4(const float a[16], const float b[16], float out[16]) {
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k)
                s += a[k * 4 + row] * b[col * 4 + k];
            out[col * 4 + row] = s;
        }
}

// 读 PNG 像素的逻辑用于统计（这里直接分析 readPixels，不重编码）

}  // namespace

int main() {
    const int W = 256, H = 128;
    GLFWwindow* win = BootGL(W, H);

    // ---- program ----
    unsigned int prog = BuildProgram(kVs, kFs);
    glUseProgram(prog);

    // ---- VAO + VBO/EBO ----
    Vtx vtx[4];
    BuildQuad(vtx, 1.0f, 0.0f, 0.0f, 0.0f);

    unsigned int vao, vbo, ebo;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vtx), vtx, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(kIdx), kIdx, GL_STATIC_DRAW);

    size_t stride = sizeof(Vtx);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vtx, px));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vtx, nx));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vtx, u));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vtx, tx));
    glBindVertexArray(0);

    // ---- RGBA8 法线贴图 ----
    std::vector<uint8_t> nmp = MakeNormalMap(128, 64);
    unsigned int nmap;
    glGenTextures(1, &nmap);
    glBindTexture(GL_TEXTURE_2D, nmap);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 128, 64, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nmp.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // ---- MVP (ortho-ish): 方块在透视图里从斜上方看 ----
    float mvp[16] = {
         // 简单近距正交缩放：方块 Z=0 居中，X∈[-1,1] Y∈[-1,1]
         1.0f, 0, 0, 0,
         0,    1.0f, 0, 0,
         0,    0,   -1.0f, 0,
         0,    0,   -0.5f, 1.0f,
    };
    // 正交投影 + 模型恒等：直接用手工矩阵保证确定性
    float ortho[16] = {
        1.2f, 0,    0,    0,
        0,    1.2f, 0,    0,
        0,    0,    1.0f, 0,
        0,    0,    0,    1.0f,
    };
    MulM4(ortho, mvp, mvp);

    // 世界空间 uniform
    glUniformMatrix4fv(glGetUniformLocation(prog, "uMVP"), 1, GL_FALSE, mvp);
    // uNormalMat：恒等（模型无旋转缩放）
    float normalMat[9] = {1,0,0, 0,1,0, 0,0,1};
    glUniformMatrix3fv(glGetUniformLocation(prog, "uNormalMat"), 1, GL_FALSE, normalMat);
    // 方向光：从 +Z 略上方照向原点（正对方块正面）
    glUniform3f(glGetUniformLocation(prog, "uLightDir"), 0.3f, 0.4f, 1.0f);
    glUniform3f(glGetUniformLocation(prog, "uViewPos"), 0.0f, 0.0f, 5.0f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, nmap);
    glUniform1i(glGetUniformLocation(prog, "uNormalMap"), 0);

    // ---- 渲染到默认（隐藏窗口）framebuffer ----
    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glBindVertexArray(vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);

    glFinish();

    // ---- 读回像素 ----
    std::vector<uint8_t> px(W * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());

    // 保存 PNG（stb 翻转到屏幕方向，Probe 无窗口显示，随意）
    const std::string outpath = "./output/pbr_tbn_probe/probe.png";
    system("mkdir -p ./output/pbr_tbn_probe");
    stbi_flip_vertically_on_write(1);
    int ok = stbi_write_png(outpath.c_str(), W, H, 4, px.data(), W * 4);
    LOG(INFO) << "probe PNG write: " << (ok ? "OK" : "FAIL") << " -> " << outpath;

    // ---- 分析：比较左半(flat) / 右半(bump) 平均亮度 ----
    // 屏幕坐标：glReadPixels 原点在左下。方块占满视口，左半 flat、右半 bump。
    double sumL = 0, sumR = 0;
    int cntL = 0, cntR = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            size_t i = ((size_t)y * W + x) * 4;
            double lum = 0.299 * px[i+0] + 0.587 * px[i+1] + 0.114 * px[i+2];
            if (x < W / 2) { sumL += lum; cntL++; }
            else           { sumR += lum; cntR++; }
        }
    }
    double avgL = sumL / cntL;
    double avgR = sumR / cntR;
    bool degenerate = (avgL < 1.0 && avgR < 1.0);  // 全黑
    bool tbn_effect = (std::fabs(avgL - avgR) > 8.0);  // flat/bump 亮度差显著

    LOG(INFO) << "avg flat(左)=" << avgL << ", avg bump(右)=" << avgR;

    // ---- 判定 ----
    bool validated = !degenerate && (avgL > 2.0 || avgR > 2.0) && tbn_effect;
    LOG(INFO) << "=========== TBN 探针判定 ===========";
    LOG(INFO) << "GL 版本能力: sample-map + TBN mat3 + 扰动法线光照";
    LOG(INFO) << "非退化输出: " << (!degenerate ? "YES" : "NO");
    LOG(INFO) << "TBN 扰动可见(flat vs bump 亮度差)>8: " << (tbn_effect ? "YES" : "NO");
    LOG(INFO) << "VERDICT: " << (validated ? "VALIDATED" : "FAILED");

    // 写 verdict 到 data/（工作区约定）
    FILE* f = fopen("data/pbr_tbn_probe_verdict.md", "w");
    if (f) {
        fprintf(f, "# TBN 探针结论\n\n");
        fprintf(f, "- 平均亮度 flat(左)=%.2f  bump(右)=%.2f\n", avgL, avgR);
        fprintf(f, "- 非退化: %s\n", !degenerate ? "是" : "否");
        fprintf(f, "- TBN 扰动可见: %s\n", tbn_effect ? "是" : "否");
        fprintf(f, "- **结论: %s**\n", validated ? "GL 能跑 normal map TBN，阶段0可行" : "探针失败，需排查");
        fclose(f);
        LOG(INFO) << "verdict 写入 data/pbr_tbn_probe_verdict.md";
    }

    glfwDestroyWindow(win);
    glfwTerminate();
    return validated ? 0 : 1;
}
