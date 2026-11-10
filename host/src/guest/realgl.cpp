// RealGL implementation. See realgl.h for the design contract.
#include "realgl.h"

#include <atomic>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#ifndef __ANDROID__
#include <EGL/eglext.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "dispatch.h"

namespace abr {

#ifndef __ANDROID__
bool RealGL::Ensure() {
    if (tried_) {
        return ok_;
    }
    tried_ = true;
    if (!std::getenv("HARNESS_REALGL")) {
        return false;
    }
    EGLDisplay d = EGL_NO_DISPLAY;
    // X window path needs an X-capable EGL display (surfaceless can't do
    // window surfaces); pbuffer stays on surfaceless.
    bool want_x = !std::getenv("HARNESS_EGLPB");
    if (want_x) {
        xdisp_ = XOpenDisplay(nullptr);
        std::printf("[realgl] xdisplay=%p\n", xdisp_);
        if (xdisp_) {
            d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        }
    }
    if (d == EGL_NO_DISPLAY) {
        xdisp_ = nullptr;
        d = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    }
    if (d == EGL_NO_DISPLAY) {
        d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    if (d == EGL_NO_DISPLAY || !eglInitialize(d, nullptr, nullptr)) {
        std::printf("[realgl] egl init failed\n");
        return false;
    }
    // Drawable: X window surface when a display is available (transient
    // window only, zero persistent system changes), else headless pbuffer.
    // HARNESS_EGLPB=1 forces pbuffer.
    EGLSurface s = EGL_NO_SURFACE;
    EGLConfig c = nullptr;
    if (xdisp_) {
        const EGLint wcfg[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,
                               EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE,
                               8, EGL_ALPHA_SIZE, 8, EGL_NONE};
        EGLint nc = 0;
        if (eglChooseConfig(d, wcfg, &c, 1, &nc) && nc >= 1) {
            EGLint vid = 0;
            eglGetConfigAttrib(d, c, EGL_NATIVE_VISUAL_ID, &vid);
            XVisualInfo vt = {};
            vt.visualid = (VisualID)vid;
            int nv = 0;
            XVisualInfo* vi = XGetVisualInfo((Display*)xdisp_, VisualIDMask, &vt, &nv);
            std::printf("[realgl] xvisual nv=%d\n", nv);
            if (vi && nv > 0) {
                Colormap cm = XCreateColormap((Display*)xdisp_, DefaultRootWindow((Display*)xdisp_),
                                              vi->visual, AllocNone);
                XSetWindowAttributes wa = {};
                wa.colormap = cm;
                wa.event_mask = ExposureMask | KeyPressMask;
                xwin_ = (std::uint64_t)XCreateWindow(
                    (Display*)xdisp_, DefaultRootWindow((Display*)xdisp_), 0, 0, 800, 480, 0,
                    vi->depth, InputOutput, vi->visual, CWColormap | CWEventMask, &wa);
                XFree(vi);
                if (xwin_) {
                    XStoreName((Display*)xdisp_, (Window)xwin_, "ABR-port GL");
                    XMapWindow((Display*)xdisp_, (Window)xwin_);
                    XFlush((Display*)xdisp_);
                    s = eglCreateWindowSurface(d, c, (EGLNativeWindowType)(std::uintptr_t)xwin_,
                                               nullptr);
                    std::printf("[realgl] xwindow %llx surf=%p\n", (unsigned long long)xwin_,
                                (void*)s);
                }
            }
        }
    }
    if (s == EGL_NO_SURFACE) {
        const EGLint cfg[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                              EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE,
                              8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 16, EGL_STENCIL_SIZE, 8,
                              EGL_NONE};
        EGLint nc = 0;
        if (!eglChooseConfig(d, cfg, &c, 1, &nc) || nc < 1) {
            std::printf("[realgl] no config\n");
            return false;
        }
        const EGLint pb[] = {EGL_WIDTH, 800, EGL_HEIGHT, 480, EGL_NONE};
        s = eglCreatePbufferSurface(d, c, pb);
    }
    const EGLint ctxa[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(d, c, EGL_NO_CONTEXT, ctxa);
    if (ctx == EGL_NO_CONTEXT) {
        std::printf("[realgl] no context\n");
        return false;
    }
    if (s == EGL_NO_SURFACE || !eglMakeCurrent(d, s, s, ctx)) {
        std::printf("[realgl] make-current failed\n");
        return false;
    }
    disp_ = d;
    surf_ = s;
    std::printf("[realgl] live: %s | %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    ok_ = true;
    return true;
}
#else
// ---- Android: always the real driver, in the caller's GL context
// (SingleThread model: updates run on the GL thread with Java's context
// current). No EGL init, no pbuffer, no share group. Presentation is free
// (Java swaps its own surface after onDrawFrame). A call from a thread with
// no current context goes to the driver too, which ignores it as it would
// for the original library (never seen on the five titles).
bool RealGL::Ensure() {
    // Only a diagnostic: the answer is true either way (the driver ignores a
    // call without a context). eglGetCurrentContext costs ~1 us on the
    // sandbox, as much as the GL call itself, so it is asked on a thread's
    // first GL call only (every call under HARNESS_DIAG).
    static thread_local bool checked = false;
    if (checked && !DiagOn()) {
        return true;
    }
    checked = true;
    if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
        static std::atomic<int> nolog{0};
        if (nolog.fetch_add(1) < 3) {
            std::printf("[realgl] android: gl call without a current context (driver ignores it)\n");
        }
        return true;
    }
    {
        static int yeslog = 0;
        if (yeslog < 1) {
            yeslog++;
            EGLContext c = eglGetCurrentContext();
            std::printf("[realgl] android: live ctx=%p\n", (void*)c);
        }
    }
    return true;
}

bool RealGL::Screenshot(const char* path, int w, int h) {
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) {
        return false;
    }
    if (eglGetCurrentContext() == EGL_NO_CONTEXT && !Ensure()) {
        return false;
    }
    std::vector<std::uint8_t> px((std::size_t)w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    FILE* f = std::fopen(path, "wb");
    if (!f) {
        return false;
    }
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; y--) {
        std::fwrite(px.data() + (std::size_t)y * w * 4, 1, (std::size_t)w * 4, f);
    }
    std::fclose(f);
    std::printf("[realgl] android screenshot -> %s\n", path);
    return true;
}
#endif


// Bounded guest C-string (attribute/uniform names).
static std::string GStr(Runtime* rt, std::uint32_t va) {
    std::string o;
    for (std::uint32_t i = 0; i < 128; i++) {
        if (!rt->CheckRange(va + i, 1, false)) {
            break;
        }
        char c = 0;
        std::memcpy(&c, rt->H(va + i), 1);
        if (!c) {
            break;
        }
        o.push_back(c);
    }
    return o;
}

static std::uint32_t Stk(Runtime* rt, std::uint32_t sp, int i) {
    std::uint32_t v = 0;
    if (rt->CheckRange(sp + (std::uint32_t)(i * 4), 4, false)) {
        std::memcpy(&v, rt->H(sp + (std::uint32_t)(i * 4)), 4);
    }
    return v;
}

// Bytes per pixel for (format,type); 0 = unknown combo (caller keeps the
// legacy 1-byte probe then). Packed types are 2B regardless of channels.
static std::uint64_t PixelBytes(std::uint32_t fmt, std::uint32_t typ) {
    if (typ == 0x8363u || typ == 0x8034u || typ == 0x8033u) {  // 565/5551/4444
        return 2;
    }
    std::uint64_t ch = 0;
    switch (fmt) {
        case 0x1906u:  // ALPHA
        case 0x1909u:  // LUMINANCE
            ch = 1;
            break;
        case 0x190Au:  // LUMINANCE_ALPHA
            ch = 2;
            break;
        case 0x1907u:  // RGB
            ch = 3;
            break;
        case 0x1908u:  // RGBA
            ch = 4;
            break;
        default:
            return 0;
    }
    switch (typ) {
        case 0x1401u:  // UNSIGNED_BYTE
            return ch;
        case 0x1403u:  // UNSIGNED_SHORT
            return ch * 2;
        case 0x1405u:  // UNSIGNED_INT
        case 0x1406u:  // FLOAT
            return ch * 4;
        default:
            return 0;
    }
}

// Bytes the driver reads or writes for a w x h image of `bpp`-byte pixels
// with rows aligned to `pname` (GL_UNPACK_ALIGNMENT or GL_PACK_ALIGNMENT,
// default 4): align_up(w*bpp)*(h-1) + w*bpp (GLES2 3.6.2, 4.3.1; E-05).
// 0 when bpp is unknown or the image is empty.
static std::uint64_t ImageBytes(std::uint64_t w, std::uint64_t h, std::uint64_t bpp,
                                GLenum pname) {
    if (w == 0 || h == 0 || bpp == 0) {
        return 0;
    }
    GLint a = 4;
    glGetIntegerv(pname, &a);
    if (a != 1 && a != 2 && a != 4 && a != 8) {
        a = 4;
    }
    const std::uint64_t row = w * bpp;
    const std::uint64_t stride = (row + (std::uint64_t)a - 1) / (std::uint64_t)a * (std::uint64_t)a;
    return stride * (h - 1) + row;
}

// Index byte size for DrawElements type; 0 = unknown.
static std::uint64_t IndexSize(std::uint32_t typ) {
    if (typ == 0x1401u) {
        return 1;
    }
    if (typ == 0x1403u) {
        return 2;
    }
    if (typ == 0x1405u) {
        return 4;
    }
    return 0;
}

// This thread's copy of its context's two buffer bindings. Bindings belong
// to the GL context, and the context can change behind the host: Java's
// GLSurfaceView makes a new one on resume (REVIEW E-03). Java runs only
// between Java->native calls, and the guest switches contexts only through
// eglMakeCurrent/eglReleaseThread, which the host serves; so the copy is
// dropped at each of those (ForgetGlBindings) and at glDeleteBuffers, and
// is never stale. The first query after a drop asks the driver (~1 us on
// the sandbox); the rest of that native call uses the copy.
namespace {
struct GlBindings {
    bool array_known = false, element_known = false;
    GLuint array = 0, element = 0;
    // Vertex attrib array enable flags: -1 unknown, 0 off, 1 on. Context
    // state like the bindings (GLES2 2.8), changed only by the guest's own
    // glEnable/DisableVertexAttribArray (no VAOs are offered), so the same
    // drops keep it exact. An unknown flag is learnt from the call itself.
    std::int8_t attr[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
};
}  // namespace

// Uniform values the current program already holds (minimum F9): a call
// that sets a location to the value it has is not sent (MCSM: ~70 % of
// ~65 k uniform calls a second). Uniform values are program state (GLES2
// 2.10.4), so this copy is kept exact by:
// - one thread only: the first thread to set a uniform owns the copy; a
//   uniform call from any other thread turns the elision off for good
//   (programs can be shared between contexts of several threads);
// - dropped with the bindings at every possible context change or return
//   to Java (ForgetGlBindings): measured on MCSM, this loses no repeats;
// - dropped by every glLinkProgram/glDeleteProgram on any thread (a link
//   resets values, a deleted name can come back), via a generation count;
// - the current program asked from the driver after each glUseProgram (a
//   glUseProgram of a program that failed to link changes nothing);
// - only single-value calls are kept, compared bit for bit: a call with
//   count > 1 also sets array elements whose locations are not known here,
//   so it is sent and the program's copy dropped; no program (0) is never
//   kept. The one difference to the driver: an invalid call repeated with
//   the same arguments raises its GL error once, not every time; the state
//   is the same either way.
namespace {
struct UniVal {
    GlId id;
    std::uint32_t n;      // words in v
    std::uint32_t v[17];  // up to a 4x4 matrix + its transpose flag
};
struct UniCache {
    std::uint64_t gen = 0;
    bool prog_known = false;
    GLuint prog = 0;
    std::unordered_map<GLint, UniVal>* cur = nullptr;  // prog's map
    std::unordered_map<GLuint, std::unordered_map<GLint, UniVal>> by_prog;
};
struct ClientArray {
    bool enabled = false;
    GLint size = 0;
    GLenum type = 0;
    GLsizei stride = 0;
    std::uint32_t va = 0;
    GLuint vbo = 0;
};

// GLES 1.1 entry points loaded dynamically from libGLESv1_CM.so
struct Gles1Api {
    void* handle = nullptr;
    void (*glClientActiveTexture)(GLenum) = nullptr;
    void (*glColor4f)(GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
    void (*glColorPointer)(GLint, GLenum, GLsizei, const void*) = nullptr;
    void (*glDisableClientState)(GLenum) = nullptr;
    void (*glEnableClientState)(GLenum) = nullptr;
    void (*glLightfv)(GLenum, GLenum, const GLfloat*) = nullptr;
    void (*glLoadMatrixf)(const GLfloat*) = nullptr;
    void (*glMaterialfv)(GLenum, GLenum, const GLfloat*) = nullptr;
    void (*glMatrixMode)(GLenum) = nullptr;
    void (*glNormalPointer)(GLenum, GLsizei, const void*) = nullptr;
    void (*glTexCoordPointer)(GLint, GLenum, GLsizei, const void*) = nullptr;
    void (*glTexEnvi)(GLenum, GLenum, GLint) = nullptr;
    void (*glVertexPointer)(GLint, GLenum, GLsizei, const void*) = nullptr;

    // OES framebuffer object extension in GLES 1.1
    void (*glGenFramebuffersOES)(GLsizei, GLuint*) = nullptr;
    void (*glBindFramebufferOES)(GLenum, GLuint) = nullptr;
    void (*glFramebufferTexture2DOES)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
    void (*glFramebufferRenderbufferOES)(GLenum, GLenum, GLenum, GLuint) = nullptr;
    void (*glDeleteFramebuffersOES)(GLsizei, const GLuint*) = nullptr;
    void (*glGenRenderbuffersOES)(GLsizei, GLuint*) = nullptr;
    void (*glBindRenderbufferOES)(GLenum, GLuint) = nullptr;
    void (*glRenderbufferStorageOES)(GLenum, GLenum, GLsizei, GLsizei) = nullptr;
    void (*glDeleteRenderbuffersOES)(GLsizei, const GLuint*) = nullptr;
    GLenum (*glCheckFramebufferStatusOES)(GLenum) = nullptr;

    bool Init() {
        if (handle) {
            return true;
        }
#ifdef __ANDROID__
        handle = dlopen("libGLESv1_CM.so", RTLD_NOW | RTLD_GLOBAL);
#else
        handle = dlopen("libGLESv1_CM.so", RTLD_NOW | RTLD_GLOBAL);
        if (!handle) {
            handle = dlopen("libGLESv1_CM.so.1", RTLD_NOW | RTLD_GLOBAL);
        }
#endif
        if (!handle) {
            return false;
        }
#define ABR_LOAD_GLES1(fn) \
        fn = reinterpret_cast<decltype(fn)>(dlsym(handle, #fn)); \
        if (!fn) fn = reinterpret_cast<decltype(fn)>(eglGetProcAddress(#fn))

        ABR_LOAD_GLES1(glClientActiveTexture);
        ABR_LOAD_GLES1(glColor4f);
        ABR_LOAD_GLES1(glColorPointer);
        ABR_LOAD_GLES1(glDisableClientState);
        ABR_LOAD_GLES1(glEnableClientState);
        ABR_LOAD_GLES1(glLightfv);
        ABR_LOAD_GLES1(glLoadMatrixf);
        ABR_LOAD_GLES1(glMaterialfv);
        ABR_LOAD_GLES1(glMatrixMode);
        ABR_LOAD_GLES1(glNormalPointer);
        ABR_LOAD_GLES1(glTexCoordPointer);
        ABR_LOAD_GLES1(glTexEnvi);
        ABR_LOAD_GLES1(glVertexPointer);

        ABR_LOAD_GLES1(glGenFramebuffersOES);
        ABR_LOAD_GLES1(glBindFramebufferOES);
        ABR_LOAD_GLES1(glFramebufferTexture2DOES);
        ABR_LOAD_GLES1(glFramebufferRenderbufferOES);
        ABR_LOAD_GLES1(glDeleteFramebuffersOES);
        ABR_LOAD_GLES1(glGenRenderbuffersOES);
        ABR_LOAD_GLES1(glBindRenderbufferOES);
        ABR_LOAD_GLES1(glRenderbufferStorageOES);
        ABR_LOAD_GLES1(glDeleteRenderbuffersOES);
        ABR_LOAD_GLES1(glCheckFramebufferStatusOES);
#undef ABR_LOAD_GLES1
        return true;
    }
};

static Gles1Api g_gles1;
static bool EnsureGles1() {
    static bool init_done = false;
    static bool init_ok = false;
    if (init_done) {
        return init_ok;
    }
    init_done = true;
    init_ok = g_gles1.Init();
    return init_ok;
}

static std::uint32_t GlTypeSize(GLenum type) {
    switch (type) {
        case 0x1400: // GL_BYTE
        case 0x1401: // GL_UNSIGNED_BYTE
            return 1;
        case 0x1402: // GL_SHORT
        case 0x1403: // GL_UNSIGNED_SHORT
            return 2;
        case 0x1404: // GL_INT
        case 0x1405: // GL_UNSIGNED_INT
        case 0x1406: // GL_FLOAT
        case 0x140C: // GL_FIXED
            return 4;
        default:
            return 4;
    }
}

static int LightFvCount(GLenum pname) {
    switch (pname) {
        case 0x1200: // GL_AMBIENT
        case 0x1201: // GL_DIFFUSE
        case 0x1202: // GL_SPECULAR
        case 0x1203: // GL_POSITION
            return 4;
        case 0x1204: // GL_SPOT_DIRECTION
            return 3;
        case 0x1205: // GL_SPOT_EXPONENT
        case 0x1206: // GL_SPOT_CUTOFF
        case 0x1207: // GL_CONSTANT_ATTENUATION
        case 0x1208: // GL_LINEAR_ATTENUATION
        case 0x1209: // GL_QUADRATIC_ATTENUATION
            return 1;
        default:
            return 4;
    }
}

static int MaterialFvCount(GLenum pname) {
    switch (pname) {
        case 0x1200: // GL_AMBIENT
        case 0x1201: // GL_DIFFUSE
        case 0x1202: // GL_SPECULAR
        case 0x1600: // GL_EMISSION
        case 0x1602: // GL_AMBIENT_AND_DIFFUSE
            return 4;
        case 0x1601: // GL_SHININESS
            return 1;
        default:
            return 4;
    }
}

// This thread's GL copies (bindings, uniform values, the uniform-owner
// token) in one thread_local: a GL call reaches all of them with one TLS
// lookup (each thread_local is a TLS-resolver call in a dlopened library).
struct GlThread {
    GlBindings bindings;
    UniCache uni;
    char token = 0;
    std::uint32_t client_active_texture = 0;
    ClientArray client_vertex;
    ClientArray client_normal;
    ClientArray client_color;
    ClientArray client_tex_coord[4];
};
thread_local GlThread t_gl;

// True when an enabled array reads guest memory. An enabled array whose
// pointer was set with a buffer object bound is an offset into that buffer
// (the ordinary GLES 1.1 VBO path) and needs no range check here.
static inline bool HasClientArrays() {
    const auto in_guest = [](const ClientArray& a) { return a.enabled && a.vbo == 0; };
    return in_guest(t_gl.client_vertex) || in_guest(t_gl.client_normal) ||
           in_guest(t_gl.client_color) || in_guest(t_gl.client_tex_coord[0]) ||
           in_guest(t_gl.client_tex_coord[1]) || in_guest(t_gl.client_tex_coord[2]) ||
           in_guest(t_gl.client_tex_coord[3]);
}

static bool ValidateClientArray(Runtime* r, const char* name, const char* arr_name,
                                const ClientArray& arr, std::uint32_t max_vert_idx) {
    if (!arr.enabled || arr.vbo != 0) {
        return true;
    }
    if (!arr.va) {
        std::printf("[realgl] %s LOUD NULLVERT %s max_idx=%u -- halting\n",
                    name, arr_name, max_vert_idx);
        r->HaltGuest();
        return false;
    }
    const std::uint32_t type_sz = GlTypeSize(arr.type);
    const std::uint32_t elem_sz = (arr.size > 0 ? (std::uint32_t)arr.size : 1u) * type_sz;
    const std::uint32_t stride = arr.stride > 0 ? (std::uint32_t)arr.stride : elem_sz;
    const std::uint64_t need = (std::uint64_t)max_vert_idx * stride + elem_sz;
    if (need >= 0x10000000u || !r->CheckRange(arr.va, (std::size_t)need, false)) {
        std::printf("[realgl] %s LOUD SHORTVERT %s va=%08x max_idx=%u need=%llu -- halting\n",
                    name, arr_name, arr.va, max_vert_idx, (unsigned long long)need);
        r->HaltGuest();
        return false;
    }
    return true;
}

static bool ValidateClientArrays(Runtime* r, const char* name, std::uint32_t max_vert_idx) {
    if (!ValidateClientArray(r, name, "vertex", t_gl.client_vertex, max_vert_idx)) return false;
    if (!ValidateClientArray(r, name, "normal", t_gl.client_normal, max_vert_idx)) return false;
    if (!ValidateClientArray(r, name, "color", t_gl.client_color, max_vert_idx)) return false;
    for (int i = 0; i < 4; i++) {
        if (!ValidateClientArray(r, name, "texcoord", t_gl.client_tex_coord[i], max_vert_idx)) return false;
    }
    return true;
}
std::atomic<const void*> g_uni_owner{nullptr};
std::atomic<bool> g_uni_shared{false};
std::atomic<std::uint64_t> g_prog_gen{1};

void ForgetUniforms() {
    UniCache& c = t_gl.uni;
    c.by_prog.clear();
    c.cur = nullptr;
    c.prog_known = false;
}

// True when this uniform call repeats the value its location holds and need
// not be sent; otherwise the copy takes the new value (or is dropped).
bool UniformRepeat(Runtime* r, GlId id, std::uint32_t a0, std::uint32_t a1, std::uint32_t a2,
                   std::uint32_t a3, std::uint32_t sp) {
    using G = GlId;
    int comps = 0;
    bool vec = false, mat = false;
    switch (id) {
        case G::glUniform1f: case G::glUniform1i: comps = 1; break;
        case G::glUniform2f: case G::glUniform2i: comps = 2; break;
        case G::glUniform3f: case G::glUniform3i: comps = 3; break;
        case G::glUniform4f: case G::glUniform4i: comps = 4; break;
        case G::glUniform1fv: case G::glUniform1iv: comps = 1; vec = true; break;
        case G::glUniform2fv: case G::glUniform2iv: comps = 2; vec = true; break;
        case G::glUniform3fv: case G::glUniform3iv: comps = 3; vec = true; break;
        case G::glUniform4fv: case G::glUniform4iv: comps = 4; vec = true; break;
        case G::glUniformMatrix2fv: comps = 4; mat = true; break;
        case G::glUniformMatrix3fv: comps = 9; mat = true; break;
        case G::glUniformMatrix4fv: comps = 16; mat = true; break;
        default: return false;
    }
    if (g_uni_shared.load(std::memory_order_relaxed)) return false;
    const void* me = &t_gl.token;
    const void* owner = g_uni_owner.load(std::memory_order_relaxed);
    if (owner != me) {
        if (owner != nullptr || !g_uni_owner.compare_exchange_strong(owner, me)) {
            g_uni_shared.store(true, std::memory_order_relaxed);
            return false;
        }
    }
    UniCache& c = t_gl.uni;
    const std::uint64_t gen = g_prog_gen.load(std::memory_order_acquire);
    if (c.gen != gen) {
        c.by_prog.clear();
        c.cur = nullptr;
        c.gen = gen;
    }
    if (!c.cur) {
        if (!c.prog_known) {
            GLint v = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &v);
            c.prog = static_cast<GLuint>(v);
            c.prog_known = true;
        }
        if (c.prog == 0) return false;
        c.cur = &c.by_prog[c.prog];
    }
    UniVal x;
    x.id = id;
    x.n = static_cast<std::uint32_t>(comps);
    if (vec || mat) {
        if (a1 != 1) {  // arrays: other elements too, at unknown locations
            c.cur->clear();
            return false;
        }
        const std::uint32_t vp = mat ? a3 : a2;
        if (!vp || !r->CheckRange(vp, comps * 4u, false)) return false;  // the handler halts
        std::memcpy(x.v, r->H(vp), comps * 4u);
        if (mat) x.v[x.n++] = a2 & 1u;
    } else {
        const std::uint32_t w[4] = {a1, a2, a3, comps == 4 ? Stk(r, sp, 0) : 0u};
        std::memcpy(x.v, w, comps * 4u);
    }
    auto [it, fresh] = c.cur->try_emplace(static_cast<GLint>(a0), x);
    if (!fresh) {
        const UniVal& o = it->second;
        if (o.id == x.id && o.n == x.n && std::memcmp(o.v, x.v, x.n * 4u) == 0) return true;
        it->second = x;
    }
    return false;
}

// glUseProgram: the program is asked again before the next uniform.
void UniformProgramChanged() {
    t_gl.uni.prog_known = false;
    t_gl.uni.cur = nullptr;
}
}  // namespace

#ifdef ABR_DIAG_COUNTERS
static void DiagGlForget();
#endif
void ForgetGlBindings() {
    t_gl.bindings = GlBindings{};
    ForgetUniforms();
#ifdef ABR_DIAG_COUNTERS
    DiagGlForget();
#endif
}

// HARNESS_GLDRAWLOG (draw/upload tracing), read once: EnvOn scans its table
// on every call, and this is asked on every draw, upload and attrib pointer.
static bool GlDrawLog() {
    static const bool on = EnvOn("HARNESS_GLDRAWLOG");
    return on;
}

// The buffer bound to `binding` (GL_ARRAY_BUFFER_BINDING or
// GL_ELEMENT_ARRAY_BUFFER_BINDING) in the current context.
static GLuint BoundBuffer(GLenum binding) {
    GlBindings& b = t_gl.bindings;
    const bool array = binding == GL_ARRAY_BUFFER_BINDING;
    if (array ? b.array_known : b.element_known) {
        return array ? b.array : b.element;
    }
    GLint v = 0;
    glGetIntegerv(binding, &v);
    (array ? b.array : b.element) = static_cast<GLuint>(v);
    (array ? b.array_known : b.element_known) = true;
    return static_cast<GLuint>(v);
}

#ifdef ABR_DIAG_COUNTERS
// Diag only (minimum F9 measurement): how many state calls repeat the state
// the context already has. Mirrors what an elision would see: a per-thread
// copy, dropped with the buffer copy (ForgetGlBindings), the driver asked
// on the first use after a drop. Uniform values belong to the program and
// survive context-local drops; they are dropped at link/delete. Counts only.
namespace {
enum { kRedAttr, kRedBuf, kRedProg, kRedUni, kRedMat, kRedUni1, kRedUniLocal, kRedN };
std::atomic<std::uint32_t> g_uni_threads{0};
thread_local bool t_uni_seen = false;
thread_local std::unordered_map<std::uint64_t, std::uint64_t> t_uni_local;  // dropped at every ForgetGlBindings
std::atomic<std::uint64_t> g_red_calls[kRedN], g_red_same[kRedN];
struct GlShadow {
    std::int8_t attr[16];  // -1 unknown, 0 off, 1 on
    bool prog_known = false;
    GLuint prog = 0;
    GlShadow() { std::memset(attr, -1, sizeof attr); }
};
thread_local GlShadow t_shadow;
thread_local std::unordered_map<std::uint64_t, std::uint64_t> t_diag_uni;  // (prog,loc) -> value hash
std::uint64_t Fnv(const void* p, std::size_t n, std::uint64_t h) {
    const auto* b = static_cast<const unsigned char*>(p);
    for (std::size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
void RedCount(int k, bool same) {
    g_red_calls[k].fetch_add(1, std::memory_order_relaxed);
    if (same) g_red_same[k].fetch_add(1, std::memory_order_relaxed);
}
GLuint ShadowProg() {
    if (!t_shadow.prog_known) {
        GLint v = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &v);
        t_shadow.prog = static_cast<GLuint>(v);
        t_shadow.prog_known = true;
    }
    return t_shadow.prog;
}
void DiagGlRedundant(Runtime* r, GlId id, std::uint32_t a0, std::uint32_t a1, std::uint32_t a2,
                     std::uint32_t a3, std::uint32_t sp) {
    using G = GlId;
    if (id == G::glEnableVertexAttribArray || id == G::glDisableVertexAttribArray) {
        if (a0 >= 16) return;
        std::int8_t& st = t_shadow.attr[a0];
        if (st < 0) {
            GLint v = 0;
            glGetVertexAttribiv(a0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &v);
            st = v ? 1 : 0;
        }
        const std::int8_t want = id == G::glEnableVertexAttribArray ? 1 : 0;
        RedCount(kRedAttr, st == want);
        st = want;
        return;
    }
    if (id == G::glBindBuffer) {
        if (a0 == GL_ARRAY_BUFFER || a0 == GL_ELEMENT_ARRAY_BUFFER) {
            RedCount(kRedBuf, BoundBuffer(a0 == GL_ARRAY_BUFFER ? GL_ARRAY_BUFFER_BINDING
                                                                : GL_ELEMENT_ARRAY_BUFFER_BINDING) == a1);
        }
        return;
    }
    if (id == G::glUseProgram) {
        RedCount(kRedProg, ShadowProg() == a0);
        t_shadow.prog = a0;
        return;
    }
    if (id == G::glLinkProgram || id == G::glDeleteProgram) {
        for (auto it = t_diag_uni.begin(); it != t_diag_uni.end();) {
            it = (it->first >> 32) == a0 ? t_diag_uni.erase(it) : std::next(it);
        }
        return;
    }
    // Uniforms: comps per element, where the data is.
    int comps = 0;
    bool vec = false, mat = false;
    switch (id) {
        case G::glUniform1f: case G::glUniform1i: comps = 1; break;
        case G::glUniform2f: case G::glUniform2i: comps = 2; break;
        case G::glUniform3f: case G::glUniform3i: comps = 3; break;
        case G::glUniform4f: case G::glUniform4i: comps = 4; break;
        case G::glUniform1fv: case G::glUniform1iv: comps = 1; vec = true; break;
        case G::glUniform2fv: case G::glUniform2iv: comps = 2; vec = true; break;
        case G::glUniform3fv: case G::glUniform3iv: comps = 3; vec = true; break;
        case G::glUniform4fv: case G::glUniform4iv: comps = 4; vec = true; break;
        case G::glUniformMatrix2fv: comps = 4; mat = true; break;
        case G::glUniformMatrix3fv: comps = 9; mat = true; break;
        case G::glUniformMatrix4fv: comps = 16; mat = true; break;
        default: return;
    }
    std::uint64_t h = Fnv(&id, sizeof id, 14695981039346656037ull);
    if (vec || mat) {
        const std::uint32_t vp = mat ? a3 : a2;
        const std::size_t nb = (std::size_t)a1 * comps * 4;
        if (!vp || a1 >= 4096 || !nb || !r->CheckRange(vp, nb, false)) return;
        h = Fnv(&a1, 4, h);
        if (mat) h = Fnv(&a2, 4, h);
        h = Fnv(r->H(vp), nb, h);
    } else {
        const std::uint32_t w[4] = {a1, a2, a3, comps == 4 ? Stk(r, sp, 0) : 0u};
        h = Fnv(w, comps * 4u, h);
    }
    const std::uint64_t key = (std::uint64_t)ShadowProg() << 32 | a0;
    auto [it, fresh] = t_diag_uni.try_emplace(key, h);
    const bool same = !fresh && it->second == h;
    RedCount(mat ? kRedMat : kRedUni, same);
    // count 1 only: a call with count > 1 also sets elements whose
    // locations are not known here, so it could not be elided safely.
    if (!((vec || mat) && a1 > 1)) RedCount(kRedUni1, same);
    it->second = h;
    auto [jt, jfresh] = t_uni_local.try_emplace(key, h);
    RedCount(kRedUniLocal, !jfresh && jt->second == h);
    jt->second = h;
    if (!t_uni_seen) {
        t_uni_seen = true;
        g_uni_threads.fetch_add(1, std::memory_order_relaxed);
    }
}
}  // namespace

static void DiagGlForget() {
    t_shadow = GlShadow{};
    t_uni_local.clear();
}

void RealGlDiagDump() {
    static const char* const kName[kRedN] = {"attrib enable/disable", "glBindBuffer",
                                             "glUseProgram", "uniform (vec/scalar)",
                                             "uniform matrix", "uniform, count 1 only",
                                             "uniform, drop at Java"};
    std::printf("[counters]   gl uniform threads so far: %u\n", g_uni_threads.load());
    for (int k = 0; k < kRedN; k++) {
        const std::uint64_t c = g_red_calls[k].exchange(0, std::memory_order_relaxed);
        const std::uint64_t s = g_red_same[k].exchange(0, std::memory_order_relaxed);
        std::printf("[counters]   gl repeat %-22s %10llu calls, %10llu same as set (%llu%%)\n", kName[k],
                    (unsigned long long)c, (unsigned long long)s,
                    (unsigned long long)(c ? s * 100 / c : 0));
    }
}
#endif

// The driver's extensions a GLES2 guest may use through what the host offers:
// only those with no entry points of their own (formats, shader and texture
// features). An extension with functions is left out, since eglGetProcAddress
// offers none, and a guest that saw it would call NULL.
static const char* Gles2Extensions(const char* driver) {
    static const std::set<std::string> kNoEntryPoints = {
        "GL_OES_compressed_ETC1_RGB8_texture", "GL_OES_compressed_ETC2_RGB8_texture",
        "GL_OES_compressed_ETC2_RGBA8_texture", "GL_OES_compressed_ETC2_sRGB8_texture",
        "GL_OES_compressed_ETC2_sRGB8_alpha8_texture", "GL_OES_compressed_ETC2_punchthroughA_RGBA8_texture",
        "GL_OES_compressed_ETC2_punchthroughA_sRGB8_alpha_texture", "GL_OES_compressed_EAC_R11_unsigned_texture",
        "GL_OES_compressed_EAC_R11_signed_texture", "GL_OES_compressed_EAC_RG11_unsigned_texture",
        "GL_OES_compressed_EAC_RG11_signed_texture", "GL_KHR_texture_compression_astc_ldr",
        "GL_EXT_texture_compression_dxt1", "GL_EXT_texture_compression_s3tc", "GL_EXT_texture_compression_s3tc_srgb",
        "GL_OES_depth_texture", "GL_OES_depth24", "GL_OES_depth32", "GL_OES_packed_depth_stencil",
        "GL_OES_stencil8", "GL_OES_rgb8_rgba8", "GL_ARM_rgba8", "GL_OES_standard_derivatives",
        "GL_OES_texture_float", "GL_OES_texture_float_linear", "GL_OES_texture_half_float",
        "GL_OES_texture_half_float_linear", "GL_OES_element_index_uint", "GL_OES_texture_npot",
        "GL_OES_vertex_half_float", "GL_OES_fragment_precision_high", "GL_EXT_texture_filter_anisotropic",
        "GL_EXT_texture_format_BGRA8888", "GL_EXT_read_format_bgra", "GL_EXT_texture_rg", "GL_EXT_sRGB",
        "GL_EXT_color_buffer_half_float", "GL_EXT_color_buffer_float", "GL_EXT_shadow_samplers",
        "GL_EXT_frag_depth", "GL_EXT_shader_texture_lod", "GL_EXT_shader_framebuffer_fetch",
        "GL_EXT_blend_minmax", "GL_EXT_texture_type_2_10_10_10_REV", "GL_EXT_texture_sRGB_decode"};
    static std::mutex mu;
    static std::string kept;
    std::lock_guard<std::mutex> l(mu);
    if (kept.empty() && driver) {
        std::string word;
        for (const char* p = driver;; p++) {
            if (*p == ' ' || *p == '\0') {
                if (!word.empty() && kNoEntryPoints.count(word)) {
                    kept += (kept.empty() ? "" : " ") + word;
                }
                word.clear();
                if (*p == '\0') {
                    break;
                }
            } else {
                word += *p;
            }
        }
        std::printf("[realgl] GL_EXTENSIONS for the guest: %s\n", kept.c_str());
    }
    return kept.c_str();
}

bool RealGL::Handle(GlId id, const char* name, RealGL::Regs& regs, std::uint32_t a0, std::uint32_t a1,
                    std::uint32_t a2, std::uint32_t a3) {
    if (!Ensure()) {
        return false;
    }
    // No lock on the call path: GL state belongs to each thread's context
    // and the driver keeps it. RealGL's own shared state is the glGetString
    // slots, which take mu_ (minimum F3: a lock on every GL call guarded
    // nothing else).
    Runtime* r = rt_;
    const std::uint32_t sp = regs[13];
    using G = GlId;  // the import's load-time id (gl_ids.h); name is for logs only
    // A guest pointer the call needs that is not guest memory: a real driver
    // would fault there, so the guest halts with a LOUD line (never a quiet
    // skip that leaves the caller's variable unwritten).
    auto bad = [&](std::uint32_t p) {
        std::printf("[realgl] %s LOUD BAD-PTR %08x -- halting\n", name, p);
        r->HaltGuest();
        r->Ret(regs, 0);
        return true;
    };
#ifdef ABR_DIAG_COUNTERS
    DiagGlRedundant(r, id, a0, a1, a2, a3, sp);
#endif

    // --- state (direct forward, scalar args only) ---
    if (id == G::glEnable || id == G::glDisable) {
        (id == G::glEnable ? glEnable : glDisable)((GLenum)a0);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glBlendFunc) {
        glBlendFunc((GLenum)a0, (GLenum)a1);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glBlendEquation || id == G::glDepthFunc || id == G::glCullFace ||
        id == G::glFrontFace) {
        if (id == G::glBlendEquation) {
            glBlendEquation((GLenum)a0);
        } else if (id == G::glDepthFunc) {
            glDepthFunc((GLenum)a0);
        } else if (id == G::glCullFace) {
            glCullFace((GLenum)a0);
        } else {
            glFrontFace((GLenum)a0);
        }
        r->Ret(regs, 0);
        return true;
    }
    // --- blend, stencil, masks, depth/polygon state (scalar args; floats
    //     arrive as their bits in core registers, softfp) ---
    if (id == G::glBlendEquationSeparate) {
        glBlendEquationSeparate((GLenum)a0, (GLenum)a1);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glBlendFuncSeparate) {
        glBlendFuncSeparate((GLenum)a0, (GLenum)a1, (GLenum)a2, (GLenum)a3);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glColorMask) {
        glColorMask((GLboolean)a0, (GLboolean)a1, (GLboolean)a2, (GLboolean)a3);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glClearDepthf || id == G::glPolygonOffset || id == G::glDepthRangef) {
        float f0 = 0, f1 = 0;
        std::memcpy(&f0, &a0, 4);
        std::memcpy(&f1, &a1, 4);
        if (id == G::glClearDepthf) {
            glClearDepthf(f0);
        } else if (id == G::glDepthRangef) {
            glDepthRangef(f0, f1);
        } else {
            glPolygonOffset(f0, f1);
        }
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glClearStencil || id == G::glStencilMask) {
        if (id == G::glClearStencil) {
            glClearStencil((GLint)a0);
        } else {
            glStencilMask((GLuint)a0);
        }
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glStencilFunc || id == G::glStencilOp) {
        if (id == G::glStencilFunc) {
            glStencilFunc((GLenum)a0, (GLint)a1, (GLuint)a2);
        } else {
            glStencilOp((GLenum)a0, (GLenum)a1, (GLenum)a2);
        }
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glStencilFuncSeparate || id == G::glStencilOpSeparate) {
        if (id == G::glStencilFuncSeparate) {
            glStencilFuncSeparate((GLenum)a0, (GLenum)a1, (GLint)a2, (GLuint)a3);
        } else {
            glStencilOpSeparate((GLenum)a0, (GLenum)a1, (GLenum)a2, (GLenum)a3);
        }
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glGenerateMipmap) {
        glGenerateMipmap((GLenum)a0);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glCheckFramebufferStatus || id == G::glCheckFramebufferStatusOES) {
        if (id == G::glCheckFramebufferStatusOES && EnsureGles1() && g_gles1.glCheckFramebufferStatusOES) {
            r->Ret(regs, (std::uint32_t)g_gles1.glCheckFramebufferStatusOES((GLenum)a0));
        } else {
            r->Ret(regs, (std::uint32_t)glCheckFramebufferStatus((GLenum)a0));
        }
        return true;
    }
    if (id == G::glGetFramebufferAttachmentParameteriv || id == G::glGetVertexAttribiv) {
        // (target, attachment, pname, params*) / (index, pname, params*).
        // GL_CURRENT_VERTEX_ATTRIB answers four values, every other pname one.
        const bool fb = (id == G::glGetFramebufferAttachmentParameteriv);
        const std::uint32_t out = fb ? a3 : a2;
        const std::uint32_t count = (!fb && a1 == GL_CURRENT_VERTEX_ATTRIB) ? 4u : 1u;
        if (!r->CheckRange(out, count * 4u, true)) {
            return bad(out);
        }
        GLint v[4] = {0, 0, 0, 0};
        if (fb) {
            glGetFramebufferAttachmentParameteriv((GLenum)a0, (GLenum)a1, (GLenum)a2, v);
        } else {
            glGetVertexAttribiv((GLuint)a0, (GLenum)a1, v);
        }
        std::memcpy(r->H(out), v, count * 4u);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glGetVertexAttribPointerv) {
        // (index, pname, void** pointer): a client array was passed to the
        // driver as a host address inside guest memory -- hand back the guest
        // address; a buffer offset is returned as it was given.
        if (!r->CheckRange(a2, 4, true)) {
            return bad(a2);
        }
        void* p = nullptr;
        glGetVertexAttribPointerv((GLuint)a0, (GLenum)a1, &p);
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(r->mem().base());
        const std::uintptr_t hp = reinterpret_cast<std::uintptr_t>(p);
        const std::uint32_t g = (hp >= base && hp - base <= 0xFFFFFFFFu)
                                    ? static_cast<std::uint32_t>(hp - base)
                                    : static_cast<std::uint32_t>(hp);
        std::memcpy(r->H(a2), &g, 4);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glDepthMask) {
        glDepthMask((GLboolean)a0);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glViewport) {
        glViewport((GLint)a0, (GLint)a1, (GLsizei)a2, (GLsizei)a3);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glScissor) {
        glScissor((GLint)a0, (GLint)a1, (GLsizei)a2, (GLsizei)a3);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glLineWidth) {
        float w = 0;
        std::memcpy(&w, &a0, 4);
        glLineWidth(w);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glColor4f) {
        float f[4] = {0, 0, 0, 0};
        std::memcpy(&f[0], &a0, 4);
        std::memcpy(&f[1], &a1, 4);
        std::memcpy(&f[2], &a2, 4);
        std::memcpy(&f[3], &a3, 4);
        if (!EnsureGles1() || !g_gles1.glColor4f) {
            std::printf("[realgl] %s LOUD unimplemented GLES 1.1 function -- halting\n", name);
            r->HaltGuest();
            return true;
        }
        g_gles1.glColor4f(f[0], f[1], f[2], f[3]);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glClearColor) {
        float f[4] = {0, 0, 0, 0};
        std::memcpy(&f[0], &a0, 4);
        std::memcpy(&f[1], &a1, 4);
        std::memcpy(&f[2], &a2, 4);
        std::memcpy(&f[3], &a3, 4);
        glClearColor(f[0], f[1], f[2], f[3]);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glClear) {
        glClear((GLbitfield)a0);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glPixelStorei) {
        glPixelStorei((GLenum)a0, (GLint)a1);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glActiveTexture) {
        glActiveTexture((GLenum)a0);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glFlush || id == G::glFinish) {
        (id == G::glFlush ? glFlush : glFinish)();
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glGetError) {
        GLenum e = glGetError();
        if (e) {
            std::printf("[realgl] %s -> %04x\n", name, e);
        }
        r->Ret(regs, (std::uint32_t)e);
        return true;
    }
    // --- shaders/programs (real compile; \\x01 header separators sanitized:
    // 2014 Mali/Adreno accepted them, strict compilers need newlines) ---
    if (id == G::glCreateShader) {
        GLuint s = glCreateShader((GLenum)a0);
        if (GlDrawLog()) std::printf("[realgl] %s(%04x) -> %u\n", name, a0, s);
        r->Ret(regs, s);
        return true;
    }
    if (id == G::glCreateProgram) {
        GLuint p = glCreateProgram();
        if (GlDrawLog()) std::printf("[realgl] %s -> %u\n", name, p);
        r->Ret(regs, p);
        return true;
    }
    if (id == G::glShaderSource) {
        // (shader, count, const char** strings, const GLint* lengths): the
        // driver gets every part in one call, as the guest gave them (a
        // Telltale shader is a #define header plus the body, 3-5 parts).
        GLuint s = (GLuint)a0;
        const std::uint32_t cnt = a1;
        bool ok = cnt <= 256 && r->CheckRange(a2, 4 * cnt, false) && (!a3 || r->CheckRange(a3, 4 * cnt, false));
        std::vector<std::string> parts;
        for (std::uint32_t i = 0; ok && i < cnt; i++) {
            std::uint32_t sptr = 0;
            std::memcpy(&sptr, r->H(a2 + i * 4), 4);
            std::int32_t slen = -1;
            if (a3) {
                std::memcpy(&slen, r->H(a3 + i * 4), 4);
            }
            if (!sptr || !r->CheckRange(sptr, 1, false)) {
                ok = slen == 0 && sptr;  // an empty part needs no bytes
                if (ok) {
                    parts.emplace_back();
                }
                continue;
            }
            std::size_t L = 0;
            if (slen < 0) {
                const char* t = reinterpret_cast<const char*>(r->H(sptr));
                while (L < (1u << 20) && r->CheckRange(sptr + (std::uint32_t)L, 1, false) && t[L]) {
                    L++;
                }
            } else {
                L = (std::size_t)slen;
            }
            if (L && !r->CheckRange(sptr, L, false)) {
                ok = false;
                break;
            }
            std::string src(reinterpret_cast<const char*>(r->H(sptr)), L);
            for (char& c : src) {
                if (c == '\x01') {
                    c = '\n';
                }
            }
            parts.push_back(std::move(src));
        }
        if (!ok) {  // a wild guest pointer: LOUD + halt (AGENTS §6, E-07)
            std::printf("[realgl] %s LOUD BAD-PTR shader=%u parts=%u -- halting\n", name, s, cnt);
            r->HaltGuest();
            r->Ret(regs, 0);
            return true;
        }
        std::vector<const char*> ptrs;
        std::vector<GLint> lens;
        for (const std::string& p : parts) {
            ptrs.push_back(p.c_str());
            lens.push_back((GLint)p.size());
        }
        // The driver copies the strings before returning.
        glShaderSource(s, (GLsizei)parts.size(), ptrs.data(), lens.data());
        if (DiagOn()) {
            std::printf("[realgl] %s shader=%u parts=%u ok\n", name, s, cnt);
        }
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glGetShaderSource) {
        // (shader, bufSize, GLsizei* length, char* source)
        const GLsizei size = (GLsizei)a1;
        if (size < 0 || (a2 && !r->CheckRange(a2, 4, true)) || (size > 0 && !r->CheckRange(a3, (std::uint32_t)size, true))) {
            std::printf("[realgl] %s LOUD BAD-PTR -- halting\n", name);
            r->HaltGuest();
            r->Ret(regs, 0);
            return true;
        }
        std::vector<char> buf(size > 0 ? (std::size_t)size : 1);
        GLsizei len = 0;
        glGetShaderSource((GLuint)a0, size, &len, buf.data());
        if (size > 0) {
            std::memcpy(r->H(a3), buf.data(), (std::size_t)std::min<GLsizei>(len + 1, size));
        }
        if (a2) {
            std::memcpy(r->H(a2), &len, 4);
        }
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glCompileShader) {
        glCompileShader((GLuint)a0);
        GLint ok = 0;
        glGetShaderiv((GLuint)a0, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetShaderInfoLog((GLuint)a0, sizeof(log), nullptr, log);
            std::printf("[realgl] %s(%u) FAIL: %s\n", name, a0, log);
        }
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glAttachShader || id == G::glDetachShader) {
        if (id == G::glAttachShader) {
            glAttachShader((GLuint)a0, (GLuint)a1);
        } else {
            glDetachShader((GLuint)a0, (GLuint)a1);
        }
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glLinkProgram || id == G::glValidateProgram) {
        if (id == G::glLinkProgram) {
            glLinkProgram((GLuint)a0);
            g_prog_gen.fetch_add(1, std::memory_order_release);  // values reset
        } else {
            glValidateProgram((GLuint)a0);
        }
        GLint ok = 0;
        glGetProgramiv((GLuint)a0, id == G::glLinkProgram ? GL_LINK_STATUS : GL_VALIDATE_STATUS,
                       &ok);
        if (!ok) {
            char log[1024];
            glGetProgramInfoLog((GLuint)a0, sizeof(log), nullptr, log);
            std::printf("[realgl] %s(%u) FAIL: %s\n", name, a0, log);
        } else {
            if (GlDrawLog()) std::printf("[realgl] %s(%u) ok\n", name, a0);
        }
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glUseProgram) {
        glUseProgram((GLuint)a0);
        UniformProgramChanged();
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glDeleteShader || id == G::glDeleteProgram) {
        if (id == G::glDeleteShader) {
            glDeleteShader((GLuint)a0);
        } else {
            glDeleteProgram((GLuint)a0);
            g_prog_gen.fetch_add(1, std::memory_order_release);  // the name may come back
        }
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glGetShaderiv || id == G::glGetProgramiv) {
        // (obj, pname, params): forward REAL status.
        if (!r->CheckRange(a2, 4, true)) {
            return bad(a2);
        }
        GLint v = 0;
        if (id == G::glGetShaderiv) {
            glGetShaderiv((GLuint)a0, (GLenum)a1, &v);
        } else {
            glGetProgramiv((GLuint)a0, (GLenum)a1, &v);
        }
        std::uint32_t w = (std::uint32_t)v;
        std::memcpy(r->H(a2), &w, 4);
        if (GlDrawLog()) std::printf("[realgl] %s(%u,%04x) -> %d\n", name, a0, a1, v);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glGetShaderInfoLog || id == G::glGetProgramInfoLog) {
        // (obj, bufSize, length*, infoLog*): forward REAL log.
        // length may be NULL (gl2.h); the log is fetched all the same (it
        // was skipped then).
        std::uint32_t lenptr = 0;
        if (a2 && !r->CheckRange(a2, 4, true)) {
            return bad(a2);
        }
        if ((GLsizei)a1 > 0 && !r->CheckRange(a3, a1, true)) {
            return bad(a3);
        }
        if ((GLsizei)a1 > 0) {
            if (id == G::glGetShaderInfoLog) {
                glGetShaderInfoLog((GLuint)a0, (GLsizei)a1, (GLsizei*)&lenptr, (char*)r->H(a3));
            } else {
                glGetProgramInfoLog((GLuint)a0, (GLsizei)a1, (GLsizei*)&lenptr, (char*)r->H(a3));
            }
        }
        if (a2) {
            std::memcpy(r->H(a2), &lenptr, 4);
        }
        if (GlDrawLog()) std::printf("[realgl] %s(%u) len=%u\n", name, a0, lenptr);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glGetAttribLocation) {
        std::string s = GStr(r, a1);
        GLint loc = glGetAttribLocation((GLuint)a0, s.c_str());
        if (GlDrawLog()) std::printf("[realgl] %s(%u,\"%s\") -> %d\n", name, a0, s.c_str(), loc);
        r->Ret(regs, (std::uint32_t)(std::int32_t)loc);
        return true;
    }
    if (id == G::glGetUniformLocation) {
        std::string s = GStr(r, a1);
        GLint loc = glGetUniformLocation((GLuint)a0, s.c_str());
        if (GlDrawLog()) std::printf("[realgl] %s(%u,\"%s\") -> %d\n", name, a0, s.c_str(), loc);
        r->Ret(regs, (std::uint32_t)(std::int32_t)loc);
        return true;
    }
    if (id == G::glGetActiveUniform || id == G::glGetActiveAttrib) {
        // regs: (prog,index,bufSize,length); stack: size,type,name.
        std::uint32_t blen = a2, lenp = a3, sizep = Stk(r, sp, 0), typep = Stk(r, sp, 1),
                      namep = Stk(r, sp, 2);
        GLsizei len = 0;
        GLint size = 0;
        GLenum type = 0;
        if ((GLsizei)blen > 0 && !r->CheckRange(namep, blen, true)) {
            return bad(namep);
        }
        for (std::uint32_t out : {lenp, sizep, typep}) {
            if (out && !r->CheckRange(out, 4, true)) {
                return bad(out);
            }
        }
        char* nm = (GLsizei)blen > 0 ? (char*)r->H(namep) : nullptr;
        if (id == G::glGetActiveUniform) {
            glGetActiveUniform((GLuint)a0, (GLuint)a1, (GLsizei)blen, &len, &size, &type, nm);
        } else {
            glGetActiveAttrib((GLuint)a0, (GLuint)a1, (GLsizei)blen, &len, &size, &type, nm);
        }
        if (lenp) {
            std::memcpy(r->H(lenp), &len, 4);
        }
        if (sizep) {
            std::memcpy(r->H(sizep), &size, 4);
        }
        if (typep) {
            std::memcpy(r->H(typep), &type, 4);
        }
        if (GlDrawLog()) std::printf("[realgl] %s(%u,%u) -> \"%s\"\n", name, a0, a1, nm ? nm : "?");
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glBindAttribLocation) {
        glBindAttribLocation((GLuint)a0, (GLuint)a1, GStr(r, a2).c_str());
        r->Ret(regs, 0);
        return true;
    }
    // --- textures: real objects + real RGBA data (game decodes .pvr itself;
    // observed uploads are RGBA4444/8888, host-native) ---
    if (id == G::glGenTextures || id == G::glGenBuffers || id == G::glGenFramebuffers ||
        id == G::glGenFramebuffersOES || id == G::glGenRenderbuffers || id == G::glGenRenderbuffersOES ||
        id == G::glDeleteTextures || id == G::glDeleteBuffers || id == G::glDeleteFramebuffers ||
        id == G::glDeleteFramebuffersOES || id == G::glDeleteRenderbuffers || id == G::glDeleteRenderbuffersOES) {
        // All eight names are "gl<Gen|Delete>...": the G/D switch is at index 2
        // (index 3 is 'e' in both, so n[3] was always false -> every delete ran
        // as a gen: leaked objects + the stale-buffer purge below never ran).
        const bool is_del = id == G::glDeleteTextures || id == G::glDeleteBuffers ||
                            id == G::glDeleteFramebuffers || id == G::glDeleteFramebuffersOES ||
                            id == G::glDeleteRenderbuffers || id == G::glDeleteRenderbuffersOES;
        // 64-bit multiply (was 32-bit 4*a0: wraps at n~0x40000000 -> 0-byte
        // check passes -> driver writes past the guest buffer). Cap 1M (game
        // gens handfuls; wrap/garbage caught) + LOUD instead of smash.
        const std::uint64_t bytes64 = (std::uint64_t)a0 * 4;
        if (a0 >= 0x100000u) {
            std::printf("[realgl] %s LOUD n=%u rejected\n", name, a0);
            r->Ret(regs, 0);
            return true;
        }
        if (!r->CheckRange(a1, (std::size_t)bytes64, !is_del)) {
            return bad(a1);
        }
        if (id == G::glGenTextures || id == G::glDeleteTextures) {
            is_del ? glDeleteTextures((GLsizei)a0, (const GLuint*)r->H(a1))
                   : glGenTextures((GLsizei)a0, (GLuint*)r->H(a1));
        } else if (id == G::glGenBuffers || id == G::glDeleteBuffers) {
            if (is_del) {
                ForgetGlBindings();  // deleting a bound buffer unbinds it
            }
            is_del ? glDeleteBuffers((GLsizei)a0, (const GLuint*)r->H(a1))
                   : glGenBuffers((GLsizei)a0, (GLuint*)r->H(a1));
        } else if (id == G::glGenFramebuffers || id == G::glGenFramebuffersOES ||
                   id == G::glDeleteFramebuffers || id == G::glDeleteFramebuffersOES) {
            if (id == G::glGenFramebuffersOES && EnsureGles1() && g_gles1.glGenFramebuffersOES) {
                g_gles1.glGenFramebuffersOES((GLsizei)a0, (GLuint*)r->H(a1));
            } else if (id == G::glDeleteFramebuffersOES && EnsureGles1() && g_gles1.glDeleteFramebuffersOES) {
                g_gles1.glDeleteFramebuffersOES((GLsizei)a0, (const GLuint*)r->H(a1));
            } else {
                is_del ? glDeleteFramebuffers((GLsizei)a0, (const GLuint*)r->H(a1))
                       : glGenFramebuffers((GLsizei)a0, (GLuint*)r->H(a1));
            }
        } else {
            if (id == G::glGenRenderbuffersOES && EnsureGles1() && g_gles1.glGenRenderbuffersOES) {
                g_gles1.glGenRenderbuffersOES((GLsizei)a0, (GLuint*)r->H(a1));
            } else if (id == G::glDeleteRenderbuffersOES && EnsureGles1() && g_gles1.glDeleteRenderbuffersOES) {
                g_gles1.glDeleteRenderbuffersOES((GLsizei)a0, (const GLuint*)r->H(a1));
            } else {
                is_del ? glDeleteRenderbuffers((GLsizei)a0, (const GLuint*)r->H(a1))
                       : glGenRenderbuffers((GLsizei)a0, (GLuint*)r->H(a1));
            }
        }
        if (GlDrawLog()) std::printf("[realgl] %s(%u) ok\n", name, a0);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glBindFramebuffer || id == G::glBindFramebufferOES ||
        id == G::glBindRenderbuffer || id == G::glBindRenderbufferOES) {
        if (id == G::glBindFramebufferOES && EnsureGles1() && g_gles1.glBindFramebufferOES) {
            g_gles1.glBindFramebufferOES((GLenum)a0, (GLuint)a1);
        } else if (id == G::glBindFramebuffer || id == G::glBindFramebufferOES) {
            glBindFramebuffer((GLenum)a0, (GLuint)a1);
        } else if (id == G::glBindRenderbufferOES && EnsureGles1() && g_gles1.glBindRenderbufferOES) {
            g_gles1.glBindRenderbufferOES((GLenum)a0, (GLuint)a1);
        } else {
            glBindRenderbuffer((GLenum)a0, (GLuint)a1);
        }
        if (GlDrawLog()) std::printf("[realgl] %s(%04x,%u) ok\n", name, a0, a1);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glFramebufferRenderbuffer || id == G::glFramebufferRenderbufferOES) {
        // (target, attachment, renderbuffertarget, renderbuffer)
        if (id == G::glFramebufferRenderbufferOES && EnsureGles1() && g_gles1.glFramebufferRenderbufferOES) {
            g_gles1.glFramebufferRenderbufferOES((GLenum)a0, (GLenum)a1, (GLenum)a2, (GLuint)a3);
        } else {
            glFramebufferRenderbuffer((GLenum)a0, (GLenum)a1, (GLenum)a2, (GLuint)a3);
        }
        if (GlDrawLog()) std::printf("[realgl] %s(%04x,%04x,%04x,%u) ok\n", name, a0, a1, a2, a3);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glFramebufferTexture2D || id == G::glFramebufferTexture2DOES) {
        // (target, attachment, textarget, texture, level) -- the level is
        // the first stack word.
        const std::uint32_t level = Stk(r, sp, 0);
        if (id == G::glFramebufferTexture2DOES && EnsureGles1() && g_gles1.glFramebufferTexture2DOES) {
            g_gles1.glFramebufferTexture2DOES((GLenum)a0, (GLenum)a1, (GLenum)a2, (GLuint)a3, (GLint)level);
        } else {
            glFramebufferTexture2D((GLenum)a0, (GLenum)a1, (GLenum)a2, (GLuint)a3, (GLint)level);
        }
        if (GlDrawLog()) {
            std::printf("[realgl] %s(%04x,%04x,%04x,%u,%u) ok\n", name, a0, a1, a2, a3, level);
        }
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glRenderbufferStorage || id == G::glRenderbufferStorageOES) {
        // (target, internalformat, width, height)
        if (id == G::glRenderbufferStorageOES && EnsureGles1() && g_gles1.glRenderbufferStorageOES) {
            g_gles1.glRenderbufferStorageOES((GLenum)a0, (GLenum)a1, (GLsizei)a2, (GLsizei)a3);
        } else {
            glRenderbufferStorage((GLenum)a0, (GLenum)a1, (GLsizei)a2, (GLsizei)a3);
        }
        if (GlDrawLog()) std::printf("[realgl] %s(%04x,%04x,%u,%u) ok\n", name, a0, a1, a2, a3);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glGetUniformfv) {
        // glGetUniformfv(program, location, params) writes ONE value of the
        // uniform's type (a mat4 = 16 floats). GLES2 gives no count, so find
        // the active uniform that owns this location (glGetActiveUniform
        // takes an INDEX, not a location; for an array uniform "u[0]" the
        // locations are base..base+size-1) and use its component count.
        GLint nact = 0;
        glGetProgramiv((GLuint)a0, GL_ACTIVE_UNIFORMS, &nact);
        GLenum found_type = 0;
        char nm[256];
        for (GLint i = 0; i < nact && found_type == 0; i++) {
            GLint size = 0, len = 0;
            GLenum type = 0;
            glGetActiveUniform((GLuint)a0, (GLuint)i, sizeof(nm), &len, &size, &type, nm);
            const GLint base = glGetUniformLocation((GLuint)a0, nm);
            if (base >= 0 && (GLint)a1 >= base && (GLint)a1 < base + size) {
                found_type = type;
            }
        }
        std::size_t comps = 0;
        switch (found_type) {
            case GL_FLOAT: case GL_INT: case GL_BOOL: case GL_SAMPLER_2D: case GL_SAMPLER_CUBE:
                comps = 1; break;
            case GL_FLOAT_VEC2: case GL_INT_VEC2: case GL_BOOL_VEC2: comps = 2; break;
            case GL_FLOAT_VEC3: case GL_INT_VEC3: case GL_BOOL_VEC3: comps = 3; break;
            case GL_FLOAT_VEC4: case GL_INT_VEC4: case GL_BOOL_VEC4: case GL_FLOAT_MAT2:
                comps = 4; break;
            case GL_FLOAT_MAT3: comps = 9; break;
            case GL_FLOAT_MAT4: comps = 16; break;
            default: comps = 0; break;
        }
        if (comps == 0) {
            std::printf("[realgl] %s LOUD no active uniform at program %u location %d (type %04x)\n", name,
                        a0, (int)a1, found_type);
            r->Ret(regs, 0);
            return true;
        }
        if (!r->CheckRange(a2, comps * 4, true)) {
            return bad(a2);
        }
        GLfloat vals[16] = {};  // the largest GLES2 uniform value: mat4
        glGetUniformfv((GLuint)a0, (GLint)a1, vals);
        std::memcpy(r->H(a2), vals, comps * 4);
        if (GlDrawLog()) {
            std::printf("[realgl] %s(%u,%d) type=%04x comps=%zu ok\n", name, a0, (int)a1, found_type, comps);
        }
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glBindTexture) {
        glBindTexture((GLenum)a0, (GLuint)a1);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glTexParameteri || id == G::glTexParameterf) {
        if (id == G::glTexParameteri) {
            glTexParameteri((GLenum)a0, (GLenum)a1, (GLint)a2);
        } else {
            float f = 0;
            std::memcpy(&f, &a2, 4);
            glTexParameterf((GLenum)a0, (GLenum)a1, f);
        }
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glTexImage2D) {
        // regs: target,level,intfmt,width; stack: height,border,format,type,data.
        // Bulk range check (was 1 byte: driver reads w*h*bpp past short
        // buffers -> host SIGSEGV, not a clean guest halt). Short/invalid ->
        // NULL (black texture) + LOUD instead of death.
        std::uint32_t h = Stk(r, sp, 0), fmt = Stk(r, sp, 2), typ = Stk(r, sp, 3),
                      dat = Stk(r, sp, 4);
        const std::uint64_t need = ImageBytes(a3, h, PixelBytes(fmt, typ), GL_UNPACK_ALIGNMENT);
        const void* p = nullptr;
        if (dat && need > 0 && need < 0x10000000u &&
            r->CheckRange(dat, (std::size_t)need, false)) {
            p = (const void*)r->H(dat);
        } else if (dat && need == 0 && r->CheckRange(dat, 1, false)) {
            p = (const void*)r->H(dat);  // unknown fmt/typ: legacy probe
        } else if (dat) {
            std::printf("[realgl] %s LOUD SHORTBUF %ux%u fmt=%04x typ=%04x need=%llu -- halting\n", name,
                        a3, h, fmt, typ, (unsigned long long)need);
            r->HaltGuest();
            r->Ret(regs, 0);
            return true;
        }
        glTexImage2D((GLenum)a0, (GLint)a1, (GLint)a2, (GLsizei)a3, (GLsizei)h, 0, (GLenum)fmt,
                     (GLenum)typ, p);
        // Don't consume the driver's error queue on the fast path: the game
        // polls glGetError itself for the ETC->soft-decode fallback, and an
        // intermediate query eats its signal. Query only for diag.
        if (GlDrawLog()) {
            const GLenum e = glGetError();
            std::printf("[realgl] %s %ux%u fmt=%04x typ=%04x %s err=%04x\n", name, a3, h, fmt, typ,
                        p ? "data" : "NULL", e);
        }
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glTexSubImage2D) {
        // regs: target,level,xoff,yoff; stack: w,h,format,type,data.
        // Same bulk-check reasoning as TexImage2D.
        std::uint32_t w = Stk(r, sp, 0), h = Stk(r, sp, 1), fmt = Stk(r, sp, 2), typ = Stk(r, sp, 3),
                      dat = Stk(r, sp, 4);
        const std::uint64_t need = ImageBytes(w, h, PixelBytes(fmt, typ), GL_UNPACK_ALIGNMENT);
        const void* p = nullptr;
        if (dat && need > 0 && need < 0x10000000u &&
            r->CheckRange(dat, (std::size_t)need, false)) {
            p = (const void*)r->H(dat);
        } else if (dat && need == 0 && r->CheckRange(dat, 1, false)) {
            p = (const void*)r->H(dat);
        } else if (dat) {
            std::printf("[realgl] %s LOUD SHORTBUF %ux%u fmt=%04x typ=%04x need=%llu -- halting\n", name,
                        w, h, fmt, typ, (unsigned long long)need);
            r->HaltGuest();
            r->Ret(regs, 0);
            return true;
        }
        glTexSubImage2D((GLenum)a0, (GLint)a1, (GLint)a2, (GLint)a3, (GLsizei)w, (GLsizei)h,
                        (GLenum)fmt, (GLenum)typ, p);
        // Same error-queue reasoning as TexImage2D (game-owned signal).
        if (GlDrawLog()) {
            const GLenum e = glGetError();
            std::printf("[realgl] %s %ux%u fmt=%04x typ=%04x %s err=%04x\n", name, w, h, fmt, typ,
                        p ? "data" : "NULL", e);
        }
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glCompressedTexImage2D || id == G::glCompressedTexSubImage2D) {
        // Real forwarding (llvmpipe accepts ETC1; NVIDIA errors -> game falls
        // back to software decode). Args: regs(target,level,intfmt,w,h,border)
        // + stack(imageSize,data) for Tex; (target,level,x,y,w,h,format,
        // imageSize,data) for Sub (regs target,level,x,y + stack w,h,fmt,size,data).
        if (id == G::glCompressedTexImage2D) {
            // regs: target,level,intfmt,width; stack: height,border,imageSize,data.
            std::uint32_t h = Stk(r, sp, 0), size = Stk(r, sp, 2), dat = Stk(r, sp, 3);
            if (dat && size && !r->CheckRange(dat, size, false)) {
                return bad(dat);
            }
            const void* p = (dat && size) ? (const void*)r->H(dat) : nullptr;
            glCompressedTexImage2D((GLenum)a0, (GLint)a1, (GLenum)a2, (GLsizei)a3, (GLsizei)h, 0,
                                   (GLsizei)size, p);
            // Game-owned error signal (ETC fallback); query + print for diag only.
            if (GlDrawLog()) {
                GLenum e = glGetError();
                std::printf("[realgl] %s ifmt=%04x %ux%u %uB %s err=%04x\n", name, a2, a3, h, size,
                            p ? "data" : "NULL", e);
            }
        } else {
            // regs: target,level,xoff,yoff; stack: w,h,format,imageSize,data.
            std::uint32_t w = Stk(r, sp, 0), h = Stk(r, sp, 1), fmt = Stk(r, sp, 2),
                          size = Stk(r, sp, 3), dat = Stk(r, sp, 4);
            if (dat && size && !r->CheckRange(dat, size, false)) {
                return bad(dat);
            }
            const void* p = (dat && size) ? (const void*)r->H(dat) : nullptr;
            glCompressedTexSubImage2D((GLenum)a0, (GLint)a1, (GLint)a2, (GLint)a3, (GLsizei)w,
                                      (GLsizei)h, (GLenum)fmt, (GLsizei)size, p);
            // Same game-owned-signal reasoning as the Tex branch.
            if (GlDrawLog()) {
                GLenum e = glGetError();
                std::printf("[realgl] %s %ux%u fmt=%04x %uB %s err=%04x\n", name, w, h, fmt, size,
                            p ? "data" : "NULL", e);
            }
        }
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glCopyTexImage2D || id == G::glCopyTexSubImage2D) {
        // Eight scalar args: four in r0-r3, four on the stack (gl2.h).
        //   CopyTexImage2D(target, level, internalformat, x | y, w, h, border)
        //   CopyTexSubImage2D(target, level, xoff, yoff | x, y, w, h)
        std::uint32_t st[4] = {0, 0, 0, 0};
        if (!r->CheckRange(sp, sizeof(st), false)) {
            return bad(sp);  // the stack arguments
        }
        std::memcpy(st, r->H(sp), sizeof(st));
        if (id == G::glCopyTexImage2D) {
            glCopyTexImage2D((GLenum)a0, (GLint)a1, (GLenum)a2, (GLint)a3, (GLint)st[0],
                             (GLsizei)st[1], (GLsizei)st[2], (GLint)st[3]);
        } else {
            glCopyTexSubImage2D((GLenum)a0, (GLint)a1, (GLint)a2, (GLint)a3, (GLint)st[0],
                                (GLint)st[1], (GLsizei)st[2], (GLsizei)st[3]);
        }
        r->Ret(regs, 0);
        return true;
    }
    // --- buffers + draws. A vertex/index pointer is a buffer offset when a
    // buffer is bound, else a guest address. The binding is asked from the
    // driver at the point of use: bindings are per-context state (GLES2 2.9),
    // and Android recreates the context when the app is paused, so a copy
    // kept here could go stale (E-03): the per-thread copy above is dropped
    // wherever the context can change.
    if (id == G::glBindBuffer) {
        // A bind of the buffer already bound changes nothing (GLES2 2.9) and
        // cannot fail for these two targets: it is not sent (MCSM: ~32 %).
        if ((a0 == GL_ARRAY_BUFFER && t_gl.bindings.array_known && t_gl.bindings.array == a1) ||
            (a0 == GL_ELEMENT_ARRAY_BUFFER && t_gl.bindings.element_known && t_gl.bindings.element == a1)) {
            r->Ret(regs, 0);
            return true;
        }
        glBindBuffer((GLenum)a0, (GLuint)a1);
        if (a0 == GL_ARRAY_BUFFER) {
            t_gl.bindings.array = (GLuint)a1;
            t_gl.bindings.array_known = true;
        } else if (a0 == GL_ELEMENT_ARRAY_BUFFER) {
            t_gl.bindings.element = (GLuint)a1;
            t_gl.bindings.element_known = true;
        }
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glBufferData || id == G::glBufferSubData) {
        // (target,size,data,usage) / (target,offset,size,data).
        std::uint32_t size = 0, dat = 0;
        if (id == G::glBufferData) {
            size = a1;
            dat = a2;
        } else {
            // (target,offset,size,data): all in regs.
            size = a2;
            dat = a3;
        }
        if (dat && size && !r->CheckRange(dat, size, false)) {
            return bad(dat);
        }
        const void* p = (dat && size) ? (const void*)r->H(dat) : nullptr;
        if (id == G::glBufferData) {
            glBufferData((GLenum)a0, (GLsizeiptr)size, p, (GLenum)a3);
        } else {
            glBufferSubData((GLenum)a0, (GLintptr)a1, (GLsizeiptr)size, p);
        }
        if (GlDrawLog()) {
            std::printf("[realgl] %s tgt=%04x %uB %s\n", name, a0, size, p ? "data" : "NULL");
        }
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glVertexAttribPointer) {
        // regs: (index,size,type,normalized); stack: stride,pointer.
        // pointer is guest VA unless an array buffer is bound (raw offset).
        std::uint32_t stride = Stk(r, sp, 0), gp = Stk(r, sp, 1);
        const void* p = nullptr;
        const bool vbo = BoundBuffer(GL_ARRAY_BUFFER_BINDING) != 0;
        if (vbo) {
            p = (const void*)(std::uintptr_t)gp;  // VBO offset passthrough
        } else if (gp && r->CheckRange(gp, 1, false)) {
            p = (const void*)r->H(gp);
        }
        glVertexAttribPointer((GLuint)a0, (GLint)a1, (GLenum)a2, (GLboolean)a3, (GLsizei)stride,
                              p);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glEnableVertexAttribArray || id == G::glDisableVertexAttribArray) {
        // Setting a flag to the value it has changes nothing; it is not sent
        // (MCSM disables every array after each draw: ~97 % repeats). Only
        // indices below GL_MAX_VERTEX_ATTRIBS are kept, so a call that must
        // raise GL_INVALID_VALUE always reaches the driver.
        static const GLuint max_attr = [] {
            GLint v = 0;
            glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &v);
            return static_cast<GLuint>(v);
        }();
        const std::int8_t want = id == G::glEnableVertexAttribArray ? 1 : 0;
        const bool kept = a0 < 16 && a0 < max_attr;
        if (kept && t_gl.bindings.attr[a0] == want) {
            r->Ret(regs, 0);
            return true;
        }
        (want ? glEnableVertexAttribArray : glDisableVertexAttribArray)((GLuint)a0);
        if (kept) t_gl.bindings.attr[a0] = want;
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glClientActiveTexture) {
        if (!EnsureGles1() || !g_gles1.glClientActiveTexture) {
            std::printf("[realgl] %s LOUD unimplemented GLES 1.1 function -- halting\n", name);
            r->HaltGuest();
            return true;
        }
        g_gles1.glClientActiveTexture((GLenum)a0);
        const std::uint32_t unit = (a0 >= 0x84C0u && a0 < 0x84C4u) ? (a0 - 0x84C0u) : 0u;
        t_gl.client_active_texture = unit;
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glEnableClientState || id == G::glDisableClientState) {
        const bool want = (id == G::glEnableClientState);
        if (!EnsureGles1() || (want ? !g_gles1.glEnableClientState : !g_gles1.glDisableClientState)) {
            std::printf("[realgl] %s LOUD unimplemented GLES 1.1 function -- halting\n", name);
            r->HaltGuest();
            return true;
        }
        if (want) g_gles1.glEnableClientState((GLenum)a0);
        else g_gles1.glDisableClientState((GLenum)a0);
        switch (a0) {
            case 0x8074: // GL_VERTEX_ARRAY
                t_gl.client_vertex.enabled = want;
                break;
            case 0x8075: // GL_NORMAL_ARRAY
                t_gl.client_normal.enabled = want;
                break;
            case 0x8076: // GL_COLOR_ARRAY
                t_gl.client_color.enabled = want;
                break;
            case 0x8078: { // GL_TEXTURE_COORD_ARRAY
                const std::uint32_t u = t_gl.client_active_texture;
                if (u < 4) t_gl.client_tex_coord[u].enabled = want;
                break;
            }
            default:
                break;
        }
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glVertexPointer) {
        const bool vbo = BoundBuffer(GL_ARRAY_BUFFER_BINDING) != 0;
        const void* p = nullptr;
        if (vbo) {
            p = (const void*)(std::uintptr_t)a3;
        } else if (a3) {
            if (!r->CheckRange(a3, 1, false)) {
                std::printf("[realgl] %s LOUD BADPTR va=%08x -- halting\n", name, a3);
                r->HaltGuest();
                return true;
            }
            p = (const void*)r->H(a3);
        }
        if (!EnsureGles1() || !g_gles1.glVertexPointer) {
            std::printf("[realgl] %s LOUD unimplemented GLES 1.1 function -- halting\n", name);
            r->HaltGuest();
            return true;
        }
        g_gles1.glVertexPointer((GLint)a0, (GLenum)a1, (GLsizei)a2, p);
        t_gl.client_vertex.size = (GLint)a0;
        t_gl.client_vertex.type = (GLenum)a1;
        t_gl.client_vertex.stride = (GLsizei)a2;
        t_gl.client_vertex.va = a3;
        t_gl.client_vertex.vbo = vbo ? 1 : 0;
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glColorPointer) {
        const bool vbo = BoundBuffer(GL_ARRAY_BUFFER_BINDING) != 0;
        const void* p = nullptr;
        if (vbo) {
            p = (const void*)(std::uintptr_t)a3;
        } else if (a3) {
            if (!r->CheckRange(a3, 1, false)) {
                std::printf("[realgl] %s LOUD BADPTR va=%08x -- halting\n", name, a3);
                r->HaltGuest();
                return true;
            }
            p = (const void*)r->H(a3);
        }
        if (!EnsureGles1() || !g_gles1.glColorPointer) {
            std::printf("[realgl] %s LOUD unimplemented GLES 1.1 function -- halting\n", name);
            r->HaltGuest();
            return true;
        }
        g_gles1.glColorPointer((GLint)a0, (GLenum)a1, (GLsizei)a2, p);
        t_gl.client_color.size = (GLint)a0;
        t_gl.client_color.type = (GLenum)a1;
        t_gl.client_color.stride = (GLsizei)a2;
        t_gl.client_color.va = a3;
        t_gl.client_color.vbo = vbo ? 1 : 0;
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glTexCoordPointer) {
        const bool vbo = BoundBuffer(GL_ARRAY_BUFFER_BINDING) != 0;
        const void* p = nullptr;
        if (vbo) {
            p = (const void*)(std::uintptr_t)a3;
        } else if (a3) {
            if (!r->CheckRange(a3, 1, false)) {
                std::printf("[realgl] %s LOUD BADPTR va=%08x -- halting\n", name, a3);
                r->HaltGuest();
                return true;
            }
            p = (const void*)r->H(a3);
        }
        if (!EnsureGles1() || !g_gles1.glTexCoordPointer) {
            std::printf("[realgl] %s LOUD unimplemented GLES 1.1 function -- halting\n", name);
            r->HaltGuest();
            return true;
        }
        g_gles1.glTexCoordPointer((GLint)a0, (GLenum)a1, (GLsizei)a2, p);
        const std::uint32_t u = t_gl.client_active_texture;
        if (u < 4) {
            t_gl.client_tex_coord[u].size = (GLint)a0;
            t_gl.client_tex_coord[u].type = (GLenum)a1;
            t_gl.client_tex_coord[u].stride = (GLsizei)a2;
            t_gl.client_tex_coord[u].va = a3;
            t_gl.client_tex_coord[u].vbo = vbo ? 1 : 0;
        }
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glNormalPointer) {
        const bool vbo = BoundBuffer(GL_ARRAY_BUFFER_BINDING) != 0;
        const void* p = nullptr;
        if (vbo) {
            p = (const void*)(std::uintptr_t)a2;
        } else if (a2) {
            if (!r->CheckRange(a2, 1, false)) {
                std::printf("[realgl] %s LOUD BADPTR va=%08x -- halting\n", name, a2);
                r->HaltGuest();
                return true;
            }
            p = (const void*)r->H(a2);
        }
        if (!EnsureGles1() || !g_gles1.glNormalPointer) {
            std::printf("[realgl] %s LOUD unimplemented GLES 1.1 function -- halting\n", name);
            r->HaltGuest();
            return true;
        }
        g_gles1.glNormalPointer((GLenum)a0, (GLsizei)a1, p);
        t_gl.client_normal.size = 3;
        t_gl.client_normal.type = (GLenum)a0;
        t_gl.client_normal.stride = (GLsizei)a1;
        t_gl.client_normal.va = a2;
        t_gl.client_normal.vbo = vbo ? 1 : 0;
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glDrawArrays || id == G::glDrawElements) {
        if (id == G::glDrawArrays) {
            if (HasClientArrays() && (GLsizei)a2 > 0) {
                const std::uint32_t max_idx = (std::uint32_t)(GLint)a1 + (std::uint32_t)(GLsizei)a2 - 1u;
                if (!ValidateClientArrays(r, name, max_idx)) {
                    return true;
                }
            }
            glDrawArrays((GLenum)a0, (GLint)a1, (GLsizei)a2);
        } else {
            // (mode,count,type,indices): all in regs; offset if an element
            // buffer is bound, guest VA otherwise. Bulk index check (was 1
            // byte: driver reads count*sizeof(type) past short buffers ->
            // host SIGSEGV). Short -> skip draw + LOUD instead of death.
            const void* p = nullptr;
            const bool ibo = BoundBuffer(GL_ELEMENT_ARRAY_BUFFER_BINDING) != 0;
            if (ibo) {
                p = (const void*)(std::uintptr_t)a3;
                if (HasClientArrays()) {
                    std::printf("[realgl] %s LOUD MIXEDIBO element buffer bound with client vertex arrays -- halting\n", name);
                    r->HaltGuest();
                    return true;
                }
            } else if (a3) {
                const std::uint64_t isz = IndexSize(a2);
                const std::uint64_t need = (std::uint64_t)(GLsizei)a1 * isz;
                if (isz > 0 && need < 0x10000000u &&
                    r->CheckRange(a3, (std::size_t)need, false)) {
                    p = (const void*)r->H(a3);
                } else if (isz == 0 && r->CheckRange(a3, 1, false)) {
                    p = (const void*)r->H(a3);  // unknown type: legacy probe
                } else {
                    std::printf("[realgl] %s LOUD SHORTIDX count=%d type=%04x need=%llu -- halting\n", name,
                                (GLsizei)a1, a2, (unsigned long long)need);
                    r->HaltGuest();
                }
            }
            if ((p || ibo) && (GLsizei)a1 > 0) {
                if (p && HasClientArrays()) {
                    std::uint32_t max_idx = 0;
                    if (a2 == 0x1401u) { // GL_UNSIGNED_BYTE
                        const auto* u8 = (const std::uint8_t*)p;
                        for (GLsizei i = 0; i < (GLsizei)a1; i++) if (u8[i] > max_idx) max_idx = u8[i];
                    } else if (a2 == 0x1403u) { // GL_UNSIGNED_SHORT
                        const auto* u16 = (const std::uint16_t*)p;
                        for (GLsizei i = 0; i < (GLsizei)a1; i++) if (u16[i] > max_idx) max_idx = u16[i];
                    } else if (a2 == 0x1405u) { // GL_UNSIGNED_INT
                        const auto* u32 = (const std::uint32_t*)p;
                        for (GLsizei i = 0; i < (GLsizei)a1; i++) if (u32[i] > max_idx) max_idx = u32[i];
                    }
                    if (!ValidateClientArrays(r, name, max_idx)) {
                        return true;
                    }
                }
                glDrawElements((GLenum)a0, (GLsizei)a1, (GLenum)a2, p);
            }
        }
        // Per-draw glGetError stalls the GPU pipeline every draw; diag only.
        if (GlDrawLog()) {
            GLenum e = glGetError();
            std::printf("[realgl] %s err=%04x\n", name, e);
        }
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glMatrixMode) {
        if (!EnsureGles1() || !g_gles1.glMatrixMode) {
            std::printf("[realgl] %s LOUD unimplemented GLES 1.1 function -- halting\n", name);
            r->HaltGuest();
            return true;
        }
        g_gles1.glMatrixMode((GLenum)a0);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glLoadMatrixf) {
        if (!a0 || !r->CheckRange(a0, 16 * sizeof(float), false)) {
            std::printf("[realgl] %s LOUD BADPTR va=%08x -- halting\n", name, a0);
            r->HaltGuest();
            return true;
        }
        if (!EnsureGles1() || !g_gles1.glLoadMatrixf) {
            std::printf("[realgl] %s LOUD unimplemented GLES 1.1 function -- halting\n", name);
            r->HaltGuest();
            return true;
        }
        g_gles1.glLoadMatrixf((const GLfloat*)r->H(a0));
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glLightfv) {
        const int cnt = LightFvCount((GLenum)a1);
        if (!a2 || !r->CheckRange(a2, cnt * sizeof(float), false)) {
            std::printf("[realgl] %s LOUD BADPTR va=%08x count=%d -- halting\n", name, a2, cnt);
            r->HaltGuest();
            return true;
        }
        if (!EnsureGles1() || !g_gles1.glLightfv) {
            std::printf("[realgl] %s LOUD unimplemented GLES 1.1 function -- halting\n", name);
            r->HaltGuest();
            return true;
        }
        g_gles1.glLightfv((GLenum)a0, (GLenum)a1, (const GLfloat*)r->H(a2));
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glMaterialfv) {
        const int cnt = MaterialFvCount((GLenum)a1);
        if (!a2 || !r->CheckRange(a2, cnt * sizeof(float), false)) {
            std::printf("[realgl] %s LOUD BADPTR va=%08x count=%d -- halting\n", name, a2, cnt);
            r->HaltGuest();
            return true;
        }
        if (!EnsureGles1() || !g_gles1.glMaterialfv) {
            std::printf("[realgl] %s LOUD unimplemented GLES 1.1 function -- halting\n", name);
            r->HaltGuest();
            return true;
        }
        g_gles1.glMaterialfv((GLenum)a0, (GLenum)a1, (const GLfloat*)r->H(a2));
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glTexEnvi) {
        if (!EnsureGles1() || !g_gles1.glTexEnvi) {
            std::printf("[realgl] %s LOUD unimplemented GLES 1.1 function -- halting\n", name);
            r->HaltGuest();
            return true;
        }
        g_gles1.glTexEnvi((GLenum)a0, (GLenum)a1, (GLint)a2);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glReadPixels) {
        // (x,y,w,h,format,type,pixels) — x,y,w,h in regs; rest on stack.
        // Bulk write check (was 1 byte: driver writes w*h*bpp into the guest
        // buffer -> heap smash past short buffers). Short -> skip (existing
        // graceful path) + LOUD.
        std::uint32_t fmt = Stk(r, sp, 0), typ = Stk(r, sp, 1), dat = Stk(r, sp, 2);
        const std::uint64_t need =
            (GLsizei)a2 > 0 && (GLsizei)a3 > 0
                ? ImageBytes((std::uint32_t)a2, (std::uint32_t)a3, PixelBytes(fmt, typ),
                             GL_PACK_ALIGNMENT)
                : 0;
        void* p = nullptr;
        if (dat && need > 0 && need < 0x10000000u &&
            r->CheckRange(dat, (std::size_t)need, true)) {
            p = (void*)r->H(dat);
        } else if (dat && need == 0 && r->CheckRange(dat, 1, true)) {
            p = (void*)r->H(dat);  // unknown fmt/typ: legacy probe
        } else if (dat) {
            std::printf("[realgl] %s LOUD SHORTBUF %ux%u fmt=%04x typ=%04x need=%llu -- halting\n", name,
                        a2, a3, fmt, typ, (unsigned long long)need);
            r->HaltGuest();
        }
        if (p) {
            glReadPixels((GLint)a0, (GLint)a1, (GLsizei)a2, (GLsizei)a3, (GLenum)fmt, (GLenum)typ,
                          p);
        }
        r->Ret(regs, 0);
        return true;
    }
    // --- queries: REAL driver answers in real mode ---
    if (id == G::glGetString) {
        // HARNESS_GLVENDOR / HARNESS_GLRENDERER override the driver's
        // strings. The guest has renderer-keyed logic (its texture quirk
        // tables), so the era-correct name matters: without the override the
        // guest sees this device's GPU and may take a different path than
        // on the phone the game shipped for.
        const char* s = nullptr;
        if (a0 == GL_VENDOR) {
            const char* e = std::getenv("HARNESS_GLVENDOR");
            s = e ? e : (const char*)glGetString(GL_VENDOR);
        } else if (a0 == GL_RENDERER) {
            const char* e = std::getenv("HARNESS_GLRENDERER");
            s = e ? e : (const char*)glGetString(GL_RENDERER);
        } else if (a0 == GL_VERSION || a0 == GL_SHADING_LANGUAGE_VERSION) {
            // The host forwards the GLES2 API and no more: a guest told
            // "OpenGL ES 3.2" asks eglGetProcAddress for ES3 entry points it
            // will not get (MCSM: mapped buffers, then a NULL store).
            s = a0 == GL_VERSION ? "OpenGL ES 2.0" : "OpenGL ES GLSL ES 1.00";
        } else if (a0 == GL_EXTENSIONS) {
            s = Gles2Extensions((const char*)glGetString(GL_EXTENSIONS));
        } else {
            s = (const char*)glGetString((GLenum)a0);
        }
        std::uint32_t out = 0;
        if (s) {
            // One stable guest slot per name: the guest keeps the pointer, and
            // the driver string for a live context does not change. Refresh the
            // content in place (grows beyond the slot -> fresh allocation).
            std::size_t L = std::strlen(s) + 1;
            std::lock_guard<std::mutex> l(mu_);  // glstr_slots_
            auto it = glstr_slots_.find(a0);
            if (it != glstr_slots_.end() && r->CheckRange(it->second, L, true)) {
                out = it->second;
            } else {
                out = r->GuestMalloc(static_cast<std::uint32_t>(L));
                if (out) {
                    glstr_slots_[a0] = out;
                }
            }
            if (out && r->CheckRange(out, L, true)) {
                std::memcpy(r->H(out), s, L);
            } else {
                out = 0;
            }
        }
        if (DiagOn()) {
            std::printf("[realgl] %s(%04x) -> \"%s\"\n", name, a0, s ? s : "(null)");
        }
        r->Ret(regs, out);
        return true;
    }
    if (id == G::glGetBufferParameteriv) {
        // (target, pname, params): one GLint for every GLES2 pname.
        if (!r->CheckRange(a2, 4, true)) {
            return bad(a2);
        }
        GLint v = 0;
        glGetBufferParameteriv((GLenum)a0, (GLenum)a1, &v);
        std::memcpy(r->H(a2), &v, 4);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glGetIntegerv || id == G::glGetFloatv || id == G::glGetBooleanv) {
        // (pname, params): forward with the SPEC count per pname (a 24B-dirent
        // lesson: the guest passes exact-size buffers; the old always-16B
        // write smashed neighbors, e.g. the 0x26219C canary slot via a 4B
        // local_2c at glGetIntegerv(0x8B4D) -> 198 identical deaths).
        int cnt = 1;
        switch (a0) {
            case 0x0D3A: cnt = 2; break;  // MAX_VIEWPORT_DIMS
            case 0x0B70: cnt = 2; break;  // DEPTH_RANGE
            case 0x846D: cnt = 2; break;  // ALIASED_POINT_SIZE_RANGE
            case 0x846E: cnt = 2; break;  // ALIASED_LINE_WIDTH_RANGE
            case 0x0C10: cnt = 4; break;  // SCISSOR_BOX
            case 0x0BA2: cnt = 4; break;  // VIEWPORT
            case 0x0C22: cnt = 4; break;  // COLOR_CLEAR_VALUE
            case 0x0C23: cnt = 4; break;  // COLOR_WRITEMASK
            case 0x8005: cnt = 4; break;  // BLEND_COLOR
            default: break;               // scalars
        }
        // The two variable-length lists: as many values as the driver
        // reports (COMPRESSED_TEXTURE_FORMATS can exceed 64 on modern
        // drivers, so every form sizes its buffer from the count, E-06).
        if (a0 == 0x86A3 || a0 == 0x8DF8) {
            GLint nn = 0;
            glGetIntegerv(a0 == 0x86A3 ? 0x86A2 : 0x8DF9, &nn);
            cnt = (nn > 0 && nn <= 4096) ? nn : 0;
        }
        if (id == G::glGetIntegerv) {
            std::vector<GLint> v(static_cast<std::size_t>(cnt < 4 ? 4 : cnt), 0);
            glGetIntegerv((GLenum)a0, v.data());
            if (r->CheckRange(a1, (std::uint32_t)(cnt * 4), true)) {
                for (int i = 0; i < cnt; i++) {
                    std::uint32_t w = (std::uint32_t)v[i];
                    std::memcpy(r->H(a1 + (std::uint32_t)(i * 4)), &w, 4);
                }
            } else if (cnt > 0) {
                std::printf("[realgl] %s(%04x) LOUD SMALLBUF need=%d -- halting\n", name, a0, cnt * 4);
                r->HaltGuest();
            }
            if (DiagOn()) {
                std::printf("[realgl] %s(%04x) -> %d,%d,%d,%d\n", name, a0, v[0], v[1], v[2], v[3]);
            }
        } else {
            // glGetFloatv / glGetBooleanv: the same counts, 4-byte floats or
            // 1-byte booleans (they fell to the generic stub, which wrote
            // nothing).
            const bool fl = (id == G::glGetFloatv);
            const std::uint32_t bytes = static_cast<std::uint32_t>(cnt) * (fl ? 4u : 1u);
            if (!r->CheckRange(a1, bytes, true)) {
                return bad(a1);
            }
            const std::size_t slots = static_cast<std::size_t>(cnt < 4 ? 4 : cnt);
            if (fl) {
                std::vector<GLfloat> v(slots, 0.0f);
                glGetFloatv((GLenum)a0, v.data());
                std::memcpy(r->H(a1), v.data(), bytes);
            } else {
                std::vector<GLboolean> v(slots, 0);
                glGetBooleanv((GLenum)a0, v.data());
                std::memcpy(r->H(a1), v.data(), bytes);
            }
        }
        r->Ret(regs, 0);
        return true;
    }
    // --- uniforms (forward when observed; log LOUD otherwise) ---
    if (std::strncmp(name, "glUniform", 9) == 0) {
        if (UniformRepeat(r, id, a0, a1, a2, a3, sp)) {  // the location holds this value
            r->Ret(regs, 0);
            return true;
        }
        // (location, v0, ...): scalar-first forms in regs, rest on stack.
        // Only matrix/few forms need pointers; handle the common ones.
        if (id == G::glUniform1f || id == G::glUniform1i) {
            if (id == G::glUniform1f) {
                float f = 0;
                std::memcpy(&f, &a1, 4);
                glUniform1f((GLint)a0, f);
            } else {
                glUniform1i((GLint)a0, (GLint)a1);
            }
            r->Ret(regs, 0);
            return true;
        }
        if (id == G::glUniform2f || id == G::glUniform2i || id == G::glUniform3f ||
            id == G::glUniform3i || id == G::glUniform4f || id == G::glUniform4i) {
            float f[4] = {0};
            std::uint32_t w3 = Stk(r, sp, 0);
            std::memcpy(&f[0], &a1, 4);
            std::memcpy(&f[1], &a2, 4);
            std::memcpy(&f[2], &a3, 4);
            std::memcpy(&f[3], &w3, 4);
            const std::int32_t* ii = (const std::int32_t*)f;
            if (id == G::glUniform2f) {
                glUniform2f((GLint)a0, f[0], f[1]);
            } else if (id == G::glUniform2i) {
                glUniform2i((GLint)a0, ii[0], ii[1]);
            } else if (id == G::glUniform3f) {
                glUniform3f((GLint)a0, f[0], f[1], f[2]);
            } else if (id == G::glUniform3i) {
                glUniform3i((GLint)a0, ii[0], ii[1], ii[2]);
            } else if (id == G::glUniform4f) {
                glUniform4f((GLint)a0, f[0], f[1], f[2], f[3]);
            } else {
                glUniform4i((GLint)a0, ii[0], ii[1], ii[2], ii[3]);
            }
            r->Ret(regs, 0);
            return true;
        }
        if (id == G::glUniformMatrix4fv || id == G::glUniformMatrix3fv || id == G::glUniformMatrix2fv ||
            id == G::glUniform1fv || id == G::glUniform1iv || id == G::glUniform2fv ||
            id == G::glUniform2iv || id == G::glUniform3fv || id == G::glUniform3iv ||
            id == G::glUniform4fv || id == G::glUniform4iv) {
            // glUniformMatrix*: (location, count, transpose, value*);
            // glUniform{1-4}{f,i}v: (location, count, value*) -- no transpose,
            // the pointer is the THIRD argument (r2).
            const bool matrix = id == G::glUniformMatrix4fv || id == G::glUniformMatrix3fv ||
                                id == G::glUniformMatrix2fv;
            std::uint32_t cnt = a1, vp = matrix ? a3 : a2;
            std::uint32_t per = 1;
            if (id == G::glUniformMatrix4fv) {
                per = 16;
            } else if (id == G::glUniformMatrix3fv) {
                per = 9;
            } else if (id == G::glUniformMatrix2fv) {
                per = 4;
            } else {
                // "glUniform<1|2|3|4>{f,i}v": the digit is index 9 ('f'/'i' is 10).
                per = (name[9] == '1') ? 1 : (name[9] == '2') ? 2 : (name[9] == '3') ? 3 : 4;
            }
            std::size_t nb = (std::size_t)cnt * per * 4;
            const void* p =
                (vp && cnt < 4096 && nb && r->CheckRange(vp, nb, false)) ? (const void*)r->H(vp)
                                                                         : nullptr;
            if (p) {
                GLboolean tr = (GLboolean)(a2 & 1u);
                if (id == G::glUniformMatrix4fv) {
                    glUniformMatrix4fv((GLint)a0, (GLsizei)cnt, tr, (const GLfloat*)p);
                } else if (id == G::glUniformMatrix3fv) {
                    glUniformMatrix3fv((GLint)a0, (GLsizei)cnt, tr, (const GLfloat*)p);
                } else if (id == G::glUniformMatrix2fv) {
                    glUniformMatrix2fv((GLint)a0, (GLsizei)cnt, tr, (const GLfloat*)p);
                } else if (id == G::glUniform1fv) {
                    glUniform1fv((GLint)a0, (GLsizei)cnt, (const GLfloat*)p);
                } else if (id == G::glUniform1iv) {
                    glUniform1iv((GLint)a0, (GLsizei)cnt, (const GLint*)p);
                } else if (id == G::glUniform2fv) {
                    glUniform2fv((GLint)a0, (GLsizei)cnt, (const GLfloat*)p);
                } else if (id == G::glUniform2iv) {
                    glUniform2iv((GLint)a0, (GLsizei)cnt, (const GLint*)p);
                } else if (id == G::glUniform3fv) {
                    glUniform3fv((GLint)a0, (GLsizei)cnt, (const GLfloat*)p);
                } else if (id == G::glUniform3iv) {
                    glUniform3iv((GLint)a0, (GLsizei)cnt, (const GLint*)p);
                } else if (id == G::glUniform4fv) {
                    glUniform4fv((GLint)a0, (GLsizei)cnt, (const GLfloat*)p);
                } else {
                    glUniform4iv((GLint)a0, (GLsizei)cnt, (const GLint*)p);
                }
            } else {
                std::printf("[realgl] %s LOUD BAD-PTR loc=%u cnt=%u -- halting\n", name, a0, cnt);
                r->HaltGuest();
            }
            r->Ret(regs, 0);
            return true;
        }
        std::printf("[realgl] %s UNHANDLED-uniform -> fake\n", name);
        return false;
    }
    std::printf("[realgl] %s UNHANDLED -> fake shim\n", name);
    return false;
}

#ifndef __ANDROID__
bool RealGL::Screenshot(const char* path, int w, int h) {
    if (!ok_ || w <= 0 || h <= 0 || w > 2048 || h > 2048) {
        return false;
    }
    // NOTE: read GL_RGBA, never GL_RGB: RGB reads from an alpha-bearing
    // config are GL_INVALID_OPERATION on NVIDIA (every black screenshot so
    // far was this read bug, not a rendering failure).
    std::string px((std::size_t)w * h * 4, '\0');
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    // Present to the live window (swap AFTER the readback: reads come from
    // the back buffer, the display shows the front; without this the window
    // stays black while screenshots are correct).
    if (xwin_ && disp_ && surf_) {
        eglSwapBuffers((EGLDisplay)disp_, (EGLSurface)surf_);
    }
    FILE* f = std::fopen(path, "wb");
    if (!f) {
        return false;
    }
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; y--) {
        const char* row = px.data() + (std::size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            std::fwrite(row + (std::size_t)x * 4, 1, 3, f);
        }
    }
    std::fclose(f);
    std::printf("[realgl] screenshot -> %s\n", path);
    return true;
}

#endif  // PC Screenshot


}  // namespace abr
