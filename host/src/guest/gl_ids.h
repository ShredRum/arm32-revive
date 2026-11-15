// The GL imports RealGL::Handle serves, as load-time ids: Import::gl_id is
// set once from the name (GlIdForName), and Handle compares integers instead
// of strings on every call (a GL call is the hottest import: Minecraft: Story
// Mode makes well over 100000 a second, and the old per-call std::string, snprintf and
// name chain cost ~3-4 us each on the sandbox).
//
// One X-macro list: the enum and the name table come from it, so they cannot
// drift apart. Generated from the names RealGL::Handle compared against.
#pragma once

#include <cstdint>
#include <cstring>

namespace abr {

#define ABR_GL_IDS(X) \
    X(glActiveTexture) \
    X(glAttachShader) \
    X(glBindAttribLocation) \
    X(glBindBuffer) \
    X(glBindFramebuffer) \
    X(glBindFramebufferOES) \
    X(glBindRenderbuffer) \
    X(glBindRenderbufferOES) \
    X(glBindTexture) \
    X(glBlendEquation) \
    X(glBlendEquationSeparate) \
    X(glBlendFunc) \
    X(glBlendFuncSeparate) \
    X(glBufferData) \
    X(glBufferSubData) \
    X(glCheckFramebufferStatus) \
    X(glCheckFramebufferStatusOES) \
    X(glClear) \
    X(glClearColor) \
    X(glClearDepthf) \
    X(glClearStencil) \
    X(glClientActiveTexture) \
    X(glColor4f) \
    X(glColorMask) \
    X(glColorPointer) \
    X(glCompileShader) \
    X(glCompressedTexImage2D) \
    X(glCompressedTexSubImage2D) \
    X(glCopyTexImage2D) \
    X(glCopyTexSubImage2D) \
    X(glCreateProgram) \
    X(glCreateShader) \
    X(glCullFace) \
    X(glDeleteBuffers) \
    X(glDeleteFramebuffers) \
    X(glDeleteFramebuffersOES) \
    X(glDeleteProgram) \
    X(glDeleteRenderbuffers) \
    X(glDeleteRenderbuffersOES) \
    X(glDeleteShader) \
    X(glDeleteTextures) \
    X(glDepthFunc) \
    X(glDepthMask) \
    X(glDepthRangef) \
    X(glDetachShader) \
    X(glDisable) \
    X(glDisableClientState) \
    X(glDisableVertexAttribArray) \
    X(glDrawArrays) \
    X(glDrawElements) \
    X(glEnable) \
    X(glEnableClientState) \
    X(glEnableVertexAttribArray) \
    X(glFinish) \
    X(glFlush) \
    X(glFramebufferRenderbuffer) \
    X(glFramebufferRenderbufferOES) \
    X(glFramebufferTexture2D) \
    X(glFramebufferTexture2DOES) \
    X(glFrontFace) \
    X(glGenBuffers) \
    X(glGenerateMipmap) \
    X(glGenFramebuffers) \
    X(glGenFramebuffersOES) \
    X(glGenRenderbuffers) \
    X(glGenRenderbuffersOES) \
    X(glGenTextures) \
    X(glGetActiveAttrib) \
    X(glGetActiveUniform) \
    X(glGetAttribLocation) \
    X(glGetBooleanv) \
    X(glGetBufferParameteriv) \
    X(glGetError) \
    X(glGetFloatv) \
    X(glGetFramebufferAttachmentParameteriv) \
    X(glGetIntegerv) \
    X(glGetProgramInfoLog) \
    X(glGetProgramiv) \
    X(glGetShaderInfoLog) \
    X(glGetShaderiv) \
    X(glGetShaderSource) \
    X(glGetString) \
    X(glGetUniformfv) \
    X(glGetUniformLocation) \
    X(glGetVertexAttribiv) \
    X(glGetVertexAttribPointerv) \
    X(glLightfv) \
    X(glLineWidth) \
    X(glLinkProgram) \
    X(glLoadMatrixf) \
    X(glMaterialfv) \
    X(glMatrixMode) \
    X(glNormalPointer) \
    X(glPixelStorei) \
    X(glPolygonOffset) \
    X(glReadPixels) \
    X(glRenderbufferStorage) \
    X(glRenderbufferStorageOES) \
    X(glScissor) \
    X(glShaderSource) \
    X(glStencilFunc) \
    X(glStencilFuncSeparate) \
    X(glStencilMask) \
    X(glStencilOp) \
    X(glStencilOpSeparate) \
    X(glTexCoordPointer) \
    X(glTexEnvi) \
    X(glTexImage2D) \
    X(glTexParameterf) \
    X(glTexParameteri) \
    X(glTexSubImage2D) \
    X(glUniform1f) \
    X(glUniform1fv) \
    X(glUniform1i) \
    X(glUniform1iv) \
    X(glUniform2f) \
    X(glUniform2fv) \
    X(glUniform2i) \
    X(glUniform2iv) \
    X(glUniform3f) \
    X(glUniform3fv) \
    X(glUniform3i) \
    X(glUniform3iv) \
    X(glUniform4f) \
    X(glUniform4fv) \
    X(glUniform4i) \
    X(glUniform4iv) \
    X(glUniformMatrix2fv) \
    X(glUniformMatrix3fv) \
    X(glUniformMatrix4fv) \
    X(glUseProgram) \
    X(glValidateProgram) \
    X(glVertexAttribPointer) \
    X(glVertexPointer) \
    X(glViewport)

enum class GlId : std::uint16_t {
    None = 0,
#define ABR_GL_ENUM(n) n,
    ABR_GL_IDS(ABR_GL_ENUM)
#undef ABR_GL_ENUM
    Count
};

// The id of a GL import name, or GlId::None for a name RealGL does not serve.
inline GlId GlIdForName(const char* name) {
    static const char* const kNames[] = {
#define ABR_GL_NAME(n) #n,
        ABR_GL_IDS(ABR_GL_NAME)
#undef ABR_GL_NAME
    };
    for (std::size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++) {
        if (std::strcmp(kNames[i], name) == 0) {
            return static_cast<GlId>(i + 1);
        }
    }
    return GlId::None;
}

}  // namespace abr
