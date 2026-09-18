// JPOV gen3d — 生成输入图片的本地格式审查
//
// 【为什么需要】
//   Tripo 只在任务被受理后计费，但格式/尺寸不合规的图会直接被拒或生成垃圾，
//   等于白烧 credit。且上传本身有硬性限制（≤20MB、JPEG/PNG/WebP、建议 ≥256px）。
//   因此**在提交前、在本地**审查，不通过的直接拒绝、不发任何 HTTP 请求。
//
// 【设计原则：只截断"明显浪费"，不做美学判断】
//   本模块校验的是**硬性规则**（文件存在/格式支持/尺寸上限/分辨率下限），
//   不判断"图好不好看""主体是否居中"——那类判断没有可靠阈值，硬卡会误伤。
//   审查的唯一边界是：**"这张图一定不可能被受理，或一定产出垃圾"**。
//
// 【同时产出的信息】
//   审查顺带报告图片的 alpha 通道情况（是否有非平凡 alpha / 是否全透明）。
//   原因：Tripo 对 alpha 的处理方式未在官方文档中说明（可能保留，也可能先
//   合成到背景色），底稿是否能用透明底交付**取决于实测结论**。本模块只负责
//   **如实报告**，不替调用方做取舍——决策点见
//   docs/jpov_gen3d_image_input_design.md §3.6。
//
// 依赖 stb_image（JPOV 已有的图片解码库），不依赖 GL。

#ifndef JPOV_GEN3D_IMAGE_INPUT_H_
#define JPOV_GEN3D_IMAGE_INPUT_H_

#include <string>
#include <vector>

namespace jpov {
namespace gen3d {

// 供应商无关的图片格式规则（Tripo 现行限制；换供应商时调整此处）。
struct ImageLimits {
    // 单文件字节上限（Tripo: 20 MB）。
    long max_file_bytes = 20L * 1024 * 1024;
    // 每边最小像素（Tripo multiview 文档推荐 ≥256）。
    int min_edge_px = 256;
    // 每边最大像素。Tripo 未给出硬上限，但按 image-to-image 的
    // "最大总像素 6000×6000" 取单边 6000 作为宽松上界（超过基本无必要）。
    int max_edge_px = 6000;
};

// 一张图的审查结果。
struct ImageCheckResult {
    // 是否通过（= 所有 applicable 的检查都过）。false 时看 errors。
    bool ok = false;
    // 图片真实解码出的宽 / 高（像素）。解码失败时为 0。
    int width = 0;
    int height = 0;
    // 文件字节数（stat 得到）。
    long file_bytes = 0;
    // 解码出的通道数（stb 语义：1=灰,2=灰+alpha,3=RGB,4=RGBA）。
    int channels = 0;
    // 是否携带 alpha 通道（channels==2 或 4）。
    bool has_alpha_channel = false;
    // 是否存在"非平凡 alpha"：至少一个像素 alpha != 255。
    // 全不透明图即使有 alpha 通道，此值也是 false。
    bool has_nontrivial_alpha = false;
    // 是否存在"非平凡透明度"：至少一个像素 alpha == 0。
    // 用于区分"有 alpha 通道但内容全不透明"与"真的用了透明区域"。
    bool has_transparent_pixels = false;
    // 所有未通过项的可读原因（空 = 全过）。按检查顺序排列。
    std::vector<std::string> errors;
};

// 审查单张图片。纯函数、无 HTTP、无副作用。
// 检查项：文件存在且可读 / 扩展名与 magic bytes 均为受支持格式 /
//         文件大小 ≤ limits.max_file_bytes / 可解码 /
//         每边 ∈ [min_edge_px, max_edge_px]。
// 顺带填充 alpha 相关字段（不参与 ok 判定——alpha 是否可用取决于实测结论）。
ImageCheckResult CheckImageFile(const std::string& path,
                                const ImageLimits& limits = ImageLimits());

// 审查一组视图图片（四视图链路用）。
//   - paths 顺序即视图顺序 [front, left, back, right]（Tripo 约定）。
//   - 额外检查：数量 ∈ [2,4]（Tripo multiview 要求至少 2 张）。
//   - front（paths[0]）必须存在且通过审查（Tripo 要求 front 不可省）。
// 返回每个文件的审查结果（与 paths 一一对应），errors 里含组合层面的问题。
// ⚠️ 本函数**专用于 multiview**：单图链路请用 CheckImageFile（单张），
//    否则会被“至少 2 张”的规则误拦。
std::vector<ImageCheckResult> CheckImageFiles(
    const std::vector<std::string>& paths,
    const ImageLimits& limits = ImageLimits());

// 把审查结果格式化成人类可读的多行文本（供 CLI 打印，避免调用方各自拼串）。
//   path 用于标题；result 为对应审查结果。
std::string FormatImageCheckReport(const std::string& path,
                                   const ImageCheckResult& result);

}  // namespace gen3d
}  // namespace jpov

#endif  // JPOV_GEN3D_IMAGE_INPUT_H_
