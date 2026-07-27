// JPOV FontManager 实现
//
// 三层 atlas (16/32/48px)，自动层级选择，atlas 满时 fallback。

#include "tools/jpov/src/font_manager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <glog/logging.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace jpov {

constexpr float FontManager::kAtlasLevelBaseSizes[3];

// ==================== 构造 / 析构 / Move ====================

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
      font_name_(std::move(other.font_name_)),
      font_path_(std::move(other.font_path_)),
      preraster_charset_(std::move(other.preraster_charset_)),
      ttc_font_index_(other.ttc_font_index_),
      loaded_(other.loaded_),
      ascent_(other.ascent_),
      descent_(other.descent_),
      linegap_(other.linegap_),
      glyphs_(std::move(other.glyphs_)) {
    for (int i = 0; i < kNumLevels; ++i) {
        levels_[i] = std::move(other.levels_[i]);
    }
    other.ttf_data_ = nullptr;
    other.font_info_ = nullptr;
    other.loaded_ = false;
}

FontManager& FontManager::operator=(FontManager&& other) noexcept {
    if (this != &other) {
        if (font_info_) { STBTT_free(font_info_, nullptr); }
        if (ttf_data_) { STBTT_free(ttf_data_, nullptr); }

        ttf_data_ = other.ttf_data_;
        ttf_data_size_ = other.ttf_data_size_;
        font_info_ = other.font_info_;
        font_name_ = std::move(other.font_name_);
        font_path_ = std::move(other.font_path_);
        preraster_charset_ = std::move(other.preraster_charset_);
        ttc_font_index_ = other.ttc_font_index_;
        loaded_ = other.loaded_;
        ascent_ = other.ascent_;
        descent_ = other.descent_;
        linegap_ = other.linegap_;
        glyphs_ = std::move(other.glyphs_);
        for (int i = 0; i < kNumLevels; ++i) {
            levels_[i] = std::move(other.levels_[i]);
        }

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
    mgr.preraster_charset_ = config.preraster_charset;
    mgr.ttc_font_index_ = config.ttc_font_index;

    if (!mgr.LoadFontFile()) {
        return std::nullopt;
    }
    if (!mgr.ParseFont()) {
        return std::nullopt;
    }
    mgr.InitAtlas();

    mgr.loaded_ = true;
    mgr.PrerasterCharset();

    LOG(INFO) << "FontManager[" << mgr.font_name_
              << "]: loaded, 3-level atlas (16/32/48) " << kAtlasDim << "x" << kAtlasDim;
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

    // 检测 TTC (TrueType Collection)
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

    // 获取度量信息（16px 基准）
    float scale = stbtt_ScaleForPixelHeight(font_info_, 16.0f);
    int asc, desc, lg;
    stbtt_GetFontVMetrics(font_info_, &asc, &desc, &lg);
    ascent_ = static_cast<float>(asc) * scale;
    descent_ = static_cast<float>(desc) * scale;
    linegap_ = static_cast<float>(lg) * scale;
    return true;
}

void FontManager::InitAtlas() {
    // 初始化动态 atlas：三层独立的 4096x4096 空灰度图
    for (int i = 0; i < kNumLevels; ++i) {
        levels_[i].base_size = kAtlasLevelBaseSizes[i];
        levels_[i].pixels.resize(
            static_cast<size_t>(kAtlasDim) * static_cast<size_t>(kAtlasDim), 0);
        levels_[i].cursor_x = 0;
        levels_[i].cursor_y = 0;
        levels_[i].row_h = 0;
        levels_[i].dirty = false;
    }
}

void FontManager::PrerasterCharset() {
    if (preraster_charset_.empty()) {
        return;
    }
    for (uint32_t cp : preraster_charset_) {
        // 预渲染到所有三层
        Glyph g;
        for (int level = 0; level < kNumLevels; ++level) {
            RasterizeToLevel(cp, level, &g);
        }
        glyphs_.emplace(cp, std::move(g));
    }
    LOG(INFO) << "FontManager[" << font_name_ << "]: prerastered "
              << preraster_charset_.size() << " codepoints across 3 levels";
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

// ==================== 层级选择 ====================

int FontManager::SelectBestLevel(float font_size) const {
    // 选缩放比最接近 1.0 的层级（abs(log(scale)) 最小）
    float best_ratio = 999.0f;
    int best_level = kLevel16;
    for (int i = 0; i < kNumLevels; ++i) {
        float scale = font_size / levels_[i].base_size;
        // 优先选 scale >= 0.5 的层（防止太小的字形过度缩小）
        if (scale < 0.4f) continue;
        float penalty = std::abs(std::log2(scale));
        if (penalty < best_ratio) {
            best_ratio = penalty;
            best_level = i;
        }
    }
    return best_level;
}

// ==================== 字形光栅化 + 图集 packing ====================

bool FontManager::RasterizeToLevel(uint32_t codepoint, int level,
                                    Glyph* out_glyph) {
    GlyphLayer& layer = out_glyph->layers[level];
    float base = levels_[level].base_size;

    if (!font_info_ || !loaded_) {
        return false;
    }

    // 光栅化 codepoint
    float scale = stbtt_ScaleForPixelHeight(font_info_, base);
    int pw, ph, xoff, yoff;
    unsigned char* pixels = stbtt_GetCodepointBitmap(
        font_info_, 0, scale, static_cast<int>(codepoint),
        &pw, &ph, &xoff, &yoff);

    int advance_width;
    stbtt_GetCodepointHMetrics(font_info_, static_cast<int>(codepoint),
                               &advance_width, nullptr);
    layer.advance = static_cast<float>(advance_width) * scale;
    layer.base_size = base;

    if (!pixels) {
        // 空白字符（空格等），advance 已有
        layer.w = 0;
        layer.h = 0;
        layer.xoff = 0.0f;
        layer.yoff = 0.0f;
        layer.atlas_x = 0;
        layer.atlas_y = 0;
        layer.valid = true;  // 空格也算 valid
        return true;
    }

    layer.w = pw;
    layer.h = ph;
    layer.xoff = static_cast<float>(xoff);
    layer.yoff = static_cast<float>(yoff);

    // 行式 packing
    AtlasLevel& al = levels_[level];
    int padded_w = pw + kGlyphPadding * 2;
    int padded_h = ph + kGlyphPadding * 2;

    if (al.cursor_x + padded_w > kAtlasDim) {
        // 换行
        al.cursor_x = 0;
        al.cursor_y += al.row_h;
        al.row_h = 0;
    }

    if (al.cursor_y + padded_h > kAtlasDim) {
        // 本层 atlas 满了
        STBTT_free(pixels, nullptr);
        LOG_FIRST_N(WARNING, 1) << "FontManager[" << font_name_
                                << "]: atlas level " << level << " ("
                                << base << "px) full";
        return false;
    }

    // packing 位置（padding 后的内部原点）
    int pack_x = al.cursor_x + kGlyphPadding;
    int pack_y = al.cursor_y + kGlyphPadding;
    layer.atlas_x = pack_x;
    layer.atlas_y = pack_y;

    // 拷贝像素到 atlas
    for (int gy = 0; gy < ph; ++gy) {
        unsigned char* src = pixels + static_cast<size_t>(gy) * pw;
        unsigned char* dst = al.pixels.data()
            + static_cast<size_t>(pack_y + gy) * kAtlasDim + pack_x;
        std::memcpy(dst, src, static_cast<size_t>(pw));
    }

    // 更新 cursor
    al.cursor_x += padded_w;
    al.row_h = std::max(al.row_h, padded_h);
    al.dirty = true;

    // 释放光栅化像素（已拷贝到 atlas）
    STBTT_free(pixels, nullptr);
    layer.valid = true;
    return true;
}

const Glyph* FontManager::GetOrRasterizeGlyph(uint32_t codepoint,
                                                int preferred_level) {
    // 已存在
    auto it = glyphs_.find(codepoint);
    if (it != glyphs_.end()) {
        const Glyph& g = it->second;
        // 检查 preferred_level 是否有有效数据，有则直接返回
        if (preferred_level >= 0 && g.layers[preferred_level].valid) {
            return const_cast<Glyph*>(&g);
        }
        // 如果有指定 level 但该层无效，尝试光栅化到这个 level
        if (preferred_level >= 0 && !g.layers[preferred_level].valid) {
            Glyph* gptr = const_cast<Glyph*>(&g);
            if (RasterizeToLevel(codepoint, preferred_level, gptr)) {
                return gptr;
            }
        }
        // 找最近的有效层
        for (int i = 0; i < kNumLevels; ++i) {
            if (g.layers[i].valid) return const_cast<Glyph*>(&g);
        }
        return nullptr;
    }

    if (!font_info_ || !loaded_) {
        return nullptr;
    }

    // 新字形：新插入
    Glyph g;
    auto result = glyphs_.emplace(codepoint, g);
    Glyph* gptr = &result.first->second;

    if (preferred_level < 0) {
        preferred_level = kLevel16;  // 默认从小开始
    }

    // 尝试从 preferred_level 开始 pack，不行则 fallback 到更小层级
    int level = preferred_level;
    while (level >= 0) {
        if (RasterizeToLevel(codepoint, level, gptr)) {
            return gptr;
        }
        --level;
    }

    // 全层都满了，但可能空格已经被写入
    if (gptr->layers[0].valid) {
        return gptr;
    }
    return nullptr;
}

// ==================== DrawText2D 顶点生成 ====================

bool FontManager::GenerateVerticesAtLevel(std::string_view text,
                                            float font_size, int level,
                                            float pos_x, float pos_y,
                                            int alignment,
                                            int* selected_level,
                                            std::vector<float>* out_verts) {
    float base = kAtlasLevelBaseSizes[level];
    float scale = font_size / base;

    float min_x = 0.0f;
    float max_x = 0.0f;
    float min_y = 0.0f;
    float max_y = 0.0f;
    float cur_x = 0.0f;
    float cur_y = 0.0f;
    bool first_glyph = true;
    int lowest_in_pass = level;

    // ---- Pass 1: 包围盒 + 确保字形光栅化 ----
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

            const Glyph* g = GetOrRasterizeGlyph(cp, level);
            if (!g) continue;

            // 找可用层，记录最低层级
            int use_lv = level;
            const GlyphLayer* layer = nullptr;
            if (g->layers[level].valid) {
                layer = &g->layers[level];
            } else {
                for (int lv = level - 1; lv >= 0; --lv) {
                    if (g->layers[lv].valid) {
                        layer = &g->layers[lv];
                        use_lv = lv;
                        break;
                    }
                }
                if (!layer) continue;
            }
            lowest_in_pass = std::min(lowest_in_pass, use_lv);

            CHECK_GT(layer->base_size, 0.0f) << "GlyphLayer base_size is 0";
            float ls = font_size / layer->base_size;
            float gx = cur_x + layer->xoff * ls;
            float gy = cur_y + layer->yoff * ls;
            float gw = static_cast<float>(layer->w) * ls;
            float gh = static_cast<float>(layer->h) * ls;

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
            cur_x += layer->advance * ls;
        }
    }

    // 有字形需要 fallback → 让调用者降级重试
    if (lowest_in_pass < level) {
        *selected_level = lowest_in_pass;
        return false;
    }

    if (first_glyph) return false;

    // 对齐偏移
    float offset_x = pos_x;
    float offset_y = pos_y;
    {
        float bb_w = max_x - min_x;
        float bb_h = max_y - min_y;
        switch (static_cast<TextAlignment>(alignment)) {
            case TextAlignment::kTopLeft: break;
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

    // ---- Pass 2: 顶点生成 ----
    // Pass 2 不走 fallback：只用 level 层的数据。如果该层没有数据，
    // 说明 Pass 1 已经检测到 lowest_in_pass < level 并返回 false 了。
    static constexpr int kMaxTextChars = 1024;
    out_verts->clear();
    out_verts->reserve(kMaxTextChars * 6 * 4);

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

        const Glyph* g = GetOrRasterizeGlyph(cp, level);
        if (!g || !g->layers[level].valid) {
            char_count++;
            continue;
        }

        const GlyphLayer& layer = g->layers[level];

        if (layer.w == 0) {
            // 空格
            cur_x += layer.advance * scale;
            char_count++;
            continue;
        }

        float ls = font_size / layer.base_size;
        float gx = offset_x + cur_x + layer.xoff * ls;
        float gy = offset_y + cur_y + layer.yoff * ls;
        float gw = static_cast<float>(layer.w) * ls;
        float gh = static_cast<float>(layer.h) * ls;

        float inv_a = 1.0f / static_cast<float>(kAtlasDim);
        float tx0 = static_cast<float>(layer.atlas_x) * inv_a;
        float ty0 = static_cast<float>(layer.atlas_y) * inv_a;
        float tx1 = static_cast<float>(layer.atlas_x + layer.w) * inv_a;
        float ty1 = static_cast<float>(layer.atlas_y + layer.h) * inv_a;

        out_verts->push_back(gx);
        out_verts->push_back(gy);
        out_verts->push_back(tx0);
        out_verts->push_back(ty0);
        out_verts->push_back(gx + gw);
        out_verts->push_back(gy);
        out_verts->push_back(tx1);
        out_verts->push_back(ty0);
        out_verts->push_back(gx + gw);
        out_verts->push_back(gy + gh);
        out_verts->push_back(tx1);
        out_verts->push_back(ty1);
        out_verts->push_back(gx);
        out_verts->push_back(gy);
        out_verts->push_back(tx0);
        out_verts->push_back(ty0);
        out_verts->push_back(gx + gw);
        out_verts->push_back(gy + gh);
        out_verts->push_back(tx1);
        out_verts->push_back(ty1);
        out_verts->push_back(gx);
        out_verts->push_back(gy + gh);
        out_verts->push_back(tx0);
        out_verts->push_back(ty1);

        cur_x += layer.advance * ls;
        char_count++;
    }

    *selected_level = level;
    return !out_verts->empty();
}



bool FontManager::GenerateTextVertices(std::string_view text,
                                        float font_size,
                                        float pos_x, float pos_y,
                                        int alignment,
                                        int fbo_w, int fbo_h,
                                        int* selected_level,
                                        std::vector<float>* out_verts) {
    CHECK_GT(font_size, 0.0f);
    CHECK_NOTNULL(selected_level);
    CHECK_NOTNULL(out_verts);

    if (!loaded_) {
        LOG_EVERY_N(WARNING, kNotLoadedLogInterval)
            << "FontManager[" << font_name_ << "]: not loaded";
        return false;
    }

    int try_level = SelectBestLevel(font_size);

    // 降级尝试：从最佳层级开始，如果任何字形需要 fallback 到更小层级，
    // 就用那个更小层级重试，确保所有字形使用同一张 atlas 纹理。
    for (int lv = try_level; lv >= 0; --lv) {
        bool ok = GenerateVerticesAtLevel(text, font_size, lv,
                                           pos_x, pos_y, alignment,
                                           selected_level, out_verts);
        if (ok) return true;
    }
    return false;
}
}  // namespace jpov
