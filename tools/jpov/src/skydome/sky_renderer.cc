// JPOV SkyRenderer 实现
//
// 程序化天光背景：独立 program，全屏三角形 + 相机逆 VP 重建视线方向
// → Preetham 大气模型算天空色 + 地平线下地色 + 日月发光圆盘。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/skydome/sky_renderer.h"

#include <cmath>

// GL 头文件必须最先 include（在 MinGW #define 宏替换之前）
#ifdef _WIN32
#include <GL/gl.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#ifdef _WIN32
// MinGW: windows.h 定义 ERROR 宏与 glog 冲突，必须在 glog 之前 suppress
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

// Windows/MinGW: windef.h 定义 near/far 宏，与 Camera::near/far 字段冲突。
#ifdef _WIN32
#undef near
#undef far
#endif

#include <glog/logging.h>

// ==================== 匿名空间：矩阵工具 ====================
namespace {

// 4x4 列主序矩阵乘法 out = a * b。
void Mat4Mul(const float a[16], const float b[16], float out[16]) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[k*4 + r] * b[c*4 + k];
            }
            out[c*4 + r] = sum;
        }
    }
}

// 透视投影（列主序），与 Primitives3DRenderer::BuildPerspProj 一致。
void BuildPersp(float fov_y, float aspect, float near_, float far_, float out[16]) {
    const float f = 1.0f / std::tan(fov_y * 0.5f);
    const float range_inv = 1.0f / (near_ - far_);
    out[0] = f / aspect; out[4] = 0.0f; out[8] = 0.0f; out[12] = 0.0f;
    out[1] = 0.0f; out[5] = f; out[9] = 0.0f; out[13] = 0.0f;
    out[2] = 0.0f; out[6] = 0.0f; out[10] = (near_ + far_) * range_inv; out[14] = 2.0f * near_ * far_ * range_inv;
    out[3] = 0.0f; out[7] = 0.0f; out[11] = -1.0f; out[15] = 0.0f;
}

// 列主序 lookAt（转置布局，与 Primitives3DRenderer::BuildMVP / renderer.cc BuildLookAt 同一套约定）。
void BuildLookAt(const float eye[3], const float center[3], const float up[3], float out[16]) {
    float fx = center[0]-eye[0], fy = center[1]-eye[1], fz = center[2]-eye[2];
    float fl = std::sqrt(fx*fx+fy*fy+fz*fz);
    if (fl < 1e-8f) { fx = 0.0f; fy = 0.0f; fz = -1.0f; } else { fx/=fl; fy/=fl; fz/=fl; }
    float sx = fy*up[2]-fz*up[1], sy = fz*up[0]-fx*up[2], sz = fx*up[1]-fy*up[0];
    float sl = std::sqrt(sx*sx+sy*sy+sz*sz);
    if (sl < 1e-8f) { sx = 1.0f; sy = 0.0f; sz = 0.0f; } else { sx/=sl; sy/=sl; sz/=sl; }
    float ux = sy*fz - sz*fy, uy = sz*fx - sx*fz, uz = sx*fy - sy*fx;
    out[0]=sx; out[1]=ux; out[2]=-fx; out[3]=0.0f;
    out[4]=sy; out[5]=uy; out[6]=-fy; out[7]=0.0f;
    out[8]=sz; out[9]=uz; out[10]=-fz; out[11]=0.0f;
    out[12]=-(sx*eye[0]+sy*eye[1]+sz*eye[2]);
    out[13]=-(ux*eye[0]+uy*eye[1]+uz*eye[2]);
    out[14]= (fx*eye[0]+fy*eye[1]+fz*eye[2]);
    out[15]=1.0f;
}

// 4x4 列主序求逆（通用高斯消元，返回是否可逆）。
bool Mat4Invert(const float m[16], float out[16]) {
    float a[16]; for (int i=0;i<16;i++) a[i]=m[i];
    float inv[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    for (int col=0; col<4; ++col) {
        int piv = col;
        float best = std::fabs(a[col*4+col]);
        for (int r=col+1; r<4; ++r) {
            float v = std::fabs(a[col*4+r]);
            if (v > best) { best = v; piv = r; }
        }
        if (best < 1e-12f) return false;
        if (piv != col) {
            for (int c=0;c<4;++c){ float t=a[c*4+col]; a[c*4+col]=a[c*4+piv]; a[c*4+piv]=t;
                                    t=inv[c*4+col]; inv[c*4+col]=inv[c*4+piv]; inv[c*4+piv]=t; }
        }
        float d = a[col*4+col];
        for (int c=0;c<4;++c){ a[c*4+col]/=d; inv[c*4+col]/=d; }
        for (int r=0;r<4;++r){
            if (r==col) continue;
            float f = a[col*4+r];
            for (int c=0;c<4;++c){ a[c*4+r]-=f*a[c*4+col]; inv[c*4+r]-=f*inv[c*4+col]; }
        }
    }
    for (int i=0;i<16;++i) out[i]=inv[i];
    return true;
}

}  // anonymous namespace

// ==================== SkyRenderer ====================

namespace jpov {

void SkyRenderer::DrawSky(const DaySkyCommand& sky_cmd,
                          const Camera& cam, int fbo_w, int fbo_h,
                          ShaderManager& shader_mgr) {
    // ---- 构建 相机 逆(Proj*View) ----
    const float aspect = static_cast<float>(fbo_w) / static_cast<float>(std::max(fbo_h,1));
    const float fov_rad = cam.fov * 3.14159265358979323846f / 180.0f;

    const float eye[3] = {cam.position.x(), cam.position.y(), cam.position.z()};
    const float tgt[3] = {cam.target.x(), cam.target.y(), cam.target.z()};
    const float upv[3] = {cam.up.x(), cam.up.y(), cam.up.z()};

    float proj[16], view[16], vp[16];
    BuildPersp(fov_rad, aspect, cam.near, cam.far, proj);
    BuildLookAt(eye, tgt, upv, view);
    Mat4Mul(proj, view, vp);

    float inv_vp[16];
    CHECK(Mat4Invert(vp, inv_vp))
        << "DrawSky: 相机矩阵不可逆（近/远裁剪面非法或退化）";

    // 太阳位置（sun_dir 是 y-up 单位向量，直接上传）

    // ---- 渲染（全屏三角形，画当前绑定 FBO 的整张背景）----
    unsigned int prog = shader_mgr.GetOrCreate("sky", {kSkyVs, kSkyFs});
    glUseProgram(prog);

    glUniformMatrix4fv(shader_mgr.GetUniform(prog, "uInvVP"), 1, GL_FALSE, inv_vp);
    glUniform2f(shader_mgr.GetUniform(prog, "uResolution"),
                static_cast<float>(fbo_w), static_cast<float>(fbo_h));
    glUniform3f(shader_mgr.GetUniform(prog, "uCamPos"), eye[0], eye[1], eye[2]);
    glUniform3f(shader_mgr.GetUniform(prog, "uSunDir"),
                sky_cmd.sun_dir.x(), sky_cmd.sun_dir.y(), sky_cmd.sun_dir.z());
    glUniform1f(shader_mgr.GetUniform(prog, "uTurbidity"), sky_cmd.turbidity);
    glUniform3f(shader_mgr.GetUniform(prog, "uSeason"),
                sky_cmd.season.r, sky_cmd.season.g, sky_cmd.season.b);
    glUniform1f(shader_mgr.GetUniform(prog, "uIntensity"), sky_cmd.intensity);
    glUniform3f(shader_mgr.GetUniform(prog, "uGroundColor"),
                sky_cmd.ground_color.r, sky_cmd.ground_color.g, sky_cmd.ground_color.b);

    // 画全屏三角形（无 VAO/VBO，用 gl_VertexID 内建变量）
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glUseProgram(0);
}

}  // namespace jpov
