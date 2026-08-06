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

static inline LPCSTR debugstr_us( const UNICODE_STRING *us )
{
    if (!us) return "<null>";
    return debugstr_wn( us->Buffer, us->Length / sizeof(WCHAR) );
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

static NTSTATUS load_service(UNICODE_STRING *service)
{
    const WCHAR keypath[] = L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\";

    NTSTATUS status;
    UNICODE_STRING service_keypath;

    service_keypath.Length = sizeof(keypath) - sizeof(WCHAR);
    service_keypath.MaximumLength = sizeof(keypath) + service->Length;
    service_keypath.Buffer = RtlAllocateHeap(GetProcessHeap(), HEAP_ZERO_MEMORY, service_keypath.MaximumLength);
    if (!service_keypath.Buffer)
        return STATUS_NO_MEMORY;
    memcpy(service_keypath.Buffer, keypath, sizeof(keypath));
    RtlAppendUnicodeToString(&service_keypath, service->Buffer);

    status = ZwLoadDriver(&service_keypath);

    RtlFreeHeap(GetProcessHeap(), 0, service_keypath.Buffer);
    return status;
}

static NTSTATUS load_kmdf_library(WCHAR *component, WDF_VERSION *version)
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

    status = load_service(&service_str);
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

    TRACE("%p %s %p %p\n", driver, debugstr_us(reg_path), bind_info, component_globals);

    status = load_kmdf_library(bind_info->Component, &bind_info->Version);
    if (!NT_SUCCESS(status))
    {
        ERR("failed to load required KMDF component %s\n", debugstr_w(bind_info->Component));
        return STATUS_DLL_INIT_FAILED; /* FIXME what should be the correct status */
    }

    return STATUS_SUCCESS;
}

NTSTATUS WINAPI WdfVersionUnbind(UNICODE_STRING *reg_path, WDF_BIND_INFO *bind_info,
                                  WDF_COMPONENT_GLOBALS *component_globals)
{
    FIXME("%s %p %p stub!\n", debugstr_us(reg_path), bind_info, component_globals);
    return STATUS_SUCCESS;
}
