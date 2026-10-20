// WBO 权重函数「文本契约」测试
//
// 背景（2026-10-20 重构）：WBO 的权重函数决定观感的一切（颜色加权平均 + 近处优先），
// 但它有两份副本分处两个文件：
//   - **权威副本**：src/wbo/wbo_shader.h 的 kWboWeightGlsl（粒子/条带/体积火注入用）
//   - **内联副本**：src/object3d/object3d_renderer.h 的 PBR shader（玻璃 kBlend 变体）
//     —— 那份 shader 是单个大字符串，注入需要拆串，本次不动。
//
// 只改一边的后果：同一个像素，玻璃和粒子用了不同的权重 —— 画面"有点不对"，
// 但没有任何逐字节 gold 会红（两个 gold 各自和自己比）。所以用这个测试把两份文本钉死。
//
// 判据：两段 `//@WBO_WEIGHT_BEGIN` .. `//@WBO_WEIGHT_END` 之间的代码，
// **去掉行注释 + 归一化空白**后逐字相同。
// 另外做一次「比较器确实能分辨差异」的自检（把常量改一位必须判不等），
// 防止哪天 Normalize 退化成恒等比较而测试仍然绿。

#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/src/object3d/object3d_renderer.h"
#include "tools/jpov/src/wbo/wbo_shader.h"

namespace {

constexpr const char* kBeginMarker = "//@WBO_WEIGHT_BEGIN";
constexpr const char* kEndMarker = "//@WBO_WEIGHT_END";

// 去掉 `//` 行注释（GLSL 片段里没有块注释，够用）。
std::string StripLineComments(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    size_t i = 0;
    while (i < src.size()) {
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') {
                ++i;
            }
            continue;
        }
        out.push_back(src[i]);
        ++i;
    }
    return out;
}

// 所有空白（含换行/制表）压成单个空格，并去掉首尾空白。
std::string CollapseWhitespace(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    bool pending_space = false;
    for (const char ch : src) {
        const bool is_space = (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n');
        if (is_space) {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) {
            out.push_back(' ');
            pending_space = false;
        }
        out.push_back(ch);
    }
    return out;
}

std::string Normalize(const std::string& src) {
    return CollapseWhitespace(StripLineComments(src));
}

// 取 [begin_marker, end_marker) 之间的文本。找不到返回 false（out 置空）。
bool ExtractBlock(const std::string& src, std::string* out) {
    out->clear();
    const size_t b = src.find(kBeginMarker);
    if (b == std::string::npos) {
        return false;
    }
    const size_t e = src.find(kEndMarker, b);
    if (e == std::string::npos) {
        return false;
    }
    *out = src.substr(b + std::string(kBeginMarker).size(),
                      e - b - std::string(kBeginMarker).size());
    return true;
}

}  // namespace

int main() {
    // ---- 1) 两份副本都必须带标记（缺标记 = 契约失效，必须红，不能静默跳过）----
    std::string canonical_block;
    std::string inline_block;
    CHECK(ExtractBlock(jpov::kWboWeightGlsl, &canonical_block))
        << "wbo_shader.h 的 kWboWeightGlsl 缺少 " << kBeginMarker << " / " << kEndMarker;
    CHECK(ExtractBlock(jpov::Object3DRenderer::kMeshFs3dPBR, &inline_block))
        << "object3d_renderer.h 的 PBR shader 缺少 " << kBeginMarker << " / " << kEndMarker;

    const std::string canonical = Normalize(canonical_block);
    const std::string inline_copy = Normalize(inline_block);

    // 标记放错位置（比如只括住了注释）时，块会退化成空串 —— 空的相等没有意义。
    CHECK_GT(canonical.size(), 100u) << "权威副本的契约块过短，标记可能放错了";
    CHECK_GT(inline_copy.size(), 100u) << "内联副本的契约块过短，标记可能放错了";
    CHECK(canonical.find("return") != std::string::npos) << "契约块里没有 return，标记位置可疑";

    // ---- 2) 比较器自检：改一个常量必须判不等 ----
    {
        std::string perturbed = canonical;
        const size_t pos = perturbed.find("0.03");
        CHECK(pos != std::string::npos) << "自检失败：契约文本里找不到常量 0.03";
        perturbed.replace(pos, 4, "0.031");
        CHECK(perturbed != canonical)
            << "自检失败：Normalize 之后改常量竟然判相等 —— 比较器退化，测试无效";
    }

    // ---- 3) 契约本体 ----
    if (canonical != inline_copy) {
        LOG(ERROR) << "WBO 权重函数两份副本不一致！\n"
                   << "  权威（src/wbo/wbo_shader.h）: " << canonical
                   << "\n  内联（src/object3d/object3d_renderer.h）: " << inline_copy;
        return 1;
    }

    LOG(INFO) << "WBO 权重契约一致（归一化后 " << canonical.size() << " 字符）";
    std::printf("PASS\n");
    return 0;
}
