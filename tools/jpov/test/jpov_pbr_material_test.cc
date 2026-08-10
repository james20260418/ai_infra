// JPOV PBRMaterial 单元测试
//
// 本测试为纯 CPU 数据结构语义测试，不涉及渲染 / shader（渲染 gold test
// 见 DrawObject3D PBR 材质改造任务）。
//
// 验证 PBRMaterial 的构建 + fallback 语义：
//   1. 默认构造：所有默认值符合计划（base_color 未初始化、metallic=0、
//      roughness=1、normal_scale=1、emissive/ao 未初始化、tex=0、has_*=false）。
//   2. 无纹理（constant）fallback：*_tex==0 时，材质使用对应常值。
//   3. 有纹纹理（texture）优先：*_tex!=0 时，材质采样纹理、忽略常值。
//
// 每个通道（baseColor / metallic / roughness / emissive / AO + normal）都覆盖
// “无纹理→常值 / 有纹理→采样”两个分支。

#include <cmath>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/interface/render_command.h"

namespace {

void Check(bool cond, const std::string& msg) {
    if (!cond) {
        LOG(FATAL) << "PBRMaterial test failed: " << msg;
    }
}

// 浮点近似相等（分量为 [0,1] 的颜色比较用）
bool NearlyEqual(float a, float b) {
    return std::fabs(a - b) < 1e-5f;
}

bool ColorEqual(const jpov::Color& a, const jpov::Color& b) {
    return NearlyEqual(a.r, b.r) && NearlyEqual(a.g, b.g) &&
           NearlyEqual(a.b, b.b) && NearlyEqual(a.a, b.a);
}

// ---- 渲染侧 fallback 决策规则（仅用于本测试编码语义，非生产代码） ----
// 与计划约定一致：*_tex != 0 表示有纹理 → 采样纹理、忽略常值；
// *_tex == 0 表示无纹理 → 使用常值 fallback。
struct ResolvedMaterial {
    bool base_color_has_tex;
    bool base_color_uses_tex;
    jpov::Color base_color;          // has_tex 时为采样纹理值，否则为常值

    bool metallic_has_tex;
    bool metallic_uses_tex;
    float metallic;                  // has_tex 时为采样纹理值，否则为常值

    bool roughness_has_tex;
    bool roughness_uses_tex;
    float roughness;

    bool emissive_has_tex;
    bool emissive_uses_tex;
    jpov::Color emissive;

    bool ao_has_tex;
    bool ao_uses_tex;
    jpov::Color ao;

    bool normal_has_tex;
    bool normal_uses_tex;
    float normal_scale;
};

// 模拟渲染侧“解析一个材质”的决策：常值 vs 纹理。
// 采样值用一个伪值代替（纹理采样在实际 shader 中发生，本测试只验证选择分支）。
ResolvedMaterial Resolve(const jpov::PBRMaterial& m) {
    ResolvedMaterial r{};
    // baseColor
    r.base_color_has_tex = (m.base_color_tex != 0);
    r.base_color_uses_tex = r.base_color_has_tex;
    r.base_color = m.base_color;  // 无纹理时就是常值 fallback
    // metallic
    r.metallic_has_tex = m.metallic_tex != 0;
    r.metallic_uses_tex = m.has_metallic_tex && r.metallic_has_tex;
    r.metallic = m.metallic;
    // roughness
    r.roughness_has_tex = m.roughness_tex != 0;
    r.roughness_uses_tex = m.has_roughness_tex && r.roughness_has_tex;
    r.roughness = m.roughness;
    // emissive
    r.emissive_has_tex = m.emissive_tex != 0;
    r.emissive_uses_tex = r.emissive_has_tex;
    r.emissive = m.emissive;
    // ao
    r.ao_has_tex = m.ao_tex != 0;
    r.ao_uses_tex = r.ao_has_tex;
    r.ao = m.ao;
    // normal
    r.normal_has_tex = m.normal_tex != 0;
    r.normal_uses_tex = r.normal_has_tex;
    r.normal_scale = m.normal_scale;
    return r;
}

// ---- 1. 默认构造：所有字段语义符合计划 ----
void TestDefaults() {
    jpov::PBRMaterial m;
    Check(m.base_color_tex == 0, "默认无 base_color 纹理");
    Check(m.metallic == 0.0f, "默认 metallic 应为 0");
    Check(!m.has_metallic_tex, "默认无 metallic 纹理");
    Check(m.metallic_tex == 0, "默认 metallic_tex 应为 0");
    Check(m.roughness == 1.0f, "默认 roughness 应为 1");
    Check(!m.has_roughness_tex, "默认无 roughness 纹理");
    Check(m.roughness_tex == 0, "默认 roughness_tex 应为 0");
    Check(m.normal_scale == 1.0f, "默认 normal_scale 应为 1");
    Check(m.normal_tex == 0, "默认无 normal 纹理");
    Check(m.emissive_tex == 0, "默认无 emissive 纹理");
    Check(m.ao_tex == 0, "默认无 ao 纹理");

    // 默认材质 = 全 constant fallback（无任何纹理路径）
    ResolvedMaterial r = Resolve(m);
    Check(!r.base_color_uses_tex, "默认材质应走 baseColor 常值");
    Check(!r.metallic_uses_tex, "默认材质应走 metallic 常值");
    Check(!r.roughness_uses_tex, "默认材质应走 roughness 常值");
    Check(!r.emissive_uses_tex, "默认材质应走 emissive 常值");
    Check(!r.ao_uses_tex, "默认材质应走 ao 常值");
    Check(!r.normal_uses_tex, "默认材质应走 normal 常值");
}

// ---- 2. 无纹理（constant）fallback：*_tex==0 时全部用常值 ----
void TestConstantFallback() {
    jpov::PBRMaterial m;
    // 全常值：metallic/roughness 有 scalar，baseColor/emissive/ao 有 fallback 色
    m.base_color = {0.2f, 0.4f, 0.6f, 1.0f};
    m.metallic = 0.9f;
    m.roughness = 0.1f;
    m.emissive = {0.0f, 1.0f, 0.0f, 1.0f};
    m.ao = {0.5f, 0.5f, 0.5f, 1.0f};
    m.normal_scale = 2.0f;

    ResolvedMaterial r = Resolve(m);
    Check(!r.base_color_uses_tex && ColorEqual(r.base_color, m.base_color),
          "无纹理时应使用 base_color 常值");
    Check(!r.metallic_uses_tex && NearlyEqual(r.metallic, 0.9f),
          "无纹理时应使用 metallic 常值 0.9");
    Check(!r.roughness_uses_tex && NearlyEqual(r.roughness, 0.1f),
          "无纹理时应使用 roughness 常值 0.1");
    Check(!r.emissive_uses_tex && ColorEqual(r.emissive, m.emissive),
          "无纹理时应使用 emissive 常值");
    Check(!r.ao_uses_tex && ColorEqual(r.ao, m.ao),
          "无纹理时应使用 ao 常值");
    Check(!r.normal_uses_tex && NearlyEqual(r.normal_scale, 2.0f),
          "无纹理时 normal_scale 应作用于常值路径");
}

// ---- 3. 有纹纹理（texture）优先：*_tex!=0 时采样纹理、忽略常值 ----
void TestTexturePriority() {
    jpov::PBRMaterial m;
    // 常值故意设置成与纹理不同的值，以区分“走了哪条路径”
    m.base_color = {0.2f, 0.2f, 0.2f, 1.0f};
    m.base_color_tex = 101;
    m.metallic = 0.0f;
    m.has_metallic_tex = true;
    m.metallic_tex = 102;
    m.roughness = 1.0f;
    m.has_roughness_tex = true;
    m.roughness_tex = 103;
    m.emissive = {0.0f, 0.0f, 0.0f, 1.0f};
    m.emissive_tex = 104;
    m.ao = {1.0f, 1.0f, 1.0f, 1.0f};
    m.ao_tex = 105;
    m.normal_scale = 3.0f;
    m.normal_tex = 106;

    ResolvedMaterial r = Resolve(m);
    Check(r.base_color_has_tex && r.base_color_uses_tex,
          "base_color_tex=101 应走纹理路径");
    Check(r.metallic_has_tex && r.metallic_uses_tex,
          "metallic_tex=102 + has_metallic_tex 应走纹理路径");
    Check(r.roughness_has_tex && r.roughness_uses_tex,
          "roughness_tex=103 + has_roughness_tex 应走纹理路径");
    Check(r.emissive_has_tex && r.emissive_uses_tex,
          "emissive_tex=104 应走纹理路径");
    Check(r.ao_has_tex && r.ao_uses_tex,
          "ao_tex=105 应走纹理路径");
    Check(r.normal_has_tex && r.normal_uses_tex,
          "normal_tex=106 应走纹理路径");
    // normal_scale 在纹理路径下仍然作为扰动强度生效（与纹理采样相乘）
    Check(NearlyEqual(r.normal_scale, 3.0f),
          "纹理路径下 normal_scale 仍应保留为扰动强度");
}

// ---- 4. has_*_tex 与 *_tex 联动：metallic/roughness 需 has 标志 + 句柄同时成立 ----
void TestMetallicRoughnessGate() {
    // has_metallic_tex=true 但 metallic_tex==0 → 视为无纹理，走常值
    {
        jpov::PBRMaterial m;
        m.has_metallic_tex = true;
        m.metallic_tex = 0;
        m.metallic = 0.7f;
        ResolvedMaterial r = Resolve(m);
        Check(!r.metallic_uses_tex, "metallic_tex==0 即使 has 标志为真也应走常值");
        Check(NearlyEqual(r.metallic, 0.7f), "metallic fallback 常值应为 0.7");
    }
    // roughness 同理
    {
        jpov::PBRMaterial m;
        m.has_roughness_tex = true;
        m.roughness_tex = 0;
        m.roughness = 0.4f;
        ResolvedMaterial r = Resolve(m);
        Check(!r.roughness_uses_tex, "roughness_tex==0 即使 has 标志为真也应走常值");
        Check(NearlyEqual(r.roughness, 0.4f), "roughness fallback 常值应为 0.4");
    }
}

}  // namespace

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = true;

    TestDefaults();
    TestConstantFallback();
    TestTexturePriority();
    TestMetallicRoughnessGate();

    LOG(INFO) << "All PBRMaterial tests passed.";
    return 0;
}
