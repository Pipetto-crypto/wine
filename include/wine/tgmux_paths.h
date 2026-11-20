/*
 * Wine for Termux glibc
 *
 * Centralised host path policy for a private Termux-glibc prefix.
 *
 * Wine normally takes its host paths from FHS defaults (/usr, /etc, /tmp).
 * On Termux those locations either do not exist or are the Bionic rootfs,
 * while the whole glibc toolchain lives in a private prefix such as
 * /data/data/com.termux/files/usr/glibc. Route every such path through this
 * header so the prefix can be changed at build time or overridden per run.
 *
 * Resolution order, highest priority first:
 *   1. environment variable (runtime override)
 *   2. value baked in from WINE_TGMUX_PREFIX, or WINE_TGMUX_DEFAULT_TMPDIR
 *      for the temp directory, which lives in $PREFIX/tmp rather than in
 *      the glibc prefix.
 *
 * WINE_TGMUX_PREFIX can be overridden at configure time, e.g.
 *   CPPFLAGS='-DWINE_TGMUX_PREFIX="/data/data/com.termux/files/usr/glibc"'
 */

#ifndef __WINE_TGMUX_PATHS_H
#define __WINE_TGMUX_PATHS_H

#ifndef __WINE_CONFIG_H
#error This header must be included after config.h
#endif

#include <stdlib.h>

#ifndef WINE_TGMUX_PREFIX
#define WINE_TGMUX_PREFIX "/data/data/com.termux/files/usr/glibc"
#endif

/* Compile-time concatenation of a literal suffix onto the prefix. */
#define WINE_TGMUX_PATH( suffix ) WINE_TGMUX_PREFIX suffix

/*
 * Termux keeps its scratch space in $PREFIX/tmp, which lives outside the
 * glibc prefix and is the only reliably writable temp directory on Android.
 */
#ifndef WINE_TGMUX_DEFAULT_TMPDIR
#define WINE_TGMUX_DEFAULT_TMPDIR "/data/data/com.termux/files/usr/tmp"
#endif

/* Return `name` from the environment when set and non-empty, else `fallback`. */
static inline const char *tgmux_env( const char *name, const char *fallback )
{
    const char *value = getenv( name );
    return (value && *value) ? value : fallback;
}

/* $WINE_TMP_DIR, then $TMPDIR, then the Termux tmp directory. */
static inline const char *tgmux_tmpdir(void)
{
    const char *value = getenv( "WINE_TMP_DIR" );
    if (value && *value) return value;
    value = getenv( "TMPDIR" );
    if (value && *value) return value;
    return WINE_TGMUX_DEFAULT_TMPDIR;
}

static inline const char *tgmux_sysconfdir(void)
{
    return tgmux_env( "WINE_SYSCONF_DIR", WINE_TGMUX_PATH( "/etc" ) );
}

/*
 * D-Bus machine-id. On a normal Linux host this lives at
 * /var/lib/dbus/machine-id, but Wine also consults /etc/machine-id, and on
 * Termux glibc the whole sysconf tree (including machine-id) lives under the
 * glibc prefix, so resolve it through sysconfdir. Allow a dedicated override
 * for setups that place it elsewhere.
 */
static inline const char *tgmux_machine_id(void)
{
    return tgmux_env( "WINE_MACHINE_ID", WINE_TGMUX_PATH( "/etc/machine-id" ) );
}

static inline const char *tgmux_datadir(void)
{
    return tgmux_env( "WINE_DATA_DIR", WINE_TGMUX_PATH( "/share" ) );
}

static inline const char *tgmux_localstatedir(void)
{
    return tgmux_env( "WINE_LOCALSTATE_DIR", WINE_TGMUX_PATH( "/var" ) );
}

static inline const char *tgmux_zonedir(void)
{
    return tgmux_env( "WINE_TZDIR", WINE_TGMUX_PATH( "/share/zoneinfo" ) );
}

/*
 * Root exported as the Z: drive. Exposing the whole host root inside a
 * Windows prefix is not desirable on Android, so keep it inside the
 * Termux private tree by default.
 */
static inline const char *tgmux_zdrive_root(void)
{
    return tgmux_env( "WINE_ZDRIVE_ROOT", "/data/data/com.termux/files" );
}

/* The host font directory shipped with the glibc prefix. Overridable at
 * runtime so the font set can be relocated without rebuilding Wine.
 * Used by the freetype font loader to scan the Termux glibc font tree
 * directly, independent of fontconfig's own configuration. */
static inline const char *tgmux_fontdir(void)
{
    return tgmux_env( "WINE_FONT_DIR", WINE_TGMUX_PATH( "/share/fonts" ) );
}

#endif /* __WINE_TGMUX_PATHS_H */
