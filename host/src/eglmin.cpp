// eglmin: absolute minimal clear+readback probe.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <cstdio>

int main() {
    EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY,
                                          nullptr);
    if (d == EGL_NO_DISPLAY) {
        d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    if (!eglInitialize(d, nullptr, nullptr)) {
        std::printf("init fail\n");
        return 1;
    }
    const EGLint cfg[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                          EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                          EGL_NONE};
    EGLConfig c = nullptr;
    EGLint nc = 0;
    eglChooseConfig(d, cfg, &c, 1, &nc);
    std::printf("configs=%d eglerr=%04x\n", nc, eglGetError());
    const EGLint ctxa[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext ctx = eglCreateContext(d, c, EGL_NO_CONTEXT, ctxa);
    const EGLint pb[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(d, c, pb);
    std::printf("ctx=%p surf=%p\n", (void*)ctx, (void*)s);
    if (!eglMakeCurrent(d, s, s, ctx)) {
        std::printf("current fail %04x\n", eglGetError());
        return 1;
    }
    GLint vp[4] = {-1, -1, -1, -1};
    glGetIntegerv(GL_VIEWPORT, vp);
    std::printf("viewport=%d,%d,%d,%d\n", vp[0], vp[1], vp[2], vp[3]);
    // Bisect: trivial shader compile before clear (gltest does this).
    {
        const char* vsrc = "attribute vec4 p; void main(){ gl_Position = p; }";
        const char* fsrc = "void main(){ gl_FragColor = vec4(1.0); }";
        GLuint vs = glCreateShader(GL_VERTEX_SHADER);
        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(vs, 1, &vsrc, nullptr);
        glShaderSource(fs, 1, &fsrc, nullptr);
        glCompileShader(vs);
        glCompileShader(fs);
        GLint ok1 = 0, ok2 = 0;
        glGetShaderiv(vs, GL_COMPILE_STATUS, &ok1);
        glGetShaderiv(fs, GL_COMPILE_STATUS, &ok2);
        std::printf("trivial shaders: %d %d\n", ok1, ok2);
    }
    GLboolean m[4] = {0};
    glGetBooleanv(GL_COLOR_WRITEMASK, m);
    std::printf("colormask=%d%d%d%d scissor=%d\n", m[0], m[1], m[2], m[3],
                glIsEnabled(GL_SCISSOR_TEST));
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    std::printf("clear err=%04x\n", glGetError());
    unsigned char px[4] = {0};
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    std::printf("read err=%04x px=%u %u %u %u\n", glGetError(), px[0], px[1], px[2], px[3]);
    // Step 2: RGB (not RGBA) read.
    unsigned char px3[3] = {0};
    glReadPixels(32, 32, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, px3);
    std::printf("rgb-read err=%04x px=%u %u %u\n", glGetError(), px3[0], px3[1], px3[2]);
    // Step 3: FBO render + read.
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    std::printf("fbo status=%04x err=%04x\n", glCheckFramebufferStatus(GL_FRAMEBUFFER),
                glGetError());
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    unsigned char pxf[4] = {0};
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pxf);
    std::printf("fbo-read err=%04x px=%u %u %u %u\n", glGetError(), pxf[0], pxf[1], pxf[2], pxf[3]);
    // Step 4: blit default->FBO then read (gltest's failing op).
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glClearColor(0, 0, 1, 1);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClearColor(1, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glBlitFramebuffer(0, 0, 64, 64, 0, 0, 64, 64, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    std::printf("blit err=%04x\n", glGetError());
    unsigned char pxb[4] = {0};
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pxb);
    std::printf("blit-read err=%04x px=%u %u %u %u (want 255 255 0)\n", glGetError(), pxb[0], pxb[1],
                pxb[2], pxb[3]);
    return 0;
}
