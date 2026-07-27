// JPOV FontManager — 字体管理器
//
// FontManager 唯一对应一种字体。它维护三层 atlas（16/32/48px），
// 每层独立的 CPU 图集 + 行式 packing。
//
// 绘制时自动选择最合适的层级（缩放比最小的那层），
// atlas 填满时按 48→32→16 顺序 fallback，全满才跳过。
//
// FontManager 不持有 OpenGL 纹理资源（atlas 纹理由 Renderer 持有）。
// 通过静态工厂方法 FontManager::Create() 构造。

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

// 字体管理器的配置（不含 GPU 资源句柄）。
struct FontManagerConfig {
    // 字体名称（仅用于日志和调试）
    std::string font_name;

    // 字体文件路径（相对于 cwd 或绝对路径）
    std::string font_path;

    // 预渲染字符集（如常用汉字和 ASCII）。
    // 在 Create 时一次性光栅化这些字符到所有层级的图集。
    // 为空则仅按需加载。
    std::vector<uint32_t> preraster_charset;

    // TTC 字体索引（TrueType Collection 的子字体序号）。
    // 对普通 ttf/otf 文件忽略此字段。
    int ttc_font_index = 0;
};

// ==================== GlyphLayer ====================

// 一个字形的单层图集信息。
// valid=true 表示该层成功 pack 到图集中。
struct GlyphLayer {
    int w = 0;              // 字形宽度（像素）
    int h = 0;              // 字形高度（像素）
    float advance = 0.0f;   // 水平步进宽度（像素，针对该层的 base_size）
    float xoff = 0.0f;      // 左侧偏移（bearing X，像素）
    float yoff = 0.0f;      // 顶部偏移（bearing Y，像素）
    int atlas_x = 0;        // 图集中左下角 x（像素）
    int atlas_y = 0;        // 图集中左下角 y（像素）
    float base_size = 0.0f; // 该层的光栅化基础字号（16/32/48）
    bool valid = false;     // 是否真正 pack 到图集（空格等无像素字符也为 true）
};

// ==================== Glyph ====================

// 一个字形的三层 atlas 信息。[0]=16px, [1]=32px, [2]=48px。
// GenerateTextVertices 遍历三层找最佳匹配。
struct Glyph {
    GlyphLayer layers[3];

    // 是否至少有一层有像素（非空格）
    bool has_pixels() const {
        for (const auto& layer : layers) {
            if (layer.valid && layer.w > 0) return true;
        }
        return false;
    }

    // 取某一层的缩放因子（目标字号 / 该层 base_size）
    float layer_scale(int level) const {
        return layers[level].base_size;
    }
};

// ==================== AtlasLevel ====================

// 图集的某一层（16/32/48px），独立 CPU 像素 + cursor
struct AtlasLevel {
    static constexpr float kBaseSizes[3] = {16.0f, 32.0f, 48.0f};

    float base_size = 0.0f;
    std::vector<uint8_t> pixels;
    int cursor_x = 0;
    int cursor_y = 0;
    int row_h = 0;
    bool dirty = false;     // 本层有新字形加入，需要 GL 上传
};

// ==================== FontManager ====================

class FontManager {
public:
    static constexpr int kAtlasDim = 4096;

    // 层级索引常量
    static constexpr int kLevel16 = 0;
    static constexpr int kLevel32 = 1;
    static constexpr int kLevel48 = 2;
    static constexpr int kNumLevels = 3;

    // 字形间间隔像素（避免渲染时相邻字符颜色渗出）
    static constexpr int kGlyphPadding = 2;

    // 全局日志频率控制
    static constexpr int kUploadLogInterval = 5;
    static constexpr int kNotLoadedLogInterval = 60;

    // 工厂：加载字体，初始化三层 atlas，光栅化预渲染字符集。
    // 返回 std::nullopt 表示加载失败。
    static std::optional<FontManager> Create(const FontManagerConfig& config);

    ~FontManager();

    // Move
    FontManager(FontManager&& other) noexcept;
    FontManager& operator=(FontManager&& other) noexcept;
    FontManager(const FontManager&) = delete;
    FontManager& operator=(const FontManager&) = delete;

    // ---- 只读属性 ----

    bool loaded() const { return loaded_; }

    // 字体度量（16px base 的像素值）
    float ascent() const { return ascent_; }
    float descent() const { return descent_; }
    float linegap() const { return linegap_; }

    // 某一层图集的 CPU 像素（只读，灰度，R8 格式）
    const std::vector<uint8_t>& atlas_pixels(int level) const {
        return levels_[level].pixels;
    }

    // 某一层是否需要 GL 上传
    bool atlas_dirty(int level) const { return levels_[level].dirty; }

    // 标记某层已上传
    void mark_atlas_clean(int level) { levels_[level].dirty = false; }

    // 全局是否有任何层 dirty
    bool any_atlas_dirty() const {
        for (int i = 0; i < kNumLevels; ++i) {
            if (levels_[i].dirty) return true;
        }
        return false;
    }

    // ---- 核心操作 ----

    // UTF-8 解码
    static uint32_t DecodeUtf8(const char*& p);

    // 获取或光栅化一个字形。
    // 先在所有已有层中查找最佳匹配，找不到则尝试从最佳层开始光栅化+packing。
    // 返回 nullptr 表示码点无字形数据。
    //
    // preferred_level — hint：优先用哪一层（-1 表示自动选择）
    const Glyph* GetOrRasterizeGlyph(uint32_t codepoint,
                                     int preferred_level = -1);

    // 为 DrawText2D 生成顶点数据。
    //
    // selected_level — output: 实际使用的 atlas 层级（用于选择 GL 纹理）
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

    // 为特定层级光栅化并 pack 一个字形
    // 返回 true 表示成功 pack（或字符为空格）
    bool RasterizeToLevel(uint32_t codepoint, int level, Glyph* out_glyph);

    // 根据目标字号选择最佳层级
    int SelectBestLevel(float font_size) const;

    // font file data
    unsigned char* ttf_data_ = nullptr;
    long ttf_data_size_ = 0;

    // stb_truetype state
    stbtt_fontinfo* font_info_ = nullptr;

    // configured properties
    std::string font_name_;
    std::string font_path_;
    std::vector<uint32_t> preraster_charset_;
    int ttc_font_index_ = 0;

    // runtime state
    bool loaded_ = false;
    float ascent_ = 0.0f;    // 16px 度量值（实际绘制时按 scale 换算）
    float descent_ = 0.0f;
    float linegap_ = 0.0f;

    // 三层 atlas
    AtlasLevel levels_[kNumLevels];

    // 字形缓存：codepoint → 三层信息
    std::unordered_map<uint32_t, Glyph> glyphs_;

    // ---- UTF-8 常量 ----
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
