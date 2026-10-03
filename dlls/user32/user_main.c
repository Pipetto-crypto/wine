/*
 * USER initialization code
 *
 * Copyright 2000 Alexandre Julliard
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

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "user_private.h"
#include "controls.h"
#include "imm.h"
#include "immdev.h"
#include "wine/debug.h"


WINE_DEFAULT_DEBUG_CHANNEL(graphics);
WINE_DECLARE_DEBUG_CHANNEL(message);

HMODULE user32_module = 0;

extern void WDML_NotifyThreadDetach(void);


static NTSTATUS WINAPI User32CopyImage( void *args, ULONG size )
{
    const struct copy_image_params *params = args;
    HANDLE ret = CopyImage( params->hwnd, params->type, params->dx, params->dy, params->flags );
    if (!ret) return STATUS_NO_MEMORY;
    return NtCallbackReturn( &ret, sizeof(ret), STATUS_SUCCESS );
}

static NTSTATUS WINAPI User32DrawNonClientButton( void *args, ULONG size )
{
    const struct draw_non_client_button_params *params = args;
    user_api->pNonClientButtonDraw( params->hwnd, params->hdc, params->type, params->rect,
                                    params->down, params->grayed );
    return STATUS_SUCCESS;
}

static NTSTATUS WINAPI User32DrawScrollBar( void *args, ULONG size )
{
    const struct draw_scroll_bar_params *params = args;
    RECT rect = params->rect;
    user_api->pScrollBarDraw( params->hwnd, params->hdc, params->bar, params->hit_test,
                              &params->tracking_info, params->arrows, params->interior,
                              &rect, params->enable_flags, params->arrow_size, params->thumb_pos,
                              params->thumb_size, params->vertical );
    return STATUS_SUCCESS;
}

static NTSTATUS WINAPI User32DrawText( void *args, ULONG size )
{
    const struct draw_text_params *params = args;
    struct draw_text_result result;

    size -= FIELD_OFFSET( struct draw_text_params, str );
    result.rect = params->rect;
    result.height = DrawTextW( params->hdc, params->str, size / sizeof(WCHAR), &result.rect, params->flags );
    return NtCallbackReturn( &result, sizeof(result), STATUS_SUCCESS );
}

static NTSTATUS WINAPI User32ImmProcessKey( void *args, ULONG size )
{
    const struct imm_process_key_params *params = args;
    if (ImmProcessKey( params->hwnd, params->hkl, params->vkey, params->key_data, 0 ))
        return STATUS_SUCCESS;
    return STATUS_INVALID_PARAMETER;
}

static NTSTATUS WINAPI User32ImmTranslateMessage( void *args, ULONG size )
{
    const struct imm_translate_message_params *params = args;
    if (ImmTranslateMessage( params->hwnd, params->msg, params->wparam, params->key_data ))
        return STATUS_SUCCESS;
    return STATUS_INVALID_PARAMETER;
}

static NTSTATUS WINAPI User32LoadImage( void *args, ULONG size )
{
    const struct load_image_params *params = args;
    HANDLE ret = LoadImageW( params->hinst, params->name, params->type,
                             params->dx, params->dy, params->flags );
    if (!ret) return STATUS_NO_MEMORY;
    return NtCallbackReturn( &ret, sizeof(ret), STATUS_SUCCESS );
}

static NTSTATUS WINAPI User32LoadSysMenu( void *args, ULONG size )
{
    const struct load_sys_menu_params *params = args;
    HMENU ret = LoadMenuW( user32_module, params->mdi ? L"SYSMENUMDI" : L"SYSMENU" );
    if (!ret) return STATUS_NO_MEMORY;
    return NtCallbackReturn( &ret, sizeof(ret), STATUS_SUCCESS );
}

static NTSTATUS WINAPI User32FreeCachedClipboardData( void *args, ULONG size )
{
    const struct free_cached_data_params *params = args;
    free_cached_data( params->format, params->handle );
    return STATUS_SUCCESS;
}

static NTSTATUS WINAPI User32PostDDEMessage( void *args, ULONG size )
{
    const struct post_dde_message_params *params = args;
    return post_dde_message( params->hwnd, params->msg, params->wparam, params->lparam,
                             params->dest_tid );
}

static NTSTATUS WINAPI User32RenderSynthesizedFormat( void *args, ULONG size )
{
    const struct render_synthesized_format_params *params = args;
    render_synthesized_format( params->format, params->from );
    return STATUS_SUCCESS;
}

static BOOL path_contains( const WCHAR *path, const WCHAR *needle )
{
    size_t len = wcslen( needle );

    for (; *path; path++)
        if (!wcsnicmp( path, needle, len )) return TRUE;
    return FALSE;
}

static BOOL dir_contains( const WCHAR *image, const WCHAR *name, const WCHAR *entry )
{
    WCHAR path[MAX_PATH];
    size_t len = name - image;

    if (len + wcslen( entry ) >= ARRAY_SIZE(path)) return FALSE;
    memcpy( path, image, len * sizeof(WCHAR) );
    wcscpy( path + len, entry );
    return GetFileAttributesW( path ) != INVALID_FILE_ATTRIBUTES;
}

static BOOL use_x11_launcher_driver( const WCHAR *image, const WCHAR *name )
{
    WCHAR custom[MAX_PATH];
    BOOL custom_is_path;
    DWORD size;

    size = GetEnvironmentVariableW( L"PROTON_USE_X11_EXCLUSIVE", custom, ARRAY_SIZE(custom) );
    if (size && size < ARRAY_SIZE(custom))
    {
        custom_is_path = wcschr( custom, '\\' ) || wcschr( custom, '/' );
        if ((!custom_is_path && !wcsicmp( name, custom )) ||
            (custom_is_path && path_contains( image, custom )))
            return TRUE;
    }

    if (!wcsicmp( name, L"UbisoftConnect.exe" ) ||
        !wcsicmp( name, L"UbisoftGameLauncher.exe" ) ||
        !wcsicmp( name, L"UbisoftGameLauncher64.exe" ) ||
        !wcsicmp( name, L"upc.exe" ) ||
        !wcsicmp( name, L"UplayWebCore.exe" ))
        return TRUE;

    if (path_contains( image, L"\\HoYoPlay\\" ) &&
        !path_contains( image, L"\\HoYoPlay\\games\\" ))
        return TRUE;

    if (path_contains( image, L"\\GRYPHLINK\\" ) &&
        !path_contains( image, L"\\GRYPHLINK\\games\\" ))
        return TRUE;

    if (path_contains( image, L"\\ArknightsEndfieldgowoU\\" ) &&
        !path_contains( image, L"\\ArknightsEndfieldgowoU\\games\\" ))
        return TRUE;

    if (path_contains( image, L"\\Electronic Arts\\EA Desktop\\" ) ||
        path_contains( image, L"\\Program Files (x86)\\Origin\\" ) ||
        path_contains( image, L"\\Program Files\\Origin\\" ))
        return TRUE;

    if (!wcsicmp( name, L"nikke_launcher.exe" ) ||
        !wcsicmp( name, L"tbs_browser.exe" ) ||
        !wcsicmp( name, L"INTLWebViewHelper.exe" ) ||
        !wcsicmp( name, L"msedgewebview2.exe" ))
        return TRUE;

    /* kuro launchers */
    if (!wcsicmp( name, L"launcher_main.exe" ) &&
        dir_contains( image, name, L"WebView2Loader.dll" ))
        return TRUE;

    return FALSE;
}

static NTSTATUS WINAPI User32LoadDriver( void *args, ULONG size )
{
    const WCHAR *path = args;
    const WCHAR *image = NtCurrentTeb()->Peb->ProcessParameters->ImagePathName.Buffer;
    const WCHAR *name = image, *p;
    UNICODE_STRING str;
    HMODULE module;
    NTSTATUS status;

    if ((p = wcsrchr( name, '\\' ))) name = p + 1;
    if ((p = wcsrchr( name, '/' ))) name = p + 1;

    /* These multi-process launcher UIs need X11's cross-process client
     * surfaces. Keep games launched by them on the desktop's Wayland driver. */
    if (!wcsicmp( path, L"winewayland.drv" ) && use_x11_launcher_driver( image, name ))
    {
        RtlInitUnicodeString( &str, L"winex11.drv" );
        status = LdrLoadDll( L"c:\\windows\\system32", 0, &str, &module );
        if (!status)
        {
            TRACE( "using winex11.drv for launcher process %s\n", debugstr_w(image) );
            return status;
        }
        TRACE( "winex11.drv unavailable for launcher process %s, using winewayland.drv, status %#lx\n",
               debugstr_w(image), status );
    }

    RtlInitUnicodeString( &str, path );
    return LdrLoadDll( L"c:\\windows\\system32", 0, &str, &module );
}

static NTSTATUS WINAPI User32UnpackDDEMessage( void *args, ULONG size )
{
    const struct unpack_dde_message_params *params = args;
    struct unpack_dde_message_result result = { .wparam = params->wparam, .lparam = params->lparam };

    size -= FIELD_OFFSET( struct unpack_dde_message_params, data );
    if (!unpack_dde_message( params->hwnd, params->message, &result.wparam, &result.lparam,
                             params->data, size ))
        return STATUS_NO_MEMORY;

    return NtCallbackReturn( &result, sizeof(result), STATUS_SUCCESS );
}

static NTSTATUS WINAPI User32CallDispatchCallback( void *args, ULONG size )
{
    struct dispatch_callback_params *params = args;
    ntuser_callback callback = (void *)(UINT_PTR)params->callback;
    return callback( params, size );
}

static NTSTATUS WINAPI User32DragDropEnter( void *args, ULONG size )
{
    if (!drag_drop_enter( size, args )) return STATUS_UNSUCCESSFUL;
    return STATUS_SUCCESS;
}

static NTSTATUS WINAPI User32DragDropLeave( void *args, ULONG size )
{
    drag_drop_leave();
    return STATUS_SUCCESS;
}

static NTSTATUS WINAPI User32DragDropDrag( void *args, ULONG size )
{
    const struct drag_drop_drag_params *params = args;
    UINT effect = drag_drop_drag( params->hwnd, params->point, params->effect );
    return NtCallbackReturn( &effect, sizeof(effect), STATUS_SUCCESS );
}

static NTSTATUS WINAPI User32DragDropDrop( void *args, ULONG size )
{
    const struct drag_drop_drop_params *params = args;
    UINT effect = drag_drop_drop( params->hwnd );
    return NtCallbackReturn( &effect, sizeof(effect), STATUS_SUCCESS );
}

static NTSTATUS WINAPI User32DragDropPost( void *args, ULONG size )
{
    const struct drag_drop_post_params *params = args;
    drag_drop_post( params->hwnd, params->drop_size, (DROPFILES *)&params->drop );
    return STATUS_SUCCESS;
}

C_ASSERT( NtUserCallCount <= 256 );

static KERNEL_CALLBACK_PROC kernel_callback_table[256] =
{
#define USER32_CALLBACK_ENTRY(name) User32##name,
    ALL_USER32_CALLBACKS
#undef USER32_CALLBACK_ENTRY
};

/***********************************************************************
 *           USER initialisation routine
 */
static BOOL process_attach(void)
{
    NtCurrentTeb()->Peb->KernelCallbackTable = kernel_callback_table;
    RegisterWaitForInputIdle( WaitForInputIdle );

    winproc_init();
    SYSPARAMS_Init();
    return TRUE;
}


/**********************************************************************
 *           thread_detach
 */
static void thread_detach(void)
{
    struct ntuser_thread_info *thread_info = NtUserGetThreadInfo();

    NtUserCallNoParam( NtUserExitingThread );

    WDML_NotifyThreadDetach();

    NtUserCallNoParam( NtUserThreadDetach );
    HeapFree( GetProcessHeap(), 0, (void *)(UINT_PTR)thread_info->wmchar_data );
}


/***********************************************************************
 *           UserClientDllInitialize  (USER32.@)
 *
 * USER dll initialisation routine (exported as UserClientDllInitialize for compatibility).
 */
BOOL WINAPI DllMain( HINSTANCE inst, DWORD reason, LPVOID reserved )
{
    static HMODULE imm32_module;
    BOOL ret = TRUE;

    switch(reason)
    {
    case DLL_PROCESS_ATTACH:
        user32_module = inst;
        ret = process_attach();
        if(ret)
            imm32_module = LoadLibraryW(L"imm32.dll");
        break;
    case DLL_THREAD_DETACH:
        thread_detach();
        break;
    case DLL_PROCESS_DETACH:
        FreeLibrary(imm32_module);
        break;
    }
    return ret;
}


/***********************************************************************
 *		ExitWindowsEx (USER32.@)
 */
BOOL WINAPI ExitWindowsEx( UINT flags, DWORD reason )
{
    WCHAR app[MAX_PATH];
    WCHAR cmdline[MAX_PATH + 64];
    PROCESS_INFORMATION pi;
    STARTUPINFOW si;
    void *redir;

    GetSystemDirectoryW( app, MAX_PATH - ARRAY_SIZE( L"\\wineboot.exe" ));
    lstrcatW( app, L"\\wineboot.exe" );
    lstrcpyW( cmdline, app );

    if (flags & EWX_FORCE) lstrcatW( cmdline, L" --kill" );
    else
    {
        lstrcatW( cmdline, L" --end-session" );
        if (flags & EWX_FORCEIFHUNG) lstrcatW( cmdline, L" --force" );
    }
    if (!(flags & EWX_REBOOT)) lstrcatW( cmdline, L" --shutdown" );

    memset( &si, 0, sizeof si );
    si.cb = sizeof si;
    Wow64DisableWow64FsRedirection( &redir );
    if (!CreateProcessW( app, cmdline, NULL, NULL, FALSE, DETACHED_PROCESS, NULL, NULL, &si, &pi ))
    {
        Wow64RevertWow64FsRedirection( redir );
        ERR( "Failed to run %s\n", debugstr_w(cmdline) );
        return FALSE;
    }
    Wow64RevertWow64FsRedirection( redir );
    CloseHandle( pi.hProcess );
    CloseHandle( pi.hThread );
    return TRUE;
}

/***********************************************************************
 *		LockWorkStation (USER32.@)
 */
BOOL WINAPI LockWorkStation(void)
{
    TRACE(": stub\n");
    SetLastError( ERROR_CALL_NOT_IMPLEMENTED );
    return FALSE;
}

/***********************************************************************
 *		RegisterServicesProcess (USER32.@)
 */
int WINAPI RegisterServicesProcess(DWORD ServicesProcessId)
{
    FIXME("(0x%lx): stub\n", ServicesProcessId);
    return 0;
}

/***********************************************************************
 *		ShutdownBlockReasonCreate (USER32.@)
 */
BOOL WINAPI ShutdownBlockReasonCreate(HWND hwnd, LPCWSTR reason)
{
    TRACE("(%p, %s)\n", hwnd, debugstr_w(reason));

    if (GetWindowThreadProcessId(hwnd, NULL) != GetCurrentThreadId())
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }

    return TRUE;
}

/***********************************************************************
 *		ShutdownBlockReasonDestroy (USER32.@)
 */
BOOL WINAPI ShutdownBlockReasonDestroy(HWND hwnd)
{
    TRACE("(%p)\n", hwnd);

    if (GetWindowThreadProcessId(hwnd, NULL) != GetCurrentThreadId())
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }

    return TRUE;
}

const char *SPY_GetMsgName( UINT msg, HWND hwnd )
{
    char buf[128];
    NtUserMessageCall( hwnd, msg, ARRAYSIZE(buf), 0, buf, NtUserSpyGetMsgName, FALSE );
    return wine_dbg_sprintf( "%s", buf );
}

void SPY_EnterMessage( INT flag, HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    if (TRACE_ON(message)) NtUserMessageCall( hwnd, msg, wparam, lparam, 0, NtUserSpyEnter, flag );
}

void SPY_ExitMessage( INT flag, HWND hwnd, UINT msg, LRESULT lreturn, WPARAM wparam, LPARAM lparam )
{
    if (TRACE_ON(message)) NtUserMessageCall( hwnd, msg, wparam, lparam, (void *)lreturn,
                                              NtUserSpyExit, flag );
}
