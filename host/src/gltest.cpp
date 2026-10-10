// gltest: real GLES2 compile check for captured game shaders (harness-only).
// Usage: gltest <vertex.glsl> <fragment.glsl> — EGL surfaceless (fallback
// pbuffer), compile + link, print status/logs. Exit 0 iff link succeeds.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <X11/Xlib.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

static std::string Slurp(const char* p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        std::printf("no such file: %s\n", p);
        std::exit(2);
    }
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

static GLuint Compile(GLenum type, const std::string& src) {
    GLuint s = glCreateShader(type);
    const char* p = src.c_str();
    GLint n = (GLint)src.size();
    glShaderSource(s, 1, &p, &n);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    std::printf("%s compile: %s\n", type == GL_VERTEX_SHADER ? "vertex" : "fragment",
                ok ? "OK" : "FAIL");
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
        std::string log((std::size_t)(len > 0 ? len : 1), '\0');
        glGetShaderInfoLog(s, len, nullptr, log.data());
        std::printf("--- log ---\n%s\n--- end ---\n", log.c_str());
    }
    return s;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: gltest <vert.glsl> <frag.glsl>\n");
        return 2;
    }
    // HARNESS_EGLX=1: X window surface path (mirrors RealGL).
    bool want_x = std::getenv("HARNESS_EGLX") != nullptr;
    // Display selection: HARNESS_EGLD=std forces eglGetDisplay (NVIDIA
    // native headless); HARNESS_EGLD=dev uses EGLDevice (renderD128);
    // HARNESS_EGLX=1 also uses eglGetDisplay (X-capable, for windows);
    // default tries surfaceless first.
    EGLDisplay d = EGL_NO_DISPLAY;
    const char* eglmode = std::getenv("HARNESS_EGLD");
    if (want_x && !eglmode) {
        d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    if (eglmode && eglmode[0] == 'd' && eglmode[1] == 'e' && eglmode[2] == 'v') {
        auto qdev = (EGLBoolean (*)(EGLint, void**, EGLint*))eglGetProcAddress("eglQueryDevicesEXT");
        auto pdis = (EGLDisplay(*)(EGLenum, void*, const EGLint*))eglGetProcAddress(
            "eglGetPlatformDisplayEXT");
        auto qstr = (const char* (*)(void*, EGLint))eglGetProcAddress("eglQueryDeviceStringEXT");
        if (qdev && pdis) {
            void* devs[8] = {};
            EGLint ndev = 0;
            if (qdev(8, devs, &ndev)) {
                int want = -1;
                if (eglmode[3] >= '0' && eglmode[3] <= '7') {
                    want = eglmode[3] - '0';
                }
                for (EGLint i = 0; i < ndev; i++) {
                    const char* nm = (qstr && i != want) ? qstr(devs[i], 0x3053) : "";
                    std::printf("egl dev %d: %s\n", i, nm ? nm : "?");
                }
                if (want >= 0 && want < ndev) {
                    d = pdis(EGL_PLATFORM_DEVICE_EXT, devs[want], nullptr);
                } else {
                    for (EGLint i = 0; i < ndev && d == EGL_NO_DISPLAY; i++) {
                        d = pdis(EGL_PLATFORM_DEVICE_EXT, devs[i], nullptr);
                    }
                }
            }
        } else {
            std::printf("no device ext\n");
        }
    } else if (!eglmode && !want_x) {
        d = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    }
    if (d == EGL_NO_DISPLAY) {
        d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    if (d == EGL_NO_DISPLAY || !eglInitialize(d, nullptr, nullptr)) {
        std::printf("egl init failed\n");
        return 1;
    }
    const EGLint cfg_w[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,
                            EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                            EGL_ALPHA_SIZE, 8, EGL_NONE};
    const EGLint cfg_p[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                            EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                            EGL_ALPHA_SIZE, 8, EGL_NONE};
    const EGLint* cfg = want_x ? cfg_w : cfg_p;
    EGLConfig c = nullptr;
    EGLint nc = 0;
    if (!eglChooseConfig(d, cfg, &c, 1, &nc) || nc < 1) {
        std::printf("no config\n");
        return 1;
    }
    const EGLint ctxa[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext ctx = eglCreateContext(d, c, EGL_NO_CONTEXT, ctxa);
    if (ctx == EGL_NO_CONTEXT) {
        std::printf("no context\n");
        return 1;
    }
    EGLSurface s = EGL_NO_SURFACE;
    if (want_x) {
        Display* xd = XOpenDisplay(nullptr);
        if (xd) {
            Window w = XCreateSimpleWindow(xd, DefaultRootWindow(xd), 0, 0, 64, 64, 0, 0, 0);
            XMapWindow(xd, w);
            XFlush(xd);
            s = eglCreateWindowSurface(d, c, (EGLNativeWindowType)w, nullptr);
            std::printf("xwindow surface=%p\n", (void*)s);
        }
    }
    if (s == EGL_NO_SURFACE) {
        const EGLint pb[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
        s = eglCreatePbufferSurface(d, c, pb);
        std::printf("pbuffer surface=%p\n", (void*)s);
    }
    if (s == EGL_NO_SURFACE || !eglMakeCurrent(d, s, s, ctx)) {
        std::printf("make-current failed\n");
        return 1;
    }
    std::printf("GL: %s | %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    GLuint vs = Compile(GL_VERTEX_SHADER, Slurp(argv[1]));
    GLuint fs = Compile(GL_FRAGMENT_SHADER, Slurp(argv[2]));
    GLuint pr = glCreateProgram();
    glAttachShader(pr, vs);
    glAttachShader(pr, fs);
    glLinkProgram(pr);
    GLint ok = 0;
    glGetProgramiv(pr, GL_LINK_STATUS, &ok);
    std::printf("link: %s\n", ok ? "OK" : "FAIL");
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(pr, GL_INFO_LOG_LENGTH, &len);
        std::string log((std::size_t)(len > 0 ? len : 1), '\0');
        glGetProgramInfoLog(pr, len, nullptr, log.data());
        std::printf("--- link log ---\n%s\n--- end ---\n", log.c_str());
        return 1;
    }
    // Readback self-test: clear red on default FB, blit to FBO, read.
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    std::printf("clear err=%04x current=%p\n", glGetError(), (void*)eglGetCurrentContext());
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 64, 64, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    GLenum st = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    std::printf("fbo status: %04x (want 8cd5)\n", st);
    glBlitFramebuffer(0, 0, 64, 64, 0, 0, 64, 64, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    unsigned char px[3] = {0};
    glReadPixels(32, 32, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, px);
    std::printf("fbo-readback: %u %u %u (want 255 0 0)\n", px[0], px[1], px[2]);
    // Direct-FBO render test: clear while DRAW_FBO bound, then read.
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glReadPixels(32, 32, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, px);
    std::printf("fbo-direct-clear: %u %u %u (want 0 255 0)\n", px[0], px[1], px[2]);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadPixels(32, 32, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, px);
    std::printf("direct-readback: %u %u %u\n", px[0], px[1], px[2]);
    return 0;
}
