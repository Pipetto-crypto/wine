#if defined(ARM64) || defined(__aarch64__)
#define STEP 0
#include "dynarec_arm64_avx.c"
#endif /* arm64 */