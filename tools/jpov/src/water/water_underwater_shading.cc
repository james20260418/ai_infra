// JPOV 水下照明衰减 —— uniform 上传（唯一的名字表 + 唯一的上传实现）

#include "tools/jpov/src/water/water_underwater_shading.h"

#define GL_GLEXT_PROTOTYPES

#include <GL/gl.h>

#ifdef _WIN32
#include <GL/glext.h>
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

#include "tools/jpov/src/shader_manager.h"

namespace jpov {
namespace water {

void UploadUnderwaterAttenuation(ShaderManager& shader_mgr, unsigned int prog,
                                 const UnderwaterAttenuation* atten) {
    if (prog == 0) {
        return;
    }
    glUseProgram(prog);
    const bool on = (atten != nullptr) && atten->enabled();
    glUniform1i(shader_mgr.GetUniform(prog, "uWaterAttenOn"), on ? 1 : 0);
    if (!on) {
        // 关闭态：FS 第一行就 return，其余字段不会被读。仍然写一份确定值，
        // 让「任何时刻读到的 uniform 都是确定的」这条约束继续成立。
        glUniform1i(shader_mgr.GetUniform(prog, "uWaterAttenAmbient"), 0);
        glUniform1i(shader_mgr.GetUniform(prog, "uWaterAttenSun"), 0);
        glUniform1f(shader_mgr.GetUniform(prog, "uWaterAttenLevelY"), 0.0f);
        glUniform1f(shader_mgr.GetUniform(prog, "uWaterAttenIor"), kWaterIorDefault);
        glUniform3f(shader_mgr.GetUniform(prog, "uWaterAttenSigmaT"), 0.0f, 0.0f, 0.0f);
        glUniform3f(shader_mgr.GetUniform(prog, "uWaterAttenKd"), 0.0f, 0.0f, 0.0f);
        glUniform3f(shader_mgr.GetUniform(prog, "uWaterAttenSunToward"), 0.0f, 1.0f, 0.0f);
        glUniform1f(shader_mgr.GetUniform(prog, "uWaterAttenAmbientT"),
                    kAmbientInterfaceTransmittance);
        return;
    }
    glUniform1i(shader_mgr.GetUniform(prog, "uWaterAttenAmbient"), atten->ambient ? 1 : 0);
    glUniform1i(shader_mgr.GetUniform(prog, "uWaterAttenSun"), atten->sun ? 1 : 0);
    glUniform1f(shader_mgr.GetUniform(prog, "uWaterAttenLevelY"), atten->level_y);
    glUniform1f(shader_mgr.GetUniform(prog, "uWaterAttenIor"), atten->ior);
    glUniform3f(shader_mgr.GetUniform(prog, "uWaterAttenSigmaT"),
                atten->sigma_t.x(), atten->sigma_t.y(), atten->sigma_t.z());
    glUniform3f(shader_mgr.GetUniform(prog, "uWaterAttenKd"),
                atten->kd.x(), atten->kd.y(), atten->kd.z());
    glUniform3f(shader_mgr.GetUniform(prog, "uWaterAttenSunToward"),
                atten->sun_toward.x(), atten->sun_toward.y(), atten->sun_toward.z());
    glUniform1f(shader_mgr.GetUniform(prog, "uWaterAttenAmbientT"), atten->ambient_t);
}

}  // namespace water
}  // namespace jpov
