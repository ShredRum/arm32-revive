// apphost: Android product entry (online-first). Java_* forwarders live in
// apphost_gen.inc (generated). This file: JNI_OnLoad, per-thread enter,
// AAPCS marshalling, guest symbol lookup, mini-zip asset extract.
#pragma once
#include <cstdint>
namespace abr {
// name = JNI export (Java_...), sig = JNI signature, env/thiz = real Java.
// Returns r0|r1<<32 (stubs convert per return type).
std::uint64_t AppHost_Call(const char* name, const char* sig, void* env, void* thiz, ...);
}  // namespace abr
