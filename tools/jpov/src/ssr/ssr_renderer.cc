// JPOV SsrRenderer —— 实现（**当前为接口评审阶段的空实现单元**）
//
// 本文件暂时只有 include：它让 `ssr_renderer.h` 被真正编译一次。
// （纯 hdrs 的 cc_library 不会解析头文件 —— 接口写错了没人会告诉你，
//   而"头文件语法错"在 review 里是最容易漏、最容易被下一个人撞上的一类。）
//
// 实现按 docs/jpov_ssr_v2_design.md §16 的顺序补：
//   ① 两个 GL-free 单测（1/d 线性恒等式、体素区间判据 —— 整套算法仅有的公理）
//   ② 模块骨架（全屏 pass + DDA + 区间判据 + min 金字塔）
//   ③ 镜面地板 + 已知平面物体的解析 gold
//   ④ 消费端（composite 的柔和与兜底，属于另一处代码，可独立迭代）

#include "tools/jpov/src/ssr/ssr_renderer.h"
