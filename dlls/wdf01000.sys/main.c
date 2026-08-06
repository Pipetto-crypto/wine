/*
 * Copyright 2026 mkrsym1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <stdlib.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "ddk/wdm.h"
#include "wine/list.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(kmdf);

typedef struct _WDF_VERSION {
    ULONG Major;
    ULONG Minor;
    ULONG Build;
} WDF_VERSION;

typedef struct _WDF_BIND_INFO {
    ULONG Size;
    WCHAR *Component;
    WDF_VERSION Version;
    ULONG FuncCount;
    void **FuncTable;
    void *Module;
} WDF_BIND_INFO, *PWDF_BIND_INFO;

typedef struct _WDF_COMPONENT_GLOBALS {
    ULONG Size;
    void *DriverObject;
    void *RegistryPath;
    void *FuncTable;
    ULONG Reserved[16];
} WDF_COMPONENT_GLOBALS, *PWDF_COMPONENT_GLOBALS;

typedef NTSTATUS WINAPI (*PFNLIBRARYCOMMISSION)(VOID);
typedef NTSTATUS WINAPI (*PFNLIBRARYDECOMMISSION)(VOID);
typedef NTSTATUS WINAPI (*PFNLIBRARYREGISTERCLIENT)(WDF_BIND_INFO *info, PWDF_COMPONENT_GLOBALS *globals, void *context);
typedef NTSTATUS WINAPI (*PFNLIBRARYUNREGISTERCLIENT)(WDF_BIND_INFO *info, WDF_COMPONENT_GLOBALS *globals);

typedef struct _WDF_LIBRARY_INFO {
    ULONG Size;
    PFNLIBRARYCOMMISSION LibraryCommission;
    PFNLIBRARYDECOMMISSION LibraryDecommission;
    PFNLIBRARYREGISTERCLIENT LibraryRegisterClient;
    PFNLIBRARYUNREGISTERCLIENT LibraryUnregisterClient;
    WDF_VERSION Version;
} WDF_LIBRARY_INFO, *PWDF_LIBRARY_INFO;

NTSTATUS WINAPI WdfRegisterLibrary(WDF_LIBRARY_INFO *info, UNICODE_STRING *service_path, UNICODE_STRING *device_name);


static inline LPCSTR debugstr_us( const UNICODE_STRING *us )
{
    if (!us) return "<null>";
    return debugstr_wn( us->Buffer, us->Length / sizeof(WCHAR) );
}

static void *kmdf_functable[1024];

static WDF_COMPONENT_GLOBALS kmdf_globals = {
    sizeof(WDF_COMPONENT_GLOBALS), NULL, NULL, kmdf_functable, {0}
};

static NTSTATUS WINAPI kmdf_LibraryCommission(void) {
    FIXME("stub\n");
    return STATUS_SUCCESS;
}

static NTSTATUS WINAPI kmdf_LibraryDecommission(void) {
    FIXME("stub\n");
    return STATUS_SUCCESS;
}

static NTSTATUS WINAPI kmdf_LibraryRegisterClient(WDF_BIND_INFO *info, PWDF_COMPONENT_GLOBALS *globals, void *context) {
    FIXME("stub %p %p %p\n", info, globals, context);

    if (info->FuncCount > ARRAY_SIZE(kmdf_functable))
    {
        ERR("FuncTable size mismatch, we have %lu, client wants %lu\n",
            (ULONG)ARRAY_SIZE(kmdf_functable), info->FuncCount);
        return STATUS_UNSUCCESSFUL;
    }

    *(info->FuncTable) = kmdf_functable;
    *globals = &kmdf_globals;

    return STATUS_SUCCESS;
}

static NTSTATUS WINAPI kmdf_LibraryUnregisterClient(WDF_BIND_INFO *info, WDF_COMPONENT_GLOBALS *globals) {
    FIXME("stub: %p %p\n", info, globals);
    return STATUS_SUCCESS;
}

static WDF_LIBRARY_INFO kmdf_wdf_library_info = {
    sizeof(WDF_LIBRARY_INFO),
    kmdf_LibraryCommission,
    kmdf_LibraryDecommission,
    kmdf_LibraryRegisterClient,
    kmdf_LibraryUnregisterClient,
    { 1, 0, 0 }
};

/* stub for WdfDriverMiniportUnload (idx 201) */
static void WINAPI wdf_stub_void_noop(void *arg)
{
    TRACE("stub called with %p\n", arg);
}

NTSTATUS WINAPI DriverEntry(DRIVER_OBJECT *driver, UNICODE_STRING *path)
{
    NTSTATUS status;

    TRACE("%p %s\n", driver, debugstr_us(path));

    kmdf_globals.DriverObject = driver;
    kmdf_globals.RegistryPath = path;

    /* populate index 201 (WdfDriverMiniportUnload) */
    kmdf_functable[201] = wdf_stub_void_noop;

    status = WdfRegisterLibrary(&kmdf_wdf_library_info, path, NULL);

    return status;
}
