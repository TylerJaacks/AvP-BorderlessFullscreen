/*
 * d3d9_borderless - drop-in d3d9.dll proxy that forces borderless fullscreen.
 *
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

typedef IDirect3D9 *(WINAPI *PFN_Create9)(UINT);
typedef HRESULT (WINAPI *PFN_Create9Ex)(UINT, IDirect3D9Ex **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateDevice)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD,
                                                       D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateDeviceEx)(IDirect3D9Ex *, UINT, D3DDEVTYPE, HWND, DWORD,
                                                         D3DPRESENT_PARAMETERS *, D3DDISPLAYMODEEX *,
                                                         IDirect3DDevice9Ex **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Reset)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);

static HMODULE            g_real;
static PFN_CreateDevice   o_CreateDevice;
static PFN_CreateDeviceEx o_CreateDeviceEx;
static PFN_Reset          o_Reset;
static HWND               g_hwnd;
static WNDPROC            g_oldProc;
static RECT               g_mon;

/* ---------- window ---------- */

static void MonitorRect(HWND h)
{
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi);
    g_mon = mi.rcMonitor;
}

static LRESULT CALLBACK SubProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_WINDOWPOSCHANGING: {
        WINDOWPOS *wp = (WINDOWPOS *)l;
        if (!IsIconic(h)) {
            wp->x  = g_mon.left;
            wp->y  = g_mon.top;
            wp->cx = g_mon.right - g_mon.left;
            wp->cy = g_mon.bottom - g_mon.top;
            wp->flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
        }
        break;
    }
    case WM_STYLECHANGING:
        if (w == GWL_STYLE) {
            STYLESTRUCT *ss = (STYLESTRUCT *)l;
            ss->styleNew = (ss->styleNew & ~(WS_OVERLAPPEDWINDOW | WS_DLGFRAME)) | WS_POPUP;
        } else if (w == GWL_EXSTYLE) {
            STYLESTRUCT *ss = (STYLESTRUCT *)l;
            ss->styleNew &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE |
                              WS_EX_WINDOWEDGE | WS_EX_TOPMOST);
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(w) != WA_INACTIVE) ClipCursor(&g_mon);
        else                          ClipCursor(NULL);
        break;
    case WM_NCCALCSIZE:          /* no non-client area at all */
        if (w) return 0;
        break;
    }
    return CallWindowProcW(g_oldProc, h, m, w, l);
}

static void MakeBorderless(HWND h)
{
    if (!h) return;
    if (h != g_hwnd) {
        g_hwnd    = h;
        g_oldProc = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)SubProc);
    }
    MonitorRect(h);
    SetWindowLongW(h, GWL_STYLE, (GetWindowLongW(h, GWL_STYLE) & ~WS_OVERLAPPEDWINDOW) | WS_POPUP | WS_VISIBLE);
    SetWindowLongW(h, GWL_EXSTYLE, GetWindowLongW(h, GWL_EXSTYLE) & ~(WS_EX_CLIENTEDGE | WS_EX_WINDOWEDGE | WS_EX_TOPMOST));
    SetWindowPos(h, HWND_TOP, g_mon.left, g_mon.top, g_mon.right - g_mon.left, g_mon.bottom - g_mon.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    SetForegroundWindow(h);
}

static void FixParams(D3DPRESENT_PARAMETERS *pp, HWND focus)
{
    HWND h = pp->hDeviceWindow ? pp->hDeviceWindow : focus;
    MakeBorderless(h);
    if (!pp->Windowed) {
        pp->Windowed                   = TRUE;
        pp->FullScreen_RefreshRateInHz = 0;
        if (!pp->BackBufferWidth || !pp->BackBufferHeight) {
            pp->BackBufferWidth  = g_mon.right - g_mon.left;
            pp->BackBufferHeight = g_mon.bottom - g_mon.top;
        }
    }
}

/* ---------- vtable patching ---------- */

static void *Patch(void *obj, int idx, void *fn)
{
    void **vt = *(void ***)obj, *old = vt[idx];
    DWORD prot;
    if (old == fn) return NULL;
    VirtualProtect(&vt[idx], sizeof(void *), PAGE_EXECUTE_READWRITE, &prot);
    vt[idx] = fn;
    VirtualProtect(&vt[idx], sizeof(void *), prot, &prot);
    return old;
}

static HRESULT STDMETHODCALLTYPE hk_Reset(IDirect3DDevice9 *d, D3DPRESENT_PARAMETERS *pp)
{
    if (pp) FixParams(pp, g_hwnd);
    return o_Reset(d, pp);
}

static void HookDevice(IDirect3DDevice9 *d)
{
    void *o = Patch(d, 16, (void *)hk_Reset);           /* IDirect3DDevice9::Reset */
    if (o) o_Reset = (PFN_Reset)o;
}

static HRESULT STDMETHODCALLTYPE hk_CreateDevice(IDirect3D9 *self, UINT a, D3DDEVTYPE t, HWND focus, DWORD f,
                                                 D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    if (pp) FixParams(pp, focus);
    HRESULT hr = o_CreateDevice(self, a, t, focus, f, pp, out);
    if (SUCCEEDED(hr) && out && *out) HookDevice(*out);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreateDeviceEx(IDirect3D9Ex *self, UINT a, D3DDEVTYPE t, HWND focus, DWORD f,
                                                   D3DPRESENT_PARAMETERS *pp, D3DDISPLAYMODEEX *mode,
                                                   IDirect3DDevice9Ex **out)
{
    if (pp) FixParams(pp, focus);
    HRESULT hr = o_CreateDeviceEx(self, a, t, focus, f, pp, NULL /* windowed: must be NULL */, out);
    if (SUCCEEDED(hr) && out && *out) HookDevice((IDirect3DDevice9 *)*out);
    return hr;
}

static void HookD3D(IDirect3D9 *d3d, BOOL ex)
{
    void *o = Patch(d3d, 16, (void *)hk_CreateDevice);  /* IDirect3D9::CreateDevice */
    if (o) o_CreateDevice = (PFN_CreateDevice)o;
    if (ex) {
        o = Patch(d3d, 20, (void *)hk_CreateDeviceEx);  /* IDirect3D9Ex::CreateDeviceEx */
        if (o) o_CreateDeviceEx = (PFN_CreateDeviceEx)o;
    }
}

/* ---------- loading the real d3d9 ---------- */

static HMODULE Real(void)
{
    if (g_real) return g_real;
    WCHAR p[MAX_PATH], *s;
    GetModuleFileNameW(NULL, p, MAX_PATH);
    if ((s = wcsrchr(p, L'\\'))) {
        lstrcpyW(s + 1, L"d3d9_dxvk.dll");
        if ((g_real = LoadLibraryW(p))) return g_real;
    }
    GetSystemDirectoryW(p, MAX_PATH);
    lstrcatW(p, L"\\d3d9.dll");
    return g_real = LoadLibraryW(p);
}

#define REAL(name) GetProcAddress(Real(), name)

__declspec(dllexport) IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk)
{
    PFN_Create9 f = (PFN_Create9)REAL("Direct3DCreate9");
    IDirect3D9 *d = f ? f(sdk) : NULL;
    if (d) HookD3D(d, FALSE);
    return d;
}

__declspec(dllexport) HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex **out)
{
    PFN_Create9Ex f = (PFN_Create9Ex)REAL("Direct3DCreate9Ex");
    HRESULT hr = f ? f(sdk, out) : D3DERR_NOTAVAILABLE;
    if (SUCCEEDED(hr) && out && *out) HookD3D((IDirect3D9 *)*out, TRUE);
    return hr;
}

/* D3DPERF_* pass-throughs (some games import them) */
__declspec(dllexport) int WINAPI D3DPERF_BeginEvent(D3DCOLOR c, LPCWSTR n)
{ FARPROC f = REAL("D3DPERF_BeginEvent"); return f ? ((int (WINAPI *)(D3DCOLOR, LPCWSTR))f)(c, n) : 0; }
__declspec(dllexport) int WINAPI D3DPERF_EndEvent(void)
{ FARPROC f = REAL("D3DPERF_EndEvent"); return f ? ((int (WINAPI *)(void))f)() : 0; }
__declspec(dllexport) void WINAPI D3DPERF_SetMarker(D3DCOLOR c, LPCWSTR n)
{ FARPROC f = REAL("D3DPERF_SetMarker"); if (f) ((void (WINAPI *)(D3DCOLOR, LPCWSTR))f)(c, n); }
__declspec(dllexport) void WINAPI D3DPERF_SetRegion(D3DCOLOR c, LPCWSTR n)
{ FARPROC f = REAL("D3DPERF_SetRegion"); if (f) ((void (WINAPI *)(D3DCOLOR, LPCWSTR))f)(c, n); }
__declspec(dllexport) BOOL WINAPI D3DPERF_QueryRepeatFrame(void)
{ FARPROC f = REAL("D3DPERF_QueryRepeatFrame"); return f ? ((BOOL (WINAPI *)(void))f)() : FALSE; }
__declspec(dllexport) void WINAPI D3DPERF_SetOptions(DWORD o)
{ FARPROC f = REAL("D3DPERF_SetOptions"); if (f) ((void (WINAPI *)(DWORD))f)(o); }
__declspec(dllexport) DWORD WINAPI D3DPERF_GetStatus(void)
{ FARPROC f = REAL("D3DPERF_GetStatus"); return f ? ((DWORD (WINAPI *)(void))f)() : 0; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p)
{
    (void)p;
    if (r == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(h);
    return TRUE;
}
