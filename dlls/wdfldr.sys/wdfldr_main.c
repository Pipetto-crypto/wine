/*
 * Copyright 2026 bluechxin, mkrsym1
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

WINE_DEFAULT_DEBUG_CHANNEL(wdfldr);

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

struct wdf_library
{
    struct list entry;
    UNICODE_STRING *service_path;
    WDF_LIBRARY_INFO *info;
};
static struct list wdf_libraries = LIST_INIT(wdf_libraries);

static inline LPCSTR debugstr_us( const UNICODE_STRING *us )
{
    if (!us) return "<null>";
    return debugstr_wn( us->Buffer, us->Length / sizeof(WCHAR) );
}

NTSTATUS WINAPI WdfRegisterLibrary(WDF_LIBRARY_INFO *info, UNICODE_STRING *service_path, UNICODE_STRING *device_name)
{
    NTSTATUS status;
    struct wdf_library *library;

    TRACE("%p %s %s\n", info, debugstr_us(service_path), debugstr_us(device_name));

    status = info->LibraryCommission();
    if (!NT_SUCCESS(status))
    {
        ERR("%s LibraryCommission failed %08lx\n", debugstr_us(service_path), status);
        return status;
    }

    library = RtlAllocateHeap(GetProcessHeap(), 0, sizeof(struct wdf_library));
    if (!library)
        return STATUS_NO_MEMORY;

    /* FIXME maybe it's not nice to expect these to live forever */
    library->service_path = service_path;
    library->info = info;

    /* FIXME we assume the world is nice and there will not be duplicates
     * FIXME we also assume there is no multithreading which is most likely true */
    list_add_tail(&wdf_libraries, &library->entry);

    TRACE("registered library %p\n", library);

    return status;
}

static NTSTATUS read_service_value(UNICODE_STRING *out, HANDLE key)
{
    const UNICODE_STRING service_val_us = RTL_CONSTANT_STRING(L"Service");

    NTSTATUS status;
    KEY_VALUE_PARTIAL_INFORMATION *info;
    ULONG info_size = 1, info_real_size;

    info = RtlAllocateHeap(GetProcessHeap(), 0, info_size);
    if (!info)
        return STATUS_NO_MEMORY;

    status = NtQueryValueKey(key, &service_val_us, KeyValuePartialInformation,
                             info, info_size, &info_real_size);
    if (status == STATUS_BUFFER_OVERFLOW || status == STATUS_BUFFER_TOO_SMALL)
    {
        info_size = info_real_size;
        info = RtlReAllocateHeap(GetProcessHeap(), 0, info, info_size);
        if (!info)
        {
            status = STATUS_NO_MEMORY;
            goto done;
        }
        status = NtQueryValueKey(key, &service_val_us, KeyValuePartialInformation,
                                 info, info_size, &info_real_size);
    }
    if (!NT_SUCCESS(status))
        goto done;

    if (info->Type != REG_SZ)
    {
        status = STATUS_UNSUCCESSFUL;
        goto done;
    }

    RtlCreateUnicodeString(out, (WCHAR *)info->Data);

done:
    RtlFreeHeap(GetProcessHeap(), 0, info);
    return status;
}

static NTSTATUS find_service_for_version(UNICODE_STRING *out, HANDLE key, ULONG *version_data, ULONG depth)
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG component;
    WCHAR subkey_backing[16] = {0};
    UNICODE_STRING subkey_us = { 0, sizeof(subkey_backing), subkey_backing };
    OBJECT_ATTRIBUTES subkey_attrs;
    HANDLE subkey;

    if (depth == 3)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    component = version_data[depth];

    TRACE("trying to specify version component %lu: %lu\n", depth, component);

    RtlIntegerToUnicodeString(component, 10, &subkey_us);
    InitializeObjectAttributes(&subkey_attrs, &subkey_us, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, key, NULL);

    status = NtOpenKey(&subkey, KEY_ALL_ACCESS, &subkey_attrs);
    if (!NT_SUCCESS(status))
        return status;

    /* there is a possibility we can specify the version further */
    /* FIXME this is likely not exactly how this api works */
    status = find_service_for_version(out, subkey, version_data, depth + 1);
    if (NT_SUCCESS(status))
        goto done;

    /* if we couldnt read at the lower layer, try reading at this one */
    status = read_service_value(out, subkey);

done:
    NtClose(subkey);
    return status;
}

static struct wdf_library *get_loaded_library(UNICODE_STRING *keypath)
{
    struct wdf_library *elem;
    LIST_FOR_EACH_ENTRY(elem, &wdf_libraries, struct wdf_library, entry)
    {
        if (RtlCompareUnicodeString(elem->service_path, keypath, TRUE) == 0)
        {
            TRACE("found library for %s\n", debugstr_us(keypath));
            return elem;
        }
    }
    TRACE("%s likely not yet loaded\n", debugstr_us(keypath));
    return NULL;
}

static NTSTATUS load_library_service(UNICODE_STRING *service, struct wdf_library **out_library)
{
    const WCHAR keypath[] = L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\";

    NTSTATUS status;
    UNICODE_STRING service_keypath;
    struct wdf_library *library = NULL;

    service_keypath.Length = sizeof(keypath) - sizeof(WCHAR);
    service_keypath.MaximumLength = sizeof(keypath) + service->Length;
    service_keypath.Buffer = RtlAllocateHeap(GetProcessHeap(), HEAP_ZERO_MEMORY, service_keypath.MaximumLength);
    if (!service_keypath.Buffer)
        return STATUS_NO_MEMORY;
    memcpy(service_keypath.Buffer, keypath, sizeof(keypath));
    RtlAppendUnicodeToString(&service_keypath, service->Buffer);

    /* if we already have it dont try loading again */
    library = get_loaded_library(&service_keypath);
    if (library)
    {
        status = STATUS_SUCCESS;
        goto done;
    }

    status = ZwLoadDriver(&service_keypath);
    library = get_loaded_library(&service_keypath);
    if (!library)
    {
        ERR("%s did not WdfRegisterLibrary after loading\n", debugstr_us(&service_keypath));
        status = STATUS_UNSUCCESSFUL;
    }

done:
    RtlFreeHeap(GetProcessHeap(), 0, service_keypath.Buffer);
    *out_library = library;
    return status;
}

static NTSTATUS load_kmdf_library(WCHAR *component, WDF_VERSION *version, struct wdf_library **out_library)
{
    const WCHAR key_pat[] = L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Wdf\\Kmdf\\%s\\Versions";

    NTSTATUS status = STATUS_SUCCESS;
    UNICODE_STRING key_str, service_str;
    OBJECT_ATTRIBUTES key_attrs;
    HANDLE key;
    ULONG *v_data = (ULONG *)version;

    TRACE("loading %s %lu.%lu.%lu\n", debugstr_w(component), v_data[0], v_data[1], v_data[2]);

    key_str.Length = _swprintf(NULL, key_pat, component) * sizeof(WCHAR);
    key_str.MaximumLength = key_str.Length + sizeof(WCHAR);
    key_str.Buffer = RtlAllocateHeap(GetProcessHeap(), 0, key_str.MaximumLength);
    if (!key_str.Buffer)
        return STATUS_NO_MEMORY;
    _swprintf(key_str.Buffer, key_pat, component);

    InitializeObjectAttributes(&key_attrs, &key_str, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    status = NtOpenKey(&key, KEY_ALL_ACCESS, &key_attrs);
    if (!NT_SUCCESS(status))
    {
        ERR("NtOpenKey failed %s status %08lx\n", debugstr_us(&key_str), status);
        goto done;
    }

    status = find_service_for_version(&service_str, key, v_data, 0);
    NtClose(key);

    if (!NT_SUCCESS(status))
    {
        ERR("no service for %s %lu.%lu.%lu status %08lx\n",
            debugstr_w(component), v_data[0], v_data[1], v_data[2], status);
        goto done;
    }

    TRACE("service for %s %lu.%lu.%lu is %s\n",
          debugstr_w(component), v_data[0], v_data[1], v_data[2], debugstr_us(&service_str));

    status = load_library_service(&service_str, out_library);
    if (!NT_SUCCESS(status))
    {
        ERR("loading service failed for %s (%s) status %08lx\n",
            debugstr_us(&service_str), debugstr_w(component), status);
    }

    RtlFreeUnicodeString(&service_str);

done:
    RtlFreeHeap(GetProcessHeap(), 0, key_str.Buffer);
    return status;
}

NTSTATUS WINAPI WdfVersionBind(DRIVER_OBJECT *driver, UNICODE_STRING *reg_path,
                                WDF_BIND_INFO *bind_info, PWDF_COMPONENT_GLOBALS *component_globals)
{
    NTSTATUS status;
    struct wdf_library *library;
    void *context;

    TRACE("%p %s %p %p\n", driver, debugstr_us(reg_path), bind_info, component_globals);

    status = load_kmdf_library(bind_info->Component, &bind_info->Version, &library);
    if (!NT_SUCCESS(status))
    {
        ERR("failed to load required KMDF component %s\n", debugstr_w(bind_info->Component));
        return STATUS_DLL_INIT_FAILED; /* FIXME what should be the correct status */
    }

    TRACE("found library %p %s\n", library, debugstr_us(library->service_path));

    status = library->info->LibraryRegisterClient(bind_info, component_globals, &context);
    if (!NT_SUCCESS(status))
        ERR("%s failed to register client\n", debugstr_w(bind_info->Component));
    else
        TRACE("%s bind succeeded\n", debugstr_w(bind_info->Component));

    return status;
}

NTSTATUS WINAPI WdfVersionUnbind(UNICODE_STRING *reg_path, WDF_BIND_INFO *bind_info,
                                  WDF_COMPONENT_GLOBALS *component_globals)
{
    FIXME("%s %p %p stub!\n", debugstr_us(reg_path), bind_info, component_globals);
    return STATUS_SUCCESS;
}
