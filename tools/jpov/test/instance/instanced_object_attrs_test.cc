// instanced_object_attrs_test — 守静态实例渲染器 per-instance 布局 spec 的自洽性
//
// 守的是什么（2026-10-10 抽出独立 spec 系统时新增）：
//   静态实例路径的 per-instance 属性布局（src/instanced/instanced_object_attrs.h）必须自洽 ——
//   每个 spec 合法、两两不重叠、且不侵占顶点属性槽。布局与蒙皮布局解耦后，这些不变量
//   由本文件的 spec 单独负责，不再有「蒙皮布局顺带兜底」。
//
// 为什么有「负向参照」段：
//   上面的正向断言（合法 / 不重叠 / 不侵占）只有在**谓词真能区分对错**时才有意义；
//   若谓词恒真，正向断言就是摆设。故末尾用一批**明知非法**的输入反证谓词会拒绝它们
//   （并附一条「相邻但不重叠＝合法」的反向对照，防止谓词过严）。这与本仓库「不跑代码
//   也要看出恒真断言」的纪律一致。

#include <glog/logging.h>

#include "tools/jpov/src/instanced/instanced_object_attrs.h"

namespace {

using jpov::InstanceAttrSpec;

namespace io_attrs = jpov::instanced_object;

constexpr int kActiveCount =
    static_cast<int>(sizeof(io_attrs::kActiveAttrSpecs) / sizeof(io_attrs::kActiveAttrSpecs[0]));

}  // namespace

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);

    // ---- 1) 每个启用的 spec 合法 ----
    CHECK_GT(kActiveCount, 0) << "kActiveAttrSpecs 不能为空（否则本测试恒真）";
    for (int i = 0; i < kActiveCount; ++i) {
        CHECK(io_attrs::SpecIsValid(io_attrs::kActiveAttrSpecs[i]))
            << "kActiveAttrSpecs[" << i << "] 非法（越 16 或 stride 不符）";
    }

    // ---- 2) 两两槽区间不重叠 ----
    for (int i = 0; i < kActiveCount; ++i) {
        for (int j = i + 1; j < kActiveCount; ++j) {
            CHECK(io_attrs::SpecsDisjoint(io_attrs::kActiveAttrSpecs[i],
                                          io_attrs::kActiveAttrSpecs[j]))
                << "kActiveAttrSpecs[" << i << "] 与 [" << j << "] 槽区间重叠";
        }
    }

    // ---- 3) 不侵占顶点属性槽 ----
    for (int i = 0; i < kActiveCount; ++i) {
        CHECK(io_attrs::SpecAvoidsVertexAttrs(io_attrs::kActiveAttrSpecs[i]))
            << "kActiveAttrSpecs[" << i << "] 侵占了顶点属性槽";
    }

    // ---- 4) 锚定：模型矩阵确实在 loc7..10、stride 16（防“值被悄悄改掉”）----
    CHECK_EQ(io_attrs::kModelAttrSpec.base_loc, 7u);
    CHECK_EQ(io_attrs::kModelAttrSpec.slot_count, 4);
    CHECK_EQ(io_attrs::kModelAttrSpec.slot_components, 4);
    CHECK_EQ(io_attrs::kModelAttrSpec.stride_floats, 16);
    LOG(INFO) << "OK: 静态实例布局自洽（spec 合法 / 不重叠 / 不侵占顶点槽）";

    // ---- 5) 负向参照：谓词必须拒绝非法输入，否则上面的正向断言全是恒真 ----
    //   (a) 越界：loc15 + 4 slots = 19 > 16。
    CHECK(!io_attrs::SpecIsValid(InstanceAttrSpec{/*base_loc*/ 15, 4, 4, 16}))
        << "应以越界拒绝 loc15+4";
    //   (b) stride 与 (slot_count × slot_components) 不符。
    CHECK(!io_attrs::SpecIsValid(InstanceAttrSpec{/*base_loc*/ 7, 4, 4, 15}))
        << "应以 stride 不符拒绝 (4×4 != 15)";
    //   (c) 侵占顶点属性槽 loc0。
    CHECK(!io_attrs::SpecAvoidsVertexAttrs(InstanceAttrSpec{/*base_loc*/ 0, 1, 4, 4}))
        << "应检出侵占顶点槽 loc0";
    //   (d) 重叠：{7,4} 与 {9,1} 交叠于 loc9..10。
    CHECK(!io_attrs::SpecsDisjoint(InstanceAttrSpec{/*base_loc*/ 7, 4, 4, 16},
                                   InstanceAttrSpec{/*base_loc*/ 9, 1, 4, 4}))
        << "应检出 {7,4} 与 {9,1} 重叠";
    //   (e) 反向对照：{7,4} 与 {11,1} 相邻但不重叠 —— 必须判为「不重叠」，防止谓词过严。
    CHECK(io_attrs::SpecsDisjoint(InstanceAttrSpec{/*base_loc*/ 7, 4, 4, 16},
                                  InstanceAttrSpec{/*base_loc*/ 11, 1, 4, 4}))
        << "{7,4} 与 {11,1} 相邻不重叠，不应误判为重叠";
    LOG(INFO) << "OK: 负向参照通过（谓词确实能区分对错，非恒真）";

    LOG(INFO) << "instanced_object_attrs_test PASSED";
    return 0;
}
