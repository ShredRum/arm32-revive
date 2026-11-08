// Android Dynamic Performance Framework (ADPF) hints for the thread that
// makes the frames. Each frame reports the CPU time it took to a performance
// hint session (Android 13+) whose target is the display's frame period, so
// the system can run the cores no faster than the frames need: less heat at
// the same frame rate. Android 15+: the session also prefers power
// efficiency.
//
// A frame ends where the guest presents it: its own eglSwapBuffers, or, for
// an engine that renders from a Java GLSurfaceView, the return of its
// per-frame render call (AppHost_Call). The CPU time of the frame thread
// between two such marks is the frame's work.
//
// On by default; off with HARNESS_NOADPF=1 (env or files/abflags.txt) or
// `setprop debug.abport.adpf 0` (a shell, rish or adb, no root). Off still
// counts frames and their CPU time for the [adpf] line, so the two can be
// compared from logcat alone. Nothing happens below Android 13.
#pragma once

#include <cstdint>

namespace abr::adpf {

// Reads the switch and looks the functions up. The display's refresh rate is
// read through Java with the JNIEnv `env_for_thread` gives the calling
// thread (a JNIEnv*; the host attaches a guest-created thread, such as SDL's
// game thread, and detaches it when it exits); null: 120 Hz is assumed (the
// short side).
void Init(void* (*env_for_thread)());
// A frame ended on the calling thread.
void FrameDone();
// For the periodic log, since the last call. Cycles are each frame's CPU
// time times the clock of the core its thread was on at the frame's end
// (cpufreq, readable without root on most devices; 0 if not): with less work
// the governor lowers the clock and the same thread stays about as busy, so
// CPU time alone hides what ADPF saves.
struct Stats {
    std::uint64_t frames = 0;
    std::uint64_t cpu_ns = 0;
    std::uint64_t cycles = 0;          // 0: clocks unreadable
    std::uint64_t cycles_cpu_ns = 0;   // the CPU time those cycles cover
    std::uint64_t target_ns = 0;       // the current target
    bool session = false;              // a session is live
};
// False when no frame was counted.
bool TakeStats(Stats* s);

}  // namespace abr::adpf
