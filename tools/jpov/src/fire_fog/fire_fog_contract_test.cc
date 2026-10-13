// FireFog 团属性纹理（uFogBodyTex）布局契约单测。
//
// 目的：CPU 打包（fire_fog_renderer.cc）与 GLSL 解码（fire_fog_shader.h）共用同一套
// uFogBodyTex 布局，但分处 C++ 与 GLSL 字符串、无编译器强制 ⇒ 用本单测把两侧钉在一起：
//   - 解析 GLSL 里的 `//@FOGLAYOUT <scope> texel=T <comp>=<name> ...` 标记 → 按 scope 分组的字段表；
//   - 与 fog_body.h 的 kFogBodyCommonLayout / kRoundBoxUnionLayout / kFlameUnionLayout 逐项比对；
//   - 校验 `#define FOG_TEXELS_PER_BODY` / `FOG_KIND_*` 与 C++ 常量一致。
// 改布局时两处一起改；漏改则本测红（防止静默漂移）。
//
// scope：`head`（公共头，所有 kind 固定）/ `roundbox` / `flame`（各 kind 的 union 区，占同一段 texel）。

#include <algorithm>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "tools/jpov/src/fire_fog/fire_fog_shader.h"
#include "tools/jpov/src/fire_fog/fog_body.h"

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

// 解析 GLSL 源码里所有 //@FOGLAYOUT 行 → scope → 字段表。
std::map<std::string, std::vector<ParsedField>> ParseFogLayout(const std::string& glsl) {
    std::map<std::string, std::vector<ParsedField>> out;
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
        std::string scope;
        int texel = -1;
        while (ls >> tok) {
            if (tok.rfind("texel=", 0) == 0) {
                texel = std::stoi(tok.substr(6));
                continue;
            }
            int comp = -1;
            std::string name;
            if (!ParseCompTok(tok, &comp, &name)) {
                // 首个无法解析为 comp 的 token 即 scope（head/roundbox/flame）。
                if (scope.empty()) {
                    scope = tok;
                }
                continue;
            }
            out[scope].push_back(ParsedField{texel, comp, name});
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

// GLSL 某 scope 的字段表与 C++ 表逐项（texel/comp/name）一致。
void ExpectScopeMatches(const std::map<std::string, std::vector<ParsedField>>& parsed,
                        const std::string& scope,
                        const FogBodyLayoutEntry* table,
                        size_t table_n) {
    const auto it = parsed.find(scope);
    ASSERT_NE(it, parsed.end()) << "GLSL 里找不到 scope=" << scope << " 的 //@FOGLAYOUT";
    std::vector<ParsedField> got = it->second;
    SortFields(&got);

    std::vector<ParsedField> expected;
    expected.reserve(table_n);
    for (size_t i = 0; i < table_n; ++i) {
        expected.push_back(ParsedField{table[i].texel, table[i].comp, table[i].name});
    }
    SortFields(&expected);

    ASSERT_EQ(got.size(), table_n) << "scope=" << scope << " 字段数不一致";
    for (size_t i = 0; i < table_n; ++i) {
        EXPECT_EQ(got[i].texel, expected[i].texel)
            << "scope=" << scope << " 第 " << i << " 项 texel 不一致";
        EXPECT_EQ(got[i].comp, expected[i].comp)
            << "scope=" << scope << " 第 " << i << " 项 comp 不一致";
        EXPECT_EQ(got[i].name, expected[i].name)
            << "scope=" << scope << " 第 " << i << " 项字段名不一致";
    }
}

// 编译期取数组长度（替代 sizeof/sizeof 宏，符合「禁宏」规范）。
template <size_t N>
constexpr size_t ArrayN(const FogBodyLayoutEntry (&)[N]) {
    return N;
}

}  // namespace

// GLSL 的 #define 与 C++ 常量一致。
TEST(FireFogContract, DefineConstantsMatchCpp) {
    EXPECT_EQ(ParseDefineInt(kFireFogCommonGlsl, "FOG_TEXELS_PER_BODY"),
              fog_layout::kTexelsPerBody);
    EXPECT_EQ(ParseDefineInt(kFireFogCommonGlsl, "FOG_KIND_ROUNDBOX"),
              static_cast<int>(FogFieldKind::kRoundBox));
    EXPECT_EQ(ParseDefineInt(kFireFogCommonGlsl, "FOG_KIND_FLAME"),
              static_cast<int>(FogFieldKind::kFlame));
    EXPECT_EQ(ParseDefineInt(kFireFogCommonGlsl, "FOG_KIND_VOLUMETEX"),
              static_cast<int>(FogFieldKind::kVolumeTex));
}

// GLSL 各 scope 的 //@FOGLAYOUT 字段表与 fog_body.h 对应表逐项一致。
TEST(FireFogContract, LayoutMarkersMatchCppTable) {
    const std::map<std::string, std::vector<ParsedField>> parsed = ParseFogLayout(kFireFogCommonGlsl);
    ExpectScopeMatches(parsed, "head", kFogBodyCommonLayout, ArrayN(kFogBodyCommonLayout));
    ExpectScopeMatches(parsed, "roundbox", kRoundBoxUnionLayout, ArrayN(kRoundBoxUnionLayout));
    ExpectScopeMatches(parsed, "flame", kFlameUnionLayout, ArrayN(kFlameUnionLayout));
    ExpectScopeMatches(parsed, "volumetex", kVolumeTexUnionLayout,
                       ArrayN(kVolumeTexUnionLayout));
}

// 所有字段表不越界：texel ∈ [0, kTexelsPerBody)、comp ∈ [0,3]；公共头 texel 数自洽。
TEST(FireFogContract, LayoutWithinBudget) {
    const FogBodyLayoutEntry* tables[] = {
        kFogBodyCommonLayout, kRoundBoxUnionLayout, kFlameUnionLayout, kVolumeTexUnionLayout};
    const size_t table_n[] = {
        ArrayN(kFogBodyCommonLayout), ArrayN(kRoundBoxUnionLayout), ArrayN(kFlameUnionLayout),
        ArrayN(kVolumeTexUnionLayout)};
    for (size_t t = 0; t < sizeof(tables) / sizeof(tables[0]); ++t) {
        for (size_t i = 0; i < table_n[t]; ++i) {
            EXPECT_GE(tables[t][i].texel, 0);
            EXPECT_LT(tables[t][i].texel, fog_layout::kTexelsPerBody);
            EXPECT_GE(tables[t][i].comp, 0);
            EXPECT_LE(tables[t][i].comp, 3);
        }
    }
    EXPECT_LE(fog_layout::kCommonTexels, fog_layout::kTexelsPerBody);
}

}  // namespace jpov
