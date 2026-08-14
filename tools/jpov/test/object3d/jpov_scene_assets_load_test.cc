// JPOV 第三方 glTF/GLB 加载 smoke test
//
// 验证本次引入的第三方模型能被 GltfLoader/Scene 正确解析（不渲染）:
//   - stool.glb / table.glb / houseplant.glb (poly.pizza, 纯色材质, GLB 容器)
//   - wall_rock/wall_rock.gltf (程序化生成, 外部 bin + 石头纹理)
//
// 注: 地面(砖/土地)是程序化生成的 tileable 贴图, 在场景 test 中作为
// PBR 材质纹理使用, 不走 glTF 加载。
//
// 纯 CPU 解析检查, 不涉及 GPU。
#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/src/gltf_loader.h"

namespace {

struct Case {
    const char* path;
    const char* label;
};

const Case kCases[] = {
    {"tools/jpov/test/object3d/scene_assets/stool.glb", "stool.glb"},
    {"tools/jpov/test/object3d/scene_assets/table.glb", "table.glb"},
    {"tools/jpov/test/object3d/scene_assets/houseplant.glb", "houseplant.glb"},
    {"tools/jpov/test/object3d/scene_assets/wall_rock/wall_rock.gltf", "wall_rock"},
    {"tools/jpov/test/object3d/scene_assets/ground/ground_dirt.gltf", "ground_dirt"},
    {"tools/jpov/test/object3d/scene_assets/ground/ground_brick.gltf", "ground_brick"},
};

}  // namespace

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    int pass = 0;
    for (const Case& c : kCases) {
        // 统计 primitive 数 + 顶点/索引 + 是否有 baseColor/normal 纹理
        struct Stat { int prims=0; int verts=0; int inds=0; bool tex=false; bool nrm=false; } s;
        auto collect = [](const jpov::GltfMeshEntry* e, void* d) {
            auto* st = static_cast<Stat*>(d);
            st->prims++;
            st->verts += static_cast<int>(e->mesh.VertexCount());
            st->inds += static_cast<int>(e->mesh.indices.size());
            if (!e->material.base_color_tex.empty()) st->tex = true;
            if (!e->material.normal_tex.empty()) st->nrm = true;
        };
        bool ok = jpov::LoadGltfScene(c.path, collect, &s);
        if (ok) {
            LOG(INFO) << "[OK] " << c.label
                      << ": prims=" << s.prims
                      << " verts=" << s.verts
                      << " inds=" << s.inds
                      << " baseColor=" << (s.tex ? "tex" : "const")
                      << " normal=" << (s.nrm ? "tex" : "none");
            ++pass;
        } else {
            LOG(ERROR) << "[FAIL] " << c.label << " 加载失败";
        }
    }
    LOG(INFO) << "第三方 glTF 加载: " << pass << "/" << (sizeof(kCases)/sizeof(kCases[0])) << " 通过";
    return (pass == static_cast<int>(sizeof(kCases)/sizeof(kCases[0]))) ? 0 : 1;
}
