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
typedef void (*GL_GenTextures)(int, unsigned int*);
typedef void (*GL_DeleteTextures)(int, const unsigned int*);
typedef void (*GL_BindTexture)(unsigned int, unsigned int);
typedef void (*GL_TexImage2D)(unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void*);
typedef void (*GL_TexParameteri)(unsigned int, unsigned int, int);
typedef void (*GL_BlitFramebuffer)(int, int, int, int, int, int, int, int, unsigned int, unsigned int);
typedef void (*GL_EnableVertexAttribArray)(unsigned int);
typedef void (*GL_DisableVertexAttribArray)(unsigned int);
typedef void (*GL_VertexAttribPointer)(unsigned int, int, unsigned int, unsigned char, int, const void*);

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
extern GL_GenTextures            gl_GenTextures;
extern GL_DeleteTextures         gl_DeleteTextures;
extern GL_BindTexture            gl_BindTexture;
extern GL_TexImage2D             gl_TexImage2D;
extern GL_TexParameteri          gl_TexParameteri;
extern GL_BlitFramebuffer        gl_BlitFramebuffer;
extern GL_EnableVertexAttribArray gl_EnableVertexAttribArray;
extern GL_DisableVertexAttribArray gl_DisableVertexAttribArray;
extern GL_VertexAttribPointer    gl_VertexAttribPointer;

// Load all function pointers. Call once after GL context creation.
int gl_loader_init(void);

#ifdef __cplusplus
}
#endif

#endif // GL_LOADER_H_
