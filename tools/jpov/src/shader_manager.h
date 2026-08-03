// JPOV ShaderManager — GLSL shader program 的注册 / 编译 / 缓存管理
//
// 将 shader 从 Renderer 的裸成员变量（prog_ / prog_3d_ / tex_prog_ 等）
// 抽离为可注册、可查询、可复用的资源单元。
//
// 用法：
//   unsigned int prog = shader_mgr.GetOrCreate("solid", {kVs, kFs});
//   glUseProgram(prog);
//   int loc = shader_mgr.GetUniform(prog, "uColor");
//
// 新增 shader 时只需一行 GetOrCreate，无需改 Renderer 成员变量。

#ifndef JPOV_SHADER_MANAGER_H_
#define JPOV_SHADER_MANAGER_H_

#include <string>
#include <unordered_map>

namespace jpov {

// 一对 GLSL 源码（vertex + fragment）。
struct ShaderSource {
    const char* vertex;
    const char* fragment;
};

class ShaderManager {
public:
    ShaderManager() = default;
    ~ShaderManager();

    ShaderManager(const ShaderManager&) = delete;
    ShaderManager& operator=(const ShaderManager&) = delete;

    // GetOrCreate: 根据名字和源码获取 shader program。
    // 首次调用时编译 + 链接 + 缓存，后续直接返回缓存的 program。
    // 编译 / 链接失败 → LOG(FATAL) crash。
    //
    // Pre-condition: 已存在同名的 program 时，直接返回缓存（忽略新源码）。
    // Pre-condition: GL context 已激活。
    unsigned int GetOrCreate(const std::string& name, const ShaderSource& source);

    // 根据 program ID 获取 uniform location（首次查询后缓存）。
    //
    // 返回 -1 表示该 uniform 不存在（GL 惯例，合法，由调用方决定是否使用）。
    // Pre-condition: program 已通过 GetOrCreate 注册。
    int GetUniform(unsigned int program, const std::string& uniform_name);

    // 获取注册的 program 数量（供测试 / 调试使用）。
    size_t Size() const { return programs_.size(); }

private:
    struct ShaderProgram {
        unsigned int program = 0;
        // uniform name → location（首次查询后缓存，避免重复 glGetUniformLocation）
        std::unordered_map<std::string, int> uniforms;
    };

    // key（shader 名字）→ 缓存 program
    std::unordered_map<std::string, ShaderProgram> programs_;
};

}  // namespace jpov

#endif  // JPOV_SHADER_MANAGER_H_
