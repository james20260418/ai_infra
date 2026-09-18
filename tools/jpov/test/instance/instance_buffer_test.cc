// InstanceBuffer 属性挂载/摘除的不变量门禁测试
//
// 守的是什么（2026-09-17 重构新增）：
//   per-instance 属性（loc6+，divisor=1）必须挂在「draw 用的那个 VAO」上，draw 后**摘掉**。
//   漏摘会把 divisor=1 的启用态**残留**在 VAO 上，泄漏给后续在该 VAO 上的普通 draw
//   （同一 mesh 既走 instanced 又走普通 draw 时就会踩到）。这正是重构前真实存在的缺陷：
//   UploadInstance* 里 enable 了 loc6..10，之后没有任何地方 disable，
//   而当时写的「安全阀」DisableInstanceAttributes 是**死代码**（零调用点）。
//
// 本测试把「挂载/摘除严格配对」变成被断言的**事实**：
//   1) 正向：Attach 后 loc6..10 必须是 enabled + divisor=1（证明查询有效、状态真的生效）；
//   2) 反向：Detach 后必须全部 disabled + divisor=0（这是门禁本体）；
//   3) RAII：InstanceBufferBinding 出作用域后同样必须全清（结构性保证，无需人手配对）。
// 第 1 步是必要的「负向参照」—— 没有它，第 2 步可能因为「压根没挂上去」而假通过。

#include <cstdint>
#include <vector>

#include <glog/logging.h>

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/src/instance_buffer.h"

namespace {

// 查询某 VAO 上某 location 的启用态与 divisor。
struct AttribState {
    int enabled = 0;
    int divisor = 0;
    int size = 0;
};

AttribState QueryAttrib(unsigned int vao, unsigned int loc) {
    glBindVertexArray(vao);
    AttribState s;
    glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &s.enabled);
    glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_DIVISOR, &s.divisor);
    glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_SIZE, &s.size);
    glBindVertexArray(0);
    return s;
}

// 断言 [base, base+slot_count) 区间全部为 enabled(given) 且 divisor(given)。
void CheckRange(unsigned int vao, const jpov::InstanceAttrSpec& spec, bool expect_enabled,
                int expect_divisor, const char* what) {
    for (int k = 0; k < spec.slot_count; ++k) {
        const unsigned int loc = spec.base_loc + static_cast<unsigned int>(k);
        const AttribState s = QueryAttrib(vao, loc);
        CHECK_EQ(s.enabled, expect_enabled ? 1 : 0)
            << what << ": loc" << loc << " enabled 应为 " << (expect_enabled ? 1 : 0)
            << "，实际 " << s.enabled;
        if (expect_enabled) {
            CHECK_EQ(s.divisor, expect_divisor)
                << what << ": loc" << loc << " divisor 应为 " << expect_divisor
                << "，实际 " << s.divisor;
            CHECK_EQ(s.size, spec.slot_components)
                << what << ": loc" << loc << " size 应为 " << spec.slot_components
                << "，实际 " << s.size;
        } else {
            // 摘除后 divisor 必须归零，避免留 divisor 影响后续普通 draw。
            CHECK_EQ(s.divisor, 0) << what << ": loc" << loc << " 摘除后 divisor 应为 0，实际 "
                                   << s.divisor;
        }
    }
}

class InstanceBufferTestApp : public JPOV {
public:
    using JPOV::JPOV;

    // 本测试只验 GL 属性挂载状态，不渲染：空实现即可。
    void OneIteration(int64_t, const jpov::InputSnapshot&, const jpov::WindowInfo&,
                      jpov::RenderCommandList*) override {}
};

}  // namespace

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);

    JPOV::Config cfg;
    cfg.title = "JPOV InstanceBuffer 不变量测试";
    cfg.headless = true;
    InstanceBufferTestApp app(cfg);
    app.Init();

    const std::vector<float> model_k2 = {
        // 实例 0：单位矩阵
        1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1,
        // 实例 1：平移 (5,0,0)
        1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  5, 0, 0, 1,
    };
    const std::vector<float> pose_k2 = {0, 0, 0,  92, 184, 0.5f};

    jpov::InstanceBuffer model_buf(jpov::kInstanceModelAttrSpec);
    jpov::InstanceBuffer pose_buf(jpov::kInstancePoseAttrSpec);
    model_buf.Upload(model_k2);
    pose_buf.Upload(pose_k2);
    CHECK_EQ(model_buf.instance_count(), 2);
    CHECK_EQ(pose_buf.instance_count(), 2);

    // 造一个「代表 mesh VAO」的空 VAO（本测试只验属性挂载状态，不真画）。
    unsigned int vao = 0;
    glGenVertexArrays(1, &vao);
    CHECK_NE(vao, 0u) << "glGenVertexArrays 失败";

    // ---- 前置：空 VAO 上这些槽必须是干净的（否则后面测不出东西）----
    CheckRange(vao, jpov::kInstanceModelAttrSpec, /*expect_enabled=*/false, 0, "前置(未挂)");
    CheckRange(vao, jpov::kInstancePoseAttrSpec, /*expect_enabled=*/false, 0, "前置(未挂)");

    // ---- 1) 手写 Attach：必须是 enabled + divisor=1（负向参照：证明查询有效）----
    model_buf.AttachToVao(vao);
    pose_buf.AttachToVao(vao);
    CheckRange(vao, jpov::kInstanceModelAttrSpec, /*expect_enabled=*/true, 1, "Attach 后");
    CheckRange(vao, jpov::kInstancePoseAttrSpec, /*expect_enabled=*/true, 1, "Attach 后");
    LOG(INFO) << "OK: Attach 后 loc6..10 均已启用且 divisor=1";

    // ---- 2) 手写 Detach：必须全部清干净（门禁本体）----
    pose_buf.DetachFromVao(vao);
    model_buf.DetachFromVao(vao);
    CheckRange(vao, jpov::kInstanceModelAttrSpec, /*expect_enabled=*/false, 0, "Detach 后");
    CheckRange(vao, jpov::kInstancePoseAttrSpec, /*expect_enabled=*/false, 0, "Detach 后");
    LOG(INFO) << "OK: Detach 后 loc6..10 全部禁用且 divisor 归零（无残留启用态）";

    // ---- 3) RAII 守卫：出作用域必须自动清干净（结构性保证）----
    {
        jpov::InstanceBufferBinding bind(vao, {&model_buf, &pose_buf});
        CheckRange(vao, jpov::kInstanceModelAttrSpec, /*expect_enabled=*/true, 1, "守卫作用域内");
        CheckRange(vao, jpov::kInstancePoseAttrSpec, /*expect_enabled=*/true, 1, "守卫作用域内");
    }
    CheckRange(vao, jpov::kInstanceModelAttrSpec, /*expect_enabled=*/false, 0, "守卫出作用域后");
    CheckRange(vao, jpov::kInstancePoseAttrSpec, /*expect_enabled=*/false, 0, "守卫出作用域后");
    LOG(INFO) << "OK: InstanceBufferBinding 出作用域后自动摘除（Attach/Detach 不可能失配）";

    glDeleteVertexArrays(1, &vao);
    app.Finalize();

    LOG(INFO) << "instance_buffer_test PASSED";
    return 0;
}
