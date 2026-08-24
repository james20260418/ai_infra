// JPOV FontManager 实现
//
// 三层 atlas (16/32/48px)。FindGlyph / BuildGlyph / GenerateTextVertices
// 职责严格分离。

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
    if (font_info_) { STBTT_free(font_info_, nullptr); font_info_ = nullptr; }
    if (ttf_data_)   { STBTT_free(ttf_data_, nullptr);   ttf_data_ = nullptr; }
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
    for (int i = 0; i < kNumLevels; ++i) levels_[i] = std::move(other.levels_[i]);
    other.ttf_data_ = nullptr;
    other.font_info_ = nullptr;
    other.loaded_ = false;
}

FontManager& FontManager::operator=(FontManager&& other) noexcept {
    if (this != &other) {
        if (font_info_) { STBTT_free(font_info_, nullptr); }
        if (ttf_data_)   { STBTT_free(ttf_data_, nullptr); }
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
        for (int i = 0; i < kNumLevels; ++i) levels_[i] = std::move(other.levels_[i]);
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

    if (!mgr.LoadFontFile()) return std::nullopt;
    if (!mgr.ParseFont())   return std::nullopt;
    mgr.InitAtlas();
    mgr.loaded_ = true;
    mgr.PrerasterCharset();

    LOG(INFO) << "FontManager[" << mgr.font_name_
              << "]: loaded, 3-level atlas " << kAtlasDim << "x" << kAtlasDim;
    return mgr;
}

// ==================== 字体加载 ====================

bool FontManager::LoadFontFile() {
    FILE* fp = std::fopen(font_path_.c_str(), "rb");
    if (!fp) {
        LOG(WARNING) << "FontManager[" << font_name_ << "]: cannot open " << font_path_;
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
    int font_offset = 0;
    if (stbtt_GetNumberOfFonts(ttf_data_) > 1) {
        font_offset = stbtt_GetFontOffsetForIndex(ttf_data_, ttc_font_index_);
        LOG(INFO) << "FontManager[" << font_name_ << "]: TTC, index="
                  << ttc_font_index_;
    }
    if (!stbtt_InitFont(font_info_, ttf_data_, font_offset)) {
        STBTT_free(font_info_, nullptr);
        font_info_ = nullptr;
        LOG(WARNING) << "FontManager[" << font_name_ << "]: stbtt_InitFont failed";
        return false;
    }
    float scale = stbtt_ScaleForPixelHeight(font_info_, 16.0f);
    int asc, desc, lg;
    stbtt_GetFontVMetrics(font_info_, &asc, &desc, &lg);
    ascent_ = static_cast<float>(asc) * scale;
    descent_ = static_cast<float>(desc) * scale;
    linegap_ = static_cast<float>(lg) * scale;
    return true;
}

void FontManager::InitAtlas() {
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
    if (preraster_charset_.empty()) return;
    for (uint32_t cp : preraster_charset_) {
        BuildGlyph(cp);
    }
    LOG(INFO) << "FontManager[" << font_name_ << "]: prerastered "
              << preraster_charset_.size() << " codepoints";
}

// ==================== UTF-8 解码 ====================

uint32_t FontManager::DecodeUtf8(const char*& p) {
    uint8_t c = static_cast<uint8_t>(*p);
    if (c < kUtf8Cont) { ++p; return c; }
    if (c < kUtf8Lead2) { ++p; return kReplacementChar; }
    if (c < kUtf8Lead3) {
        uint32_t cp = c & kMask5;
        if ((static_cast<uint8_t>(p[1]) & ~kUtf8Mask6) != kUtf8Cont) { ++p; return kReplacementChar; }
        cp = (cp << 6) | (static_cast<uint8_t>(p[1]) & kUtf8Mask6);
        p += 2; return cp;
    }
    if (c < kUtf8Lead4) {
        uint32_t cp = c & kMask4;
        if ((static_cast<uint8_t>(p[1]) & ~kUtf8Mask6) != kUtf8Cont) { ++p; return kReplacementChar; }
        if ((static_cast<uint8_t>(p[2]) & ~kUtf8Mask6) != kUtf8Cont) { p += 2; return kReplacementChar; }
        cp = (cp << 6) | (static_cast<uint8_t>(p[1]) & kUtf8Mask6);
        cp = (cp << 6) | (static_cast<uint8_t>(p[2]) & kUtf8Mask6);
        p += 3; return cp;
    }
    uint32_t cp = c & kMask3;
    if ((static_cast<uint8_t>(p[1]) & ~kUtf8Mask6) != kUtf8Cont) { ++p; return kReplacementChar; }
    if ((static_cast<uint8_t>(p[2]) & ~kUtf8Mask6) != kUtf8Cont) { p += 2; return kReplacementChar; }
    if ((static_cast<uint8_t>(p[3]) & ~kUtf8Mask6) != kUtf8Cont) { p += 3; return kReplacementChar; }
    cp = (cp << 6) | (static_cast<uint8_t>(p[1]) & kUtf8Mask6);
    cp = (cp << 6) | (static_cast<uint8_t>(p[2]) & kUtf8Mask6);
    cp = (cp << 6) | (static_cast<uint8_t>(p[3]) & kUtf8Mask6);
    p += 4; return cp;
}

// ==================== FindGlyph / BuildGlyph ====================

const Glyph* FontManager::FindGlyph(uint32_t codepoint) const {
    auto it = glyphs_.find(codepoint);
    if (it != glyphs_.end()) return &it->second;
    return nullptr;
}

const Glyph* FontManager::BuildGlyph(uint32_t codepoint) {
    auto it = glyphs_.find(codepoint);
    if (it != glyphs_.end()) return &it->second;

    if (!font_info_ || !loaded_) return nullptr;

    // 一次性构建三层
    Glyph g;
    bool any_valid = false;
    for (int lv = 0; lv < kNumLevels; ++lv) {
        if (RasterizeToLevel(codepoint, lv, &g.layers[lv])) {
            any_valid = true;
        }
    }

    if (!any_valid) return nullptr;

    auto result = glyphs_.emplace(codepoint, std::move(g));
    return &result.first->second;
}

// ==================== 层级选择 ====================

int FontManager::SelectBestLevel(float font_size) const {
    int best = kLevel16;
    float best_penalty = 999.0f;
    for (int i = 0; i < kNumLevels; ++i) {
        float scale = font_size / levels_[i].base_size;
        if (scale < 0.4f) continue;
        float penalty = std::abs(std::log2(scale));
        if (penalty < best_penalty) {
            best_penalty = penalty;
            best = i;
        }
    }
    return best;
}

// ==================== 字形光栅化 ====================

bool FontManager::RasterizeToLevel(uint32_t codepoint, int level,
                                    GlyphLayer* layer) {
    float base = levels_[level].base_size;
    if (!font_info_ || !loaded_) return false;

    float scale = stbtt_ScaleForPixelHeight(font_info_, base);
    int pw, ph, xoff, yoff;
    unsigned char* pixels = stbtt_GetCodepointBitmap(
        font_info_, 0, scale, static_cast<int>(codepoint),
        &pw, &ph, &xoff, &yoff);

    int advance_width;
    stbtt_GetCodepointHMetrics(font_info_, static_cast<int>(codepoint),
                               &advance_width, nullptr);
    layer->advance = static_cast<float>(advance_width) * scale;
    layer->base_size = base;

    if (!pixels) {
        layer->w = 0; layer->h = 0;
        layer->xoff = 0.0f; layer->yoff = 0.0f;
        layer->atlas_x = 0; layer->atlas_y = 0;
        layer->valid = true;
        return true;
    }

    layer->w = pw; layer->h = ph;
    layer->xoff = static_cast<float>(xoff);
    layer->yoff = static_cast<float>(yoff);

    AtlasLevel& al = levels_[level];
    int padded_w = pw + kGlyphPadding * 2;
    int padded_h = ph + kGlyphPadding * 2;

    if (al.cursor_x + padded_w > kAtlasDim) {
        al.cursor_x = 0;
        al.cursor_y += al.row_h;
        al.row_h = 0;
    }
    if (al.cursor_y + padded_h > kAtlasDim) {
        STBTT_free(pixels, nullptr);
        LOG_FIRST_N(WARNING, 1) << "FontManager[" << font_name_
                                << "]: atlas level " << level << " full";
        return false;
    }

    int pack_x = al.cursor_x + kGlyphPadding;
    int pack_y = al.cursor_y + kGlyphPadding;
    layer->atlas_x = pack_x;
    layer->atlas_y = pack_y;

    for (int gy = 0; gy < ph; ++gy) {
        unsigned char* src = pixels + static_cast<size_t>(gy) * pw;
        unsigned char* dst = al.pixels.data()
            + static_cast<size_t>(pack_y + gy) * kAtlasDim + pack_x;
        std::memcpy(dst, src, static_cast<size_t>(pw));
    }

    al.cursor_x += padded_w;
    al.row_h = std::max(al.row_h, padded_h);
    al.dirty = true;

    STBTT_free(pixels, nullptr);
    layer->valid = true;
    return true;
}

// ==================== DrawText2D 顶点生成 ====================

static const GlyphLayer* PickBestLayer(const Glyph& g, int preferred) {
    if (g.layers[preferred].valid) return &g.layers[preferred];
    for (int lv = preferred - 1; lv >= 0; --lv) {
        if (g.layers[lv].valid) return &g.layers[lv];
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// 内部辅助: 构建所有字形 + 降级选层
// ---------------------------------------------------------------------------

static bool EnsureGlyphsBuilt(FontManager& fm, std::string_view text) {
    const char* p = text.data();
    const char* end = text.data() + text.size();
    while (p < end) {
        uint32_t cp = FontManager::DecodeUtf8(p);
        if (cp != '\n' && !fm.BuildGlyph(cp)) return false;
    }
    return true;
}

static int SelectDrawLevel(const FontManager& fm, std::string_view text,
                            int best_level) {
    for (int lv = best_level; lv >= 0; --lv) {
        bool all_ok = true;
        const char* p = text.data();
        const char* end = text.data() + text.size();
        while (p < end) {
            uint32_t cp = FontManager::DecodeUtf8(p);
            if (cp == '\n') continue;
            const Glyph* g = fm.FindGlyph(cp);
            if (!g || !PickBestLayer(*g, lv)) { all_ok = false; break; }
        }
        if (all_ok) return lv;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// 内部辅助: 计算包围盒
// ---------------------------------------------------------------------------

static bool ComputeTextBounds(const FontManager& fm, std::string_view text,
                               int use_level, float font_size,
                               float scale,
                               float* out_min_x, float* out_max_x,
                               float* out_min_y, float* out_max_y) {
    float min_x = 0.0f, max_x = 0.0f, min_y = 0.0f, max_y = 0.0f;
    float cur_x = 0.0f, cur_y = 0.0f;
    bool first = true;

    const char* p = text.data();
    const char* end = text.data() + text.size();
    while (p < end) {
        uint32_t cp = FontManager::DecodeUtf8(p);
        if (cp == '\n') {
            cur_x = 0.0f;
            cur_y += (fm.ascent() - fm.descent() + fm.linegap()) * scale;
            continue;
        }
        const Glyph* g = fm.FindGlyph(cp);
        if (!g) continue;
        const GlyphLayer* layer = PickBestLayer(*g, use_level);
        if (!layer) continue;

        float ls = font_size / layer->base_size;
        float gx = cur_x + layer->xoff * ls;
        float gy = cur_y + layer->yoff * ls;
        float gw = static_cast<float>(layer->w) * ls;
        float gh = static_cast<float>(layer->h) * ls;
        if (first) {
            min_x = gx; max_x = gx + gw;
            min_y = gy; max_y = gy + gh;
            first = false;
        } else {
            min_x = std::min(min_x, gx);
            max_x = std::max(max_x, gx + gw);
            min_y = std::min(min_y, gy);
            max_y = std::max(max_y, gy + gh);
        }
        cur_x += layer->advance * ls;
    }
    if (first) return false;

    *out_min_x = min_x;
    *out_max_x = max_x;
    *out_min_y = min_y;
    *out_max_y = max_y;
    return true;
}

// ---------------------------------------------------------------------------
// 内部辅助: 将对齐点转换为逐字起始偏移（以使 pos 成为包围盒的对应角）
// ---------------------------------------------------------------------------

static void ComputeAlignmentOffset(float pos_x, float pos_y,
                                    float min_x, float max_x,
                                    float min_y, float max_y,
                                    int alignment,
                                    float* out_off_x, float* out_off_y) {
    float bw = max_x - min_x;
    float bh = max_y - min_y;
    switch (static_cast<TextAlignment>(alignment)) {
        case TextAlignment::kTopLeft:
            *out_off_x = pos_x - min_x;
            *out_off_y = pos_y - min_y;
            break;
        case TextAlignment::kTopRight:
            *out_off_x = pos_x - max_x;
            *out_off_y = pos_y - min_y;
            break;
        case TextAlignment::kCenter:
            *out_off_x = pos_x - min_x - bw * 0.5f;
            *out_off_y = pos_y - min_y - bh * 0.5f;
            break;
        case TextAlignment::kBottomLeft:
            *out_off_x = pos_x - min_x;
            *out_off_y = pos_y - max_y;
            break;
        case TextAlignment::kBottomRight:
            *out_off_x = pos_x - max_x;
            *out_off_y = pos_y - max_y;
            break;
        case TextAlignment::kMidLeft:
            *out_off_x = pos_x - min_x;
            *out_off_y = pos_y - min_y - bh * 0.5f;
            break;
        case TextAlignment::kMidRight:
            *out_off_x = pos_x - max_x;
            *out_off_y = pos_y - min_y - bh * 0.5f;
            break;
        case TextAlignment::kMidTop:
            *out_off_x = pos_x - min_x - bw * 0.5f;
            *out_off_y = pos_y - min_y;
            break;
        case TextAlignment::kMidBottom:
            *out_off_x = pos_x - min_x - bw * 0.5f;
            *out_off_y = pos_y - max_y;
            break;
        default:
            LOG(FATAL) << "Unknown TextAlignment: " << alignment;
    }
}

// ---------------------------------------------------------------------------
// FontManager::GenerateTextVertices
// ---------------------------------------------------------------------------

bool FontManager::GenerateTextVertices(std::string_view text,
                                        float font_size,
                                        float pos_x, float pos_y,
                                        int alignment,
                                        int /*fbo_w*/, int /*fbo_h*/,
                                        int* selected_level,
                                        std::vector<float>* out_verts) {
    CHECK_GT(font_size, 0.0f);
    CHECK_NOTNULL(selected_level);
    CHECK_NOTNULL(out_verts);
    if (!loaded_) return false;

    // 步骤 0: 确保所有字形已构建
    if (!EnsureGlyphsBuilt(*this, text)) return false;

    int best_level = SelectBestLevel(font_size);
    int use_level = SelectDrawLevel(*this, text, best_level);
    if (use_level < 0) return false;

    float scale = font_size / kAtlasLevelBaseSizes[best_level];

    // 步骤 1: 计算包围盒
    float min_x = 0.0f, max_x = 0.0f, min_y = 0.0f, max_y = 0.0f;
    if (!ComputeTextBounds(*this, text, use_level, font_size, scale,
                            &min_x, &max_x, &min_y, &max_y)) {
        return false;
    }

    // 步骤 2: 对齐偏移
    float off_x = pos_x, off_y = pos_y;
    ComputeAlignmentOffset(pos_x, pos_y,
                            min_x, max_x, min_y, max_y,
                            alignment, &off_x, &off_y);

    // 步骤 3: 逐字生成顶点
    static constexpr int kMaxChars = 1024;
    out_verts->clear();
    out_verts->reserve(kMaxChars * 6 * 4);
    float cur_x = 0.0f, cur_y = 0.0f;
    {
        const char* p = text.data();
        const char* end = text.data() + text.size();
        int count = 0;
        while (p < end && count < kMaxChars) {
            uint32_t cp = DecodeUtf8(p);
            if (cp == '\n') {
                cur_x = 0.0f;
                cur_y += (ascent_ - descent_ + linegap_) * scale;
                continue;
            }
            const Glyph* g = FindGlyph(cp);
            if (!g) { count++; continue; }
            const GlyphLayer* layer = PickBestLayer(*g, use_level);
            if (!layer) { count++; continue; }
            if (layer->w == 0) {
                cur_x += layer->advance * (font_size / layer->base_size);
                count++;
                continue;
            }

            float ls = font_size / layer->base_size;
            float gx = off_x + cur_x + layer->xoff * ls;
            float gy = off_y + cur_y + layer->yoff * ls;
            float gw = static_cast<float>(layer->w) * ls;
            float gh = static_cast<float>(layer->h) * ls;
            float inv = 1.0f / static_cast<float>(kAtlasDim);
            float t0x = static_cast<float>(layer->atlas_x) * inv;
            float t0y = static_cast<float>(layer->atlas_y) * inv;
            float t1x = static_cast<float>(layer->atlas_x + layer->w) * inv;
            float t1y = static_cast<float>(layer->atlas_y + layer->h) * inv;

            out_verts->push_back(gx);      out_verts->push_back(gy);
            out_verts->push_back(t0x);     out_verts->push_back(t0y);
            out_verts->push_back(gx + gw); out_verts->push_back(gy);
            out_verts->push_back(t1x);     out_verts->push_back(t0y);
            out_verts->push_back(gx + gw); out_verts->push_back(gy + gh);
            out_verts->push_back(t1x);     out_verts->push_back(t1y);
            out_verts->push_back(gx);      out_verts->push_back(gy);
            out_verts->push_back(t0x);     out_verts->push_back(t0y);
            out_verts->push_back(gx + gw); out_verts->push_back(gy + gh);
            out_verts->push_back(t1x);     out_verts->push_back(t1y);
            out_verts->push_back(gx);      out_verts->push_back(gy + gh);
            out_verts->push_back(t0x);     out_verts->push_back(t1y);

            cur_x += layer->advance * ls;
            count++;
        }
    }

    *selected_level = use_level;
    return !out_verts->empty();
}

// ==================== FontManager::MeasureTextWidth ====================

float FontManager::MeasureTextWidth(std::string_view text,
                                     float font_size) const {
    CHECK_GT(font_size, 0.0f);
    if (!loaded_ || font_info_ == nullptr) return 0.0f;

    // 直接用 stbtt 字体度量累加每个字形的 advance，得到渲染层把文本逐个
    // 排布后 pen 的水平终点（= 光标 X）。与 RasterizeToLevel / 渲染推进的
    // advance 完全一致：advance_px = stbtt_GetCodepointHMetrics() *
    // stbtt_ScaleForPixelHeight(font, font_size)（ScaleForPixelHeight 线性，
    // 与 atlas 层级无关）。因此无需依赖字形是否已光栅化（FindGlyph 只在
    // BuildGlyph 后才有值），首次测量（渲染前）结果也正确，且与渲染完全吻合。
    // 缺字形（codepoint 超出字体范围）→ 该字符计 0 宽（与渲染层跳 0 宽一致）。
    const float scale = stbtt_ScaleForPixelHeight(font_info_, font_size);
    float width = 0.0f;
    const char* p = text.data();
    const char* end = text.data() + text.size();
    while (p < end) {
        uint32_t cp = DecodeUtf8(p);
        if (cp == '\n') {
            // 换行：水平归零、垂直换行（光标宽度按当前行水平终点计，
            // 单行输入不会走到这）。
            width = 0.0f;
            continue;
        }
        int advance_width = 0;
        int left_side_bearing = 0;  // 未使用，占位。
        stbtt_GetCodepointHMetrics(font_info_, static_cast<int>(cp),
                                   &advance_width, &left_side_bearing);
        width += static_cast<float>(advance_width) * scale;
    }
    return width;
}

}  // namespace jpov
