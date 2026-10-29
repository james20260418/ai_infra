// JPOV GPU 信息查询（GL 侧）
//
// 与 interface/gpu_info.h 的数据结构分开：接口头**不 include 任何 GL 头**
// （interface 层保持纯数据，见 interface/README.md 的分层约定），
// 只把「需要 active GL context 才能做的查询」放在这里。

#ifndef JPOV_GPU_INFO_QUERY_H_
#define JPOV_GPU_INFO_QUERY_H_

#include "tools/jpov/interface/gpu_info.h"

namespace jpov {

// 从当前 active 的 GL context 采样 GPU/驱动信息。
//
// Pre-condition: GL context 已创建并 make current。
// Post-condition: 永远返回一个值（即使查询失败）；失败时 kind == kUnknown
//                 且各字符串为空 —— 不 crash，因为 debug 信息不该拖垮引擎。
//
// 分类规则见 GpuKind：只对**已知软件光栅器名**断言 kCpuRaster，
// 其余非空 renderer 归为 kHardware（语义是「不是已知软件光栅器」）。
GpuInfo QueryGpuInfo();

}  // namespace jpov

#endif  // JPOV_GPU_INFO_QUERY_H_
