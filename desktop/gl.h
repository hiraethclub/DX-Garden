/* OpenGL 3.3 core functions, loaded through SDL at start-up (Windows'
 * opengl32.dll only exports OpenGL 1.1 directly). */
#ifndef DXG_GL_H
#define DXG_GL_H

#include <SDL3/SDL_opengl.h>

#ifndef APIENTRY
#define APIENTRY
#endif

/* name, return type, arguments */
#define DXG_GL_FUNCS(X)                                                                        \
    X(glGetString, const GLubyte *, (GLenum))                                                  \
    X(glGetError, GLenum, (void))                                                              \
    X(glViewport, void, (GLint, GLint, GLsizei, GLsizei))                                      \
    X(glScissor, void, (GLint, GLint, GLsizei, GLsizei))                                       \
    X(glClearColor, void, (GLfloat, GLfloat, GLfloat, GLfloat))                                \
    X(glClear, void, (GLbitfield))                                                             \
    X(glEnable, void, (GLenum))                                                                \
    X(glDisable, void, (GLenum))                                                               \
    X(glBlendFunc, void, (GLenum, GLenum))                                                     \
    X(glBlendFuncSeparate, void, (GLenum, GLenum, GLenum, GLenum))                             \
    X(glPixelStorei, void, (GLenum, GLint))                                                    \
    X(glDrawArrays, void, (GLenum, GLint, GLsizei))                                            \
    X(glGenBuffers, void, (GLsizei, GLuint *))                                                 \
    X(glDeleteBuffers, void, (GLsizei, const GLuint *))                                        \
    X(glBindBuffer, void, (GLenum, GLuint))                                                    \
    X(glBufferData, void, (GLenum, GLsizeiptr, const void *, GLenum))                          \
    X(glBufferSubData, void, (GLenum, GLintptr, GLsizeiptr, const void *))                     \
    X(glGenVertexArrays, void, (GLsizei, GLuint *))                                            \
    X(glDeleteVertexArrays, void, (GLsizei, const GLuint *))                                   \
    X(glBindVertexArray, void, (GLuint))                                                       \
    X(glVertexAttribPointer, void, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))  \
    X(glEnableVertexAttribArray, void, (GLuint))                                               \
    X(glCreateShader, GLuint, (GLenum))                                                        \
    X(glDeleteShader, void, (GLuint))                                                          \
    X(glShaderSource, void, (GLuint, GLsizei, const GLchar *const *, const GLint *))           \
    X(glCompileShader, void, (GLuint))                                                         \
    X(glGetShaderiv, void, (GLuint, GLenum, GLint *))                                          \
    X(glGetShaderInfoLog, void, (GLuint, GLsizei, GLsizei *, GLchar *))                        \
    X(glCreateProgram, GLuint, (void))                                                         \
    X(glDeleteProgram, void, (GLuint))                                                         \
    X(glAttachShader, void, (GLuint, GLuint))                                                  \
    X(glLinkProgram, void, (GLuint))                                                           \
    X(glGetProgramiv, void, (GLuint, GLenum, GLint *))                                         \
    X(glGetProgramInfoLog, void, (GLuint, GLsizei, GLsizei *, GLchar *))                       \
    X(glUseProgram, void, (GLuint))                                                            \
    X(glGetUniformLocation, GLint, (GLuint, const GLchar *))                                   \
    X(glUniform1i, void, (GLint, GLint))                                                       \
    X(glUniform2f, void, (GLint, GLfloat, GLfloat))                                            \
    X(glGenTextures, void, (GLsizei, GLuint *))                                                \
    X(glDeleteTextures, void, (GLsizei, const GLuint *))                                       \
    X(glBindTexture, void, (GLenum, GLuint))                                                   \
    X(glActiveTexture, void, (GLenum))                                                         \
    X(glTexImage2D, void,                                                                      \
      (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))           \
    X(glTexSubImage2D, void,                                                                   \
      (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *))           \
    X(glTexParameteri, void, (GLenum, GLenum, GLint))                                          \
    X(glGenFramebuffers, void, (GLsizei, GLuint *))                                            \
    X(glDeleteFramebuffers, void, (GLsizei, const GLuint *))                                   \
    X(glBindFramebuffer, void, (GLenum, GLuint))                                               \
    X(glFramebufferTexture2D, void, (GLenum, GLenum, GLenum, GLuint, GLint))                   \
    X(glCheckFramebufferStatus, GLenum, (GLenum))                                              \
    X(glBlitFramebuffer, void,                                                                 \
      (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))           \
    X(glUniform1f, void, (GLint, GLfloat))                                                     \
    X(glUniform3f, void, (GLint, GLfloat, GLfloat, GLfloat))                                   \
    X(glUniform4f, void, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))                          \
    X(glUniformMatrix4fv, void, (GLint, GLsizei, GLboolean, const GLfloat *))                  \
    X(glCullFace, void, (GLenum))                                                              \
    X(glFrontFace, void, (GLenum))                                                             \
    X(glGenerateMipmap, void, (GLenum))                                                        \
    X(glTexParameterf, void, (GLenum, GLenum, GLfloat))                                        \
    X(glReadPixels, void, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))            \
    X(glDepthMask, void, (GLboolean))                                                          \
    X(glDrawElements, void, (GLenum, GLsizei, GLenum, const void *))

#define DXG_GL_DECLARE(name, ret, args) extern ret(APIENTRY *dxg_##name) args;
DXG_GL_FUNCS(DXG_GL_DECLARE)
#undef DXG_GL_DECLARE

/* The rest of desktop/ calls them by their usual names. */
#ifndef DXG_GL_NO_ALIASES
#define glGetString dxg_glGetString
#define glGetError dxg_glGetError
#define glViewport dxg_glViewport
#define glScissor dxg_glScissor
#define glClearColor dxg_glClearColor
#define glClear dxg_glClear
#define glEnable dxg_glEnable
#define glDisable dxg_glDisable
#define glBlendFunc dxg_glBlendFunc
#define glBlendFuncSeparate dxg_glBlendFuncSeparate
#define glPixelStorei dxg_glPixelStorei
#define glDrawArrays dxg_glDrawArrays
#define glGenBuffers dxg_glGenBuffers
#define glDeleteBuffers dxg_glDeleteBuffers
#define glBindBuffer dxg_glBindBuffer
#define glBufferData dxg_glBufferData
#define glBufferSubData dxg_glBufferSubData
#define glGenVertexArrays dxg_glGenVertexArrays
#define glDeleteVertexArrays dxg_glDeleteVertexArrays
#define glBindVertexArray dxg_glBindVertexArray
#define glVertexAttribPointer dxg_glVertexAttribPointer
#define glEnableVertexAttribArray dxg_glEnableVertexAttribArray
#define glCreateShader dxg_glCreateShader
#define glDeleteShader dxg_glDeleteShader
#define glShaderSource dxg_glShaderSource
#define glCompileShader dxg_glCompileShader
#define glGetShaderiv dxg_glGetShaderiv
#define glGetShaderInfoLog dxg_glGetShaderInfoLog
#define glCreateProgram dxg_glCreateProgram
#define glDeleteProgram dxg_glDeleteProgram
#define glAttachShader dxg_glAttachShader
#define glLinkProgram dxg_glLinkProgram
#define glGetProgramiv dxg_glGetProgramiv
#define glGetProgramInfoLog dxg_glGetProgramInfoLog
#define glUseProgram dxg_glUseProgram
#define glGetUniformLocation dxg_glGetUniformLocation
#define glUniform1i dxg_glUniform1i
#define glUniform2f dxg_glUniform2f
#define glGenTextures dxg_glGenTextures
#define glDeleteTextures dxg_glDeleteTextures
#define glBindTexture dxg_glBindTexture
#define glActiveTexture dxg_glActiveTexture
#define glTexImage2D dxg_glTexImage2D
#define glTexSubImage2D dxg_glTexSubImage2D
#define glTexParameteri dxg_glTexParameteri
#define glGenFramebuffers dxg_glGenFramebuffers
#define glDeleteFramebuffers dxg_glDeleteFramebuffers
#define glBindFramebuffer dxg_glBindFramebuffer
#define glFramebufferTexture2D dxg_glFramebufferTexture2D
#define glCheckFramebufferStatus dxg_glCheckFramebufferStatus
#define glBlitFramebuffer dxg_glBlitFramebuffer
#define glUniform1f dxg_glUniform1f
#define glUniform3f dxg_glUniform3f
#define glUniform4f dxg_glUniform4f
#define glUniformMatrix4fv dxg_glUniformMatrix4fv
#define glCullFace dxg_glCullFace
#define glFrontFace dxg_glFrontFace
#define glGenerateMipmap dxg_glGenerateMipmap
#define glTexParameterf dxg_glTexParameterf
#define glReadPixels dxg_glReadPixels
#define glDepthMask dxg_glDepthMask
#define glDrawElements dxg_glDrawElements
#endif

/* Loads every function above from the current context. Returns 0, or -1
 * with the first missing name in *missing. */
int gl_load(const char **missing);

#endif
