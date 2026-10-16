#include "gl_loader.hpp"
#include <cstdio>
#include <cstring>
GLRDPPrecisionQuery glrdp_precision_query = nullptr;
GLRDPTextureBarrier glrdp_texture_barrier = nullptr;
decltype(glad_glActiveTexture) glad_glActiveTexture = nullptr;
decltype(glad_glAttachShader) glad_glAttachShader = nullptr;
decltype(glad_glBindAttribLocation) glad_glBindAttribLocation = nullptr;
decltype(glad_glBindBuffer) glad_glBindBuffer = nullptr;
decltype(glad_glBindFramebuffer) glad_glBindFramebuffer = nullptr;
decltype(glad_glBindSampler) glad_glBindSampler = nullptr;
decltype(glad_glBindTexture) glad_glBindTexture = nullptr;
decltype(glad_glBindVertexArray) glad_glBindVertexArray = nullptr;
decltype(glad_glBlendColor) glad_glBlendColor = nullptr;
decltype(glad_glBlendEquationSeparate) glad_glBlendEquationSeparate = nullptr;
decltype(glad_glBlendFuncSeparate) glad_glBlendFuncSeparate = nullptr;
decltype(glad_glBlitFramebuffer) glad_glBlitFramebuffer = nullptr;
decltype(glad_glBufferData) glad_glBufferData = nullptr;
decltype(glad_glCheckFramebufferStatus) glad_glCheckFramebufferStatus = nullptr;
decltype(glad_glClear) glad_glClear = nullptr;
decltype(glad_glClearBufferuiv) glad_glClearBufferuiv = nullptr;
decltype(glad_glClearColor) glad_glClearColor = nullptr;
decltype(glad_glClearDepth) glad_glClearDepth = nullptr;
decltype(glad_glColorMask) glad_glColorMask = nullptr;
decltype(glad_glCompileShader) glad_glCompileShader = nullptr;
decltype(glad_glCopyTexSubImage2D) glad_glCopyTexSubImage2D = nullptr;
decltype(glad_glCreateProgram) glad_glCreateProgram = nullptr;
decltype(glad_glCreateShader) glad_glCreateShader = nullptr;
decltype(glad_glDeleteBuffers) glad_glDeleteBuffers = nullptr;
decltype(glad_glDeleteFramebuffers) glad_glDeleteFramebuffers = nullptr;
decltype(glad_glDeleteProgram) glad_glDeleteProgram = nullptr;
decltype(glad_glDeleteShader) glad_glDeleteShader = nullptr;
decltype(glad_glDeleteTextures) glad_glDeleteTextures = nullptr;
decltype(glad_glDeleteVertexArrays) glad_glDeleteVertexArrays = nullptr;
decltype(glad_glDepthFunc) glad_glDepthFunc = nullptr;
decltype(glad_glDepthMask) glad_glDepthMask = nullptr;
decltype(glad_glDisable) glad_glDisable = nullptr;
decltype(glad_glDisableVertexAttribArray) glad_glDisableVertexAttribArray = nullptr;
decltype(glad_glDrawArrays) glad_glDrawArrays = nullptr;
decltype(glad_glDrawBuffer) glad_glDrawBuffer = nullptr;
decltype(glad_glDrawBuffers) glad_glDrawBuffers = nullptr;
decltype(glad_glEnable) glad_glEnable = nullptr;
decltype(glad_glEnableVertexAttribArray) glad_glEnableVertexAttribArray = nullptr;
decltype(glad_glFramebufferTexture2D) glad_glFramebufferTexture2D = nullptr;
decltype(glad_glGenBuffers) glad_glGenBuffers = nullptr;
decltype(glad_glGenFramebuffers) glad_glGenFramebuffers = nullptr;
decltype(glad_glGenTextures) glad_glGenTextures = nullptr;
decltype(glad_glGenVertexArrays) glad_glGenVertexArrays = nullptr;
decltype(glad_glGetBooleanv) glad_glGetBooleanv = nullptr;
decltype(glad_glGetFloatv) glad_glGetFloatv = nullptr;
decltype(glad_glGetIntegerv) glad_glGetIntegerv = nullptr;
decltype(glad_glGetProgramInfoLog) glad_glGetProgramInfoLog = nullptr;
decltype(glad_glGetProgramiv) glad_glGetProgramiv = nullptr;
decltype(glad_glGetShaderInfoLog) glad_glGetShaderInfoLog = nullptr;
decltype(glad_glGetShaderiv) glad_glGetShaderiv = nullptr;
decltype(glad_glGetString) glad_glGetString = nullptr;
decltype(glad_glGetUniformLocation) glad_glGetUniformLocation = nullptr;
decltype(glad_glGetVertexAttribPointerv) glad_glGetVertexAttribPointerv = nullptr;
decltype(glad_glGetVertexAttribiv) glad_glGetVertexAttribiv = nullptr;
decltype(glad_glIsEnabled) glad_glIsEnabled = nullptr;
decltype(glad_glLinkProgram) glad_glLinkProgram = nullptr;
decltype(glad_glPixelStorei) glad_glPixelStorei = nullptr;
decltype(glad_glPolygonMode) glad_glPolygonMode = nullptr;
decltype(glad_glReadBuffer) glad_glReadBuffer = nullptr;
decltype(glad_glReadPixels) glad_glReadPixels = nullptr;
decltype(glad_glScissor) glad_glScissor = nullptr;
decltype(glad_glShaderSource) glad_glShaderSource = nullptr;
decltype(glad_glTexImage2D) glad_glTexImage2D = nullptr;
decltype(glad_glTexImage2DMultisample) glad_glTexImage2DMultisample = nullptr;
decltype(glad_glTexParameteri) glad_glTexParameteri = nullptr;
decltype(glad_glTexSubImage2D) glad_glTexSubImage2D = nullptr;
decltype(glad_glUniform1f) glad_glUniform1f = nullptr;
decltype(glad_glUniform1i) glad_glUniform1i = nullptr;
decltype(glad_glUniform1ui) glad_glUniform1ui = nullptr;
decltype(glad_glUniform2f) glad_glUniform2f = nullptr;
decltype(glad_glUniform4f) glad_glUniform4f = nullptr;
decltype(glad_glUniform4i) glad_glUniform4i = nullptr;
decltype(glad_glUseProgram) glad_glUseProgram = nullptr;
decltype(glad_glVertexAttribPointer) glad_glVertexAttribPointer = nullptr;
decltype(glad_glViewport) glad_glViewport = nullptr;

#define LOAD(name)                                                                                 \
    glad_##name = reinterpret_cast<decltype(glad_##name)>(resolver(#name));                        \
    if (!glad_##name)                                                                              \
    return false
bool load_gl33(retro_hw_get_proc_address_t resolver, bool gles2) {
    if (!resolver)
        return false;
    LOAD(glGetString);
    const char *version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
    if (!version)
        return false;
    const bool is_es = std::strncmp(version, "OpenGL ES ", 10) == 0;
    int major = 0, minor = 0;
    if (std::sscanf(version + (is_es ? 10 : 0), "%d.%d", &major, &minor) != 2)
        return false;
    if (gles2 ? (!is_es || major < 2) : (is_es || major < 3 || (major == 3 && minor < 3)))
        return false;
    LOAD(glActiveTexture);
    LOAD(glAttachShader);
    LOAD(glBindAttribLocation);
    LOAD(glBindBuffer);
    LOAD(glBindFramebuffer);
    LOAD(glBindTexture);
    LOAD(glBufferData);
    LOAD(glColorMask);
    LOAD(glCompileShader);
    LOAD(glCreateProgram);
    LOAD(glCreateShader);
    LOAD(glDeleteBuffers);
    LOAD(glDeleteProgram);
    LOAD(glDeleteShader);
    LOAD(glDeleteTextures);
    LOAD(glDisable);
    LOAD(glDisableVertexAttribArray);
    LOAD(glDrawArrays);
    LOAD(glEnable);
    LOAD(glEnableVertexAttribArray);
    LOAD(glGenBuffers);
    LOAD(glGenTextures);
    LOAD(glGetBooleanv);
    LOAD(glGetIntegerv);
    LOAD(glGetProgramiv);
    LOAD(glGetShaderInfoLog);
    LOAD(glGetShaderiv);
    LOAD(glGetUniformLocation);
    LOAD(glGetVertexAttribPointerv);
    LOAD(glGetVertexAttribiv);
    LOAD(glIsEnabled);
    LOAD(glLinkProgram);
    LOAD(glPixelStorei);
    LOAD(glShaderSource);
    LOAD(glTexImage2D);
    LOAD(glTexParameteri);
    LOAD(glTexSubImage2D);
    LOAD(glUniform1i);
    LOAD(glUseProgram);
    LOAD(glVertexAttribPointer);
    LOAD(glViewport);
    LOAD(glCheckFramebufferStatus);
    LOAD(glClear);
    LOAD(glClearColor);
    LOAD(glCopyTexSubImage2D);
    LOAD(glDeleteFramebuffers);
    LOAD(glFramebufferTexture2D);
    LOAD(glGenFramebuffers);
    LOAD(glGetFloatv);
    LOAD(glGetProgramInfoLog);
    LOAD(glReadPixels);
    LOAD(glScissor);
    LOAD(glUniform1f);
    LOAD(glUniform2f);
    LOAD(glUniform4f);
    glrdp_precision_query =
        gles2 ? reinterpret_cast<GLRDPPrecisionQuery>(resolver("glGetShaderPrecisionFormat"))
              : nullptr;
    if (gles2 && !glrdp_precision_query)
        return false;
    if (!gles2) {
        // Optional on GL3.3. A non-null procedure alone does not imply support.
        glrdp_texture_barrier = nullptr;
        auto get_string = reinterpret_cast<PFNGLGETSTRINGIPROC>(resolver("glGetStringi"));
        GLint count = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &count);
        for (GLint i = 0; get_string && i < count; ++i) {
            const auto *extension = reinterpret_cast<const char *>(get_string(GL_EXTENSIONS, i));
            const char *procedure =
                !std::strcmp(extension, "GL_ARB_texture_barrier")  ? "glTextureBarrier"
                : !std::strcmp(extension, "GL_NV_texture_barrier") ? "glTextureBarrierNV"
                                                                   : nullptr;
            if (procedure) {
                glrdp_texture_barrier = reinterpret_cast<GLRDPTextureBarrier>(resolver(procedure));
                if (glrdp_texture_barrier)
                    break;
            }
        }
        LOAD(glBindSampler);
        LOAD(glBindVertexArray);
        LOAD(glBlendColor);
        LOAD(glBlendEquationSeparate);
        LOAD(glBlendFuncSeparate);
        LOAD(glBlitFramebuffer);
        LOAD(glClearBufferuiv);
        LOAD(glClearDepth);
        LOAD(glDeleteVertexArrays);
        LOAD(glDepthFunc);
        LOAD(glDepthMask);
        LOAD(glDrawBuffer);
        LOAD(glDrawBuffers);
        LOAD(glGenVertexArrays);
        LOAD(glPolygonMode);
        LOAD(glReadBuffer);
        LOAD(glUniform1ui);
        LOAD(glUniform4i);
    } else
        glrdp_texture_barrier = nullptr;
    return true;
}
#undef LOAD
