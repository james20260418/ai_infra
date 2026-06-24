#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>

// Need to include GL for type constants
#include <GL/gl.h>

#include "gl_loader.h"

// Helper: load a function pointer from opengl32.dll or wglGetProcAddress
static void* load_gl_func(const char* name) {
    // Try wglGetProcAddress first (handles GL 1.1+ extensions)
    HMODULE hmod = GetModuleHandleA("opengl32.dll");
    if (!hmod) return NULL;

    // Use wglGetProcAddress via GetProcAddress
    typedef PROC (WINAPI *WGL_GETPROCADDRESSPROC)(LPCSTR);
    WGL_GETPROCADDRESSPROC wglGetProcAddr = (WGL_GETPROCADDRESSPROC)
        GetProcAddress(hmod, "wglGetProcAddress");
    if (wglGetProcAddr) {
        PROC addr = wglGetProcAddr(name);
        if (addr) return (void*)addr;
    }

    // Fallback to opengl32.dll exports
    return (void*)GetProcAddress(hmod, name);
}

// Macro to load one function
#define LOAD_FUNC(ptr, name) \
    do { \
        *(void**)(&ptr) = load_gl_func(name); \
        if (!ptr) return -1; \
    } while(0)

int gl_loader_init(void) {
    LOAD_FUNC(gl_GenBuffers,             "glGenBuffers");
    LOAD_FUNC(gl_DeleteBuffers,          "glDeleteBuffers");
    LOAD_FUNC(gl_BindBuffer,             "glBindBuffer");
    LOAD_FUNC(gl_BufferData,             "glBufferData");
    LOAD_FUNC(gl_CreateShader,           "glCreateShader");
    LOAD_FUNC(gl_ShaderSource,           "glShaderSource");
    LOAD_FUNC(gl_CompileShader,          "glCompileShader");
    LOAD_FUNC(gl_GetShaderiv,            "glGetShaderiv");
    LOAD_FUNC(gl_GetShaderInfoLog,       "glGetShaderInfoLog");
    LOAD_FUNC(gl_CreateProgram,          "glCreateProgram");
    LOAD_FUNC(gl_AttachShader,           "glAttachShader");
    LOAD_FUNC(gl_LinkProgram,            "glLinkProgram");
    LOAD_FUNC(gl_GetProgramiv,           "glGetProgramiv");
    LOAD_FUNC(gl_GetProgramInfoLog,      "glGetProgramInfoLog");
    LOAD_FUNC(gl_DeleteShader,           "glDeleteShader");
    LOAD_FUNC(gl_DeleteProgram,          "glDeleteProgram");
    LOAD_FUNC(gl_UseProgram,             "glUseProgram");
    LOAD_FUNC(gl_GetUniformLocation,     "glGetUniformLocation");
    LOAD_FUNC(gl_Uniform2f,              "glUniform2f");
    LOAD_FUNC(gl_Uniform4f,              "glUniform4f");
    LOAD_FUNC(gl_GenFramebuffers,        "glGenFramebuffers");
    LOAD_FUNC(gl_DeleteFramebuffers,     "glDeleteFramebuffers");
    LOAD_FUNC(gl_BindFramebuffer,        "glBindFramebuffer");
    LOAD_FUNC(gl_FramebufferTexture2D,   "glFramebufferTexture2D");
    LOAD_FUNC(gl_CheckFramebufferStatus, "glCheckFramebufferStatus");
    LOAD_FUNC(gl_GenTextures,            "glGenTextures");
    LOAD_FUNC(gl_DeleteTextures,         "glDeleteTextures");
    LOAD_FUNC(gl_BindTexture,            "glBindTexture");
    LOAD_FUNC(gl_TexImage2D,             "glTexImage2D");
    LOAD_FUNC(gl_TexParameteri,          "glTexParameteri");
    LOAD_FUNC(gl_BlitFramebuffer,        "glBlitFramebuffer");
    LOAD_FUNC(gl_EnableVertexAttribArray, "glEnableVertexAttribArray");
    LOAD_FUNC(gl_DisableVertexAttribArray, "glDisableVertexAttribArray");
    LOAD_FUNC(gl_VertexAttribPointer,    "glVertexAttribPointer");
    return 0;
}

// Define storage for function pointers
#define DEFINE_PTR(type, name) type name = NULL
DEFINE_PTR(GL_GenBuffers,             gl_GenBuffers);
DEFINE_PTR(GL_DeleteBuffers,          gl_DeleteBuffers);
DEFINE_PTR(GL_BindBuffer,             gl_BindBuffer);
DEFINE_PTR(GL_BufferData,             gl_BufferData);
DEFINE_PTR(GL_CreateShader,           gl_CreateShader);
DEFINE_PTR(GL_ShaderSource,           gl_ShaderSource);
DEFINE_PTR(GL_CompileShader,          gl_CompileShader);
DEFINE_PTR(GL_GetShaderiv,            gl_GetShaderiv);
DEFINE_PTR(GL_GetShaderInfoLog,       gl_GetShaderInfoLog);
DEFINE_PTR(GL_CreateProgram,          gl_CreateProgram);
DEFINE_PTR(GL_AttachShader,           gl_AttachShader);
DEFINE_PTR(GL_LinkProgram,            gl_LinkProgram);
DEFINE_PTR(GL_GetProgramiv,           gl_GetProgramiv);
DEFINE_PTR(GL_GetProgramInfoLog,      gl_GetProgramInfoLog);
DEFINE_PTR(GL_DeleteShader,           gl_DeleteShader);
DEFINE_PTR(GL_DeleteProgram,          gl_DeleteProgram);
DEFINE_PTR(GL_UseProgram,             gl_UseProgram);
DEFINE_PTR(GL_GetUniformLocation,     gl_GetUniformLocation);
DEFINE_PTR(GL_Uniform2f,              gl_Uniform2f);
DEFINE_PTR(GL_Uniform4f,              gl_Uniform4f);
DEFINE_PTR(GL_GenFramebuffers,        gl_GenFramebuffers);
DEFINE_PTR(GL_DeleteFramebuffers,     gl_DeleteFramebuffers);
DEFINE_PTR(GL_BindFramebuffer,        gl_BindFramebuffer);
DEFINE_PTR(GL_FramebufferTexture2D,   gl_FramebufferTexture2D);
DEFINE_PTR(GL_CheckFramebufferStatus, gl_CheckFramebufferStatus);
DEFINE_PTR(GL_GenTextures,            gl_GenTextures);
DEFINE_PTR(GL_DeleteTextures,         gl_DeleteTextures);
DEFINE_PTR(GL_BindTexture,            gl_BindTexture);
DEFINE_PTR(GL_TexImage2D,             gl_TexImage2D);
DEFINE_PTR(GL_TexParameteri,          gl_TexParameteri);
DEFINE_PTR(GL_BlitFramebuffer,        gl_BlitFramebuffer);
DEFINE_PTR(GL_EnableVertexAttribArray, gl_EnableVertexAttribArray);
DEFINE_PTR(GL_DisableVertexAttribArray, gl_DisableVertexAttribArray);
DEFINE_PTR(GL_VertexAttribPointer,    gl_VertexAttribPointer);
