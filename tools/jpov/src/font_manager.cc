// JPOV FontManager 实现
//
// 字体文件加载 → stb_truetype 初始化 → 字形光栅化 → 图集 packing
// → UTF-8 解码 → DrawText2D 顶点生成。
//
// 不持有 GL 纹理对象；CPU 图集像素暴露给 Renderer 上传。

#include "tools/jpov/src/font_manager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <glog/logging.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace jpov {

// ==================== 构造 / 析构 ====================

FontManager::~FontManager() {
    if (font_info_) {
        STBTT_free(font_info_, nullptr);
        font_info_ = nullptr;
    }
    if (ttf_data_) {
        STBTT_free(ttf_data_, nullptr);
        ttf_data_ = nullptr;
    }
}

FontManager::FontManager(FontManager&& other) noexcept
    : ttf_data_(other.ttf_data_),
      ttf_data_size_(other.ttf_data_size_),
      font_info_(other.font_info_),
      base_font_size_(other.base_font_size_),
      font_name_(std::move(other.font_name_)),
      font_path_(std::move(other.font_path_)),
      preraster_charset_(std::move(other.preraster_charset_)),
      ttc_font_index_(other.ttc_font_index_),
      loaded_(other.loaded_),
      atlas_dirty_(other.atlas_dirty_),
      ascent_(other.ascent_),
      descent_(other.descent_),
      linegap_(other.linegap_),
      atlas_pixels_(std::move(other.atlas_pixels_)),
      atlas_cursor_x_(other.atlas_cursor_x_),
      atlas_cursor_y_(other.atlas_cursor_y_),
      atlas_row_h_(other.atlas_row_h_),
      glyphs_(std::move(other.glyphs_)) {
    // 转移 raw pointer 所有权，源对象置空避免 double-free
    other.ttf_data_ = nullptr;
    other.font_info_ = nullptr;
    other.loaded_ = false;
}

FontManager& FontManager::operator=(FontManager&& other) noexcept {
    if (this != &other) {
        // 先清理自己的资源
        if (font_info_) { STBTT_free(font_info_, nullptr); }
        if (ttf_data_) { STBTT_free(ttf_data_, nullptr); }

        ttf_data_ = other.ttf_data_;
        ttf_data_size_ = other.ttf_data_size_;
        font_info_ = other.font_info_;
        base_font_size_ = other.base_font_size_;
        font_name_ = std::move(other.font_name_);
        font_path_ = std::move(other.font_path_);
        preraster_charset_ = std::move(other.preraster_charset_);
        ttc_font_index_ = other.ttc_font_index_;
        loaded_ = other.loaded_;
        atlas_dirty_ = other.atlas_dirty_;
        ascent_ = other.ascent_;
        descent_ = other.descent_;
        linegap_ = other.linegap_;
        atlas_pixels_ = std::move(other.atlas_pixels_);
        atlas_cursor_x_ = other.atlas_cursor_x_;
        atlas_cursor_y_ = other.atlas_cursor_y_;
        atlas_row_h_ = other.atlas_row_h_;
        glyphs_ = std::move(other.glyphs_);

        other.ttf_data_ = nullptr;
        other.font_info_ = nullptr;
        other.loaded_ = false;
    }
    return *this;
}

// ==================== 工厂方法 ====================

std::optional<FontManager> FontManager::Create(const FontManagerConfig& config) {
    FontManager mgr;
    mgr.font_name_ = config.font_name;
    mgr.font_path_ = config.font_path;
    mgr.base_font_size_ = config.base_font_size;
    mgr.preraster_charset_ = config.preraster_charset;
    mgr.ttc_font_index_ = config.ttc_font_index;

    if (!mgr.LoadFontFile()) {
        return std::nullopt;
    }
    if (!mgr.ParseFont()) {
        return std::nullopt;
    }
    if (!mgr.InitAtlas()) {
        return std::nullopt;
    }

    mgr.loaded_ = true;
    mgr.PrerasterCharset();

    LOG(INFO) << "FontManager[" << mgr.font_name_ << "]: loaded, base_size="
              << mgr.base_font_size_ << ", atlas=" << kAtlasDim << "x" << kAtlasDim;
    return mgr;
}

// ==================== 字体加载 ====================

bool FontManager::LoadFontFile() {
    FILE* fp = std::fopen(font_path_.c_str(), "rb");
    if (!fp) {
        LOG(WARNING) << "FontManager[" << font_name_ << "]: cannot open "
                     << font_path_;
        return false;
    }
    std::fseek(fp, 0, SEEK_END);
    ttf_data_size_ = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);

    ttf_data_ = static_cast<unsigned char*>(
        STBTT_malloc(static_cast<size_t>(ttf_data_size_), nullptr));
    if (std::fread(ttf_data_, 1, static_cast<size_t>(ttf_data_size_), fp)
        != static_cast<size_t>(ttf_data_size_)) {
        STBTT_free(ttf_data_, nullptr);
        ttf_data_ = nullptr;
        std::fclose(fp);
        LOG(WARNING) << "FontManager[" << font_name_ << "]: read failed";
        return false;
    }
    std::fclose(fp);
    return true;
}

bool FontManager::ParseFont() {
    font_info_ = static_cast<stbtt_fontinfo*>(
        STBTT_malloc(sizeof(stbtt_fontinfo), nullptr));

    // 检测 TTC (TrueType Collection) 文件，取指定 font_index
    int font_offset = 0;
    if (stbtt_GetNumberOfFonts(ttf_data_) > 1) {
        font_offset = stbtt_GetFontOffsetForIndex(ttf_data_, ttc_font_index_);
        LOG(INFO) << "FontManager[" << font_name_ << "]: TTC detected, "
                  << "using font_index=" << ttc_font_index_;
    }

    if (!stbtt_InitFont(font_info_, ttf_data_, font_offset)) {
        STBTT_free(font_info_, nullptr);
        font_info_ = nullptr;
        LOG(WARNING) << "FontManager[" << font_name_ << "]: stbtt_InitFont failed";
        return false;
    }

    // 获取度量信息（基于 base_font_size）
    float scale = stbtt_ScaleForPixelHeight(font_info_, base_font_size_);
    int ascent, descent, linegap;
    stbtt_GetFontVMetrics(font_info_, &ascent, &descent, &linegap);
    ascent_ = static_cast<float>(ascent) * scale;
    descent_ = static_cast<float>(descent) * scale;
    linegap_ = static_cast<float>(linegap) * scale;
    return true;
}

bool FontManager::InitAtlas() {
    // 初始化动态 atlas：全部为 0 的空灰度图
    atlas_pixels_.resize(
        static_cast<size_t>(kAtlasDim) * static_cast<size_t>(kAtlasDim), 0);
    atlas_cursor_x_ = 0;
    atlas_cursor_y_ = 0;
    atlas_row_h_ = 0;
    atlas_dirty_ = false;
    return true;
}

void FontManager::PrerasterCharset() {
    if (preraster_charset_.empty()) {
        return;
    }
    for (uint32_t cp : preraster_charset_) {
        GetOrRasterizeGlyph(cp);
    }
    LOG(INFO) << "FontManager[" << font_name_ << "]: prerastered "
              << preraster_charset_.size() << " codepoints, "
              << glyphs_.size() << " glyphs packed";
}

// ==================== UTF-8 解码 ====================

uint32_t FontManager::DecodeUtf8(const char*& p) {
    uint8_t c = static_cast<uint8_t>(*p);
    if (c < kUtf8Cont) {
        // 0xxxxxxx — single byte (ASCII)
        ++p;
        return c;
    }
    if (c < kUtf8Lead2) {
        // continuation byte without leading byte — illegal, skip
        ++p;
        return kReplacementChar;
    }
    if (c < kUtf8Lead3) {
        // 2-byte sequence: 110xxxxx 10xxxxxx
        uint32_t cp = c & kMask5;
        if ((static_cast<uint8_t>(p[1]) & ~kUtf8Mask6) != kUtf8Cont) {
            ++p;
            return kReplacementChar;
        }
        cp = (cp << 6) | (static_cast<uint8_t>(p[1]) & kUtf8Mask6);
        p += 2;
        return cp;
    }
    if (c < kUtf8Lead4) {
        // 3-byte sequence: 1110xxxx 10xxxxxx 10xxxxxx
        uint32_t cp = c & kMask4;
        if ((static_cast<uint8_t>(p[1]) & ~kUtf8Mask6) != kUtf8Cont) {
            ++p;
            return kReplacementChar;
        }
        if ((static_cast<uint8_t>(p[2]) & ~kUtf8Mask6) != kUtf8Cont) {
            p += 2;
            return kReplacementChar;
        }
        cp = (cp << 6) | (static_cast<uint8_t>(p[1]) & kUtf8Mask6);
        cp = (cp << 6) | (static_cast<uint8_t>(p[2]) & kUtf8Mask6);
        p += 3;
        return cp;
    }
    // 4-byte sequence: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
    uint32_t cp = c & kMask3;
    if ((static_cast<uint8_t>(p[1]) & ~kUtf8Mask6) != kUtf8Cont) {
        ++p;
        return kReplacementChar;
    }
    if ((static_cast<uint8_t>(p[2]) & ~kUtf8Mask6) != kUtf8Cont) {
        p += 2;
        return kReplacementChar;
    }
    if ((static_cast<uint8_t>(p[3]) & ~kUtf8Mask6) != kUtf8Cont) {
        p += 3;
        return kReplacementChar;
    }
    cp = (cp << 6) | (static_cast<uint8_t>(p[1]) & kUtf8Mask6);
    cp = (cp << 6) | (static_cast<uint8_t>(p[2]) & kUtf8Mask6);
    cp = (cp << 6) | (static_cast<uint8_t>(p[3]) & kUtf8Mask6);
    p += 4;
    return cp;
}

// ==================== 字形光栅化 + 图集 packing ====================

const GlyphMetadata* FontManager::GetOrRasterizeGlyph(uint32_t codepoint) {
    // 已存在 → 返回
    auto it = glyphs_.find(codepoint);
    if (it != glyphs_.end()) {
        return &it->second;
    }

    if (!font_info_ || !loaded_) {
        return nullptr;
    }

    // 光栅化 codepoint（基于 base_font_size）
    float scale = stbtt_ScaleForPixelHeight(font_info_, base_font_size_);
    GlyphMetadata g;

    int pw, ph, xoff, yoff;
    unsigned char* pixels = stbtt_GetCodepointBitmap(
        font_info_, 0, scale, static_cast<int>(codepoint),
        &pw, &ph, &xoff, &yoff);

    int advance_width;
    stbtt_GetCodepointHMetrics(font_info_, static_cast<int>(codepoint),
                               &advance_width, nullptr);
    g.advance = static_cast<float>(advance_width) * scale;

    if (!pixels) {
        // 空白字符（空格等），advance 已有，跳过 packing
        g.w = 0;
        g.h = 0;
        g.xoff = 0.0f;
        g.yoff = 0.0f;
        g.atlas_x = 0;
        g.atlas_y = 0;
        auto result = glyphs_.emplace(codepoint, g);
        return &result.first->second;
    }

    g.w = pw;
    g.h = ph;
    g.xoff = static_cast<float>(xoff);
    g.yoff = static_cast<float>(yoff);

    // 行式 packing：如果当前行放不下，换行
    int padded_w = pw + kGlyphPadding * 2;
    int padded_h = ph + kGlyphPadding * 2;

    if (atlas_cursor_x_ + padded_w > kAtlasDim) {
        // 换行
        atlas_cursor_x_ = 0;
        atlas_cursor_y_ += atlas_row_h_;
        atlas_row_h_ = 0;
    }

    // 如果超出 atlas 高度，报 warning 并跳过 packing
    if (atlas_cursor_y_ + padded_h > kAtlasDim) {
        LOG(WARNING) << "FontManager[" << font_name_
                     << "]: atlas full, codepoint=" << codepoint
                     << " not packed";
        g.atlas_x = 0;
        g.atlas_y = 0;
        auto result = glyphs_.emplace(codepoint, g);
        return &result.first->second;
    }

    // packing 位置（padding 后的内部原点）
    int pack_x = atlas_cursor_x_ + kGlyphPadding;
    int pack_y = atlas_cursor_y_ + kGlyphPadding;
    g.atlas_x = pack_x;
    g.atlas_y = pack_y;

    // 拷贝像素到 atlas
    for (int gy = 0; gy < ph; ++gy) {
        unsigned char* src = pixels + static_cast<size_t>(gy) * pw;
        unsigned char* dst = atlas_pixels_.data()
            + static_cast<size_t>(pack_y + gy) * kAtlasDim + pack_x;
        std::memcpy(dst, src, static_cast<size_t>(pw));
    }

    // 更新 cursor
    atlas_cursor_x_ += padded_w;
    atlas_row_h_ = std::max(atlas_row_h_, padded_h);
    atlas_dirty_ = true;

    // 释放光栅化像素（已拷贝到 atlas）
    STBTT_free(pixels, nullptr);

    auto result = glyphs_.emplace(codepoint, g);
    return &result.first->second;
}

// ==================== DrawText2D 顶点生成 ====================

bool FontManager::GenerateTextVertices(std::string_view text,
                                        float font_size,
                                        float pos_x, float pos_y,
                                        int alignment,
                                        int /*fbo_w*/, int /*fbo_h*/,
                                        std::vector<float>* out_verts) {
    CHECK_GT(font_size, 0.0f);
    CHECK_NOTNULL(out_verts);

    if (!loaded_) {
        LOG_EVERY_N(WARNING, kNotLoadedLogInterval)
            << "FontManager[" << font_name_ << "]: not loaded";
        return false;
    }

    // 计算缩放比例：目标字号 / 图集基本字号
    float scale = font_size / base_font_size_;

    // ---- Pass 1: 计算文本包围盒（用于对齐补偿） ----
    // 注意：字形度量在 scale 下以 base_font_size 图集为准，但 xoff/yoff 是图集字符的像素偏移量
    float min_x = 0.0f;
    float max_x = 0.0f;
    float min_y = 0.0f;
    float max_y = 0.0f;
    float cur_x = 0.0f;
    float cur_y = 0.0f;
    bool first_glyph = true;

    {
        const char* p = text.data();
        const char* end = text.data() + text.size();
        while (p < end) {
            uint32_t cp = DecodeUtf8(p);
            if (cp == '\n') {
                cur_x = 0.0f;
                cur_y += (ascent_ - descent_ + linegap_) * scale;
                continue;
            }

            const GlyphMetadata* g = GetOrRasterizeGlyph(cp);
            if (!g) {
                continue;
            }

            float gx = cur_x + g->xoff * scale;
            float gy = cur_y + g->yoff * scale;
            float gw = static_cast<float>(g->w) * scale;
            float gh = static_cast<float>(g->h) * scale;

            if (first_glyph) {
                min_x = gx;
                max_x = gx + gw;
                min_y = gy;
                max_y = gy + gh;
                first_glyph = false;
            } else {
                min_x = std::min(min_x, gx);
                max_x = std::max(max_x, gx + gw);
                min_y = std::min(min_y, gy);
                max_y = std::max(max_y, gy + gh);
            }

            cur_x += g->advance * scale;
        }
    }

    // 计算对齐偏移
    float offset_x = pos_x;
    float offset_y = pos_y;
    if (!first_glyph) {
        float bb_w = max_x - min_x;
        float bb_h = max_y - min_y;
        switch (static_cast<TextAlignment>(alignment)) {
            case TextAlignment::kTopLeft:
                break;
            case TextAlignment::kTopRight:
                offset_x = pos_x - max_x;
                break;
            case TextAlignment::kCenter:
                offset_x = pos_x - min_x - bb_w * 0.5f;
                offset_y = pos_y - min_y - bb_h * 0.5f;
                break;
            case TextAlignment::kBottomLeft:
                offset_y = pos_y - max_y;
                break;
            case TextAlignment::kBottomRight:
                offset_x = pos_x - max_x;
                offset_y = pos_y - max_y;
                break;
        }
    }

    // ---- Pass 2: 生成顶点数据（带对齐偏移） ----
    static constexpr int kMaxTextChars = 1024;
    // 每个字符 6 个顶点，每顶点 4 个 float (x,y,u,v)
    out_verts->reserve(kMaxTextChars * 6 * 4);
    out_verts->clear();

    cur_x = 0.0f;
    cur_y = 0.0f;

    const char* p = text.data();
    const char* send = text.data() + text.size();
    int char_count = 0;

    while (p < send && char_count < kMaxTextChars) {
        uint32_t cp = DecodeUtf8(p);
        if (cp == '\n') {
            cur_x = 0.0f;
            cur_y += (ascent_ - descent_ + linegap_) * scale;
            continue;
        }

        const GlyphMetadata* g = GetOrRasterizeGlyph(cp);
        if (!g || g->w == 0) {
            cur_x += (g ? g->advance : 0.0f) * scale;
            char_count++;
            continue;
        }

        float gx = offset_x + cur_x + g->xoff * scale;
        float gy = offset_y + cur_y + g->yoff * scale;
        float gw = static_cast<float>(g->w) * scale;
        float gh = static_cast<float>(g->h) * scale;

        float inv_a = 1.0f / static_cast<float>(kAtlasDim);
        float tx0 = static_cast<float>(g->atlas_x) * inv_a;
        float ty0 = static_cast<float>(g->atlas_y) * inv_a;
        float tx1 = static_cast<float>(g->atlas_x + g->w) * inv_a;
        float ty1 = static_cast<float>(g->atlas_y + g->h) * inv_a;

        // 两个三角形
        // v0: top-left
        out_verts->insert(out_verts->end(), {gx,      gy,      tx0, ty0});
        // v1: top-right
        out_verts->insert(out_verts->end(), {gx + gw, gy,      tx1, ty0});
        // v2: bottom-right
        out_verts->insert(out_verts->end(), {gx + gw, gy + gh, tx1, ty1});
        // v3: top-left
        out_verts->insert(out_verts->end(), {gx,      gy,      tx0, ty0});
        // v4: bottom-right
        out_verts->insert(out_verts->end(), {gx + gw, gy + gh, tx1, ty1});
        // v5: bottom-left
        out_verts->insert(out_verts->end(), {gx,      gy + gh, tx0, ty1});

        cur_x += g->advance * scale;
        char_count++;
    }

    return !out_verts->empty();
}

}  // namespace jpov
