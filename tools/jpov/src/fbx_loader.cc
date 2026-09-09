// JPOV — FBX 动画/骨架加载器实现（见 fbx_loader.h）。CPU、GL-free，经 ufbx 读文件。
//
// 实现只做一件事：把 FBX 的「骨架 + 动画原始全帧 + 帧频」原样抓进 interface/FBXClip。
// 本步不做重定向 / 重采样 / 播放；那属于基于 FBXClip 的二次开发。
//
// 具体步骤:
//   1. 戴 ufbx load opts（跳过几何大件，只留 node/bone/anim），读文件。
//   2. 沿 node 树深度优先收集『带 bone 属性』的节点（Skeleton node）当骨架 → 按遍历序给
//      索引发 SkeletonType: name / parent(沿树向上最近 bone) / rest_offset(local 平移)。
//   3. 逐帧（k=0..N-1，t=begin+k/fps）对每骨取 ufbx_evaluate_transform(anim,node,t) 的
//      local 旋转四元数 → 填 SkeletonPose.joint_rotation；根骨平移填 root_offset。
//
// 坐标系/单位（与 JPOV 数据模型对齐, 见 fbx_loader.h「坐标系 / 单位」一段）:
//   SkeletonPose 存的是相对父的旋转 + 根位移 —— 姿态内容与全局轴无关（角色最终朝哪由放置
//   层 up/front 决定，不锁在 pose）。故本 loader 不需把源轴“硬转”进 pose；
//   源 FBX 轴/单位原样进 clip（对齐 glTF loader 透传原生单位的惯例）。Mixamo 人形源默认
//   y-up + cm, 传过即已是 y-up。成功 LOG 里带出源 scene 的 axes.up 与 unit_meters 供 debug。

#include "tools/jpov/src/fbx_loader.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "ufbx.h"

namespace jpov {

namespace {

// 把 ufbx 错误描述成可读字符串（LOG 用）。
std::string UfbxErrorMessage(const ufbx_error* e) {
    char buf[512];
    ufbx_format_error(buf, sizeof(buf), e);
    return std::string(buf);
}

// 戴 load opts。只载 node/bone/anim：跳过几何等大件(load 更快、内存更小)。
// ⚠️ 只设存在/需要的字段；不存在的布尔(如 per-category ignore)勿猜，会编译错。
ufbx_load_opts MakeLoadOpts() {
    ufbx_load_opts o;
    std::memset(&o, 0, sizeof(o));
    o.ignore_geometry = true;      // 只要骨骼与动画，不要 mesh/蒙皮顶点数据。
    o.ignore_all_content = false;  // 动画本身要(evaluate) → 保留。
    o.ignore_embedded = true;
    o.load_external_files = false;
    return o;
}

// 轴枚举 → 短名（成功 LOG 打出来便于 debug 源 FBX 的轴/单位）。
const char* AxName(ufbx_coordinate_axis a) {
    switch (a) {
        case UFBX_COORDINATE_AXIS_POSITIVE_X: return "+X";
        case UFBX_COORDINATE_AXIS_NEGATIVE_X: return "-X";
        case UFBX_COORDINATE_AXIS_POSITIVE_Y: return "+Y";
        case UFBX_COORDINATE_AXIS_NEGATIVE_Y: return "-Y";
        case UFBX_COORDINATE_AXIS_POSITIVE_Z: return "+Z";
        case UFBX_COORDINATE_AXIS_NEGATIVE_Z: return "-Z";
        default: return "(unknown)";
    }
}

// 收集 node 子树里『带 ufbx_bone』的节点（Skeleton/FBX Bone），深度优先序遍历 → 天然拓扑序。
void CollectBoneNodes(const ufbx_node* n, std::vector<const ufbx_node*>* out) {
    if (n == nullptr) return;
    if (n->bone != nullptr) out->push_back(n);
    for (size_t i = 0; i < n->children.count; ++i) {
        CollectBoneNodes(n->children.data[i], out);
    }
}

}  // namespace

bool LoadFbxAnimation(const std::string& path, FBXClip* out) {
    ufbx_load_opts opts = MakeLoadOpts();
    ufbx_error err;
    std::memset(&err, 0, sizeof(err));
    ufbx_scene* scene = ufbx_load_file(path.c_str(), &opts, &err);
    if (scene == nullptr) {
        LOG(ERROR) << "LoadFbxAnimation: 无法加载 " << path << " — "
                   << UfbxErrorMessage(&err);
        return false;
    }

    // ---- 动画描述: 默认 lane(scene->anim); 无则取第一个 stack ----
    ufbx_anim* anim = scene->anim;
    if (anim == nullptr && scene->anim_stacks.count > 0) {
        anim = scene->anim_stacks.data[0]
                   ? scene->anim_stacks.data[0]->anim
                   : nullptr;
    }
    const double fps = scene->settings.frames_per_second;
    if (anim == nullptr || fps <= 0.0) {
        LOG(ERROR) << "LoadFbxAnimation: " << path
                   << " 没有可读动画(fps=" << fps << ")";
        ufbx_free_scene(scene);
        return false;
    }

    // ---- 骨架: 收 bone 子树 ----
    std::vector<const ufbx_node*> nodes;
    CollectBoneNodes(scene->root_node, &nodes);
    if (nodes.empty()) {
        LOG(ERROR) << "LoadFbxAnimation: " << path << " 无 bone(Skeleton) 节点";
        ufbx_free_scene(scene);
        return false;
    }

    const int bone_count = static_cast<int>(nodes.size());
    FBXClip clip;
    SkeletonType& skel = clip.skeleton;
    skel.joints.resize(static_cast<size_t>(bone_count));
    // 骨 index → 沿树父链里第一个同为 bone 的节点(SkeletonType.joints 索引)。
    // 遍历序即拓扑序: 父骨必在其子树骨之前已入 nodes(DFS 先父后子)。
    for (int i = 0; i < bone_count; ++i) {
        const ufbx_node* b = nodes[i];
        SkeletonJoint& j = skel.joints[i];
        j.parent = kSkeletonNoParent;
        // b->local_transform.translation = 相对父的静止(bind)平移 —— 骨长/朝向骨架。
        const ufbx_vec3& t = b->local_transform.translation;
        j.rest_offset = Vec3f(static_cast<float>(t.x), static_cast<float>(t.y),
                              static_cast<float>(t.z));
        // 名字原样(debug 比对凭据); 空安全。
        if (b->name.data != nullptr && b->name.length > 0) {
            j.name.assign(b->name.data, b->name.length);
        }
        // 沿父链找最近的 bone 祖先作为骨架里的 parent。
        for (const ufbx_node* p = b->parent; p != nullptr; p = p->parent) {
            if (p->bone == nullptr) continue;  // 跳过非 bone 的空/轴节点
            // p 必已在本 nodes(收集阶段 DFS 先父后子) → 线性扫(骨少) 找其 index。
            for (int k = 0; k < i; ++k) {
                if (nodes[k] == p) {
                    j.parent = k;
                    break;
                }
            }
            break;
        }
    }

    // ---- 帧序时基: 数 = floor((end-begin)*fps)+1, t_k = begin + k/fps ----
    const double t_begin = anim->time_begin;
    const double t_end = anim->time_end;
    const double dur = (t_end > t_begin) ? (t_end - t_begin) : 0.0;
    const int frame_count = static_cast<int>(std::floor(dur * fps + 0.5)) + 1;
    if (frame_count <= 0) {
        LOG(ERROR) << "LoadFbxAnimation: " << path << " 动画时窗为空("
                   << t_begin << ".." << t_end << ")";
        ufbx_free_scene(scene);
        return false;
    }

    clip.frames_per_second = fps;
    clip.frames.reserve(static_cast<size_t>(frame_count));
    for (int k = 0; k < frame_count; ++k) {
        const double t = t_begin + static_cast<double>(k) / fps;
        SkeletonPose pose;
        pose.bone_count = bone_count;
        pose.joint_rotation.resize(static_cast<size_t>(bone_count));
        for (int i = 0; i < bone_count; ++i) {
            const ufbx_node* b = nodes[i];
            // local 变换(相对父): 每骨该时刻旋转; 根骨平移 → root_offset。
            const ufbx_transform tf = ufbx_evaluate_transform(anim, b, t);
            geom::Quaternion<float> q(static_cast<float>(tf.rotation.x),
                                      static_cast<float>(tf.rotation.y),
                                      static_cast<float>(tf.rotation.z),
                                      static_cast<float>(tf.rotation.w));
            q.NormalizeInPlace();  // 保单位(loader 守恒, 防御性)
            pose.joint_rotation[i] = q;
            if (i == 0) {
                // 根骨(角色骨盆/原点)的动画位移 = root motion; 静止动作恒≈bind 位置。
                pose.root_offset = Vec3f(static_cast<float>(tf.translation.x),
                                         static_cast<float>(tf.translation.y),
                                         static_cast<float>(tf.translation.z));
            }
        }
        clip.frames.push_back(std::move(pose));
    }

    LOG(INFO) << "LoadFbxAnimation ok: " << path << " bones=" << bone_count
              << " fps=" << fps << " frames=" << frame_count << "[" << t_begin
              << ".." << t_end << "]s | src axes: right="
              << AxName(scene->settings.axes.right)
              << " up=" << AxName(scene->settings.axes.up)
              << " front=" << AxName(scene->settings.axes.front)
              << " unit_meters=" << scene->settings.unit_meters;
    if (out != nullptr) *out = std::move(clip);
    ufbx_free_scene(scene);
    return true;
}

}  // namespace jpov
