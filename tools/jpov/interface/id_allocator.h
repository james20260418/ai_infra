// JPOV IdAllocator — 可复用/避回绕的整数句柄分配器（GL-free）
//
// 背景：JPOV 里 texture / mesh / (skeleton) 都以 uint32 id 作为外部资源句柄。若用裸
// uint32 next_id_ 单调分配、release 的空号不回填 —— 在进程内高频 分配/释放 循环里（如
// 10 帧慢刷监控反复 load/unload 图片）可能把 next_id_ 推到 uint32 回绕；若回绕到仍被 live
// 资源占用的编号，新 Register 会覆盖旧资源 → 泄漏 + 幽灵错位。
//
// 本类提供正确的通用分配（freelist / LIFO 空槽复用 + 避回绕）：
//   - Acquire()：优先取“最近释放的空槽”(LIFO，O(1)，热槽先复用)；空槽池空才单调递增开新号。
//   - Release(id)：仅当 id 当前**被占用（live）**才放回空槽池；否则（从未分配 / 已释放）静默忽略。
//   - 维护 live 集合：Acquire 永只给“live 集外”的号；Release 只认 live 内的号 → 天然不踩 live、
//     不因重复 release(double-free) 把同一号重复发出去。
//
// id 语义（对齐 JPOV 资源 id 惯例）：
//   - uint32，0 保留“无效”；Acquire() 返回非 0。
//   - Release 不存在的 id → 静默忽略（允许重复释放，与 MeshManager/TextureManager 契约一致）。
//   - 复用的 id 数值与旧者相同 → 使用者须保证“release 后不再引用”，再让新资源 occupy
//     （mesh/texture 既有惯例：id 不承诺终身有效，release 即失效、可被复用）。
//
// 用途：作为【新增】skeleton renderer 侧的 id 管理；将来修 MeshManager/TextureManager 的
//   裸递增回绕 bug 时同款（旧 manager 修 bug 不进本骨架 PR，下个 PR 单独做，本类为范例）。

#ifndef JPOV_INTERFACE_ID_ALLOCATOR_H_
#define JPOV_INTERFACE_ID_ALLOCATOR_H_

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace jpov {

// 一个 uint32 句柄分配器（freelist / LIFO 空槽复用 + live 集）。语义见文件头。
class IdAllocator {
public:
    IdAllocator() = default;

    // 分配一个新句柄：优先复用最近释放的空槽，否则单调递增。返回非 0 合法 id。
    //   极端：uint32 空间被 live 集挤满（实际不可达）时返回 0，调用方按空间耗尽处理。
    uint32_t Acquire();

    // 释放一个**当前被占用**的句柄回空槽池。id 非 live（从未分配 / 已释放）→ 静默忽略
    // （允许重复释放）。0 恒忽略。
    void Release(uint32_t id);

    // 是否在“未被占用”状态（live 集为空）—— 供测试/调试断言。
    bool empty() const { return live_.empty(); }
    // 当前被占用的 id 集合大小（live 资源数）。0 不计。
    std::size_t size() const { return live_.size(); }
    // 指定 id 当前是否被占用（live）。
    bool occupied(uint32_t id) const { return live_.count(id) != 0; }

private:
    // 单调递增取新号的游标（0 = 无效，1 起）。
    uint32_t next_id_ = 1;

    // 当前被占用的 id 集（live）。Acquire 只看它外 / Release 只认它内。
    std::unordered_set<uint32_t> live_;
    // 已释放、待复用的空槽池（LIFO：Acquire 取尾、Release 推尾；刚释放的最热最先复用）。
    std::vector<uint32_t> released_;
};

}  // namespace jpov

#endif  // JPOV_INTERFACE_ID_ALLOCATOR_H_
