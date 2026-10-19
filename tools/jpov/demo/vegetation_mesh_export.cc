#include "tools/jpov/demo/vegetation_mesh_export.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/src/gltf_loader.h"

namespace jpov_vegetation {
namespace {

using jpov::GltfMeshEntry;
using jpov::MeshData;
using jpov::Vec3f;

struct Entry {
    MeshData mesh;
    size_t index = 0;
};

void CollectEntry(const GltfMeshEntry* entry, void* user_data) {
    std::vector<Entry>* out = static_cast<std::vector<Entry>*>(user_data);
    Entry e;
    e.mesh = entry->mesh;   // 拷贝：导出模式只跑一次，体积可接受
    e.index = out->size();
    out->push_back(std::move(e));
}

float AreaRest(const MeshData& m, uint32_t a, uint32_t b, uint32_t c) {
    const Vec3f& A = m.positions[a];
    const Vec3f& B = m.positions[b];
    const Vec3f& C = m.positions[c];
    const float u[3] = {B.x() - A.x(), B.y() - A.y(), B.z() - A.z()};
    const float v[3] = {C.x() - A.x(), C.y() - A.y(), C.z() - A.z()};
    const float cr[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                         u[0] * v[1] - u[1] * v[0]};
    return 0.5f * std::sqrt(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]);
}

float AreaDeformed(const std::vector<Vec3f>& P, uint32_t a, uint32_t b, uint32_t c) {
    const Vec3f& A = P[a];
    const Vec3f& B = P[b];
    const Vec3f& C = P[c];
    const float u[3] = {B.x() - A.x(), B.y() - A.y(), B.z() - A.z()};
    const float v[3] = {C.x() - A.x(), C.y() - A.y(), C.z() - A.z()};
    const float cr[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                         u[0] * v[1] - u[1] * v[0]};
    return 0.5f * std::sqrt(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]);
}

float BoundsHeight(const MeshData& m) {
    if (m.positions.empty()) {
        return 1.0f;
    }
    float lo = m.positions[0].y();
    float hi = lo;
    for (const Vec3f& p : m.positions) {
        lo = std::min(lo, p.y());
        hi = std::max(hi, p.y());
    }
    const float h = hi - lo;
    return (h > 1e-4f) ? h : 1.0f;
}

}  // namespace

bool SaveDeformedMesh(const std::string& glb_path,
                      const std::string& prefix,
                      const jpov::WindDeformParams& params) {
    std::vector<Entry> entries;
    if (!jpov::LoadGltfScene(glb_path, &CollectEntry, &entries) || entries.empty()) {
        LOG(ERROR) << "SaveDeformedMesh: 无法加载 " << glb_path;
        return false;
    }

    FILE* obj = std::fopen((prefix + ".obj").c_str(), "w");
    FILE* verts = std::fopen((prefix + "_verts.csv").c_str(), "w");
    FILE* tris = std::fopen((prefix + "_tris.csv").c_str(), "w");
    CHECK(obj != nullptr && verts != nullptr && tris != nullptr)
        << "SaveDeformedMesh: 写文件失败（prefix=" << prefix << "）";

    std::fprintf(verts, "prim,vertex,joint,L,x,y,z\n");
    std::fprintf(tris, "prim,tri,ja,jb,jc,area_rest,area_deformed,ratio\n");

    uint32_t base = 1;   // OBJ 的 1-based 顶点偏移
    for (const Entry& e : entries) {
        const MeshData& m = e.mesh;
        const std::vector<float> L = jpov::BuildWindPathLengths(m);
        std::vector<Vec3f> pos;
        std::vector<Vec3f> nrm;
        jpov::WindDeformParams p = params;
        p.ref_length = BoundsHeight(m);

        jpov::DeformMeshWithWind(m, L, p, &pos, &nrm);

        std::fprintf(obj, "o prim%zu\n", e.index);
        for (size_t i = 0; i < pos.size(); ++i) {
            std::fprintf(obj, "v %.9g %.9g %.9g\n", pos[i].x(), pos[i].y(), pos[i].z());
        }
        for (size_t i = 0; i < nrm.size(); ++i) {
            std::fprintf(obj, "vn %.9g %.9g %.9g\n", nrm[i].x(), nrm[i].y(), nrm[i].z());
        }
        for (size_t i = 0; i < pos.size(); ++i) {
            const int jid = (i < m.wind_link.size())
                                ? static_cast<int>(std::lround(m.wind_link[i].x()))
                                : -1;
            std::fprintf(verts, "%zu,%zu,%d,%.9g,%.9g,%.9g,%.9g\n", e.index, i, jid,
                         (i < L.size()) ? L[i] : 0.0f, pos[i].x(), pos[i].y(),
                         pos[i].z());
        }

        const size_t tri_count = m.indices.size() / 3;
        for (size_t t = 0; t < tri_count; ++t) {
            const uint32_t a = m.indices[t * 3 + 0];
            const uint32_t b = m.indices[t * 3 + 1];
            const uint32_t c = m.indices[t * 3 + 2];
            const float ar = AreaRest(m, a, b, c);
            const float ad = AreaDeformed(pos, a, b, c);
            const int ja = (a < m.wind_link.size())
                               ? static_cast<int>(std::lround(m.wind_link[a].x())) : -1;
            const int jb = (b < m.wind_link.size())
                               ? static_cast<int>(std::lround(m.wind_link[b].x())) : -1;
            const int jc = (c < m.wind_link.size())
                               ? static_cast<int>(std::lround(m.wind_link[c].x())) : -1;
            std::fprintf(tris, "%zu,%zu,%d,%d,%d,%.9g,%.9g,%.6g\n",
                         e.index, t, ja, jb, jc, ar, ad, (ar > 1e-12f) ? ad / ar : 0.0f);
        }
        for (size_t t = 0; t < tri_count; ++t) {
            const uint32_t a = m.indices[t * 3 + 0];
            const uint32_t b = m.indices[t * 3 + 1];
            const uint32_t c = m.indices[t * 3 + 2];
            std::fprintf(obj, "f %u//%u %u//%u %u//%u\n", base + a, base + a,
                         base + b, base + b, base + c, base + c);
        }
        base += static_cast<uint32_t>(pos.size());
        LOG(INFO) << "SaveDeformedMesh: prim " << e.index << " verts=" << pos.size()
                  << " tris=" << tri_count << " H=" << p.ref_length;
    }

    std::fclose(obj);
    std::fclose(verts);
    std::fclose(tris);
    LOG(INFO) << "SaveDeformedMesh: 写出 " << prefix << ".obj / _verts.csv / _tris.csv";
    return true;
}

}  // namespace jpov_vegetation
