// JPOV 穿衣工具 — 坐标填值输入解析（纯函数 / 可单测 / 无 GL）
//
// 穿衣工具面板的 x/y/z 填值输入框，在"回车 / 焦点丧失"时把文本框内容解析成 float，
// 写回衣服 center 的对应分量。本文件只放这个**纯解析逻辑**，与 UI/GL 解耦，便于单测：
//   - 合法（能解析出一个数，且剩余只有空白）→ 采用新值；
//   - 非法（空串 / 非数字 / 带垃圾后缀）→ **不改变**，由调用方把文本框还原成当前值。
//
// 为什么单列一个纯函数：输入框的"提交"语义（何时生效、非法输入怎么办）是这一步的
// 唯一行为要点，但它藏在交互路径里、难以在 headless 下自动触发。抽成纯函数后可用
// 单测覆盖边界（空串、纯空格、"+1.5"、"-3"、"1e3"、"12abc"、"." 等），不依赖 GL。

#ifndef JPOV_CLOTHING_CLOTHING_AXIS_INPUT_H_
#define JPOV_CLOTHING_CLOTHING_AXIS_INPUT_H_

#include <cmath>
#include <cstdlib>

namespace jpov {
namespace clothing {

// 解析填值文本框内容为 float。
//
// text：待解析文本（C 字符串，可含前后空白）。
// out ：解析成功时的输出值（仅返回 true 时写）。
//
// 返回：true = 文本是一个合法数值（strtof 至少吃掉一个字符，且其后只剩空白）。
//        false = 非法（空串 / 全空白 / 非数字 / 数字后跟非空白垃圾）。
//
// 说明：strtof 会接受 "1.5"、"+2"、"-3"、"1e3"、".5"、"inf"、"nan" 等形式；
//   其中 inf/nan 虽是合法浮点字面却不应作为坐标，故本函数**额外拒绝非有限值**。
//
// Pre-condition: out != nullptr。
inline bool ParseAxisValue(const char* text, float* out /*output*/) {
    if (text == nullptr || out == nullptr) {
        return false;
    }
    char* end = nullptr;
    const float value = std::strtof(text, &end);
    if (end == text) {
        return false;  // 没有任何数字被解析（空串 / 全空白 / 非数字开头）。
    }
    for (const char* p = end; *p != '\0'; ++p) {
        if (*p != ' ' && *p != '\t') {
            return false;  // 数字后跟非空白垃圾。
        }
    }
    if (!std::isfinite(value)) {
        return false;  // NaN / Inf：合法浮点字面但不是合法坐标。
    }
    *out = value;
    return true;
}

}  // namespace clothing
}  // namespace jpov

#endif  // JPOV_CLOTHING_CLOTHING_AXIS_INPUT_H_
