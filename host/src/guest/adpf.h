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

// Reads the switch and looks the functions up. `java_vm` (a JavaVM*) is used
// to read the display's refresh rate; null: 60 Hz is assumed.
void Init(void* java_vm);
// A frame ended on the calling thread.
void FrameDone();
// For the periodic log: frames since the last call, their total CPU time, the
// current target and whether a session is live. False when nothing was
// counted.
bool TakeStats(std::uint64_t* frames, std::uint64_t* cpu_ns, std::uint64_t* target_ns, bool* session);

}  // namespace abr::adpf
