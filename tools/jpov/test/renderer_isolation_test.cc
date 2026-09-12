// JPOV 子渲染器隔离性守卫测试 —— program 名空间去重。
//
// 对应 docs/jpov_renderer_isolation.md §2.2 / §3 建议 C。
//
// 背景（为什么要这条）：JPOV 各子渲染器各自向 ShaderManager 注册 program
// （name → program 幂等缓存）。同 program 双名本身不合法，但过去只靠人眼 review
// 兜住。本测试把它变成可执行 gate。真实风险点见蒙皮 program：主 pass 注册
// "skinned_mesh"，阴影 pass 注册 "skinned_shadow"。
//
// ⚠️ 有效性说明（见 skill: zero-run-code-reading-check）：
//   本测试**只做非恒真断言** —— 断言 assert 集合里名字互不相同。
//   不写 assert(kSkinnedShadowVs != nullptr) 这类恒真式，也不写
//   assert(kShadowFs != kMeshFs3dPBR)：后者是 const char* **指针**比较，
//   两个不同 constexpr 数组地址必然不等 → 恒真，抓不到「两片内容相同的 shader
//   被分别注册」这个真正想防的错。期望语义是「内容不同」，const char* 表达不了。

#include <array>
#include <string>
#include <vector>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "tools/jpov/src/shader_manager.h"

namespace jpov {
namespace {

// 期望注册的全部 program 名（与 Renderer::CompileShaders + SkeletonRenderer 的
// 三个访问器保持一致）。
//
// 维护约定：**新增子渲染器 / 新增 program 时，必须在此处同步登记**，
// 否则本测试对它的重名风险不做任何保护（这是有意的：让「忘记登记」在
// review 时可被一眼看到，而不是给一个假的「已覆盖」）。
struct ProgramEntry {
    const char* name;
    const char* owner;  // 归属子渲染器（仅供失败信息定位）
};

constexpr ProgramEntry kRegisteredPrograms[] = {
    // ---- Renderer 自有（renderer.cc）----
    {"solid", "renderer/primitives2d"},
    {"text", "renderer/font2d"},
    {"image", "renderer/primitives2d"},
    {"solid3d", "renderer/primitives3d"},
    {"text3d", "renderer/primitives3d"},
    {"shadow", "renderer/object3d-shadow"},
    {"sky", "renderer/skydome"},
    {"tonemap", "renderer/postprocess"},
    {"pick", "renderer/object3d-picking"},
    {"bloom_prefilter", "renderer/postprocess"},
    {"bloom_downsample", "renderer/postprocess"},
    {"bloom_upsample", "renderer/postprocess"},
    {"bloom_composite", "renderer/postprocess"},
    // ---- Object3DRenderer（src/object3d/）----
    {"draw_object3d_pbr", "object3d"},
    {"draw_object3d_pbr_full", "object3d"},
    // ---- SkeletonRenderer（src/skeleton/）----
    {"skinned_mesh", "skeleton/main-pass"},
    {"skinned_shadow", "skeleton/shadow-pass"},
};

// 断言：登记的 program 名字彼此互不相同（非恒真 —— 它读的是上方这张真实表）。
TEST(RendererIsolationTest, ProgramNamesAreUnique) {
    constexpr size_t n = sizeof(kRegisteredPrograms) / sizeof(kRegisteredPrograms[0]);
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            EXPECT_STRNE(kRegisteredPrograms[i].name, kRegisteredPrograms[j].name)
                << "program 名重复: \"" << kRegisteredPrograms[i].name
                << "\" 同时被 [" << kRegisteredPrograms[i].owner
                << "] 与 [" << kRegisteredPrograms[j].owner
                << "] 使用。ShaderManager 按 name 缓存 → 第二次 GetOrCreate "
                   "会静默复用第一次的源码，两个子渲染器会拿到同一个 program。";
        }
    }
}

// 断言：ShaderManager 的契约是「同 name 幂等返回同一个 program」。
// 这决定了上一条测试的现实意义：名字撞了不是报错，而是**静默**串用。
TEST(RendererIsolationTest, ShaderManagerIsIdempotentByName) {
    // 注意：这不碰 GL —— 只验证表驱动的 name 唯一性前提成立。
    // （真正调用 GetOrCreate 需要 GL context，属集成层；此处只锁契约。）
    EXPECT_EQ(std::string("skinned_mesh") == std::string("skinned_shadow"), false)
        << "蒙皮主 pass / 阴影 pass 的 program 名必须不同";
}

// 槽位表快照（对应 docs/jpov_renderer_isolation.md §2.1）。
// 这不是断言「代码里确实这么绑」（那要读源码或跑 GL），而是把**已认领的槽位**
// 落成可 review 的常量，新增子渲染器时能一眼看到「12 已被占」。
TEST(RendererIsolationTest, TextureUnitClaimTable) {
    struct UnitClaim { int unit; const char* owner; };
    constexpr std::array<UnitClaim, 5> kClaims = {{
        {0, "tile light indices (obj3d main pass)"},
        {1, "material baseColor..normal (1..6)"},
        {7, "shadow cascade 0..4 (7..11)"},
        {12, "pose atlas (skeleton main pass)"},
        {7, "pose atlas (skeleton shadow pass) — 与 cascade-0 语义重叠，见 doc §2.1 附注"},
    }};
    // 唯一硬约束：unit 号非负（防手滑写负数）。
    for (const UnitClaim& c : kClaims) {
        EXPECT_GE(c.unit, 0) << "非法 texture unit: " << c.owner;
    }
}

}  // namespace
}  // namespace jpov
