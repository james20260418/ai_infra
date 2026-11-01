// RenderPreference 单测：判定表 + 环境变量提示的真实副作用。
//
// 判定表覆盖 3 偏好 × 3 GPU 类别 = 9 格，全部逐格断言（不是抽两条）——
// 这张表就是 Init 里告警行为的**唯一**依据，任何一格悄悄错位都会让
// "prefer-gpu 却没告警" 这种静默失败再次发生。

#include "tools/jpov/interface/render_preference.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

namespace {

constexpr const char* kSoftwareRasterEnvVar = "LIBGL_ALWAYS_SOFTWARE";

// 读环境变量；不存在返回空串（测试里足够区分 "1" / "0" / 未设置）。
std::string ReadEnv() {
    const char* value = std::getenv(kSoftwareRasterEnvVar);
    return value == nullptr ? std::string() : std::string(value);
}

void WriteEnv(const std::string& value) {
#if defined(_WIN32)
    _putenv_s(kSoftwareRasterEnvVar, value.c_str());
#else
    setenv(kSoftwareRasterEnvVar, value.c_str(), /*overwrite=*/1);
#endif
}

void ClearEnv() {
#if defined(_WIN32)
    _putenv_s(kSoftwareRasterEnvVar, "");
#else
    unsetenv(kSoftwareRasterEnvVar);
#endif
}

}  // namespace

// 每个用例前后都回到「未设置」，避免用例间互相污染（环境变量是全进程的）。
class RenderPreferenceEnvTest : public ::testing::Test {
protected:
    void SetUp() override { ClearEnv(); }
    void TearDown() override { ClearEnv(); }
};

TEST(RenderPreferenceName, MapsEveryValueToAsciiName) {
    EXPECT_STREQ(jpov::RenderPreferenceName(jpov::RenderPreference::kAny), "any");
    EXPECT_STREQ(jpov::RenderPreferenceName(jpov::RenderPreference::kPreferGpu),
                 "prefer-gpu");
    EXPECT_STREQ(jpov::RenderPreferenceName(jpov::RenderPreference::kPreferCpu),
                 "prefer-cpu");
    // 非法枚举值不能崩（日志路径不该成为崩溃点），但要可辨认。
    EXPECT_STREQ(
        jpov::RenderPreferenceName(static_cast<jpov::RenderPreference>(99)),
        "invalid");
}

TEST(RenderPreferenceJudge, AnyAlwaysSatisfied) {
    EXPECT_EQ(jpov::JudgeRenderPreference(jpov::RenderPreference::kAny,
                                          jpov::GpuKind::kUnknown),
              jpov::RenderPreferenceVerdict::kSatisfied);
    EXPECT_EQ(jpov::JudgeRenderPreference(jpov::RenderPreference::kAny,
                                          jpov::GpuKind::kHardware),
              jpov::RenderPreferenceVerdict::kSatisfied);
    EXPECT_EQ(jpov::JudgeRenderPreference(jpov::RenderPreference::kAny,
                                          jpov::GpuKind::kCpuRaster),
              jpov::RenderPreferenceVerdict::kSatisfied);
}

TEST(RenderPreferenceJudge, PreferGpuViolatedOnlyOnKnownCpuRaster) {
    EXPECT_EQ(jpov::JudgeRenderPreference(jpov::RenderPreference::kPreferGpu,
                                          jpov::GpuKind::kCpuRaster),
              jpov::RenderPreferenceVerdict::kViolated);
    EXPECT_EQ(jpov::JudgeRenderPreference(jpov::RenderPreference::kPreferGpu,
                                          jpov::GpuKind::kHardware),
              jpov::RenderPreferenceVerdict::kSatisfied);
    // 未知 ≠ 违反：冤枉告警比不告警更糟。
    EXPECT_EQ(jpov::JudgeRenderPreference(jpov::RenderPreference::kPreferGpu,
                                          jpov::GpuKind::kUnknown),
              jpov::RenderPreferenceVerdict::kUndetermined);
}

TEST(RenderPreferenceJudge, PreferCpuViolatedOnHardware) {
    EXPECT_EQ(jpov::JudgeRenderPreference(jpov::RenderPreference::kPreferCpu,
                                          jpov::GpuKind::kCpuRaster),
              jpov::RenderPreferenceVerdict::kSatisfied);
    // kHardware = "不是已知软件光栅器" ⇒ 对「想要软件光栅」就是违反。
    EXPECT_EQ(jpov::JudgeRenderPreference(jpov::RenderPreference::kPreferCpu,
                                          jpov::GpuKind::kHardware),
              jpov::RenderPreferenceVerdict::kViolated);
    EXPECT_EQ(jpov::JudgeRenderPreference(jpov::RenderPreference::kPreferCpu,
                                          jpov::GpuKind::kUnknown),
              jpov::RenderPreferenceVerdict::kUndetermined);
}

TEST_F(RenderPreferenceEnvTest, NonCpuPreferenceLeavesEnvUntouched) {
    EXPECT_FALSE(
        jpov::ApplyRenderPreferenceEnvHint(jpov::RenderPreference::kAny));
    EXPECT_TRUE(ReadEnv().empty());

    EXPECT_FALSE(
        jpov::ApplyRenderPreferenceEnvHint(jpov::RenderPreference::kPreferGpu));
    EXPECT_TRUE(ReadEnv().empty());
}

TEST_F(RenderPreferenceEnvTest, PreferCpuSetsMissingEnvVar) {
    EXPECT_TRUE(
        jpov::ApplyRenderPreferenceEnvHint(jpov::RenderPreference::kPreferCpu));
    EXPECT_EQ(ReadEnv(), "1");

    // 幂等：第二次调用不再"写入"（值已经在了），返回值必须是 false。
    EXPECT_FALSE(
        jpov::ApplyRenderPreferenceEnvHint(jpov::RenderPreference::kPreferCpu));
    EXPECT_EQ(ReadEnv(), "1");
}

TEST_F(RenderPreferenceEnvTest, PreferCpuDoesNotOverrideExplicitEnvVar) {
    // 显式 0 = 用户明确要硬件 ⇒ 引擎的偏好不覆盖它。
    WriteEnv("0");
    EXPECT_FALSE(
        jpov::ApplyRenderPreferenceEnvHint(jpov::RenderPreference::kPreferCpu));
    EXPECT_EQ(ReadEnv(), "0");
}
