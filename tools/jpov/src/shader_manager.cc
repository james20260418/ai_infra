// JPOV ShaderManager 实现
//
// shader program 的编译 / 链接 / 缓存。

// GL_GLEXT_PROTOTYPES：在 include GL 头之前定义，
// 使 glCreateShader / glLinkProgram 等 GL 函数在 Linux/Mesa 下被声明。
#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/shader_manager.h"

// GL 头文件必须最先 include（在 MinGW #define 宏替换之前），
// 否则 GL 常量（GL_VERTEX_SHADER 等）在 MinGW 路径下不可见。
// 顺序：GL 常量声明 → 再 #define 函数名映射
#include <GL/gl.h>

#ifdef _WIN32
// MinGW: windows.h 定义 ERROR 宏与 glog 冲突，必须在 glog 之前 suppress
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"

// glXxx→gl_Xxx 别名宏已集成在 gl_loader.h 中，本文件不再重复定义。

// MinGW 的 GL/gl.h 是 OpenGL 1.1 头，缺少 shader 相关常量，手动补齐。
// 数值与 GL/glext.h 一致。
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_INFO_LOG_LENGTH
#define GL_INFO_LOG_LENGTH 0x8B84
#endif
#endif

#include <glog/logging.h>

namespace jpov {

namespace {

// 取 shader / program 的 info log **全文**。
//
// 为什么不能再用固定 1024 字节缓冲：NVIDIA 的编译器会给每个「可能未初始化」的
// 局部变量单独发一条警告（C7050）—— 本仓库的 tile 光照 shader 一次就能刷十几条，
// 1KB 刚好**被警告填满，真正的 error 行被截掉**。2026-11-01 Windows 真卡上首次
// 出现的链接失败就因此只剩 “Program link error: Fragment info …warning…” ，最关键
// 的失败原因看不见（Mesa/llvmpipe 编译得过，所以本地一直没暴露）。
std::string ShaderInfoLog(unsigned int shader) {
    GLint len = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
    if (len <= 1) {
        return "(empty info log)";
    }
    std::vector<char> buf(static_cast<size_t>(len));
    glGetShaderInfoLog(shader, len, nullptr, buf.data());
    return std::string(buf.data());
}

std::string ProgramInfoLog(unsigned int program) {
    GLint len = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &len);
    if (len <= 1) {
        return "(empty info log)";
    }
    std::vector<char> buf(static_cast<size_t>(len));
    glGetProgramInfoLog(program, len, nullptr, buf.data());
    return std::string(buf.data());
}

// 编译单个 shader（vertex / fragment）。
// 编译失败 → LOG(FATAL) crash。
// name 只用于日志定位（同一个 shader 源可能被多个 program 复用）。
unsigned int CompileShader(GLenum type, const char* source, const std::string& name) {
    CHECK(source != nullptr) << "ShaderManager: null shader source, name=" << name;
    unsigned int shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        const char* tn = (type == GL_VERTEX_SHADER) ? "VS" : "FS";
        LOG(FATAL) << "Shader compile error [" << tn << "] name=" << name << ":\n"
                   << ShaderInfoLog(shader);
    }
    return shader;
}

// 链接 vertex + fragment 为 program。
// 链接失败 → LOG(FATAL) crash，日志带 program 名（不同驱动的报错文本差异很大，
// 带名字才能一眼定位是哪个 shader）。
unsigned int LinkProgram(unsigned int vs, unsigned int fs, const std::string& name) {
    unsigned int prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        LOG(FATAL) << "Program link error name=" << name << ":\n"
                   << ProgramInfoLog(prog);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

// 在 shader 源的 `#version` 行之后插入 `#define <name>` 行。
// GLSL 要求 `#version` 必须是首条语句（本项目的 shader 字串以 `\n#version` 开头，
// 故不能简单地插在第一个换行后），因此定位到 `#version` 所在行的末尾再插。
// defines 为空时原样返回。
std::string InjectDefines(const char* src, const std::vector<std::string>& defines) {
    std::string s(src);
    if (defines.empty()) {
        return s;
    }
    std::string defs;
    for (const std::string& d : defines) {
        defs += "#define " + d + "\n";
    }
    const size_t vp = s.find("#version");
    if (vp == std::string::npos) {
        // 无 #version：放最前（保持可用；正常不会走到）。
        return defs + s;
    }
    const size_t nl = s.find('\n', vp);
    if (nl == std::string::npos) {
        return s + "\n" + defs;
    }
    return s.substr(0, nl + 1) + defs + s.substr(nl + 1);
}

}  // anonymous namespace

ShaderManager::~ShaderManager() {
    for (auto& [name, entry] : programs_) {
        (void)name;
        if (entry.program) {
            glDeleteProgram(entry.program);
        }
    }
    programs_.clear();
}

unsigned int ShaderManager::GetOrCreate(const std::string& name,
                                        const ShaderSource& source) {
    // 已注册 → 直接返回缓存的 program
    auto it = programs_.find(name);
    if (it != programs_.end()) {
        return it->second.program;
    }

    CHECK(source.vertex != nullptr) << "ShaderManager: null vertex source, name=" << name;
    CHECK(source.fragment != nullptr) << "ShaderManager: null fragment source, name=" << name;

    const std::string vs_src = InjectDefines(source.vertex, source.defines);
    const std::string fs_src = InjectDefines(source.fragment, source.defines);
    // 编译前先打一行：「最后一条日志停在哪个 program」是崩溃定位最快的线索
    //（编译/链接之外也可能崩，那时候只有这行能告诉我们是哪个 shader）。
    LOG(INFO) << "ShaderManager: compiling program " << name
              << " (vs=" << vs_src.size() << "B, fs=" << fs_src.size() << "B)";
    unsigned int vs = CompileShader(GL_VERTEX_SHADER, vs_src.c_str(), name);
    unsigned int fs = CompileShader(GL_FRAGMENT_SHADER, fs_src.c_str(), name);
    unsigned int prog = LinkProgram(vs, fs, name);
    CHECK_NE(prog, 0u) << "ShaderManager: link failed, name=" << name;

    ShaderProgram entry;
    entry.program = prog;
    programs_[name] = entry;

    LOG(INFO) << "ShaderManager: compiled program " << name
              << " → " << prog;
    return prog;
}

int ShaderManager::GetUniform(unsigned int program,
                              const std::string& uniform_name) {
    // 定位该 program 的缓存条目（必须已注册）
    ShaderProgram* entry = nullptr;
    for (auto& [name, e] : programs_) {
        (void)name;
        if (e.program == program) {
            entry = &e;
            break;
        }
    }
    CHECK(entry != nullptr) << "ShaderManager: program not registered, program=" << program;

    // 首次查询后缓存
    auto it = entry->uniforms.find(uniform_name);
    if (it != entry->uniforms.end()) {
        return it->second;
    }

    int loc = glGetUniformLocation(program, uniform_name.c_str());
    entry->uniforms[uniform_name] = loc;
    return loc;
}

}  // namespace jpov
