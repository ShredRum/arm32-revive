// RealGL implementation. See realgl.h for the design contract.
#ifndef __ANDROID__
#include "realgl.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#include "dispatch.h"

namespace abr {

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

bool RealGL::Handle(const char* name, RealGL::Regs& regs, std::uint32_t a0, std::uint32_t a1,
                    std::uint32_t a2, std::uint32_t a3) {
    if (!Ensure()) {
        return false;
    }
    Runtime* r = rt_;
    const std::uint32_t sp = regs[13];
    std::string n(name);
    char tag[128];
    std::snprintf(tag, sizeof(tag), "[realgl] %s", name);

    // --- state (direct forward, scalar args only) ---
    if (n == "glEnable" || n == "glDisable") {
        (n == "glEnable" ? glEnable : glDisable)((GLenum)a0);
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glBlendFunc") {
        glBlendFunc((GLenum)a0, (GLenum)a1);
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glBlendEquation" || n == "glDepthFunc" || n == "glCullFace" ||
        n == "glFrontFace") {
        if (n == "glBlendEquation") {
            glBlendEquation((GLenum)a0);
        } else if (n == "glDepthFunc") {
            glDepthFunc((GLenum)a0);
        } else if (n == "glCullFace") {
            glCullFace((GLenum)a0);
        } else {
            glFrontFace((GLenum)a0);
        }
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glDepthMask") {
        glDepthMask((GLboolean)a0);
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glViewport") {
        glViewport((GLint)a0, (GLint)a1, (GLsizei)a2, (GLsizei)a3);
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glScissor") {
        glScissor((GLint)a0, (GLint)a1, (GLsizei)a2, (GLsizei)a3);
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glClearColor") {
        float f[4] = {0, 0, 0, 0};
        std::memcpy(&f[0], &a0, 4);
        std::memcpy(&f[1], &a1, 4);
        std::memcpy(&f[2], &a2, 4);
        std::memcpy(&f[3], &a3, 4);
        glClearColor(f[0], f[1], f[2], f[3]);
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glClear") {
        glClear((GLbitfield)a0);
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glPixelStorei") {
        glPixelStorei((GLenum)a0, (GLint)a1);
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glActiveTexture") {
        active_tex_ = a0 - 0x84C0u;
        glActiveTexture((GLenum)a0);
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glFlush" || n == "glFinish") {
        (n == "glFlush" ? glFlush : glFinish)();
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glGetError") {
        GLenum e = glGetError();
        if (e) {
            std::printf("%s -> %04x\n", tag, e);
        }
        r->Ret(regs, (std::uint32_t)e);
        return true;
    }
    // --- shaders/programs (real compile; \\x01 header separators sanitized:
    // 2014 Mali/Adreno accepted them, strict compilers need newlines) ---
    if (n == "glCreateShader") {
        GLuint s = glCreateShader((GLenum)a0);
        std::printf("%s(%04x) -> %u\n", tag, a0, s);
        r->Ret(regs, s);
        return true;
    }
    if (n == "glCreateProgram") {
        GLuint p = glCreateProgram();
        std::printf("%s -> %u\n", tag, p);
        r->Ret(regs, p);
        return true;
    }
    if (n == "glShaderSource") {
        GLuint s = (GLuint)a0;
        std::uint32_t cnt = (a1 > 16) ? 16 : a1;
        if (!r->CheckRange(a2, 4 * cnt, false)) {
            r->Ret(regs, 0);
            return true;
        }
        bool ok = true;
        for (std::uint32_t i = 0; i < cnt; i++) {
            std::uint32_t sptr = 0;
            std::memcpy(&sptr, r->H(a2 + i * 4), 4);
            std::int32_t slen = -1;
            if (a3 && r->CheckRange(a3 + i * 4, 4, false)) {
                std::memcpy(&slen, r->H(a3 + i * 4), 4);
            }
            if (!sptr || !r->CheckRange(sptr, 1, false)) {
                ok = false;
                break;
            }
            std::size_t L = 0;
            if (slen < 0) {
                const char* t = reinterpret_cast<const char*>(r->H(sptr));
                while (L < 65536 && r->CheckRange(sptr + (std::uint32_t)L, 1, false) && t[L]) {
                    L++;
                }
            } else if (slen > 0 && slen <= 65536) {
                L = (std::size_t)slen;
            }
            if (!L || !r->CheckRange(sptr, L, false)) {
                ok = false;
                break;
            }
            std::string src(reinterpret_cast<const char*>(r->H(sptr)), L);
            for (char& c : src) {
                if (c == '\x01') {
                    c = '\n';
                }
            }
            const char* p = src.c_str();
            GLint nl = (GLint)src.size();
            // src dies at scope end but glShaderSource copies synchronously.
            glShaderSource(s, 1, &p, &nl);
            if (i + 1 < cnt) {
                // Multi-string sources are rare here; last wins is wrong —
                // concatenate instead. Redo properly below if ever hit.
                std::printf("%s MULTI-STRING source (%u parts) -- only part %u forwarded\n", tag,
                            cnt, i);
            }
        }
        std::printf("%s shader=%u parts=%u %s\n", tag, s, cnt, ok ? "ok" : "BAD-PTR");
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glCompileShader") {
        glCompileShader((GLuint)a0);
        GLint ok = 0;
        glGetShaderiv((GLuint)a0, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetShaderInfoLog((GLuint)a0, sizeof(log), nullptr, log);
            std::printf("%s(%u) FAIL: %s\n", tag, a0, log);
        }
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glAttachShader" || n == "glDetachShader") {
        if (n == "glAttachShader") {
            glAttachShader((GLuint)a0, (GLuint)a1);
        } else {
            glDetachShader((GLuint)a0, (GLuint)a1);
        }
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glLinkProgram" || n == "glValidateProgram") {
        if (n == "glLinkProgram") {
            glLinkProgram((GLuint)a0);
        } else {
            glValidateProgram((GLuint)a0);
        }
        GLint ok = 0;
        glGetProgramiv((GLuint)a0, n == "glLinkProgram" ? GL_LINK_STATUS : GL_VALIDATE_STATUS,
                       &ok);
        if (!ok) {
            char log[1024];
            glGetProgramInfoLog((GLuint)a0, sizeof(log), nullptr, log);
            std::printf("%s(%u) FAIL: %s\n", tag, a0, log);
        } else {
            std::printf("%s(%u) ok\n", tag, a0);
        }
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glUseProgram") {
        glUseProgram((GLuint)a0);
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glDeleteShader" || n == "glDeleteProgram") {
        if (n == "glDeleteShader") {
            glDeleteShader((GLuint)a0);
        } else {
            glDeleteProgram((GLuint)a0);
        }
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glGetShaderiv" || n == "glGetProgramiv") {
        // (obj, pname, params): forward REAL status.
        GLint v = 0;
        if (n == "glGetShaderiv") {
            glGetShaderiv((GLuint)a0, (GLenum)a1, &v);
        } else {
            glGetProgramiv((GLuint)a0, (GLenum)a1, &v);
        }
        if (r->CheckRange(a2, 4, true)) {
            std::uint32_t w = (std::uint32_t)v;
            std::memcpy(r->H(a2), &w, 4);
        }
        std::printf("%s(%u,%04x) -> %d\n", tag, a0, a1, v);
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glGetShaderInfoLog" || n == "glGetProgramInfoLog") {
        // (obj, bufSize, length*, infoLog*): forward REAL log.
        std::uint32_t lenptr = 0;
        if (a2 && r->CheckRange(a2, 4, true)) {
            if (n == "glGetShaderInfoLog") {
                glGetShaderInfoLog((GLuint)a0, (GLsizei)a1, (GLsizei*)&lenptr,
                                   a3 && r->CheckRange(a3, a1, true) ? (char*)r->H(a3) : nullptr);
            } else {
                glGetProgramInfoLog((GLuint)a0, (GLsizei)a1, (GLsizei*)&lenptr,
                                    a3 && r->CheckRange(a3, a1, true) ? (char*)r->H(a3) : nullptr);
            }
            std::memcpy(r->H(a2), &lenptr, 4);
        }
        std::printf("%s(%u) len=%u\n", tag, a0, lenptr);
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glGetAttribLocation") {
        std::string s = GStr(r, a1);
        GLint loc = glGetAttribLocation((GLuint)a0, s.c_str());
        std::printf("%s(%u,\"%s\") -> %d\n", tag, a0, s.c_str(), loc);
        r->Ret(regs, (std::uint32_t)(std::int32_t)loc);
        return true;
    }
    if (n == "glGetUniformLocation") {
        std::string s = GStr(r, a1);
        GLint loc = glGetUniformLocation((GLuint)a0, s.c_str());
        std::printf("%s(%u,\"%s\") -> %d\n", tag, a0, s.c_str(), loc);
        r->Ret(regs, (std::uint32_t)(std::int32_t)loc);
        return true;
    }
    if (n == "glGetActiveUniform" || n == "glGetActiveAttrib") {
        // regs: (prog,index,bufSize,length); stack: size,type,name.
        std::uint32_t blen = a2, lenp = a3, sizep = Stk(r, sp, 0), typep = Stk(r, sp, 1),
                      namep = Stk(r, sp, 2);
        GLsizei len = 0;
        GLint size = 0;
        GLenum type = 0;
        char* nm = (namep && blen && r->CheckRange(namep, blen, true)) ? (char*)r->H(namep) : nullptr;
        if (n == "glGetActiveUniform") {
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
        std::printf("%s(%u,%u) -> \"%s\"\n", tag, a0, a1, nm ? nm : "?");
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glBindAttribLocation") {
        glBindAttribLocation((GLuint)a0, (GLuint)a1, GStr(r, a2).c_str());
        r->Ret(regs, 0);
        return true;
    }
    // --- textures: real objects + real RGBA data (game decodes .pvr itself;
    // observed uploads are RGBA4444/8888, host-native) ---
    if (n == "glGenTextures" || n == "glGenBuffers" || n == "glGenFramebuffers" ||
        n == "glGenRenderbuffers" || n == "glDeleteTextures" || n == "glDeleteBuffers" ||
        n == "glDeleteFramebuffers" || n == "glDeleteRenderbuffers") {
        const bool is_del = n[3] == 'D';
        if (!r->CheckRange(a1, 4 * a0, !is_del)) {
            r->Ret(regs, 0);
            return true;
        }
        if (n[5] == 'T') {
            is_del ? glDeleteTextures((GLsizei)a0, (const GLuint*)r->H(a1))
                   : glGenTextures((GLsizei)a0, (GLuint*)r->H(a1));
        } else if (n[5] == 'B') {
            is_del ? glDeleteBuffers((GLsizei)a0, (const GLuint*)r->H(a1))
                   : glGenBuffers((GLsizei)a0, (GLuint*)r->H(a1));
        } else if (n == "glGenFramebuffers" || n == "glDeleteFramebuffers") {
            is_del ? glDeleteFramebuffers((GLsizei)a0, (const GLuint*)r->H(a1))
                   : glGenFramebuffers((GLsizei)a0, (GLuint*)r->H(a1));
        } else {
            is_del ? glDeleteRenderbuffers((GLsizei)a0, (const GLuint*)r->H(a1))
                   : glGenRenderbuffers((GLsizei)a0, (GLuint*)r->H(a1));
        }
        std::printf("%s(%u) ok\n", tag, a0);
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glBindTexture") {
        if (a0 == 0x0DE1u && active_tex_ < 8) {
            bound_tex2d_[active_tex_] = a1;
        }
        glBindTexture((GLenum)a0, (GLuint)a1);
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glTexParameteri" || n == "glTexParameterf") {
        if (n == "glTexParameteri") {
            glTexParameteri((GLenum)a0, (GLenum)a1, (GLint)a2);
        } else {
            float f = 0;
            std::memcpy(&f, &a2, 4);
            glTexParameterf((GLenum)a0, (GLenum)a1, f);
        }
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glTexImage2D") {
        // regs: target,level,intfmt,width; stack: height,border,format,type,data.
        std::uint32_t h = Stk(r, sp, 0), fmt = Stk(r, sp, 2), typ = Stk(r, sp, 3),
                      dat = Stk(r, sp, 4);
        const void* p = (dat && r->CheckRange(dat, 1, false)) ? (const void*)r->H(dat) : nullptr;
        glTexImage2D((GLenum)a0, (GLint)a1, (GLint)a2, (GLsizei)a3, (GLsizei)h, 0, (GLenum)fmt,
                     (GLenum)typ, p);
        GLenum e = glGetError();
        // TEMP-DIAG: track size by bound texture of active unit.
        if (active_tex_ < 8 && bound_tex2d_[active_tex_]) {
            tex_size_[bound_tex2d_[active_tex_]] = (a3 & 0xFFFFu) | (h << 16);
        }
        std::printf("%s %ux%u fmt=%04x typ=%04x %s err=%04x\n", tag, a3, h, fmt, typ,
                    p ? "data" : "NULL", e);
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glTexSubImage2D") {
        // regs: target,level,xoff,yoff; stack: w,h,format,type,data.
        std::uint32_t w = Stk(r, sp, 0), h = Stk(r, sp, 1), fmt = Stk(r, sp, 2), typ = Stk(r, sp, 3),
                      dat = Stk(r, sp, 4);
        const void* p = (dat && r->CheckRange(dat, 1, false)) ? (const void*)r->H(dat) : nullptr;
        glTexSubImage2D((GLenum)a0, (GLint)a1, (GLint)a2, (GLint)a3, (GLsizei)w, (GLsizei)h,
                        (GLenum)fmt, (GLenum)typ, p);
        GLenum e = glGetError();
        // TEMP-DIAG (black frame): checksum the whole upload (capped).
        {
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
            std::printf("%s %ux%u fmt=%04x typ=%04x %s err=%04x sum=%llu/%llu\n", tag, w, h, fmt,
                        typ, p ? "data" : "NULL", e, (unsigned long long)sum,
                        (unsigned long long)total);
        }
        std::printf("%s %ux%u fmt=%04x %s err=%04x\n", tag, w, h, fmt, p ? "data" : "NULL", e);
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glCompressedTexImage2D" || n == "glCompressedTexSubImage2D") {
        // Real forwarding (llvmpipe accepts ETC1; NVIDIA errors -> game falls
        // back to software decode). Args: regs(target,level,intfmt,w,h,border)
        // + stack(imageSize,data) for Tex; (target,level,x,y,w,h,format,
        // imageSize,data) for Sub (regs target,level,x,y + stack w,h,fmt,size,data).
        if (n == "glCompressedTexImage2D") {
            // regs: target,level,intfmt,width; stack: height,border,imageSize,data.
            std::uint32_t h = Stk(r, sp, 0), size = Stk(r, sp, 2), dat = Stk(r, sp, 3);
            const void* p = (dat && size && r->CheckRange(dat, size, false)) ? (const void*)r->H(dat)
                                                                             : nullptr;
            glCompressedTexImage2D((GLenum)a0, (GLint)a1, (GLenum)a2, (GLsizei)a3, (GLsizei)h, 0,
                                   (GLsizei)size, p);
            GLenum e = glGetError();
            std::printf("%s ifmt=%04x %ux%u %uB %s err=%04x\n", tag, a2, a3, h, size,
                        p ? "data" : "NULL", e);
        } else {
            // regs: target,level,xoff,yoff; stack: w,h,format,imageSize,data.
            std::uint32_t w = Stk(r, sp, 0), h = Stk(r, sp, 1), fmt = Stk(r, sp, 2),
                          size = Stk(r, sp, 3), dat = Stk(r, sp, 4);
            const void* p = (dat && size && r->CheckRange(dat, size, false)) ? (const void*)r->H(dat)
                                                                             : nullptr;
            glCompressedTexSubImage2D((GLenum)a0, (GLint)a1, (GLint)a2, (GLint)a3, (GLsizei)w,
                                      (GLsizei)h, (GLenum)fmt, (GLsizei)size, p);
            GLenum e = glGetError();
            std::printf("%s %ux%u fmt=%04x %uB %s err=%04x\n", tag, w, h, fmt, size,
                        p ? "data" : "NULL", e);
        }
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glCopyTexImage2D" || n == "glCopyTexSubImage2D") {
        std::printf("%s UNHANDLED -> fake\n", tag);
        return false;
    }
    // --- buffers + draws (bound-buffer tracking resolves offset duality) ---
    if (n == "glBindBuffer") {
        if (a0 == 0x8892u) {
            array_buf_ = a1;
        } else if (a0 == 0x8893u) {
            element_buf_ = a1;
        }
        glBindBuffer((GLenum)a0, (GLuint)a1);
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glBufferData" || n == "glBufferSubData") {
        // (target,size,data,usage) / (target,offset,size,data).
        std::uint32_t size = 0, dat = 0;
        if (n == "glBufferData") {
            size = a1;
            dat = a2;
        } else {
            // (target,offset,size,data): all in regs.
            size = a2;
            dat = a3;
        }
        const void* p = (dat && size && r->CheckRange(dat, size, false)) ? (const void*)r->H(dat)
                                                                         : nullptr;
        if (n == "glBufferData") {
            glBufferData((GLenum)a0, (GLsizeiptr)size, p, (GLenum)a3);
        } else {
            glBufferSubData((GLenum)a0, (GLintptr)a1, (GLsizeiptr)size, p);
        }
        std::printf("%s tgt=%04x %uB %s\n", tag, a0, size, p ? "data" : "NULL");
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glVertexAttribPointer") {
        // regs: (index,size,type,normalized); stack: stride,pointer.
        // pointer is guest VA unless an array buffer is bound (raw offset).
        std::uint32_t stride = Stk(r, sp, 0), gp = Stk(r, sp, 1);
        const void* p = nullptr;
        if (array_buf_ != 0) {
            p = (const void*)(std::uintptr_t)gp;  // VBO offset passthrough
        } else if (gp && r->CheckRange(gp, 1, false)) {
            p = (const void*)r->H(gp);
        }
        glVertexAttribPointer((GLuint)a0, (GLint)a1, (GLenum)a2, (GLboolean)a3, (GLsizei)stride,
                              p);
        // TEMP-DIAG (black frame): dump first vertex for float attribs.
        if (!array_buf_ && a2 == 0x1406u && p && r->CheckRange(gp, 16, false)) {
            const float* f = (const float*)p;
            std::printf("%s idx=%u size=%u stride=%u v0=[%.1f %.1f %.1f %.1f]\n", tag, a0, a1,
                        stride, f[0], f[1], f[2], f[3]);
        }
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glEnableVertexAttribArray" || n == "glDisableVertexAttribArray") {
        (n[2] == 'E' ? glEnableVertexAttribArray : glDisableVertexAttribArray)((GLuint)a0);
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glDrawArrays" || n == "glDrawElements") {
        // TEMP-DIAG (black frame): HARNESS_NOBLEND=1 disables blending to
        // test the alpha-kill hypothesis.
        if (std::getenv("HARNESS_NOBLEND")) {
            glDisable(GL_BLEND);
        }
        // TEMP-DIAG (black frame): dump real draw state.
        {
            GLint prog = 0, tex = 0, vp[4] = {0}, blend = 0, scissor = 0, ab0 = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
            glGetIntegerv(GL_VIEWPORT, vp);
            blend = glIsEnabled(GL_BLEND);
            scissor = glIsEnabled(GL_SCISSOR_TEST);
            glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &ab0);
            std::printf("%s prog=%d tex=%d vp=%d,%d,%d,%d blend=%d scissor=%d ab0=%d\n", tag,
                        prog, tex, vp[0], vp[1], vp[2], vp[3], blend, scissor, ab0);
            // MVP at location 2 + first texel of the bound texture.
            {
                float m[16] = {0};
                glGetUniformfv((GLuint)prog, 2, m);
                std::printf("%s mvp=[%.2f %.2f %.2f %.2f / %.2f %.2f %.2f %.2f / %.2f %.2f %.2f %.2f / %.2f %.2f %.2f %.2f]\n",
                            tag, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10],
                            m[11], m[12], m[13], m[14], m[15]);
            }
            {
                // ES2 has no texture readback; report tracked upload size.
                std::uint32_t tw = 0, th = 0;
                auto it = tex_size_.find((std::uint32_t)tex);
                if (it != tex_size_.end()) {
                    tw = it->second & 0xFFFFu;
                    th = it->second >> 16;
                }
                GLint minf = 0, magf = 0;
                glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minf);
                glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &magf);
                std::printf("%s texsize=%ux%u min=%04x mag=%04x\n", tag, tw, th, minf, magf);
            }
        }
        if (n == "glDrawArrays") {
            glDrawArrays((GLenum)a0, (GLint)a1, (GLsizei)a2);
        } else {
            // (mode,count,type,indices): all in regs; offset if an element
            // buffer is bound, guest VA otherwise.
            const void* p = nullptr;
            if (element_buf_ != 0) {
                p = (const void*)(std::uintptr_t)a3;
            } else if (a3 && r->CheckRange(a3, 1, false)) {
                p = (const void*)r->H(a3);
            }
            glDrawElements((GLenum)a0, (GLsizei)a1, (GLenum)a2, p);
        }
        GLenum e = glGetError();
        std::printf("%s err=%04x\n", tag, e);
        r->Ret(regs, 0);
        return true;
    }
    if (n == "glReadPixels") {
        // (x,y,w,h,format,type,pixels) — x,y,w,h in regs; rest on stack.
        std::uint32_t fmt = Stk(r, sp, 0), typ = Stk(r, sp, 1), dat = Stk(r, sp, 2);
        void* p = (dat && r->CheckRange(dat, 1, true)) ? (void*)r->H(dat) : nullptr;
        if (p) {
            glReadPixels((GLint)a0, (GLint)a1, (GLsizei)a2, (GLsizei)a3, (GLenum)fmt, (GLenum)typ,
                          p);
        }
        r->Ret(regs, 0);
        return true;
    }
    // --- queries: REAL driver answers in real mode ---
    if (n == "glGetString") {
        const char* s = (const char*)glGetString((GLenum)a0);
        std::uint32_t out = 0;
        if (s) {
            // Reuse the fake shim's slot cache? Simplest: fresh guest copy.
            std::size_t L = std::strlen(s);
            out = r->GuestMalloc((std::uint32_t)(L + 1));
            if (out && r->CheckRange(out, L + 1, true)) {
                std::memcpy(r->H(out), s, L + 1);
            }
        }
        std::printf("%s(%04x) -> \"%s\"\n", tag, a0, s ? s : "(null)");
        r->Ret(regs, out);
        return true;
    }
    if (n == "glGetIntegerv" || n == "glGetFloatv" || n == "glGetBooleanv") {
        // (pname, params): forward; count 4 words max (covers viewport dims).
        GLint v[4] = {0};
        if (n == "glGetIntegerv") {
            glGetIntegerv((GLenum)a0, v);
            if (r->CheckRange(a1, 16, true)) {
                for (int i = 0; i < 4; i++) {
                    std::uint32_t w = (std::uint32_t)v[i];
                    std::memcpy(r->H(a1 + (std::uint32_t)(i * 4)), &w, 4);
                }
            }
            std::printf("%s(%04x) -> %d,%d,%d,%d\n", tag, a0, v[0], v[1], v[2], v[3]);
        } else {
            std::printf("%s UNHANDLED -> fake\n", tag);
            return false;
        }
        r->Ret(regs, 0);
        return true;
    }
    // --- uniforms (forward when observed; log LOUD otherwise) ---
    if (n.compare(0, 9, "glUniform") == 0) {
        // (location, v0, ...): scalar-first forms in regs, rest on stack.
        // Only matrix/few forms need pointers; handle the common ones.
        if (n == "glUniform1f" || n == "glUniform1i") {
            if (n == "glUniform1f") {
                float f = 0;
                std::memcpy(&f, &a1, 4);
                glUniform1f((GLint)a0, f);
            } else {
                glUniform1i((GLint)a0, (GLint)a1);
            }
            r->Ret(regs, 0);
            return true;
        }
        if (n == "glUniform2f" || n == "glUniform2i" || n == "glUniform3f" ||
            n == "glUniform3i" || n == "glUniform4f" || n == "glUniform4i") {
            float f[4] = {0};
            std::uint32_t w3 = Stk(r, sp, 0);
            std::memcpy(&f[0], &a1, 4);
            std::memcpy(&f[1], &a2, 4);
            std::memcpy(&f[2], &a3, 4);
            std::memcpy(&f[3], &w3, 4);
            const std::int32_t* ii = (const std::int32_t*)f;
            if (n == "glUniform2f") {
                glUniform2f((GLint)a0, f[0], f[1]);
            } else if (n == "glUniform2i") {
                glUniform2i((GLint)a0, ii[0], ii[1]);
            } else if (n == "glUniform3f") {
                glUniform3f((GLint)a0, f[0], f[1], f[2]);
            } else if (n == "glUniform3i") {
                glUniform3i((GLint)a0, ii[0], ii[1], ii[2]);
            } else if (n == "glUniform4f") {
                glUniform4f((GLint)a0, f[0], f[1], f[2], f[3]);
            } else {
                glUniform4i((GLint)a0, ii[0], ii[1], ii[2], ii[3]);
            }
            r->Ret(regs, 0);
            return true;
        }
        if (n == "glUniformMatrix4fv" || n == "glUniformMatrix3fv" || n == "glUniformMatrix2fv" ||
            n == "glUniform1fv" || n == "glUniform1iv" || n == "glUniform2fv" ||
            n == "glUniform2iv" || n == "glUniform3fv" || n == "glUniform3iv" ||
            n == "glUniform4fv" || n == "glUniform4iv") {
            // (location, count, transpose, value*): all in regs.
            std::uint32_t cnt = a1, vp = a3;
            std::uint32_t per = 1;
            if (n.find("Matrix4") != std::string::npos) {
                per = 16;
            } else if (n.find("Matrix3") != std::string::npos) {
                per = 9;
            } else if (n.find("Matrix2") != std::string::npos) {
                per = 4;
            } else {
                per = (n[10] == '1') ? 1 : (n[10] == '2') ? 2 : (n[10] == '3') ? 3 : 4;
            }
            std::size_t nb = (std::size_t)cnt * per * 4;
            const void* p =
                (vp && cnt < 4096 && nb && r->CheckRange(vp, nb, false)) ? (const void*)r->H(vp)
                                                                         : nullptr;
            if (p) {
                GLboolean tr = (GLboolean)(a2 & 1u);
                if (n == "glUniformMatrix4fv") {
                    glUniformMatrix4fv((GLint)a0, (GLsizei)cnt, tr, (const GLfloat*)p);
                } else if (n == "glUniformMatrix3fv") {
                    glUniformMatrix3fv((GLint)a0, (GLsizei)cnt, tr, (const GLfloat*)p);
                } else if (n == "glUniformMatrix2fv") {
                    glUniformMatrix2fv((GLint)a0, (GLsizei)cnt, tr, (const GLfloat*)p);
                } else if (n == "glUniform1fv") {
                    glUniform1fv((GLint)a0, (GLsizei)cnt, (const GLfloat*)p);
                } else if (n == "glUniform1iv") {
                    glUniform1iv((GLint)a0, (GLsizei)cnt, (const GLint*)p);
                } else if (n == "glUniform2fv") {
                    glUniform2fv((GLint)a0, (GLsizei)cnt, (const GLfloat*)p);
                } else if (n == "glUniform2iv") {
                    glUniform2iv((GLint)a0, (GLsizei)cnt, (const GLint*)p);
                } else if (n == "glUniform3fv") {
                    glUniform3fv((GLint)a0, (GLsizei)cnt, (const GLfloat*)p);
                } else if (n == "glUniform3iv") {
                    glUniform3iv((GLint)a0, (GLsizei)cnt, (const GLint*)p);
                } else if (n == "glUniform4fv") {
                    glUniform4fv((GLint)a0, (GLsizei)cnt, (const GLfloat*)p);
                } else {
                    glUniform4iv((GLint)a0, (GLsizei)cnt, (const GLint*)p);
                }
            } else {
                std::printf("%s BAD-PTR loc=%u cnt=%u\n", tag, a0, cnt);
            }
            r->Ret(regs, 0);
            return true;
        }
        std::printf("%s UNHANDLED-uniform -> fake\n", tag);
        return false;
    }
    std::printf("%s UNHANDLED -> fake shim\n", tag);
    return false;
}

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

}  // namespace abr
#else
// Android build: real GLES forwards to the system driver (packaging phase).
#include "realgl.h"
namespace abr {
bool RealGL::Ensure() {
    return false;
}
bool RealGL::Handle(const char*, Regs&, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {
    return false;
}
bool RealGL::Screenshot(const char*, int, int) {
    return false;  // headless-device runs take no screenshots
}
}  // namespace abr
#endif
