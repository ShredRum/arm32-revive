// Corruption hook for vendored dlmalloc (compiled into dlmalloc.c via
// -include). dlmalloc calls these INSTEAD of a bare abort() on heap
// corruption / usage errors. The hook must not call malloc, printf, or
// locks (dlmalloc is mid-flight).
#ifndef ABR_HEAP_HOOK_H
#define ABR_HEAP_HOOK_H

#ifdef __cplusplus
extern "C" {
#endif

void AbrHeapCorruptionHook(void);
void AbrAssertHook(const char* expr, int line);

#ifdef __cplusplus
}  // extern "C"
#endif

#define CORRUPTION_ERROR_ACTION(m) \
    do {                           \
        AbrHeapCorruptionHook();    \
        abort();                   \
    } while (0)
#define USAGE_ERROR_ACTION(m, p) \
    do {                         \
        AbrHeapCorruptionHook();  \
        abort();                 \
    } while (0)

// Route ALL dlmalloc aborts (including DEBUG assert() below, which expands
// to bare ABORT and would otherwise die mute) through the hook. Guarded
// #ifndef in dlmalloc.c, so this definition wins via -include.
#define ABORT                    \
    do {                         \
        AbrHeapCorruptionHook();  \
        abort();                 \
    } while (0)

#endif  // ABR_HEAP_HOOK_H
