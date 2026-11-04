// RealGL: host GLES2 backend for the Dynarmic guest. On Android it is always
// on and forwards every gl* to the real driver; on the PC harness it is
// opt-in (HARNESS_REALGL=1), the fake-success shim being the default there.
// Design: game handles ARE real host handles (Create*/Gen* call straight
// through) — no mapping tables. Guest pointers are translated to host
// pointers at call time (guest heap is bump-stable, so GL-retained client
// array pointers stay valid). VBO-offset vs client-pointer duality is
// resolved by tracking bound buffers (no address heuristics).
#pragma once

#include <array>
#include <cstdint>
#include <mutex>

#include "dispatch.h"
#include "gl_ids.h"

namespace abr {

class RealGL {
  public:
    using Regs = std::array<std::uint32_t, 16>;
    explicit RealGL(Runtime* rt) : rt_(rt) {}
    // Lazily creates EGL surfaceless/pbuffer ES2 context. True when usable.
    bool Ensure();
    bool active() const { return ok_; }
    // Handles one gl* import. Returns true if fully handled (caller Rets).
    // a0..a3 are regs r0..r3; stack args read from guest sp internally.
    // id is the import's load-time GlId (gl_ids.h); name is for logs.
    bool Handle(GlId id, const char* name, Regs& regs, std::uint32_t a0, std::uint32_t a1,
                std::uint32_t a2, std::uint32_t a3);
    // Captures the current drawable to a binary PPM file (screenshot debug).
    bool Screenshot(const char* path, int w, int h);

  private:
    std::mutex mu_;  // see Handle
    Runtime* rt_;
    bool ok_ = false;
    bool tried_ = false;
    // EGL objects for swap-before-readback experiments.
    void* disp_ = nullptr;
    void* surf_ = nullptr;
    // X window surface (transient window, no system changes).
    void* xdisp_ = nullptr;
    std::uint64_t xwin_ = 0;
    // HARNESS_EGLDEV=N: force EGL device index (default: first that reads
    // back correctly; device list via eglQueryDevicesEXT).
    std::uint32_t active_tex_ = 0;  // (informational logging only)
    std::uint32_t bound_tex2d_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    // TEMP-DIAG: uploaded texture sizes (w | h<<16) by real handle.
    std::unordered_map<std::uint32_t, std::uint32_t> tex_size_;
    // G2.4: one stable guest slot per glGetString enum (the guest holds the
    // returned pointer; allocating a fresh copy per call leaked it).
    std::unordered_map<std::uint32_t, std::uint32_t> glstr_slots_;
};

}  // namespace abr
