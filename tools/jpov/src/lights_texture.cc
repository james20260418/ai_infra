// JPOV 点光源数据纹理实现（见 lights_texture.h 顶部的来龙去脉）。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/lights_texture.h"

#include <algorithm>
#include <vector>

// GL 头文件必须最先 include（在 MinGW #define 宏替换之前）。
// ⚠️ MinGW 也要显式包含 <GL/glext.h>：RGBA32F / CLAMP_TO_EDGE 等 GL3 枚举不在
//    <GL/gl.h>（仅 GL 1.1）里，gl_loader.h 也不提供（同 fire_fog_renderer.cc）。
#include <GL/gl.h>
#include <GL/glext.h>

#ifdef _WIN32
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#endif

#include <glog/logging.h>

#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/texture_units.h"

namespace jpov {

int ClampedPointLightCount(const RenderCommandList& cmds) {
    return std::min(static_cast<int>(cmds.point_lights.size()), kMaxPointLights);
}

void EnsureLightDataTexture(unsigned int* tex) {
    CHECK(tex != nullptr);
    if (*tex != 0) {
        return;
    }
    glGenTextures(1, tex);
    GLint prev_active = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glActiveTexture(GL_TEXTURE0 + kTexUnitLightData);
    glBindTexture(GL_TEXTURE_2D, *tex);
    // NEAREST：按整数下标 fetch，任何过滤都只会引入错误；CLAMP：越界下标在本实现里
    // 不会发生（uTotalLights 兜着），但纹理参数的默认值（REPEAT + mipmap 过滤）会
    // 让纹理**不完整**——那样采样结果是未定义的，所以这三行是功能性的，不是"优化"。
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, kMaxPointLights, kLightDataRows,
                 0, GL_RGBA, GL_FLOAT, nullptr);
    glActiveTexture(static_cast<GLenum>(prev_active));
}

void UploadLightDataTexture(const RenderCommandList& cmds, unsigned int tex) {
    CHECK_NE(tex, 0u) << "UploadLightDataTexture: 需要先 EnsureLightDataTexture()";
    const int count = ClampedPointLightCount(cmds);
    if (count == 0) {
        return;   // 没有点光源：uTotalLights=0，片元不会 fetch 任何 texel。
    }

    // 布局见头文件：3 行 × count 列，行优先（glTexSubImage2D 要求逐行连续）。
    std::vector<float> data(static_cast<size_t>(count) * kLightDataRows * 4, 0.0f);
    for (int i = 0; i < count; ++i) {
        const PointLight& l = cmds.point_lights[static_cast<size_t>(i)];
        const size_t row0 = static_cast<size_t>(i) * 4;
        const size_t row1 = (static_cast<size_t>(count) + static_cast<size_t>(i)) * 4;
        const size_t row2 = (static_cast<size_t>(count) * 2 + static_cast<size_t>(i)) * 4;
        data[row0 + 0] = l.position.x();
        data[row0 + 1] = l.position.y();
        data[row0 + 2] = l.position.z();
        data[row0 + 3] = l.linear_radius;
        data[row1 + 0] = l.color.r;
        data[row1 + 1] = l.color.g;
        data[row1 + 2] = l.color.b;
        data[row1 + 3] = l.intensity;
        data[row2 + 0] = l.physical_radius;
    }

    // 在**本单元**（kTexUnitLightData）上做上传，并把 active texture 还原：
    // 直接用"当前 active unit"上传会悄悄把那个单元上别人绑的纹理顶掉
    //（JPOV 的状态约定是"碰过的自己复原"，调用方后面不会再重新绑定它）。
    GLint prev_active = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glActiveTexture(GL_TEXTURE0 + kTexUnitLightData);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, count, kLightDataRows,
                    GL_RGBA, GL_FLOAT, data.data());
    glActiveTexture(static_cast<GLenum>(prev_active));
}

void BindLightDataTexture(ShaderManager* shader_mgr, unsigned int prog,
                          unsigned int tex, int light_count, bool tile_culling) {
    CHECK(shader_mgr != nullptr);
    CHECK_NE(prog, 0u);
    CHECK_NE(tex, 0u);
    CHECK_GE(light_count, 0);
    CHECK_LE(light_count, kMaxPointLights);

    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0 + kTexUnitLightData);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(shader_mgr->GetUniform(prog, "uLightData"), kTexUnitLightData);
    glUniform1i(shader_mgr->GetUniform(prog, "uTotalLights"), light_count);
    // uTileCulling=1（默认）：片元走 tile 索引纹理；=0：遍历全部光源。
    glUniform1i(shader_mgr->GetUniform(prog, "uTileCulling"),
                tile_culling ? 1 : 0);
    glActiveTexture(GL_TEXTURE0);
}

}  // namespace jpov
