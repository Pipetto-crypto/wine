#ifndef __WINE_TGMUX_EMULATOR_H
#define __WINE_TGMUX_EMULATOR_H

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Detect whether wine runs under an x86 emulator (box64 / box86 / FEX / qemu).
 * Such emulators translate the guest code and execute it inside their own
 * (host) address space, so wine assumptions based on native layout break:
 * the preloader's 47-bit high address area, seccomp decisions of the form
 * "native libs live in high addresses", and the 64KB alignment those
 * emulators impose on wine's own mappings.
 *   WINE_EMULATOR=<name>     force on
 *   WINE_EMULATOR=0          force off (escape hatch)
 */
static inline int wine_emulator_detected(void)
{
    static int cached = -1;
    const char *env;

    if (cached >= 0) return cached;
    cached = 0;

    if ((env = getenv( "WINE_EMULATOR" )) && *env)
    {
        cached = (env[0] == '0' && !env[1]) ? 0 : 1;
        return cached;
    }

    if (getenv( "BOX64_VERSION" ) || getenv( "BOX64_LD_LIBRARY_PATH" ) || getenv( "BOX64_LOG" ) ||
        getenv( "BOX86_VERSION" ) || getenv( "BOX86_LD_LIBRARY_PATH" ) ||
        getenv( "FEX_TSOENABLED" ) || getenv( "FEX_ROOTFS" ) || getenv( "FEX_APP_CONFIG" ) ||
        getenv( "QEMU_LD_PREFIX" ) || getenv( "QEMU_CPU" ))
        cached = 1;

#ifdef __linux__
    if (!cached)
    {
        /* box64/box86/FEX keep the guest binary mapped in their own process,
         * so /proc/self/exe points at the emulator itself. */
        char buf[256];
        ssize_t len = readlink( "/proc/self/exe", buf, sizeof(buf) - 1 );

        if (len > 0)
        {
            buf[len] = 0;
            if (strstr( buf, "box64" ) || strstr( buf, "box86" ) ||
                strstr( buf, "FEXInterpreter" ) || strstr( buf, "qemu-" ))
                cached = 1;
        }
    }
#endif
    return cached;
}

#endif /* __WINE_TGMUX_EMULATOR_H */
