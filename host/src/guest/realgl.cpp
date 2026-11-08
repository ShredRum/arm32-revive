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
// This thread's GL copies (bindings, uniform values, the uniform-owner
// token) in one thread_local: a GL call reaches all of them with one TLS
// lookup (each thread_local is a TLS-resolver call in a dlopened library).
struct GlThread {
    GlBindings bindings;
    UniCache uni;
    char token = 0;
};
thread_local GlThread t_gl;
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
    // slots and the GLDRAWLOG texture-size tracking; each takes mu_ where it
    // is touched (minimum F3: a lock on every GL call guarded nothing else).
    Runtime* r = rt_;
    const std::uint32_t sp = regs[13];
    using G = GlId;  // the import's load-time id (gl_ids.h); name is for logs only
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
    if (id == G::glCheckFramebufferStatus) {
        r->Ret(regs, (std::uint32_t)glCheckFramebufferStatus((GLenum)a0));
        return true;
    }
    if (id == G::glGetFramebufferAttachmentParameteriv || id == G::glGetVertexAttribiv) {
        // (target, attachment, pname, params*) / (index, pname, params*).
        // GL_CURRENT_VERTEX_ATTRIB answers four values, every other pname one.
        const bool fb = (id == G::glGetFramebufferAttachmentParameteriv);
        const std::uint32_t out = fb ? a3 : a2;
        const std::uint32_t count = (!fb && a1 == GL_CURRENT_VERTEX_ATTRIB) ? 4u : 1u;
        if (!r->CheckRange(out, count * 4u, true)) {
            r->Ret(regs, 0);
            return true;
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
            r->Ret(regs, 0);
            return true;
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
        if (GlDrawLog()) {  // texture-size tracking for the draw log only
            std::lock_guard<std::mutex> l(mu_);
            active_tex_ = a0 - 0x84C0u;
        }
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
        GLint v = 0;
        if (id == G::glGetShaderiv) {
            glGetShaderiv((GLuint)a0, (GLenum)a1, &v);
        } else {
            glGetProgramiv((GLuint)a0, (GLenum)a1, &v);
        }
        if (r->CheckRange(a2, 4, true)) {
            std::uint32_t w = (std::uint32_t)v;
            std::memcpy(r->H(a2), &w, 4);
        }
        if (GlDrawLog()) std::printf("[realgl] %s(%u,%04x) -> %d\n", name, a0, a1, v);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glGetShaderInfoLog || id == G::glGetProgramInfoLog) {
        // (obj, bufSize, length*, infoLog*): forward REAL log.
        std::uint32_t lenptr = 0;
        if (a2 && r->CheckRange(a2, 4, true)) {
            if (id == G::glGetShaderInfoLog) {
                glGetShaderInfoLog((GLuint)a0, (GLsizei)a1, (GLsizei*)&lenptr,
                                   a3 && r->CheckRange(a3, a1, true) ? (char*)r->H(a3) : nullptr);
            } else {
                glGetProgramInfoLog((GLuint)a0, (GLsizei)a1, (GLsizei*)&lenptr,
                                    a3 && r->CheckRange(a3, a1, true) ? (char*)r->H(a3) : nullptr);
            }
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
        char* nm = (namep && blen && r->CheckRange(namep, blen, true)) ? (char*)r->H(namep) : nullptr;
        if (id == G::glGetActiveUniform) {
            glGetActiveUniform((GLuint)a0, (GLuint)a1, (GLsizei)blen, &len, &size, &type, nm);
        } else {
            glGetActiveAttrib((GLuint)a0, (GLuint)a1, (GLsizei)blen, &len, &size, &type, nm);
        }
        if (lenp && r->CheckRange(lenp, 4, true)) {
            std::memcpy(r->H(lenp), &len, 4);
        }
        if (sizep && r->CheckRange(sizep, 4, true)) {
            std::memcpy(r->H(sizep), &size, 4);
        }
        if (typep && r->CheckRange(typep, 4, true)) {
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
        id == G::glGenRenderbuffers || id == G::glDeleteTextures || id == G::glDeleteBuffers ||
        id == G::glDeleteFramebuffers || id == G::glDeleteRenderbuffers) {
        // All eight names are "gl<Gen|Delete>...": the G/D switch is at index 2
        // (index 3 is 'e' in both, so n[3] was always false -> every delete ran
        // as a gen: leaked objects + the stale-buffer purge below never ran).
        const bool is_del = id == G::glDeleteTextures || id == G::glDeleteBuffers ||
                            id == G::glDeleteFramebuffers || id == G::glDeleteRenderbuffers;
        // 64-bit multiply (was 32-bit 4*a0: wraps at n~0x40000000 -> 0-byte
        // check passes -> driver writes past the guest buffer). Cap 1M (game
        // gens handfuls; wrap/garbage caught) + LOUD instead of smash.
        const std::uint64_t bytes64 = (std::uint64_t)a0 * 4;
        if (a0 >= 0x100000u || !r->CheckRange(a1, (std::size_t)bytes64, !is_del)) {
            if (a0 >= 0x100000u) {
                std::printf("[realgl] %s LOUD n=%u rejected\n", name, a0);
            }
            r->Ret(regs, 0);
            return true;
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
        } else if (id == G::glGenFramebuffers || id == G::glDeleteFramebuffers) {
            is_del ? glDeleteFramebuffers((GLsizei)a0, (const GLuint*)r->H(a1))
                   : glGenFramebuffers((GLsizei)a0, (GLuint*)r->H(a1));
        } else {
            is_del ? glDeleteRenderbuffers((GLsizei)a0, (const GLuint*)r->H(a1))
                   : glGenRenderbuffers((GLsizei)a0, (GLuint*)r->H(a1));
        }
        if (GlDrawLog()) std::printf("[realgl] %s(%u) ok\n", name, a0);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glBindFramebuffer || id == G::glBindRenderbuffer) {
        // The guests DO use framebuffers (measured: glBindFramebuffer x3,
        // glBindRenderbuffer x2 per guest); before this the calls fell into
        // the generic tail, which answered "success" without doing
        // anything, so the game rendered into nothing.
        if (id == G::glBindFramebuffer) {
            glBindFramebuffer((GLenum)a0, (GLuint)a1);
        } else {
            glBindRenderbuffer((GLenum)a0, (GLuint)a1);
        }
        if (GlDrawLog()) std::printf("[realgl] %s(%04x,%u) ok\n", name, a0, a1);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glFramebufferRenderbuffer) {
        // (target, attachment, renderbuffertarget, renderbuffer)
        glFramebufferRenderbuffer((GLenum)a0, (GLenum)a1, (GLenum)a2, (GLuint)a3);
        if (GlDrawLog()) std::printf("[realgl] %s(%04x,%04x,%04x,%u) ok\n", name, a0, a1, a2, a3);
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glFramebufferTexture2D) {
        // (target, attachment, textarget, texture, level) -- the level is
        // the first stack word.
        const std::uint32_t level = Stk(r, sp, 0);
        glFramebufferTexture2D((GLenum)a0, (GLenum)a1, (GLenum)a2, (GLuint)a3, (GLint)level);
        if (GlDrawLog()) {
            std::printf("[realgl] %s(%04x,%04x,%04x,%u,%u) ok\n", name, a0, a1, a2, a3, level);
        }
        r->Ret(regs, 0);
        return true;
    }
    if (id == G::glRenderbufferStorage) {
        // (target, internalformat, width, height)
        glRenderbufferStorage((GLenum)a0, (GLenum)a1, (GLsizei)a2, (GLsizei)a3);
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
            std::printf("[realgl] %s LOUD params %08x out of range\n", name, a2);
            r->Ret(regs, 0);
            return true;
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
        if (GlDrawLog()) {
            std::lock_guard<std::mutex> l(mu_);
            if (a0 == 0x0DE1u && active_tex_ < 8) {
                bound_tex2d_[active_tex_] = a1;
            }
        }
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
            // TEMP-DIAG: track size by bound texture of active unit (read only
            // by the GLDRAWLOG block at :819, so write under the same gate).
            {
                std::lock_guard<std::mutex> l(mu_);
                if (active_tex_ < 8 && bound_tex2d_[active_tex_]) {
                    tex_size_[bound_tex2d_[active_tex_]] = (a3 & 0xFFFFu) | (h << 16);
                }
            }
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
        GLenum e = GlDrawLog() ? glGetError() : (GLenum)0;
        // TEMP-DIAG (black frame): checksum the whole upload (capped).
        // Gated default-off: per-upload CPU loop + printfs, zero guest effect.
        if (GlDrawLog()) {
            std::uint64_t sum = 0, total = 0;
            if (p && w && h && w < 4096 && h < 4096) {
                std::uint32_t bpp = (typ == 0x1401u) ? 4 : 2;
                total = (std::uint64_t)w * h * bpp;
                std::uint64_t cap = total > 65536 ? 65536 : total;
                if (r->CheckRange(dat, (std::size_t)cap, false)) {
                    const std::uint8_t* b = (const std::uint8_t*)p;
                    for (std::uint64_t i = 0; i < cap; i++) {
                        sum += b[i];
                    }
                }
            }
            std::printf("[realgl] %s %ux%u fmt=%04x typ=%04x %s err=%04x sum=%llu/%llu\n", name, w, h, fmt,
                        typ, p ? "data" : "NULL", e, (unsigned long long)sum,
                        (unsigned long long)total);
        }
        if (GlDrawLog()) {
            std::printf("[realgl] %s %ux%u fmt=%04x %s err=%04x\n", name, w, h, fmt, p ? "data" : "NULL",
                        e);
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
            const void* p = (dat && size && r->CheckRange(dat, size, false)) ? (const void*)r->H(dat)
                                                                             : nullptr;
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
            const void* p = (dat && size && r->CheckRange(dat, size, false)) ? (const void*)r->H(dat)
                                                                             : nullptr;
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
            std::printf("[realgl] %s LOUD stack args unreadable\n", name);
            r->Ret(regs, 0);
            return true;
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
        const void* p = (dat && size && r->CheckRange(dat, size, false)) ? (const void*)r->H(dat)
                                                                         : nullptr;
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
        // TEMP-DIAG (black frame): dump first vertex for float attribs.
        // Gated default-off (was per-attrib-setup printf).
        if (!vbo && a2 == 0x1406u && p && GlDrawLog() &&
            r->CheckRange(gp, 16, false)) {
            const float* f = (const float*)p;
            std::printf("[realgl] %s idx=%u size=%u stride=%u v0=[%.1f %.1f %.1f %.1f]\n", name, a0, a1,
                        stride, f[0], f[1], f[2], f[3]);
        }
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
    if (id == G::glDrawArrays || id == G::glDrawElements) {
        // TEMP-DIAG (black frame): dump real draw state. ~10 GL round-trips
        // + 4 printfs PER DRAW — gated default-off (old investigation).
        if (GlDrawLog()) {
            GLint prog = 0, tex = 0, vp[4] = {0}, blend = 0, scissor = 0, ab0 = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
            glGetIntegerv(GL_VIEWPORT, vp);
            blend = glIsEnabled(GL_BLEND);
            scissor = glIsEnabled(GL_SCISSOR_TEST);
            glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &ab0);
            std::printf("[realgl] %s prog=%d tex=%d vp=%d,%d,%d,%d blend=%d scissor=%d ab0=%d\n", name,
                        prog, tex, vp[0], vp[1], vp[2], vp[3], blend, scissor, ab0);
            // MVP at location 2 + first texel of the bound texture.
            {
                float m[16] = {0};
                glGetUniformfv((GLuint)prog, 2, m);
                std::printf("[realgl] %s mvp=[%.2f %.2f %.2f %.2f / %.2f %.2f %.2f %.2f / %.2f %.2f %.2f %.2f / %.2f %.2f %.2f %.2f]\n",
                            name, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10],
                            m[11], m[12], m[13], m[14], m[15]);
            }
            {
                // ES2 has no texture readback; report tracked upload size.
                std::uint32_t tw = 0, th = 0;
                std::lock_guard<std::mutex> l(mu_);
                auto it = tex_size_.find((std::uint32_t)tex);
                if (it != tex_size_.end()) {
                    tw = it->second & 0xFFFFu;
                    th = it->second >> 16;
                }
                GLint minf = 0, magf = 0;
                glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minf);
                glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &magf);
                std::printf("[realgl] %s texsize=%ux%u min=%04x mag=%04x\n", name, tw, th, minf, magf);
            }
        }
        if (id == G::glDrawArrays) {
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
            if (p || ibo) {
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
        GLint v = 0;
        glGetBufferParameteriv((GLenum)a0, (GLenum)a1, &v);
        if (r->CheckRange(a2, 4, true)) {
            std::memcpy(r->H(a2), &v, 4);
        }
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
        if (id == G::glGetIntegerv) {
            // The two variable-length lists: as many values as the driver
            // reports (COMPRESSED_TEXTURE_FORMATS can exceed 64 on modern
            // drivers, so the buffer is sized from the count, E-06).
            if (a0 == 0x86A3 || a0 == 0x8DF8) {
                GLint nn = 0;
                glGetIntegerv(a0 == 0x86A3 ? 0x86A2 : 0x8DF9, &nn);
                cnt = (nn > 0 && nn <= 4096) ? nn : 0;
            }
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
            std::printf("[realgl] %s UNHANDLED -> fake\n", name);
            return false;
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
    // TEMP-DIAG: which framebuffer are we reading? (PBO binds don't
    // exist in ES2, skip them.)
    {
        GLint fb = -1;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
        std::printf("[realgl] shot: fb=%d\n", fb);
    }
    // NOTE: read GL_RGBA, never GL_RGB: RGB reads from an alpha-bearing
    // config are GL_INVALID_OPERATION on NVIDIA (every black screenshot so
    // far was this read bug, not a rendering failure).
    std::string px((std::size_t)w * h * 4, '\0');
    // TEMP-DIAG: HARNESS_SHOTSWAP=1 swaps before reading (pbuffer
    // back/front experiment).
    if (std::getenv("HARNESS_SHOTSWAP") && disp_ && surf_) {
        eglSwapBuffers((EGLDisplay)disp_, (EGLSurface)surf_);
    }
    // TEMP-DIAG (black frame): HARNESS_SHOTCLEAR=1 fills red first to prove
    // the readback path; without it we read the game's draws.
    if (std::getenv("HARNESS_SHOTCLEAR")) {
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glClearColor(1, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        GLenum e = glGetError();
        std::printf("[realgl] shotclear err=%04x\n", e);
    }
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
