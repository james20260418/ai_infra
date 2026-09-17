// JPOV InstanceBuffer 实现 — per-instance attribute 数据缓冲（见头文件说明）

#include "tools/jpov/src/instance_buffer.h"

// GL 头文件必须最先 include（在 MinGW #define 宏替换之前），否则 GL 常量在
// MinGW 路径下不可见。顺序：GL 常量声明 → 再 #define 函数名映射。
#ifdef _WIN32
#include <GL/gl.h>
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
#else
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_DYNAMIC_DRAW
#define GL_DYNAMIC_DRAW 0x88E8
#endif
#ifndef GL_FLOAT
#define GL_FLOAT 0x1406
#endif
#ifndef GL_FALSE
#define GL_FALSE 0
#endif

#include <glog/logging.h>

namespace jpov {

InstanceBuffer::~InstanceBuffer() {
    if (vbo_ != 0u) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0u;
    }
}

void InstanceBuffer::EnsureBuffer() {
    if (vbo_ != 0u) {
        return;
    }
    glGenBuffers(1, &vbo_);
    CHECK_NE(vbo_, 0u) << "InstanceBuffer: glGenBuffers failed";
}

void InstanceBuffer::Upload(const std::vector<float>& data) {
    CHECK_GT(spec_.stride_floats, 0) << "InstanceBuffer: 未设置布局（stride=0）";
    CHECK(!data.empty()) << "InstanceBuffer::Upload: 数据不能为空（叫它干什么？）";
    CHECK_EQ(data.size() % static_cast<size_t>(spec_.stride_floats), 0u)
        << "InstanceBuffer::Upload: 长度 " << data.size() << " 不是每实例 "
        << spec_.stride_floats << " 个 float 的整数倍";
    instance_count_ = static_cast<int>(data.size() / static_cast<size_t>(spec_.stride_floats));
    CHECK_GT(instance_count_, 0);

    EnsureBuffer();
    const size_t bytes = data.size() * sizeof(float);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    // 整批重写：glBufferData 顺带在超容量时重新分配（glBufferSubData 只能在已有
    // 容量内写，扩容必须走 glBufferData）。容量够时 GL 实现可复用存储。
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes), data.data(),
                 GL_DYNAMIC_DRAW);
    capacity_bytes_ = bytes;
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void InstanceBuffer::AttachToVao(unsigned int vao) const {
    CHECK_NE(vbo_, 0u) << "InstanceBuffer::AttachToVao: 还没有数据（先 Upload）";
    CHECK_GT(instance_count_, 0);
    const int stride_bytes = spec_.stride_floats * static_cast<int>(sizeof(float));
    const int slot_bytes = spec_.slot_components * static_cast<int>(sizeof(float));

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    for (int k = 0; k < spec_.slot_count; ++k) {
        const unsigned int loc = spec_.base_loc + static_cast<unsigned int>(k);
        glEnableVertexAttribArray(loc);
        glVertexAttribPointer(loc, spec_.slot_components, GL_FLOAT, GL_FALSE, stride_bytes,
                              reinterpret_cast<const void*>(
                                  static_cast<size_t>(slot_bytes) * static_cast<size_t>(k)));
        // divisor=1：每实例推进一步（instancing 的核心）。divisor 是 VAO 状态。
        glVertexAttribDivisor(loc, 1);
    }
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void InstanceBuffer::DetachFromVao(unsigned int vao) const {
    if (vbo_ == 0u) {
        return;  // 从未挂过，no-op（构造后未 Upload 的缓冲）
    }
    glBindVertexArray(vao);
    for (int k = 0; k < spec_.slot_count; ++k) {
        const unsigned int loc = spec_.base_loc + static_cast<unsigned int>(k);
        glDisableVertexAttribArray(loc);
        glVertexAttribDivisor(loc, 0);   // 归零，避免留着 divisor 影响后续普通 draw
    }
    glBindVertexArray(0);
}

// ==================== InstanceBufferBinding ====================

InstanceBufferBinding::InstanceBufferBinding(
    unsigned int vao, std::initializer_list<const InstanceBuffer*> buffers)
    : vao_(vao) {
    CHECK_NE(vao_, 0u) << "InstanceBufferBinding: vao 不能为 0";
    CHECK_GT(buffers.size(), 0u)
        << "InstanceBufferBinding: 至少要挂一个实例缓冲（空守卫没有意义）";
    buffers_.assign(buffers.begin(), buffers.end());
    for (const InstanceBuffer* b : buffers_) {
        CHECK(b != nullptr) << "InstanceBufferBinding: 缓冲指针为空";
        b->AttachToVao(vao_);
    }
}

InstanceBufferBinding::~InstanceBufferBinding() {
    // 逆序摘除（与挂上顺序对称）。
    for (auto it = buffers_.rbegin(); it != buffers_.rend(); ++it) {
        (*it)->DetachFromVao(vao_);
    }
}

}  // namespace jpov
