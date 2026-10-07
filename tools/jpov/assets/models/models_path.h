#ifndef JPOV_ASSETS_MODELS_MODELS_PATH_H_
#define JPOV_ASSETS_MODELS_MODELS_PATH_H_

// JPOV 3D 模型资产路径常量。
//
// 所有 3D 模型资产集中在 tools/jpov/assets/models/ 下统一管理。本头文件是
// **仓库相对路径**（以工程根为基准）的单一来源：运行期工作目录为工程根的
// 工具（demo viewer / editor / clothing tool）直接用这些常量，避免同一路径
// 在多处 .cc 里各写一份而 drift。
//
// 测试请改用 jpov::GetModelsDir()（tools/jpov/test/test_utils.h）——它额外兼容
// bazel sandbox 的 $TEST_SRCDIR/__main__/tools/jpov/assets/models。
namespace jpov {

// 模型资产根目录（仓库相对，无尾斜杠）。
inline constexpr const char* kModelsDirRel = "tools/jpov/assets/models";

// 开箱即用的样例：未显式指定路径时的 fallback（demo 查看器 / 编辑器）。
inline constexpr const char* kDemoPliersGltf =
    "tools/jpov/assets/models/samples/pliers/pliers.gltf";

// 默认人体 reference（Mixamo 男性，rest/T-pose，带骨架/蒙皮）。
inline constexpr const char* kDefaultCharacterGlb =
    "tools/jpov/assets/models/characters/mixamo_male.glb";

}  // namespace jpov

#endif  // JPOV_ASSETS_MODELS_MODELS_PATH_H_
