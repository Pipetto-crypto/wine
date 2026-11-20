/*
 * Termux glibc backing files for the fsync shared-memory region.
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
#include <sys/mman.h>
#include <unistd.h>

#include "wine/tgmux_paths.h"

#define TGMUX_FSYNC_NAME_SIZE 64

/* Match the server directory identity; inode numbers alone are not unique across devices. */
static inline void tgmux_fsync_name( char *name, unsigned long long device, unsigned long long inode )
{
    snprintf( name, TGMUX_FSYNC_NAME_SIZE, "/wine-%llx-%llx-fsync", device, inode );
}

/* An explicit override must be the same for wineserver and all of its clients. */
static inline const char *tgmux_fsync_dir(void)
{
    const char *dir = getenv( "WINE_FSYNC_SHM_DIR" );

    if (dir && *dir) return dir;
#ifdef __linux__
    dir = tgmux_tmpdir();
    /* Termux glibc cannot assume that /dev/shm is mounted or writable. */
    if (!access( WINE_TGMUX_DEFAULT_TMPDIR, W_OK | X_OK ) ||
        (access( "/dev/shm", W_OK | X_OK ) && !access( dir, W_OK | X_OK ))) return dir;
#endif
    return NULL;
}

/* Use negative flags for unlink, and native POSIX shared memory when dir is NULL. */
static inline int tgmux_fsync_shm( const char *dir, const char *name, int flags, mode_t mode )
{
    int ret, err;
    char *path;

    if (!dir) return flags < 0 ? shm_unlink( name ) : shm_open( name, flags, mode );
    if (dir[0] != '/')
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
