// JPOV glTF 加载结果 —— 用户可见的数据结构
//
// 一个 glTF 文件（.gltf / .glb）在 GPU 侧被加载为一组"primitive"，
// 每个 primitive 就是一个可独立渲染的物体：一份已上传的几何（mesh_id）
// + 一份完整材质（PBRMaterial，含已上传的纹理句柄）。
//
// 设计约定（重要）：
//   - GltfObject 是"资源独占"的：它内部所有 mesh / texture 句柄
//     都是本次 LoadGltf 新建的，不与其他 GltfObject 或 RegisterMesh 的
//     内容共享。因此可以用 Renderer::ReleaseGltf(obj) 安全地整体释放，
//     不必担心误伤其它 object。
//   - 用户拿到 GltfObject 后，只需把 primitives 交给
//     RenderCommandList::DrawGltfObject() 即可渲染；无需也不应该
//     直接访问 mesh_id / texture 的内部细节。
//   - GltfObject 里的 MeshData（CPU 几何）不对外暴露 —— 上传进
//     MeshManager 后几何就固化在 GPU 侧，用户只需要 mesh_id。

#ifndef JPOV_INTERFACE_GLTF_OBJECT_H_
#define JPOV_INTERFACE_GLTF_OBJECT_H_

#include <vector>

#include "tools/jpov/interface/pbr_material.h"

namespace jpov {

// 一个 glTF primitive = 一份 GPU 几何 + 一份材质。
//
// mesh_id: 该 primitive 经 MeshManager 上传后得到的句柄
//          （由 Renderer::LoadGltf 内部填充，用户只读）。
// material: 该 primitive 的完整 PBR 材质（各 *_tex 为已上传的 GPU
//           纹理句柄，由 Renderer::LoadGltf 内部填充）。
//
// 语义上对应 glTF 的一个 primitive（一个 mesh + 一个 material 的绑定）。
struct GltfPrimitive {
    uint32_t mesh_id = 0;
    PBRMaterial material;
};

// 一个 glTF 文件加载后的完整结果：N 个可渲染 primitive。
//
// 所有 GPU 资源均为本对象独占（见文件头约定），可用
// Renderer::ReleaseGltf(obj) 整体释放。
struct GltfObject {
    std::vector<GltfPrimitive> primitives;

    // 模型在 loader 产出的**网格局部坐标**下的轴对齐包围盒（即 DrawGltfObject 施加上 up/front
    // 旋转前的局部框架，坐标轴方向取决于进货源内容，勿以“Z-up/Y-up”概括 —— JPOV 约定
    // 局部 +Y→世界 up，见 render_command.h）。由 Renderer::LoadGltf 在加载时遍历所有
    // primitive 顶点计算合并，供上层（查看器）做相机自适应/剔除等用，无需再碰 CPU 几何。
    //
    // 约定：
    //   - 模型非空时，本包围盒覆盖全部顶点（min/max 各分量取所有顶点极值）。
    //   - 模型为空（无 primitive）时不可用：bounds_valid == false，min/max 未定义。
    //
    // 坐标为 loader 产出的网格局部坐标；若以恒等摆放（center=0, up=+Y, front=+Z）
    // 经 DrawGltfObject 放置，本包围盒即该局部框架在世界坐标里的范围。
    float bounds_min[3] = {0.0f, 0.0f, 0.0f};
    float bounds_max[3] = {0.0f, 0.0f, 0.0f};
    bool  bounds_valid = false;

    // 便捷：是否有任何可渲染 primitive。
    bool empty() const { return primitives.empty(); }
    // 便捷：primitive 数量。
    size_t size() const { return primitives.size(); }
};

}  // namespace jpov

#endif  // JPOV_INTERFACE_GLTF_OBJECT_H_
