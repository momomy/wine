/*
 * Report the display as SDR to applications using D3DMetal
 *
 * On a Mac with an EDR capable screen D3DMetal's DXGI reports the output as
 * HDR10. Some games then render in HDR unconditionally and offer no way to
 * turn it off (The Witcher 3 5.0 looks blown out). D3DMetal is a binary, so
 * its IDXGIOutput6::GetDesc1() and IDXGISwapChain3::CheckColorSpaceSupport()
 * are patched in place when the application loads dxgi.dll. WINE_HDR=1 leaves
 * them alone.
 *
 * This lives in user32 because it has to run before the application queries
 * the output: user32 is loaded before dxgi by practically every game, the
 * display driver is not.
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
#define COBJMACROS
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "objbase.h"
#include "dxgi1_6.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3dmetal);

void d3dmetal_hdr_init(void);
void CDECL __wine_d3dmetal_hdr_sync(void);

static const GUID hdr_IID_IDXGIFactory2 = {0x50c83a1c, 0xe072, 0x4c48, {0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0}};
static const GUID hdr_IID_IDXGIOutput6 = {0x068346e8, 0xaaec, 0x4b84, {0xad, 0xd7, 0x13, 0x7f, 0x51, 0x3f, 0x77, 0xa1}};
static const GUID hdr_IID_IDXGISwapChain3 = {0x94d99bdb, 0xf1f8, 0x4ab0, {0xb2, 0x36, 0x7d, 0xa0, 0x17, 0x0e, 0xda, 0xb1}};

static HRESULT (STDMETHODCALLTYPE *orig_GetDesc1)(IDXGIOutput6 *, DXGI_OUTPUT_DESC1 *);
static HRESULT (STDMETHODCALLTYPE *orig_CheckColorSpaceSupport)(IDXGISwapChain3 *, DXGI_COLOR_SPACE_TYPE, UINT *);
static HRESULT (STDMETHODCALLTYPE *orig_CreateSwapChain)(IDXGIFactory2 *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
static HRESULT (STDMETHODCALLTYPE *orig_CreateSwapChainForHwnd)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *,
                                                                const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);
static LONG thread_started, swapchain_hooked;

static void patch_pointer(void **slot, void *hook, void **orig)
{
    DWORD old;

    if (*slot == hook) return;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_EXECUTE_READWRITE, &old)) return;
    *orig = *slot;
    *slot = hook;
    VirtualProtect(slot, sizeof(*slot), old, &old);
}

static HRESULT STDMETHODCALLTYPE hook_GetDesc1(IDXGIOutput6 *iface, DXGI_OUTPUT_DESC1 *desc)
{
    HRESULT hr = orig_GetDesc1(iface, desc);

    if (SUCCEEDED(hr) && desc && desc->ColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)
    {
        TRACE("reporting color space %#x as SDR\n", desc->ColorSpace);
        desc->ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        desc->BitsPerColor = 8;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hook_CheckColorSpaceSupport(IDXGISwapChain3 *iface, DXGI_COLOR_SPACE_TYPE colour_space, UINT *support)
{
    HRESULT hr = orig_CheckColorSpaceSupport(iface, colour_space, support);

    if (SUCCEEDED(hr) && support && colour_space != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) *support = 0;
    return hr;
}

static void hook_swapchain(IUnknown *swapchain)
{
    IDXGISwapChain3 *swapchain3;

    if (FAILED(IUnknown_QueryInterface(swapchain, &hdr_IID_IDXGISwapChain3, (void **)&swapchain3))) return;
    if (!InterlockedExchange(&swapchain_hooked, 1))
        patch_pointer((void **)&swapchain3->lpVtbl->CheckColorSpaceSupport, hook_CheckColorSpaceSupport,
                      (void **)&orig_CheckColorSpaceSupport);
    IDXGISwapChain3_Release(swapchain3);
}

static HRESULT STDMETHODCALLTYPE hook_CreateSwapChain(IDXGIFactory2 *iface, IUnknown *device, DXGI_SWAP_CHAIN_DESC *desc,
                                                      IDXGISwapChain **swapchain)
{
    HRESULT hr = orig_CreateSwapChain(iface, device, desc, swapchain);

    if (SUCCEEDED(hr) && swapchain && *swapchain) hook_swapchain((IUnknown *)*swapchain);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hook_CreateSwapChainForHwnd(IDXGIFactory2 *iface, IUnknown *device, HWND hwnd,
                                                             const DXGI_SWAP_CHAIN_DESC1 *desc,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen_desc,
                                                             IDXGIOutput *output, IDXGISwapChain1 **swapchain)
{
    HRESULT hr = orig_CreateSwapChainForHwnd(iface, device, hwnd, desc, fullscreen_desc, output, swapchain);

    if (SUCCEEDED(hr) && swapchain && *swapchain) hook_swapchain((IUnknown *)*swapchain);
    return hr;
}

/* runs on its own thread, a factory cannot be created under the loader lock */
static DWORD WINAPI hook_thread(void *arg)
{
    HRESULT (WINAPI *create_factory)(UINT, REFIID, void **);
    IDXGIFactory2 *factory = NULL;
    IDXGIAdapter1 *adapter;
    IDXGIOutput *output;
    IDXGIOutput6 *output6;
    HMODULE dxgi;
    UINT i, j;

    if (!(dxgi = GetModuleHandleW(L"dxgi.dll")) ||
        !(create_factory = (void *)GetProcAddress(dxgi, "CreateDXGIFactory2")) ||
        FAILED(create_factory(0, &hdr_IID_IDXGIFactory2, (void **)&factory)) || !factory)
    {
        WARN("failed to create a DXGI factory\n");
        return 0;
    }

    patch_pointer((void **)&factory->lpVtbl->CreateSwapChain, hook_CreateSwapChain, (void **)&orig_CreateSwapChain);
    patch_pointer((void **)&factory->lpVtbl->CreateSwapChainForHwnd, hook_CreateSwapChainForHwnd,
                  (void **)&orig_CreateSwapChainForHwnd);

    for (i = 0; SUCCEEDED(IDXGIFactory2_EnumAdapters1(factory, i, &adapter)); i++)
    {
        for (j = 0; SUCCEEDED(IDXGIAdapter1_EnumOutputs(adapter, j, &output)); j++)
        {
            if (SUCCEEDED(IDXGIOutput_QueryInterface(output, &hdr_IID_IDXGIOutput6, (void **)&output6)))
            {
                patch_pointer((void **)&output6->lpVtbl->GetDesc1, hook_GetDesc1, (void **)&orig_GetDesc1);
                IDXGIOutput6_Release(output6);
            }
            IDXGIOutput_Release(output);
        }
        IDXGIAdapter1_Release(adapter);
    }

    IDXGIFactory2_Release(factory);
    TRACE("D3DMetal outputs are reported as SDR\n");
    return 0;
}

static void start_hook_thread(void)
{
    HANDLE thread;

    if (InterlockedExchange(&thread_started, 1)) return;
    if ((thread = CreateThread(NULL, 0, hook_thread, NULL, 0, NULL))) CloseHandle(thread);
}

static void CALLBACK dll_notification(ULONG reason, LDR_DLL_NOTIFICATION_DATA *data, void *context)
{
    if (reason != LDR_DLL_NOTIFICATION_REASON_LOADED || !data->Loaded.BaseDllName) return;
    if (!RtlCompareUnicodeString(data->Loaded.BaseDllName, context, TRUE)) start_hook_thread();
}

static BOOL hdr_hook_enabled(void)
{
    WCHAR buffer[16];

    if (!GetEnvironmentVariableW(L"CX_ACTIVE_GRAPHICS_BACKEND", buffer, ARRAY_SIZE(buffer)) || wcsicmp(buffer, L"d3dmetal")) return FALSE;
    if (GetEnvironmentVariableW(L"WINE_HDR", buffer, ARRAY_SIZE(buffer)) && buffer[0] != '0') return FALSE;
    return TRUE;
}

/* called by ntdll right before the entry point of the process: the patch is in place before the
 * application runs when it imports dxgi.dll statically */
void CDECL __wine_d3dmetal_hdr_sync(void)
{
    if (!hdr_hook_enabled() || !GetModuleHandleW(L"dxgi.dll")) return;
    if (InterlockedExchange(&thread_started, 1)) return;
    hook_thread(NULL);
}

void d3dmetal_hdr_init(void)
{
    static UNICODE_STRING dxgi_name = RTL_CONSTANT_STRING(L"dxgi.dll");
    void *cookie;

    if (!hdr_hook_enabled()) return;
    /* for applications that load dxgi.dll later; statically imported ones are handled synchronously */
    LdrRegisterDllNotification(0, dll_notification, &dxgi_name, &cookie);
}
