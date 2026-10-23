// sapling_dump 解析的单测：正常文件、被截断的文件、坏标记。
//
// 这份解析器是「Blender 与 C++ 之间唯一的接口」，它的容错边界值得钉死：
// 文件坏掉时必须给出带行号的 error，而不是崩、也不是悄悄少读几条。

#include "tools/jpov/tools/vegetation_tool_v2/gen/sapling_dump.h"

#include <fstream>
#include <string>

#include "gtest/gtest.h"

namespace jpov {
namespace vegetation {
namespace gen {
namespace {

std::string WriteTemp(const std::string& name, const std::string& content) {
  const std::string path = ::testing::TempDir() + "/" + name;
  std::ofstream stream(path);
  stream << content;
  return path;
}

const char kValidDump[] =
    "jpov_sapling_dump 1\n"
    "bevel_depth 0.5\n"
    "spline bezier 2\n"
    "p 0 0 0  0 0 0  0 0 1  1.0\n"
    "p 0 0 2  0 0 1  0 0 2  0.5\n"
    "spline poly 2\n"
    "p 1 0 1  0 0 0  0 0 0  0.25\n"
    "p 2 0 1  0 0 0  0 0 0  0.25\n"
    "leaves 2\n"
    "v 1 0 1 0 0\n"
    "v 1 0 1 -1 -1\n";

TEST(SaplingDumpTest, ParsesValidDump) {
  SaplingDump dump;
  std::string error;
  ASSERT_TRUE(LoadSaplingDump(WriteTemp("valid.dump", kValidDump), &dump, &error)) << error;

  EXPECT_DOUBLE_EQ(dump.bevel_depth, 0.5);
  ASSERT_EQ(dump.splines.size(), 2u);

  EXPECT_TRUE(dump.splines[0].is_bezier);
  ASSERT_EQ(dump.splines[0].points.size(), 2u);
  EXPECT_DOUBLE_EQ(dump.splines[0].points[0].co.z, 0.0);
  EXPECT_DOUBLE_EQ(dump.splines[0].points[1].co.z, 2.0);
  EXPECT_DOUBLE_EQ(dump.splines[0].points[0].handle_right.z, 1.0);
  EXPECT_DOUBLE_EQ(dump.splines[0].points[1].handle_left.z, 1.0);
  EXPECT_DOUBLE_EQ(dump.splines[0].points[1].radius, 0.5);

  EXPECT_FALSE(dump.splines[1].is_bezier);
  ASSERT_EQ(dump.splines[1].points.size(), 2u);
  EXPECT_DOUBLE_EQ(dump.splines[1].points[1].co.x, 2.0);

  ASSERT_EQ(dump.leaf_vertices.size(), 2u);
  EXPECT_DOUBLE_EQ(dump.leaf_vertices[0].u, 0.0);
  EXPECT_DOUBLE_EQ(dump.leaf_vertices[1].v, -1.0);  // 缺 UV 的顶点
}

// 负向：文件头不对时必须报错（而不是当成空数据继续）。
TEST(SaplingDumpTest, RejectsBadHeader) {
  SaplingDump dump;
  std::string error;
  const std::string path = WriteTemp("bad_header.dump", "hello world\n");
  EXPECT_FALSE(LoadSaplingDump(path, &dump, &error));
  EXPECT_NE(error.find("文件头"), std::string::npos) << error;
}

// 负向：点数声明得比实际多 —— 必须报错，不能少读。
TEST(SaplingDumpTest, RejectsTruncatedSpline) {
  SaplingDump dump;
  std::string error;
  const std::string path = WriteTemp(
      "truncated.dump",
      "jpov_sapling_dump 1\n"
      "bevel_depth 0.5\n"
      "spline poly 3\n"
      "p 0 0 0  0 0 0  0 0 0  1\n"
      "p 0 0 1  0 0 0  0 0 0  1\n");
  EXPECT_FALSE(LoadSaplingDump(path, &dump, &error));
  EXPECT_NE(error.find("不足"), std::string::npos) << error;
}

// 负向：未知记录（例如以后 dump 加了字段但解析器没跟上）必须显式失败。
TEST(SaplingDumpTest, RejectsUnknownRecord) {
  SaplingDump dump;
  std::string error;
  const std::string path = WriteTemp(
      "unknown.dump",
      "jpov_sapling_dump 1\n"
      "bevel_depth 0.5\n"
      "spline poly 1\n"
      "p 0 0 0  0 0 0  0 0 0  1\n"
      "whatever 1 2 3\n");
  EXPECT_FALSE(LoadSaplingDump(path, &dump, &error));
  EXPECT_NE(error.find("未知记录"), std::string::npos) << error;
}

// 负向：一条 spline 都没有 —— 后面建 shape 没有意义，直接拒。
TEST(SaplingDumpTest, RejectsEmptySplines) {
  SaplingDump dump;
  std::string error;
  const std::string path = WriteTemp("empty.dump", "jpov_sapling_dump 1\nbevel_depth 0.5\n");
  EXPECT_FALSE(LoadSaplingDump(path, &dump, &error));
  EXPECT_NE(error.find("没有"), std::string::npos) << error;
}

}  // namespace
}  // namespace gen
}  // namespace vegetation
}  // namespace jpov
