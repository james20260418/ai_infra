// JPOV FontManager — 字体管理器
//
// FontManager 唯一对应一种字体。它负责：
//   - 加载字体文件、解析度量信息
//   - 按需光栅化字形到 CPU 图集像素缓冲区
//   - 行式 packing 管理图集空间
//   - UTF-8 解码
//   - 为 DrawText2D 生成顶点数据
//
// FontManager 不持有 OpenGL 纹理资源（atlas 纹理由 Renderer 持有）。
//
// 通过静态工厂方法 FontManager::Create() 构造，传入 FontManagerConfig。
// 图集纹理由 Renderer 创建和销毁，FontManager::atlas_pixels() 获取 CPU 像素面，
// Renderer 负责上传到 GL。

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
// GPU 纹理 ID 和 atlas 尺寸在 FontManager 构造后由 Renderer 设置。
struct FontManagerConfig {
    // 基本字号（像素单位），一切字形光栅化基于此字号
    // 设为 16px：直接光栅化尺寸最清晰，放大用 GL_LINEAR 足够
    float base_font_size = 16.0f;

    // 字体名称（仅用于日志和调试）
    std::string font_name;

    // 字体文件路径（相对于 cwd 或绝对路径）
    std::string font_path;

    // 预渲染字符集（如常用汉字和 ASCII）。
    // 在 Create 时一次性光栅化这些字符到图集，后续 DrawText 时无需逐字加载。
    // 为空则仅按需加载。
    std::vector<uint32_t> preraster_charset;

    // TTC 字体索引（TrueType Collection 的子字体序号）。
    // 对普通 ttf/otf 文件忽略此字段。
    int ttc_font_index = 0;
};

// ==================== GlyphMetadata ====================

// 图集中一个字形的元信息（不含像素数据，像素已拷贝到 CPU 图集缓冲区）。
struct GlyphMetadata {
    int w = 0;              // 字形宽度（像素）
    int h = 0;              // 字形高度（像素）
    float advance = 0.0f;   // 水平步进宽度（像素，基于 base_font_size）
    float xoff = 0.0f;      // 左侧偏移（bearing X，像素）
    float yoff = 0.0f;      // 顶部偏移（bearing Y，像素）
    int atlas_x = 0;        // 图集中左下角 x（像素）
    int atlas_y = 0;        // 图集中左下角 y（像素）
};

// ==================== FontManager ====================

class FontManager {
public:
    // 图集尺寸（w==h，正方形），纹理尺寸
    static constexpr int kAtlasDim = 4096;

    // 字形间间隔像素（避免渲染时相邻字符颜色渗出）
    static constexpr int kGlyphPadding = 2;

    // 全局日志频率控制
    static constexpr int kUploadLogInterval = 5;
    static constexpr int kNotLoadedLogInterval = 60;

    // 工厂：加载字体，光栅化预渲染字符集，初始化 CPU 图集。
    // 返回 std::nullopt 表示加载失败（config.font_path 不存在或解析错误）。
    // 调用者需创建 GL 纹理并上传 atlas_pixels()。
    static std::optional<FontManager> Create(const FontManagerConfig& config);

    ~FontManager();

    // Move: 转移 raw pointer 所有权，源对象置空
    FontManager(FontManager&& other) noexcept;
    FontManager& operator=(FontManager&& other) noexcept;
    FontManager(const FontManager&) = delete;
    FontManager& operator=(const FontManager&) = delete;

    // ---- 只读属性 ----

    // 字体是否成功加载
    bool loaded() const { return loaded_; }

    // 字体度量（基于 base_font_size，像素单位）
    float ascent() const { return ascent_; }
    float descent() const { return descent_; }
    float linegap() const { return linegap_; }

    // 基本字号
    float base_font_size() const { return base_font_size_; }

    // 图集 CPU 像素缓冲区（只读，灰度，R8 格式）。
    // 尺寸 = kAtlasDim × kAtlasDim。
    const std::vector<uint8_t>& atlas_pixels() const { return atlas_pixels_; }

    // 是否需要重新上传到 GL（有新的字形被加入图集）
    bool atlas_dirty() const { return atlas_dirty_; }

    // 标记图集已上传到 GL（由 Renderer 在 UploadAtlas 后调用）
    void mark_atlas_clean() { atlas_dirty_ = false; }

    // ---- 核心操作 ----

    // UTF-8 解码：从字符串指针读取一个 Unicode 码点，指针前进。
    // 输入 "你好" → 第一次返回 0x4F60，p→"好"；第二次返回 0x597D。
    // 非法字节序列返回 U+FFFD（replacement character）。
    static uint32_t DecodeUtf8(const char*& p);

    // 获取或光栅化一个字形到图集。
    // 如果字形已存在直接返回；否则从字体文件光栅化并 packing 到图集。
    // 返回 nullptr 表示码点无字形（空格/无数据），此时 advance 仍有效。
    //
    // 注意：返回的指针在下次 GetOrRasterizeGlyph() 调用后可能无效。
    const GlyphMetadata* GetOrRasterizeGlyph(uint32_t codepoint);

    // 为 DrawText2D 生成顶点数据（位置 + 纹理坐标），用于 VBO 上传。
    //
    // text        — input: UTF-8 文本
    // font_size   — input: 目标字号（像素单位）
    // pos_x, pos_y — input: 文本定位点
    // alignment   — input: 对齐方式
    // fbo_w, fbo_h — input: FBO 尺寸（reserved）
    // out_verts   — output: 顶点数据，每 6 个顶点一个字形四边形
    //                       交错格式: {x,y,tx,ty} 重复
    //
    // 返回 true 表示成功生成至少一个字形顶点。
    bool GenerateTextVertices(std::string_view text,
                              float font_size,
                              float pos_x, float pos_y,
                              int alignment,
                              int fbo_w, int fbo_h,
                              std::vector<float>* out_verts /*output*/);

private:
    FontManager() = default;

    bool LoadFontFile();
    bool ParseFont();
    bool InitAtlas();
    void PrerasterCharset();

    // font file data
    unsigned char* ttf_data_ = nullptr;
    long ttf_data_size_ = 0;

    // stb_truetype state
    stbtt_fontinfo* font_info_ = nullptr;

    // configured properties
    float base_font_size_ = 16.0f;
    std::string font_name_;
    std::string font_path_;
    std::vector<uint32_t> preraster_charset_;
    int ttc_font_index_ = 0;

    // runtime state
    bool loaded_ = false;
    bool atlas_dirty_ = false;
    float ascent_ = 0.0f;
    float descent_ = 0.0f;
    float linegap_ = 0.0f;

    // CPU 图集
    std::vector<uint8_t> atlas_pixels_;
    int atlas_cursor_x_ = 0;
    int atlas_cursor_y_ = 0;
    int atlas_row_h_ = 0;

    // 字形缓存：codepoint → metadata
    std::unordered_map<uint32_t, GlyphMetadata> glyphs_;

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
