// JPOV 模型编辑器 — 数值填值输入解析（纯函数 / 可单测 / 无 GL）
//
// 面板的步长 / 裁剪高度等输入框，在「回车 / 焦点丧失」时把文本框内容解析成 float。
// 本文件只放这个**纯解析逻辑**，与 UI/GL 解耦，便于单测：
//   - 合法（能解析出一个有限数，且剩余只有空白）→ 采用新值；
//   - 非法（空串 / 非数字 / 带垃圾后缀 / inf / nan）→ 不改变，由调用方还原文本框。
//
// （沿用穿衣工具 clothing_axis_input.h 的解析语义，只换命名空间。）

#ifndef JPOV_MODEL_EDITOR_NUMBER_INPUT_H_
#define JPOV_MODEL_EDITOR_NUMBER_INPUT_H_

#include <cmath>
#include <cstdlib>

namespace jpov {
namespace model_editor {

// 解析填值文本框内容为 float。
//
// text：待解析文本（C 字符串，可含前后空白）。
// out ：解析成功时的输出值（仅返回 true 时写）。
//
// 返回：true = 文本是一个合法数值（strtof 至少吃掉一个字符，且其后只剩空白）。
//        false = 非法（空串 / 全空白 / 非数字 / 数字后跟非空白垃圾 / NaN / Inf）。
//
// Pre-condition: out != nullptr。
inline bool ParseNumber(const char* text, float* out /*output*/) {
    if (text == nullptr || out == nullptr) {
        return false;
    }
    char* end = nullptr;
    const float value = std::strtof(text, &end);
    if (end == text) {
        return false;  // 没有任何数字被解析。
    }
    for (const char* p = end; *p != '\0'; ++p) {
        if (*p != ' ' && *p != '\t') {
            return false;  // 数字后跟非空白垃圾。
        }
    }
    if (!std::isfinite(value)) {
        return false;  // NaN / Inf：合法浮点字面但不是合法数值输入。
    }
    *out = value;
    return true;
}

}  // namespace model_editor
}  // namespace jpov

#endif  // JPOV_MODEL_EDITOR_NUMBER_INPUT_H_
