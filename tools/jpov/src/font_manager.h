// JPOV FontManager — 字体管理器
//
// FontManager 唯一对应一种字体。它维护三层 atlas（16/32/48px），
// 每层独立的 CPU 图集 + 行式 packing。
//
// 架构：
//   FindGlyph(cp)  → 只读查找，返回已有 Glyph 或 nullptr
//   BuildGlyph(cp) → 一次性建立三层 Glyph（全建 or nothing）
//   GenerateTextVertices → 只读消费 Glyph，不做任何光栅化
//
// FontManager 不持有 OpenGL 纹理资源。

#ifndef JPOV_FONT_MANAGER_H_
#define JPOV_FONT_MANAGER_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/interface/render_command.h"

struct stbtt_fontinfo;

namespace jpov {

// ==================== FontManagerConfig ====================

struct FontManagerConfig {
    std::string font_name;
    std::string font_path;
    std::vector<uint32_t> preraster_charset;
    int ttc_font_index = 0;
};

// ==================== GlyphLayer ====================

struct GlyphLayer {
    int w = 0;
    int h = 0;
    float advance = 0.0f;
    float xoff = 0.0f;
    float yoff = 0.0f;
    int atlas_x = 0;
    int atlas_y = 0;
    float base_size = 0.0f;
    bool valid = false;
};

// ==================== Glyph ====================

struct Glyph {
    GlyphLayer layers[3];  // [0]=16px, [1]=32px, [2]=48px

    bool has_pixels() const {
        for (const auto& l : layers) {
            if (l.valid && l.w > 0) return true;
        }
        return false;
    }
};

// ==================== AtlasLevel ====================

struct AtlasLevel {
    float base_size = 0.0f;
    std::vector<uint8_t> pixels;
    int cursor_x = 0;
    int cursor_y = 0;
    int row_h = 0;
    bool dirty = false;
};

// ==================== FontManager ====================

class FontManager {
public:
    static constexpr int kAtlasDim = 4096;
    static constexpr int kLevel16 = 0;
    static constexpr int kLevel32 = 1;
    static constexpr int kLevel48 = 2;
    static constexpr int kNumLevels = 3;
    static constexpr float kAtlasLevelBaseSizes[3] = {16.0f, 32.0f, 48.0f};
    static constexpr int kGlyphPadding = 2;
    static constexpr int kUploadLogInterval = 5;
    static constexpr int kNotLoadedLogInterval = 60;

    static std::optional<FontManager> Create(const FontManagerConfig& config);

    ~FontManager();

    FontManager(FontManager&& other) noexcept;
    FontManager& operator=(FontManager&& other) noexcept;
    FontManager(const FontManager&) = delete;
    FontManager& operator=(const FontManager&) = delete;

    // ---- 只读属性 ----
    bool loaded() const { return loaded_; }
    float ascent() const { return ascent_; }
    float descent() const { return descent_; }
    float linegap() const { return linegap_; }

    const std::vector<uint8_t>& atlas_pixels(int level) const {
        return levels_[level].pixels;
    }
    bool atlas_dirty(int level) const { return levels_[level].dirty; }
    void mark_atlas_clean(int level) { levels_[level].dirty = false; }
    bool any_atlas_dirty() const {
        for (int i = 0; i < kNumLevels; ++i) {
            if (levels_[i].dirty) return true;
        }
        return false;
    }

    // ---- 核心操作 ----

    static uint32_t DecodeUtf8(const char*& p);

    // 只读查找：返回已有 Glyph 指针，不存在则返回 nullptr。
    // 不触发任何光栅化。
    const Glyph* FindGlyph(uint32_t codepoint) const;

    // 构建一个字符的三层 Glyph（一次性全建）。
    // 已有则直接返回；不存在则三层全部光栅化 + packing。
    // 某一层 atlas 满了，该层 valid=false，其他层不受影响。
    // 返回 nullptr 表示所有三层都无效（字体未加载或全满）。
    const Glyph* BuildGlyph(uint32_t codepoint);

    // 为 DrawText2D 生成顶点数据。
    // 调用方需先对所有 codepoint 调用 BuildGlyph，后者只做只读。
    //
    // selected_level — output: 实际使用的 atlas 层级
    bool GenerateTextVertices(std::string_view text,
                              float font_size,
                              float pos_x, float pos_y,
                              int alignment,
                              int fbo_w, int fbo_h,
                              int* selected_level /*output*/,
                              std::vector<float>* out_verts /*output*/);

private:
    FontManager() = default;

    bool LoadFontFile();
    bool ParseFont();
    void InitAtlas();
    void PrerasterCharset();

    // 光栅化一个字到指定层级（不查缓存，不修改 glyphs_）。
    // 成功则填充 layer 并返回 true；atlas 满或无字形则返回 false。
    bool RasterizeToLevel(uint32_t codepoint, int level,
                          GlyphLayer* layer /*output*/);

    int SelectBestLevel(float font_size) const;

    // font file data
    unsigned char* ttf_data_ = nullptr;
    long ttf_data_size_ = 0;
    stbtt_fontinfo* font_info_ = nullptr;

    // configured
    std::string font_name_;
    std::string font_path_;
    std::vector<uint32_t> preraster_charset_;
    int ttc_font_index_ = 0;

    // runtime
    bool loaded_ = false;
    float ascent_ = 0.0f;
    float descent_ = 0.0f;
    float linegap_ = 0.0f;

    AtlasLevel levels_[kNumLevels];
    std::unordered_map<uint32_t, Glyph> glyphs_;

    // UTF-8 constants
    static constexpr uint8_t kUtf8Cont  = 0x80;
    static constexpr uint8_t kUtf8Mask6 = 0x3F;
    static constexpr uint8_t kUtf8Lead2 = 0xC0;
    static constexpr uint8_t kMask5     = 0x1F;
    static constexpr uint8_t kUtf8Lead3 = 0xE0;
    static constexpr uint8_t kMask4     = 0x0F;
    static constexpr uint8_t kUtf8Lead4 = 0xF0;
    static constexpr uint8_t kMask3     = 0x07;
    static constexpr uint32_t kReplacementChar = 0xFFFD;
};

}  // namespace jpov

#endif  // JPOV_FONT_MANAGER_H_
