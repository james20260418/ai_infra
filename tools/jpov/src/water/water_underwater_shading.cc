// 见 water_underwater_shading.h。此处只做 uniform 上传（三个 PBR 主 pass 共用）。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/water/water_underwater_shading.h"

#include <GL/gl.h>
#include <GL/glext.h>

#ifdef _WIN32
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

#include <glog/logging.h>

namespace jpov {
namespace water {

namespace {

// K_d = σa + (1−g)·σs（逐通道）—— 环境光的“折合衰减”。
// 与 shading 片段里的注释同义：这是对漫射场的近似衰减率（不是 RTE 渐近本征值）。
Vec3f DiffuseAttenuation(const WaterMediumSample& m) {
    const float factor = 1.0f - m.phase_g;
    return {m.sigma_a.x() + factor * m.sigma_s.x(),
            m.sigma_a.y() + factor * m.sigma_s.y(),
            m.sigma_a.z() + factor * m.sigma_s.z()};
}

}  // namespace

void UploadUnderwaterShading(ShaderManager& shader_mgr, unsigned int prog,
                             const WaterMediumSample* medium) {
    CHECK(prog != 0u);
    // 与 UploadAmbient/UploadSunData 一致：本函数自行绑定 program（uniform 是 program 状态）。
    glUseProgram(prog);
    const auto loc = [&shader_mgr, prog](const char* name) {
        return shader_mgr.GetUniform(prog, name);
    };

    if (medium == nullptr) {
        // 关水体：全部置恒等（不许留上一帧的残值 —— 否则切场景时水会在陆地上生效）。
        glUniform1i(loc("uWaterShadeOn"), 0);
        glUniform1f(loc("uWaterShadeLevelY"), 0.0f);
        glUniform1f(loc("uWaterShadeIor"), kWaterIorDefault);
        glUniform3f(loc("uWaterShadeSigmaT"), 0.0f, 0.0f, 0.0f);
        glUniform3f(loc("uWaterShadeKd"), 0.0f, 0.0f, 0.0f);
        glUniform3f(loc("uWaterShadeSunToward"), 0.0f, 1.0f, 0.0f);
        glUniform1f(loc("uWaterShadeAmbientT"), 0.0f);
        return;
    }

    const Vec3f st = {medium->sigma_a.x() + medium->sigma_s.x(),
                      medium->sigma_a.y() + medium->sigma_s.y(),
                      medium->sigma_a.z() + medium->sigma_s.z()};
    const Vec3f kd = DiffuseAttenuation(*medium);
    glUniform1i(loc("uWaterShadeOn"), 1);
    glUniform1f(loc("uWaterShadeLevelY"), medium->level_y);
    glUniform1f(loc("uWaterShadeIor"), medium->ior);
    glUniform3f(loc("uWaterShadeSigmaT"), st.x(), st.y(), st.z());
    glUniform3f(loc("uWaterShadeKd"), kd.x(), kd.y(), kd.z());
    glUniform3f(loc("uWaterShadeSunToward"), medium->sun_toward.x(),
                medium->sun_toward.y(), medium->sun_toward.z());
    glUniform1f(loc("uWaterShadeAmbientT"), kAmbientInterfaceTransmittance);
}

}  // namespace water
}  // namespace jpov
