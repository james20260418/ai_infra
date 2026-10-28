// JPOV 环境镜面（IBL specular）—— uniform 上传（GLSL 见 env_specular.h）

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/env_specular.h"

// GL 头文件必须最先 include（在 MinGW #define 宏替换之前）
// 注：cube map 的枚举值（GL_TEXTURE_CUBE_MAP 等）是 GL 1.3 引入的，MinGW 的
//     GL/gl.h 只到 1.1 ⇒ **两个平台都要包含 glext.h**（与 volume_manager.cc 同款）。
#include <GL/gl.h>
#include <GL/glext.h>

#ifdef _WIN32
// MinGW: windows.h 定义 ERROR 宏与 glog 冲突，必须在 glog 之前 suppress
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

#include <glog/logging.h>

#include "tools/jpov/src/texture_units.h"

namespace jpov {

void UploadEnvSpecular(ShaderManager& shader_mgr, unsigned int prog,
                       unsigned int env_cube_tex, float max_lod, float sky_scale,
                       bool enabled) {
    CHECK_GE(prog, 1u);
    // enabled=false 时允许 env_cube_tex==0（本帧根本没有 cubemap），只是关掉开关。
    CHECK(!enabled || env_cube_tex != 0u)
        << "UploadEnvSpecular: enabled=true 但没有天空 cubemap";
    CHECK_GE(sky_scale, 0.0f) << "EnvSpecular::sky_scale 必须 >= 0";
    CHECK_LE(sky_scale, 1.0f) << "EnvSpecular::sky_scale 必须 <= 1";

    glActiveTexture(GL_TEXTURE0 + kTexUnitEnvCube);
    glBindTexture(GL_TEXTURE_CUBE_MAP, env_cube_tex);

    glUseProgram(prog);
    glUniform1i(shader_mgr.GetUniform(prog, "uEnvEnabled"), enabled ? 1 : 0);
    glUniform1f(shader_mgr.GetUniform(prog, "uEnvSkyScale"), sky_scale);
    glUniform1f(shader_mgr.GetUniform(prog, "uEnvMaxLod"), max_lod);
    // sampler uniform = 纹理单元号（location 为 -1 时 glUniform 是 no-op，安全）
    glUniform1i(shader_mgr.GetUniform(prog, "uEnvCube"), kTexUnitEnvCube);

    // 恢复活动单元到 0：调用方（各 Draw*）会自己 glActiveTexture，但把状态改了就走的
    // 习惯容易被后来的人当成“默认就是 17”。这里显式复位，不依赖后续代码碰巧都显式设。
    glActiveTexture(GL_TEXTURE0);
}

}  // namespace jpov
