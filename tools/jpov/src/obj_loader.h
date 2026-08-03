// JPOV OBJ 加载器 — 把 Wavefront .obj 文件解析为 CPU 侧 MeshData
//
// 功能：
//   读取标准 Wavefront .obj 文本，把网格几何数据填充到 MeshData
//   （顶点位置 positions / 法线 normals / UV uvs / 索引 indices）。
//
// 支持范围（本 loader 聚焦静态模型加载所需的核心子集）：
//   - 顶点数据: v（位置）、vt（UV）、vn（法线）
//   - 面: f，corner 支持 v / v//vn / v/vt / v/vt/vn 四种索引形式
//   - 多边形面: 自动 fan 三角化（四边形、五边形…均可）
//   - 负索引: OBJ 相对索引（-1 = 最后一个已声明的元素）
//   - 注释(#)与空行: 自动跳过
//
// 明确不支持（超出本轮静态模型任务范围，遇之 LOG(ERROR) 并返回 false）：
//   - 材质与贴图引用: mtllib / usemtl（静态模型纯色渲染不需要）
//   - 曲线/曲面: curvs / surf / bezier（罕见且非网格）
//   - 顶点组: vp / vn 之外的顶点参数（少见）
//   - 组/对象名影响几何: o / g / s 仅作分隔提示，不影响网格数据
//
// 索引语义（corner-splitting）：
//   OBJ 的 f 面 corner 用 (v, vt, vn) 三元组引用，1-based 索引。
//   OBJ 允许同一 position 在不同 corner 引用不同 vt/vn —— 此时该 position
//   必须拆成多个 GPU 顶点。本 loader 按「唯一 (v, vt, vn) 组合」生成顶点，
//   保证 flags 声明的每个属性数组长度一致（Validate 可通过）。
//
// 用法：
//   jpov::MeshData mesh;
//   if (!jpov::LoadObj("res/beetle.obj", &mesh)) { /* 失败处理 */ }
//   mesh.Validate();  // 数据已合法（loader 内部已填充 flags 并保证对齐）

#ifndef JPOV_SRC_OBJ_LOADER_H_
#define JPOV_SRC_OBJ_LOADER_H_

#include <string>

#include "tools/jpov/interface/mesh.h"

namespace jpov {

// 从 OBJ 文件解析网格数据到 out（CPU 侧 MeshData）。
//
// 成功: 返回 true。out->flags 依据文件内容设为
//   kPosition 必有，另含 kNormal（存在 vn）/ kUV（存在 vt）;
//   positions/normals/uvs/indices 已正确填充且对齐。
// 失败: 返回 false 并 LOG(ERROR)（文件打不开 / 语法非法 / 面引用越界等），
//   out 保持未定义（调用方不得在失败后使用）。
bool LoadObj(const std::string& path, MeshData* out);

}  // namespace jpov

#endif  // JPOV_SRC_OBJ_LOADER_H_
