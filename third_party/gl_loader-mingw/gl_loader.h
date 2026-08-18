#ifndef GL_LOADER_H_
#define GL_LOADER_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// GL function pointer typedefs
typedef void (*GL_GenBuffers)(int, unsigned int*);
typedef void (*GL_DeleteBuffers)(int, const unsigned int*);
typedef void (*GL_BindBuffer)(unsigned int, unsigned int);
typedef void (*GL_BufferData)(unsigned int, int, const void*, unsigned int);
typedef unsigned int (*GL_CreateShader)(unsigned int);
typedef void (*GL_ShaderSource)(unsigned int, int, const char* const*, const int*);
typedef void (*GL_CompileShader)(unsigned int);
typedef void (*GL_GetShaderiv)(unsigned int, unsigned int, int*);
typedef void (*GL_GetShaderInfoLog)(unsigned int, int, int*, char*);
typedef unsigned int (*GL_CreateProgram)(void);
typedef void (*GL_AttachShader)(unsigned int, unsigned int);
typedef void (*GL_LinkProgram)(unsigned int);
typedef void (*GL_GetProgramiv)(unsigned int, unsigned int, int*);
typedef void (*GL_GetProgramInfoLog)(unsigned int, int, int*, char*);
typedef void (*GL_DeleteShader)(unsigned int);
typedef void (*GL_DeleteProgram)(unsigned int);
typedef void (*GL_UseProgram)(unsigned int);
typedef int (*GL_GetUniformLocation)(unsigned int, const char*);
typedef void (*GL_Uniform2f)(int, float, float);
typedef void (*GL_Uniform4f)(int, float, float, float, float);
typedef void (*GL_GenFramebuffers)(int, unsigned int*);
typedef void (*GL_DeleteFramebuffers)(int, const unsigned int*);
typedef void (*GL_BindFramebuffer)(unsigned int, unsigned int);
typedef void (*GL_FramebufferTexture2D)(unsigned int, unsigned int, unsigned int, unsigned int, int);
typedef unsigned int (*GL_CheckFramebufferStatus)(unsigned int);
typedef void (*GL_GenRenderbuffers)(int, unsigned int*);
typedef void (*GL_DeleteRenderbuffers)(int, const unsigned int*);
typedef void (*GL_BindRenderbuffer)(unsigned int, unsigned int);
typedef void (*GL_RenderbufferStorage)(unsigned int, unsigned int, int, int);
typedef void (*GL_FramebufferRenderbuffer)(unsigned int, unsigned int, unsigned int, unsigned int);
typedef void (*GL_GenTextures)(int, unsigned int*);
typedef void (*GL_DeleteTextures)(int, const unsigned int*);
typedef void (*GL_BindTexture)(unsigned int, unsigned int);
typedef void (*GL_TexImage2D)(unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void*);
typedef void (*GL_TexParameteri)(unsigned int, unsigned int, int);
typedef void (*GL_BlitFramebuffer)(int, int, int, int, int, int, int, int, unsigned int, unsigned int);
typedef void (*GL_EnableVertexAttribArray)(unsigned int);
typedef void (*GL_DisableVertexAttribArray)(unsigned int);
typedef void (*GL_VertexAttribPointer)(unsigned int, int, unsigned int, unsigned char, int, const void*);
typedef void (*GL_VertexAttribIPointer)(unsigned int, int, unsigned int, int, const void*);
typedef void (*GL_Uniform1i)(int, int);
typedef void (*GL_Uniform1f)(int, float);
typedef void (*GL_Uniform1fv)(int, int, const float*);
typedef void (*GL_Uniform3f)(int, float, float, float);
typedef void (*GL_ActiveTexture)(unsigned int);
typedef void (*GL_UniformMatrix4fv)(int, int, unsigned char, const float*);
typedef void (*GL_GenVertexArrays)(int, unsigned int*);
typedef void (*GL_DeleteVertexArrays)(int, const unsigned int*);
typedef void (*GL_BindVertexArray)(unsigned int);
typedef void (*GL_DrawArrays)(unsigned int, int, int);
typedef void (*GL_TexSubImage2D)(unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void*);
typedef unsigned int (*GL_GetError)(void);
typedef void (*GL_PushAttrib)(unsigned int);
typedef void (*GL_PopAttrib)(void);

// Loaded function pointers
extern GL_GenBuffers             gl_GenBuffers;
extern GL_DeleteBuffers          gl_DeleteBuffers;
extern GL_BindBuffer             gl_BindBuffer;
extern GL_BufferData             gl_BufferData;
extern GL_CreateShader           gl_CreateShader;
extern GL_ShaderSource           gl_ShaderSource;
extern GL_CompileShader          gl_CompileShader;
extern GL_GetShaderiv            gl_GetShaderiv;
extern GL_GetShaderInfoLog       gl_GetShaderInfoLog;
extern GL_CreateProgram          gl_CreateProgram;
extern GL_AttachShader           gl_AttachShader;
extern GL_LinkProgram            gl_LinkProgram;
extern GL_GetProgramiv           gl_GetProgramiv;
extern GL_GetProgramInfoLog      gl_GetProgramInfoLog;
extern GL_DeleteShader           gl_DeleteShader;
extern GL_DeleteProgram          gl_DeleteProgram;
extern GL_UseProgram             gl_UseProgram;
extern GL_GetUniformLocation     gl_GetUniformLocation;
extern GL_Uniform2f              gl_Uniform2f;
extern GL_Uniform4f              gl_Uniform4f;
extern GL_GenFramebuffers        gl_GenFramebuffers;
extern GL_DeleteFramebuffers     gl_DeleteFramebuffers;
extern GL_BindFramebuffer        gl_BindFramebuffer;
extern GL_FramebufferTexture2D   gl_FramebufferTexture2D;
extern GL_CheckFramebufferStatus gl_CheckFramebufferStatus;
extern GL_GenRenderbuffers        gl_GenRenderbuffers;
extern GL_DeleteRenderbuffers     gl_DeleteRenderbuffers;
extern GL_BindRenderbuffer        gl_BindRenderbuffer;
extern GL_RenderbufferStorage     gl_RenderbufferStorage;
extern GL_FramebufferRenderbuffer gl_FramebufferRenderbuffer;
extern GL_GenTextures            gl_GenTextures;
extern GL_DeleteTextures         gl_DeleteTextures;
extern GL_BindTexture            gl_BindTexture;
extern GL_TexImage2D             gl_TexImage2D;
extern GL_TexParameteri          gl_TexParameteri;
extern GL_BlitFramebuffer        gl_BlitFramebuffer;
extern GL_EnableVertexAttribArray gl_EnableVertexAttribArray;
extern GL_DisableVertexAttribArray gl_DisableVertexAttribArray;
extern GL_VertexAttribPointer    gl_VertexAttribPointer;
extern GL_VertexAttribIPointer   gl_VertexAttribIPointer;
extern GL_Uniform1i              gl_Uniform1i;
extern GL_Uniform1f              gl_Uniform1f;
extern GL_Uniform1fv             gl_Uniform1fv;
extern GL_Uniform3f              gl_Uniform3f;
extern GL_ActiveTexture          gl_ActiveTexture;
extern GL_UniformMatrix4fv       gl_UniformMatrix4fv;
extern GL_GenVertexArrays        gl_GenVertexArrays;
extern GL_DeleteVertexArrays     gl_DeleteVertexArrays;
extern GL_BindVertexArray        gl_BindVertexArray;
extern GL_DrawArrays            gl_DrawArrays;
extern GL_TexSubImage2D         gl_TexSubImage2D;
extern GL_GetError              gl_GetError;
extern GL_PushAttrib            gl_PushAttrib;
extern GL_PopAttrib             gl_PopAttrib;

// Load all function pointers. Call once after GL context creation.
int gl_loader_init(void);

#ifdef __cplusplus
}
#endif

// ==================== MinGW GL 函数名别名 ====================
//
// 注意：这些 #define 必须在每个 .cc 文件 include <GL/gl.h> *之后*
// include 本头文件时才生效。头文件自身不 include GL/gl.h，
// 由调用方控制 include 顺序：
//
//   #include <GL/gl.h>               // 1. GL 常量声明
//   #include "gl_loader.h"           // 2. 函数指针 + 别名宏
//
// 含义：glXxx 函数名 → gl_Xxx 运行时加载的函数指针。
// 这组宏以往分散在 mesh_manager.cc / renderer.cc / shader_manager.cc，
// 现集中到 gl_loader.h，一处定义、多处可用。
#ifdef _WIN32

#define glActiveTexture          gl_ActiveTexture
#define glAttachShader           gl_AttachShader
#define glBindBuffer             gl_BindBuffer
#define glBindFramebuffer        gl_BindFramebuffer
#define glBindTexture            gl_BindTexture
#define glBindVertexArray        gl_BindVertexArray
#define glDrawArrays             gl_DrawArrays
#define glTexSubImage2D          gl_TexSubImage2D
#define glGetError               gl_GetError
#define glPushAttrib             gl_PushAttrib
#define glPopAttrib              gl_PopAttrib
#define glBlitFramebuffer        gl_BlitFramebuffer
#define glBufferData             gl_BufferData
#define glCheckFramebufferStatus gl_CheckFramebufferStatus
#define glGenRenderbuffers        gl_GenRenderbuffers
#define glDeleteRenderbuffers     gl_DeleteRenderbuffers
#define glBindRenderbuffer        gl_BindRenderbuffer
#define glRenderbufferStorage     gl_RenderbufferStorage
#define glFramebufferRenderbuffer gl_FramebufferRenderbuffer
#define glCompileShader          gl_CompileShader
#define glCreateProgram          gl_CreateProgram
#define glCreateShader           gl_CreateShader
#define glDeleteBuffers          gl_DeleteBuffers
#define glDeleteFramebuffers     gl_DeleteFramebuffers
#define glDeleteProgram          gl_DeleteProgram
#define glDeleteShader           gl_DeleteShader
#define glDeleteTextures         gl_DeleteTextures
#define glDeleteVertexArrays     gl_DeleteVertexArrays
#define glDisableVertexAttribArray gl_DisableVertexAttribArray
#define glEnableVertexAttribArray  gl_EnableVertexAttribArray
#define glFramebufferTexture2D   gl_FramebufferTexture2D
#define glGenBuffers             gl_GenBuffers
#define glGenFramebuffers        gl_GenFramebuffers
#define glGenTextures            gl_GenTextures
#define glGenVertexArrays        gl_GenVertexArrays
#define glGetProgramInfoLog      gl_GetProgramInfoLog
#define glGetProgramiv           gl_GetProgramiv
#define glGetShaderInfoLog       gl_GetShaderInfoLog
#define glGetShaderiv            gl_GetShaderiv
#define glGetUniformLocation     gl_GetUniformLocation
#define glLinkProgram            gl_LinkProgram
#define glShaderSource           gl_ShaderSource
#define glTexImage2D             gl_TexImage2D
#define glTexParameteri          gl_TexParameteri
#define glUniform1i              gl_Uniform1i
#define glUniform1f              gl_Uniform1f
#define glUniform1fv             gl_Uniform1fv
#define glUniform3f              gl_Uniform3f
#define glUniform2f              gl_Uniform2f
#define glUniform4f              gl_Uniform4f
#define glUniformMatrix4fv       gl_UniformMatrix4fv
#define glUseProgram             gl_UseProgram
#define glVertexAttribIPointer   gl_VertexAttribIPointer
#define glVertexAttribPointer    gl_VertexAttribPointer

// ==================== 缺失 GL 常量 ====================
// MinGW 的 <GL/gl.h> 可能缺少以下 GL 1.1+ 常量（视具体 MinGW 版本而定）。
// 这些 define 仅在常量未定义时补值，避免与系统头文件冲突。
#ifndef GL_R8
#define GL_R8 0x8229
#endif
#ifndef GL_RED
#define GL_RED 0x1903
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24 0x81A6
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_DYNAMIC_DRAW
#define GL_DYNAMIC_DRAW 0x88E8
#endif
#ifndef GL_COLOR_BUFFER_BIT
#define GL_COLOR_BUFFER_BIT 0x00004000
#endif
#ifndef GLsizeiptr
#define GLsizeiptr ptrdiff_t
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_TEXTURE1
#define GL_TEXTURE1 0x84C1
#endif
#ifndef GL_TEXTURE2
#define GL_TEXTURE2 0x84C2
#endif
#ifndef GL_TEXTURE3
#define GL_TEXTURE3 0x84C3
#endif
#ifndef GL_TEXTURE4
#define GL_TEXTURE4 0x84C4
#endif
#ifndef GL_TEXTURE5
#define GL_TEXTURE5 0x84C5
#endif
#ifndef GL_TEXTURE6
#define GL_TEXTURE6 0x84C6
#endif
#ifndef GL_TEXTURE7
#define GL_TEXTURE7 0x84C7
#endif
#ifndef GL_TEXTURE8
#define GL_TEXTURE8 0x84C8
#endif
#ifndef GL_TEXTURE9
#define GL_TEXTURE9 0x84C9
#endif
#ifndef GL_TEXTURE10
#define GL_TEXTURE10 0x84CA
#endif
#ifndef GL_TEXTURE11
#define GL_TEXTURE11 0x84CB
#endif
#ifndef GL_TEXTURE12
#define GL_TEXTURE12 0x84CC
#endif

#endif  // _WIN32

#endif // GL_LOADER_H_
