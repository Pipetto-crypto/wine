/*
 * Termux glibc backing files for the fsync shared-memory region.
 *
 * The region is a plain file inside the Termux temp directory. POSIX
 * shm_open() is never used: /dev/shm is neither dependable nor in the
 * same mount namespace as the Termux glibc prefix on Android.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_TGMUX_FSYNC_H
#define __WINE_TGMUX_FSYNC_H

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "wine/tgmux_paths.h"

#define TGMUX_FSYNC_NAME_SIZE 64

/* Match the server directory identity; inode numbers alone are not unique across devices. */
static inline void tgmux_fsync_name( char *name, unsigned long long device, unsigned long long inode )
{
    snprintf( name, TGMUX_FSYNC_NAME_SIZE, "/wine-%llx-%llx-fsync", device, inode );
}

/*
 * Backing directory for the fsync shared-memory region.
 *
 * Always the Termux temp directory: $WINE_FSYNC_SHM_DIR, then $WINE_TMP_DIR,
 * then $TMPDIR, then /data/data/com.termux/files/usr/tmp. There is no
 * /dev/shm fallback because Bionic /dev/shm is a separate mount and is
 * frequently absent or read-only. wineserver and every client must agree,
 * so the override is read here rather than at each call site.
 */
static inline const char *tgmux_fsync_dir(void)
{
    return tgmux_env( "WINE_FSYNC_SHM_DIR", tgmux_tmpdir() );
}

/* Use negative flags for unlink. `dir` must be an absolute path. */
static inline int tgmux_fsync_shm( const char *dir, const char *name, int flags, mode_t mode )
{
    int ret, err;
    char *path;

    if (!dir || dir[0] != '/')
    {
        errno = EINVAL;
        return -1;
    }
    if (asprintf( &path, "%s%s", dir, name ) < 0) return -1;
    ret = flags < 0 ? unlink( path ) : open( path, flags | O_NOFOLLOW, mode );
    err = errno;
    free( path );
    errno = err;
    return ret;
}

#endif /* __WINE_TGMUX_FSYNC_H */
