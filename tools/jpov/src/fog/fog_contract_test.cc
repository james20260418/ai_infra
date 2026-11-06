// Fog 团属性纹理（uFogBodyTex）布局契约单测。
//
// 目的：CPU 打包（fog_renderer.cc）与 GLSL 解码（fog_shader.h）共用同一套
// uFogBodyTex 布局，但分处 C++ 与 GLSL 字符串、无编译器强制 ⇒ 用本单测把两侧钉在一起：
//   - 解析 GLSL 里的 `//@FOGLAYOUT texel=T <comp>=<name> ...` 标记 → 字段表；
//   - 与 fog_body.h 的 kFogBodyLayout 逐项（texel/comp/name）比对；
//   - 校验 `#define FOG_TEXELS_PER_BODY` / `FOG_KIND_ROUNDBOX` 与 C++ 常量一致。
// 改布局时两处一起改；漏改则本测红（防止静默漂移）。

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "tools/jpov/src/fog/fog_shader.h"
#include "tools/jpov/src/fog/fog_body.h"

namespace jpov {
namespace {

struct ParsedField {
    int texel;
    int comp;
    std::string name;
};

// 去掉行首尾空白（含 \r\n）。
std::string Trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        return "";
    }
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// 解析 token "x=kind" → (comp, name)；comp: x→0 y→1 z→2 w→3。非法 → false。
bool ParseCompTok(const std::string& tok, int* comp /*output*/, std::string* name /*output*/) {
    const size_t eq = tok.find('=');
    if (eq != 1) {
        return false;
    }
    const char c = tok[0];
    if (c == 'x') {
        *comp = 0;
    } else if (c == 'y') {
        *comp = 1;
    } else if (c == 'z') {
        *comp = 2;
    } else if (c == 'w') {
        *comp = 3;
    } else {
        return false;
    }
    *name = tok.substr(2);
    return !name->empty();
}

// 解析 GLSL 源码里所有 //@FOGLAYOUT 行 → 字段表。
std::vector<ParsedField> ParseFogLayout(const std::string& glsl) {
    std::vector<ParsedField> out;
    std::istringstream iss(glsl);
    std::string line;
    const std::string kMark = "//@FOGLAYOUT";
    while (std::getline(iss, line)) {
        const std::string t = Trim(line);
        if (t.rfind(kMark, 0) != 0) {
            continue;
        }
        std::istringstream ls(t.substr(kMark.size()));
        std::string tok;
        int texel = -1;
        while (ls >> tok) {
            if (tok.rfind("texel=", 0) == 0) {
                texel = std::stoi(tok.substr(6));
                continue;
            }
            int comp = -1;
            std::string name;
            if (!ParseCompTok(tok, &comp, &name)) {
                continue;
            }
            out.push_back(ParsedField{texel, comp, name});
        }
    }
    return out;
}

// 解析 `#define <macro> <int>` 的值；找不到 → -1。
int ParseDefineInt(const std::string& src, const std::string& macro) {
    const std::string key = "#define " + macro + " ";
    const size_t p = src.find(key);
    if (p == std::string::npos) {
        return -1;
    }
    size_t q = p + key.size();
    while (q < src.size() && (src[q] == ' ' || src[q] == '\t')) {
        ++q;
    }
    size_t r = q;
    while (r < src.size() && src[r] >= '0' && src[r] <= '9') {
        ++r;
    }
    if (r == q) {
        return -1;
    }
    return std::stoi(src.substr(q, r - q));
}

void SortFields(std::vector<ParsedField>* v) {
    std::sort(v->begin(), v->end(), [](const ParsedField& a, const ParsedField& b) {
        if (a.texel != b.texel) {
            return a.texel < b.texel;
        }
        return a.comp < b.comp;
    });
}

}  // namespace

// GLSL 的 #define 与 C++ 常量一致。
TEST(FogContract, DefineConstantsMatchCpp) {
    EXPECT_EQ(ParseDefineInt(kFogCommonGlsl, "FOG_TEXELS_PER_BODY"),
              fog_layout::kTexelsPerBody);
    EXPECT_EQ(ParseDefineInt(kFogCommonGlsl, "FOG_KIND_ROUNDBOX"),
              static_cast<int>(FogFieldKind::kRoundBox));
}

// GLSL 的 //@FOGLAYOUT 字段表与 fog_body.h 的 kFogBodyLayout 逐项一致。
TEST(FogContract, LayoutMarkersMatchCppTable) {
    std::vector<ParsedField> parsed = ParseFogLayout(kFogCommonGlsl);
    SortFields(&parsed);

    const size_t expected_n = sizeof(kFogBodyLayout) / sizeof(kFogBodyLayout[0]);
    ASSERT_EQ(parsed.size(), expected_n) << "GLSL //@FOGLAYOUT 条目数与 kFogBodyLayout 不一致";

    std::vector<ParsedField> expected;
    expected.reserve(expected_n);
    for (size_t i = 0; i < expected_n; ++i) {
        expected.push_back(ParsedField{kFogBodyLayout[i].texel, kFogBodyLayout[i].comp,
                                       kFogBodyLayout[i].name});
    }
    SortFields(&expected);

    for (size_t i = 0; i < expected_n; ++i) {
        EXPECT_EQ(parsed[i].texel, expected[i].texel) << "第 " << i << " 项 texel 不一致";
        EXPECT_EQ(parsed[i].comp, expected[i].comp) << "第 " << i << " 项 comp 不一致";
        EXPECT_EQ(parsed[i].name, expected[i].name) << "第 " << i << " 项字段名不一致";
    }
}

// 字段表不越界：最大 texel < kTexelsPerBody；公共头 texel 数自洽。
TEST(FogContract, LayoutWithinBudget) {
    for (size_t i = 0; i < sizeof(kFogBodyLayout) / sizeof(kFogBodyLayout[0]); ++i) {
        EXPECT_GE(kFogBodyLayout[i].texel, 0);
        EXPECT_LT(kFogBodyLayout[i].texel, fog_layout::kTexelsPerBody);
        EXPECT_GE(kFogBodyLayout[i].comp, 0);
        EXPECT_LE(kFogBodyLayout[i].comp, 3);
    }
    EXPECT_LE(fog_layout::kCommonTexels, fog_layout::kTexelsPerBody);
}

}  // namespace jpov
