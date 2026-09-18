// JPOV gen3d — 生成输入图片的本地格式审查（实现）
#include "tools/jpov/gen3d/static/image_input.h"

#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <string>

#include <glog/logging.h>
#include <stb_image.h>

namespace jpov {
namespace gen3d {
namespace {

// 受支持的图片扩展名（小写比较）。Tripo: JPEG / PNG / WebP。
bool IsSupportedExtension(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size()) {
        return false;
    }
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "webp";
}

// 读文件头若干字节，用于按 magic bytes 判断真实格式。
// 返回实际读到的字节数（0 = 打不开或空文件）。
size_t ReadHeadBytes(const std::string& path, unsigned char* buf, size_t n) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return 0;
    }
    const size_t got = std::fread(buf, 1, n, f);
    std::fclose(f);
    return got;
}

// 按 magic bytes 判断真实格式（不信扩展名——扩展名可能与内容不符）。
// 返回格式名（空 = 未识别）。
std::string DetectFormatByMagic(const unsigned char* b, size_t n) {
    if (n >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G' &&
        b[4] == 0x0D && b[5] == 0x0A && b[6] == 0x1A && b[7] == 0x0A) {
        return "png";
    }
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) {
        return "jpeg";
    }
    // WebP: "RIFF" .... "WEBP"
    if (n >= 12 && b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F' &&
        b[8] == 'W' && b[9] == 'E' && b[10] == 'B' && b[11] == 'P') {
        return "webp";
    }
    return "";
}

// 扫描解码后的像素，统计 alpha 情况。
//   stb 语义：channels 1=灰 2=灰+alpha 3=RGB 4=RGBA。
//   仅对含 alpha 的格式（2/4）扫描；否则直接保持全 false。
void ScanAlpha(const unsigned char* pixels, int width, int height,
               int channels, ImageCheckResult* out /*output*/) {
    CHECK_NOTNULL(out);
    if (channels != 2 && channels != 4) {
        return;  // 无 alpha 通道
    }
    out->has_alpha_channel = true;
    const size_t n_px = static_cast<size_t>(width) * static_cast<size_t>(height);
    for (size_t i = 0; i < n_px; ++i) {
        const unsigned char a = pixels[i * channels + (channels - 1)];
        if (a != 255) {
            out->has_nontrivial_alpha = true;
        }
        if (a == 0) {
            out->has_transparent_pixels = true;
        }
        // 两个标志都置位后无需继续扫描（大图省时）。
        if (out->has_nontrivial_alpha && out->has_transparent_pixels) {
            break;
        }
    }
}

}  // namespace

ImageCheckResult CheckImageFile(const std::string& path,
                                const ImageLimits& limits) {
    CHECK(!path.empty()) << "CheckImageFile 需要非空路径";
    ImageCheckResult r;

    // ---- 1) 文件存在 + 大小 ----
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        r.errors.push_back("文件不存在或无法访问");
        return r;
    }
    if (!S_ISREG(st.st_mode)) {
        r.errors.push_back("不是常规文件");
        return r;
    }
    r.file_bytes = static_cast<long>(st.st_size);
    if (r.file_bytes == 0) {
        r.errors.push_back("文件为空");
        return r;
    }
    if (r.file_bytes > limits.max_file_bytes) {
        r.errors.push_back("文件过大: " + std::to_string(r.file_bytes)
            + " 字节 > 上限 " + std::to_string(limits.max_file_bytes)
            + " 字节（Tripo 限制 20MB）");
        // 超限仍继续解码以报告尺寸，但 ok 已注定为 false。
    }

    // ---- 2) 扩展名 ----
    if (!IsSupportedExtension(path)) {
        r.errors.push_back("扩展名不受支持（需 .png / .jpg / .jpeg / .webp）");
    }

    // ---- 3) magic bytes（防扩展名与内容不符）----
    constexpr size_t kHeadLen = 12;
    unsigned char head[kHeadLen] = {0};
    const size_t head_n = ReadHeadBytes(path, head, kHeadLen);
    const std::string magic_fmt = DetectFormatByMagic(head, head_n);
    if (magic_fmt.empty()) {
        r.errors.push_back("文件内容不是受支持的图片格式（magic bytes 未识别）");
    }

    // ---- 4) 解码取尺寸 + alpha 扫描 ----
    int w = 0;
    int h = 0;
    int channels = 0;
    // 强制按原始通道数解码（req_comp=0），才能判出是否真带 alpha。
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 0);
    if (pixels == nullptr) {
        const char* reason = stbi_failure_reason();
        r.errors.push_back("图片解码失败: "
            + std::string(reason != nullptr ? reason : "(未知原因)"));
        return r;
    }
    r.width = w;
    r.height = h;
    r.channels = channels;
    ScanAlpha(pixels, w, h, channels, &r);
    stbi_image_free(pixels);

    // ---- 5) 分辨率 ----
    if (w < limits.min_edge_px || h < limits.min_edge_px) {
        r.errors.push_back("分辨率过小: " + std::to_string(w) + "x"
            + std::to_string(h) + "，每边需 >= "
            + std::to_string(limits.min_edge_px) + "px");
    }
    if (w > limits.max_edge_px || h > limits.max_edge_px) {
        r.errors.push_back("分辨率过大: " + std::to_string(w) + "x"
            + std::to_string(h) + "，每边需 <= "
            + std::to_string(limits.max_edge_px) + "px");
    }

    r.ok = r.errors.empty();
    return r;
}

std::vector<ImageCheckResult> CheckImageFiles(
    const std::vector<std::string>& paths, const ImageLimits& limits) {
    std::vector<ImageCheckResult> results;
    results.reserve(paths.size());
    for (const std::string& p : paths) {
        results.push_back(CheckImageFile(p, limits));
    }

    // ---- 组合层面检查（Tripo multiview 的输入契约）----
    // 数量问题以"往结果里追加 error"的方式表达，使调用方只需遍历 results
    // 就能拿到全部问题（含单图本身的问题）。
    constexpr size_t kMinViews = 2;  // Tripo：至少 2 张（少于 2 张无法生成）
    constexpr size_t kMaxViews = 4;  // Tripo：最多 4 张 [front,left,back,right]
    if (paths.size() < kMinViews || paths.size() > kMaxViews) {
        const std::string msg = "视图数量非法: " + std::to_string(paths.size())
            + " 张，需 " + std::to_string(kMinViews) + "~"
            + std::to_string(kMaxViews)
            + " 张（Tripo 顺序 [front, left, back, right]）";
        if (results.empty()) {
            // paths 为空：没有可挂载的结果，构造一条纯组合错误的记录，
            // 使调用方仍能通过"遍历 results"拿到该问题。
            ImageCheckResult r;
            r.errors.push_back(msg);
            results.push_back(r);
        } else {
            results[0].errors.push_back(msg);
            results[0].ok = false;
        }
    }
    // front（paths[0]）未过审 → 整个 multiview 请求不可提交（Tripo 要求 front 必填）。
    // 注：front 自身的失败原因已在 results[0].errors 里（CheckImageFile 填充，
    //   或上面的视图数量检查追加）；此处不重复造错误信息 —— 若 results[0] 已
    //   不合格却没有任何错误文本（理论上不应发生），补一条兜底提示。
    if (!results.empty() && !results[0].ok && results[0].errors.empty()) {
        results[0].errors.push_back("front 视图未通过审查（无具体原因，内部异常）");
    }
    return results;
}

std::string FormatImageCheckReport(const std::string& path,
                                   const ImageCheckResult& result) {
    std::string s;
    s += "  " + path + "\n";
    if (result.width > 0 && result.height > 0) {
        s += "    尺寸: " + std::to_string(result.width) + "x"
            + std::to_string(result.height)
            + "  通道: " + std::to_string(result.channels)
            + "  大小: " + std::to_string(result.file_bytes) + " 字节\n";
        if (result.has_alpha_channel) {
            s += std::string("    alpha: 有通道")
                + (result.has_nontrivial_alpha ? "，含非 255 像素"
                                               : "，但全不透明")
                + (result.has_transparent_pixels ? "；含全透明像素"
                                                 : "; 无全透明像素")
                + "\n";
        } else {
            s += "    alpha: 无 alpha 通道\n";
        }
    } else {
        s += "    大小: " + std::to_string(result.file_bytes) + " 字节\n";
    }
    if (result.ok) {
        s += "    结果: 通过\n";
    } else {
        s += "    结果: 不通过\n";
        for (const std::string& e : result.errors) {
            s += "      - " + e + "\n";
        }
    }
    return s;
}

}  // namespace gen3d
}  // namespace jpov
