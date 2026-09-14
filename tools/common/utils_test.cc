// tools/common/utils_test — 通用工具函数单测（纯 CPU，无依赖）
//
// 覆盖 EditTimestampSuffix：格式 / 已知时间点 / 时区无关的字段解析。

#include <ctime>
#include <string>

#include <gtest/gtest.h>
#include "tools/common/utils.h"

namespace {

// 解析 "YYYYMMDD-HHMMSS" 的各字段（不做时区假设，只检查格式与字段自洽）。
struct Stamp {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
};

bool ParseStamp(const std::string& s, Stamp* out) {
    if (s.size() != 15 || s[8] != '-') {
        return false;
    }
    auto two = [&s](size_t i) { return (s[i] - '0') * 10 + (s[i + 1] - '0'); };
    auto four = [&s](size_t i) {
        return (s[i] - '0') * 1000 + (s[i + 1] - '0') * 100 +
               (s[i + 2] - '0') * 10 + (s[i + 3] - '0');
    };
    out->year = four(0);
    out->month = two(4);
    out->day = two(6);
    out->hour = two(9);
    out->minute = two(11);
    out->second = two(13);
    return true;
}

TEST(UtilsTest, TimestampFormatIsFixedWidth) {
    const std::string s = jpov::EditTimestampSuffix(0);
    EXPECT_EQ(s.size(), 15u);
    EXPECT_EQ(s[8], '-');
    Stamp st{};
    ASSERT_TRUE(ParseStamp(s, &st));
    EXPECT_GE(st.month, 1);
    EXPECT_LE(st.month, 12);
    EXPECT_GE(st.day, 1);
    EXPECT_LE(st.day, 31);
    EXPECT_GE(st.hour, 0);
    EXPECT_LE(st.hour, 23);
    EXPECT_GE(st.minute, 0);
    EXPECT_LE(st.minute, 59);
    EXPECT_GE(st.second, 0);
    EXPECT_LE(st.second, 59);
}

TEST(UtilsTest, TimestampMatchesLocalTimeOfInput) {
    // 构造一个确定的 time_t，检查字段与 localtime 一致（不假设具体时区值）。
    std::tm want{};
    want.tm_year = 2026 - 1900;
    want.tm_mon = 8;    // 9 月（0-based）
    want.tm_mday = 13;
    want.tm_hour = 16;
    want.tm_min = 5;
    want.tm_sec = 30;
    want.tm_isdst = -1;
    const std::time_t t = std::mktime(&want);
    ASSERT_NE(t, static_cast<std::time_t>(-1));

    const std::string s = jpov::EditTimestampSuffix(t);
    Stamp st{};
    ASSERT_TRUE(ParseStamp(s, &st));
    EXPECT_EQ(st.year, 2026);
    EXPECT_EQ(st.month, 9);
    EXPECT_EQ(st.day, 13);
    EXPECT_EQ(st.hour, 16);
    EXPECT_EQ(st.minute, 5);
    EXPECT_EQ(st.second, 30);
}

// 两个不同时刻应产出不同后缀（防“函数忽略了入参、总返回当前时间”的实现）。
TEST(UtilsTest, DifferentTimesGiveDifferentSuffix) {
    std::tm a{};
    a.tm_year = 2020 - 1900;
    a.tm_mon = 0;
    a.tm_mday = 1;
    a.tm_isdst = -1;
    std::tm b = a;
    b.tm_year = 2021 - 1900;
    const std::time_t ta = std::mktime(&a);
    const std::time_t tb = std::mktime(&b);
    EXPECT_NE(jpov::EditTimestampSuffix(ta), jpov::EditTimestampSuffix(tb));
}

}  // namespace
