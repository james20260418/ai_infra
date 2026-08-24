// JPOV FontRenderer 实现
//
// 字体注册、字形光栅化（委托 FontManager）、GL atlas 管理、DrawText2D。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/font2d/font_renderer.h"

#include <cstdio>
#include <cstring>
#include <tuple>
#include <vector>

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

// MinGW 的 GL/gl.h 可能不定义 GL_CLAMP_TO_EDGE
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#endif

#include <glog/logging.h>

namespace jpov {

// ==================== 匿名 namespace：辅助函数 ====================

namespace {

// 路径查找：先试原始路径，再试 bazel test 的 runfiles（TEST_SRCDIR）
std::string ResolveFontPath(const char* raw_path) {
    FILE* fp = std::fopen(raw_path, "rb");
    if (fp) {
        std::fclose(fp);
        return raw_path;
    }
    // Try TEST_SRCDIR for bazel test sandbox
    const char* srcdir = std::getenv("TEST_SRCDIR");
    if (srcdir) {
        std::string p = srcdir;
        if (!p.empty() && p.back() != '/') {
            p.push_back('/');
        }
        p += "__main__/";
        p += raw_path;
        FILE* fp2 = std::fopen(p.c_str(), "rb");
        if (fp2) {
            std::fclose(fp2);
            return p;
        }
    }
    return "";
}

// 创建 GL atlas 纹理并上传 CPU 像素（初始全黑）
unsigned int CreateGlAtlasTexture(int atlas_dim,
                                  const std::vector<uint8_t>& pixels) {
    unsigned int tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    // 用 GL_R8 单通道纹理，shader 中 .r 读取为 alpha
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, atlas_dim, atlas_dim, 0,
                 GL_RED, GL_UNSIGNED_BYTE, pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

}  // namespace

// ==================== 生命周期 ====================

FontRenderer::~FontRenderer() {
    for (auto& [alias, slot] : font_slots_) {
        (void)alias;
        for (int lv = 0; lv < 3; ++lv) {
            if (slot.atlas_tex[lv]) {
                glDeleteTextures(1, &slot.atlas_tex[lv]);
            }
        }
    }
}

// ==================== Init ====================

void FontRenderer::Init(
    const std::vector<std::tuple<const char*, int, const char*>>& font_entries,
    const std::vector<std::tuple<const char*, int, const char*>>& default_fonts) {
    // 用户字体最多 10 种
    CHECK_LE(static_cast<int>(font_entries.size()), 10)
        << "Too many fonts: " << font_entries.size()
        << " (max 10)";

    // 用户字体内部别名查重
    for (size_t i = 0; i < font_entries.size(); ++i) {
        for (size_t j = i + 1; j < font_entries.size(); ++j) {
            CHECK(strcmp(std::get<2>(font_entries[i]),
                         std::get<2>(font_entries[j])) != 0)
                << "Duplicate font alias: " << std::get<2>(font_entries[i]);
        }
    }

    // 第一步：注册用户字体
    for (const auto& fe : font_entries) {
        RegisterFont(std::get<0>(fe),
                      std::get<1>(fe),
                      std::get<2>(fe),
                      "user",
                      &font_slots_, &font_order_);
    }

    // 第二步：注册内置默认字体（共享别名空间）
    for (const auto& de : default_fonts) {
        RegisterFont(std::get<0>(de),
                      std::get<1>(de),
                      std::get<2>(de),
                      "builtin",
                      &font_slots_, &font_order_);
    }

    // 至少一种字体可用
    CHECK(!font_slots_.empty())
        << "No fonts loaded (user nor built-in). "
        << "Provide at least one font via JPOV::Config::fonts.";
}

// ==================== 静态方法：InitOneFontSlot ====================

void FontRenderer::InitOneFontSlot(const char* alias,
                                    const std::string& resolved_path,
                                    int ttc_index,
                                    FontSlot* slot /*output*/) {
    CHECK(slot != nullptr);
    CHECK(!slot->manager.has_value())
        << "FontSlot already initialized for alias=" << alias;

    FontManagerConfig cfg;
    cfg.font_name = alias;
    cfg.font_path = resolved_path;
    cfg.ttc_font_index = ttc_index;

    std::optional<FontManager> mgr = FontManager::Create(cfg);
    CHECK(mgr.has_value())
        << "Failed to load font: alias=" << alias
        << " path=" << resolved_path
        << " ttc_index=" << ttc_index;

    slot->manager = std::move(mgr.value());
    for (int lv = 0; lv < 3; ++lv) {
        slot->atlas_tex[lv] = CreateGlAtlasTexture(
            FontManager::kAtlasDim,
            slot->manager->atlas_pixels(lv));
    }

    LOG(INFO) << "Font registered: alias=" << alias
              << " path=" << resolved_path
              << " ttc_index=" << ttc_index;
}

// ==================== 静态方法：RegisterFont ====================

void FontRenderer::RegisterFont(
    const char* path,
    int ttc_index,
    const char* alias,
    const char* source,
    std::unordered_map<std::string, FontSlot>* font_slots,
    std::vector<std::string>* font_order) {
    CHECK(path != nullptr && path[0] != '\0')
        << "FontEntry path is null or empty (alias=" << alias << ")";
    CHECK(alias != nullptr && alias[0] != '\0')
        << "Font alias is null or empty (path=" << path << ")";

    std::string resolved = ResolveFontPath(path);
    if (resolved.empty()) {
        if (strcmp(source, "user") == 0) {
            LOG(FATAL) << "Font file not found: " << path
                       << " (alias=" << alias << ")";
        }
        // builtin 字体找不到就静默跳过
        LOG(INFO) << "Builtin font not found, skipping: " << path;
        return;
    }

    // 检查 alias 是否已注册
    CHECK(font_slots->find(alias) == font_slots->end())
        << "Duplicate font alias: \"" << alias << "\" from source=" << source
        << " path=" << path;

    FontSlot slot;
    InitOneFontSlot(alias, resolved, ttc_index, &slot);

    std::pair<std::unordered_map<std::string, FontSlot>::iterator, bool> result =
        font_slots->emplace(alias, std::move(slot));
    CHECK(result.second) << "Duplicate font alias (internal): " << alias;
    font_order->push_back(alias);
}

// ==================== FontSlot 查找 ====================

FontRenderer::FontSlot* FontRenderer::FindFontSlot(const std::string& alias) {
    if (!alias.empty()) {
        std::unordered_map<std::string, FontSlot>::iterator it =
            font_slots_.find(alias);
        if (it != font_slots_.end()) {
            return &it->second;
        }
        // 别名不存在 → crash（用户指定了不存在的字体别名）
        std::string registered;
        for (const std::string& a : font_order_) {
            if (!registered.empty()) {
                registered += ", ";
            }
            registered += a;
        }
        LOG(FATAL) << "Unknown font alias: \"" << alias
                   << "\". Registered aliases: "
                   << (font_order_.empty() ? "(none)" : registered);
    }
    // 空别名 → 返回第一个
    if (font_order_.empty()) {
        return nullptr;
    }
    return &font_slots_.at(font_order_[0]);
}

// ==================== Atlas 上传 ====================

void FontRenderer::UploadAtlas(FontSlot& slot, int level) {
    if (!slot.manager.has_value() || !slot.manager->loaded()) {
        return;
    }
    if (!slot.manager->atlas_dirty(level) || !slot.atlas_tex[level]) {
        return;
    }
    // 全量更新 GL 纹理（4096x4096 不太大，全量上传即可）
    glBindTexture(GL_TEXTURE_2D, slot.atlas_tex[level]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    FontManager::kAtlasDim, FontManager::kAtlasDim,
                    GL_RED, GL_UNSIGNED_BYTE,
                    slot.manager->atlas_pixels(level).data());
    glBindTexture(GL_TEXTURE_2D, 0);
    slot.manager->mark_atlas_clean(level);
    LOG_EVERY_N(INFO, FontManager::kUploadLogInterval)
        << "UploadAtlas[" << level << "]: uploaded "
        << FontManager::kAtlasDim << "x" << FontManager::kAtlasDim;
}

void FontRenderer::UploadAllDirty(FontSlot& slot) {
    if (!slot.manager.has_value()) {
        return;
    }
    for (int lv = 0; lv < FontManager::kNumLevels; ++lv) {
        UploadAtlas(slot, lv);
    }
}

// ==================== DrawText2D ====================

void FontRenderer::DrawText2D(const Text2DCommand& cmd,
                              unsigned int stream_vbo,
                              int fbo_w, int fbo_h,
                              unsigned int text_prog) {
    // 按别名查找字体
    FontSlot* slot = FindFontSlot(cmd.font_alias);
    if (!slot || !slot->manager.has_value() || !slot->manager->loaded()) {
        LOG_EVERY_N(WARNING, FontManager::kNotLoadedLogInterval)
            << "Text2D: font not loaded for alias=\"" << cmd.font_alias
            << "\", skipping";
        return;
    }

    CHECK_GT(cmd.font_size, 0.0f);

    // GenerateTextVertices 内部执行多级 atlas 选择 + 包围盒计算 + 顶点生成
    int selected_level = 0;
    std::vector<float> verts;
    bool ok = slot->manager->GenerateTextVertices(
        cmd.text, cmd.font_size,
        cmd.pos.x(), cmd.pos.y(),
        static_cast<int>(cmd.alignment),
        fbo_w, fbo_h,
        &selected_level,
        &verts);

    if (!ok || verts.empty()) {
        return;
    }

    // 上传新光栅化的字形到 GL atlas（所有脏层）
    UploadAllDirty(*slot);

    // 保存 GL 状态
    glPushAttrib(GL_ENABLE_BIT);

    glUseProgram(text_prog);
    glUniform2f(glGetUniformLocation(text_prog, "uFboSize"),
                static_cast<float>(fbo_w), static_cast<float>(fbo_h));
    glUniform4f(glGetUniformLocation(text_prog, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);
    glUniform1i(glGetUniformLocation(text_prog, "uTexture"), 0);

    // 绑定对应层级的纹理
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, slot->atlas_tex[selected_level]);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_DYNAMIC_DRAW);

    // 位置 (location 0) | 纹理坐标 (location 1)
    // x,y,u,v interleaved, stride = 4 floats
    constexpr int kStride = 4 * static_cast<int>(sizeof(float));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, kStride, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, kStride,
                          (void*)(2 * sizeof(float)));

    int vert_count = static_cast<int>(verts.size()) / 4;
    glDrawArrays(GL_TRIANGLES, 0, vert_count);

    GLenum draw_err = glGetError();
    if (draw_err != GL_NO_ERROR) {
        LOG_FIRST_N(WARNING, 1) << "GL error after DrawText2D: " << draw_err;
    }

    // 恢复 GL 状态
    glPopAttrib();
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

// ==================== FontRenderer::MeasureTextWidth ====================

float FontRenderer::MeasureTextWidth(const std::string& alias,
                                      std::string_view text,
                                      float font_size) {
    CHECK_GT(font_size, 0.0f);
    FontSlot* slot = FindFontSlot(alias);  // 空别名 → 首个字体；未知 → crash
    if (!slot || !slot->manager.has_value() || !slot->manager->loaded()) {
        return 0.0f;
    }
    return slot->manager->MeasureTextWidth(text, font_size);
}

}  // namespace jpov
