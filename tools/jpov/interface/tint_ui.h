// JPOV base tint 调色板 UI —— 两个通道的 RGB 调色控件 + 总开关（header-only）。
//
// 为什么单独成文件：model viewer 与 clothing tool 都要这一组控件。两份拷贝迟早分叉
//（一边改成 0~255、另一边还是 0~1），故把「布局 + 控件 + 绑定字段」收成唯一一份。
//
// 语义与数据来源见 pbr_material.h 的 `BaseTint`：
//   · enabled 勾选框 = 开关保护：关掉 → 片元着色器整段跳过乘法；
//   · 每个通道 = 一个色块（当前色）+ R/G/B 三条滑条（0~1）。
//
// 布局：从 first_row 起向下排，每行高 = first_row.size.y()，行距 = step_y。
// 行数固定 = kBaseTintPaletteRows（1 个开关 + 每通道 2 行）。
// Pre-condition: tint != nullptr；first_row.size.x() > 0；step_y > 0。

#ifndef JPOV_INTERFACE_TINT_UI_H_
#define JPOV_INTERFACE_TINT_UI_H_

#include <algorithm>
#include <cstdio>

#include <glog/logging.h>

#include "tools/jpov/interface/pbr_material.h"
#include "tools/jpov/interface/ui.h"

namespace jpov {

// 调色板占用的行数：启用勾选(1) + 每通道「色块 + RGB 一行」(2)。
inline constexpr int kBaseTintPaletteRows = 1 + kNumBaseTintColors * 2;

namespace tint_ui_internal {

inline UiRect RowRect(const UiRect& first_row, int row, float step_y) {
    return UiRect{{first_row.pos.x(), first_row.pos.y() + step_y * static_cast<float>(row)},
                  first_row.size};
}

}  // namespace tint_ui_internal

// 画 base tint 调色板（就地修改 tint）。返回占用的行数（= kBaseTintPaletteRows）。
inline int DrawBaseTintPalette(Ui& ui, BaseTint* tint, const UiRect& first_row,
                               float step_y) {
    CHECK(tint != nullptr);
    CHECK_GT(first_row.size.x(), 0.0f);
    CHECK_GT(step_y, 0.0f);

    int row = 0;
    ui.Checkbox("启用 tint 乘法（关 = 不做乘法）", &tint->enabled,
                tint_ui_internal::RowRect(first_row, row, step_y));
    ++row;

    // 每通道：第一行「标签 + 右侧色块」（直观看到当前颜色），第二行三条滑条并排。
    constexpr float kSliderGap = 8.0f;
    constexpr float kSwatchSide = 28.0f;
    constexpr float kSwatchTextGap = 10.0f;
    for (int c = 0; c < kNumBaseTintColors; ++c) {
        char label[64];
        std::snprintf(label, sizeof(label), "Tint %d（id=%d）当前色", c + 1, c + 1);
        const UiRect swatch_row = tint_ui_internal::RowRect(first_row, row, step_y);
        const float text_w = std::max(swatch_row.size.x() - kSwatchSide - kSwatchTextGap,
                                      0.0f);
        ui.Text(label, UiRect{{swatch_row.pos.x(), swatch_row.pos.y()},
                              {text_w, swatch_row.size.y()}});
        ui.ColorSwatch(
            "", tint->color[c],
            UiRect{{swatch_row.pos.x() + swatch_row.size.x() - kSwatchSide,
                    swatch_row.pos.y()},
                   {kSwatchSide, swatch_row.size.y()}});
        ++row;

        const UiRect row_rect = tint_ui_internal::RowRect(first_row, row, step_y);
        const float slider_w =
            (row_rect.size.x() - kSliderGap * 2.0f) / 3.0f;
        float* const channels[3] = {&tint->color[c].r, &tint->color[c].g,
                                    &tint->color[c].b};
        const char* const names[3] = {"R", "G", "B"};
        for (int k = 0; k < 3; ++k) {
            const UiRect r{{row_rect.pos.x() + static_cast<float>(k) * (slider_w + kSliderGap),
                            row_rect.pos.y()},
                           {slider_w, row_rect.size.y()}};
            ui.SliderFloat(names[k], channels[k], r, 0.0f, 1.0f,
                           /*decimal_places*/ 2);
        }
        ++row;
    }
    return row;
}

}  // namespace jpov

#endif  // JPOV_INTERFACE_TINT_UI_H_
