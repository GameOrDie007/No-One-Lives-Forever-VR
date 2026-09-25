// A proxy DINPUT.dll, dropped beside lithtech.exe.
//
// WHY
//
// The game fails to start on this machine most of the time - an access
// violation in ntdll, or a spin at 100% CPU that never reaches the client
// shell. A stack walk of the hung process puts it here:
//
//     lithtech+0x36ba2
//       DINPUT   (a recursive descent, the same three frames repeating)
//         LocalFree -> RtlFreeHeap        <- spins, or faults
//
// It is in lithtech.exe's DirectInput init, before OnEngineInitialized, so no
// client log is ever written and none of our code has run. All 37 HID
// interfaces on this machine pass a GetPreparsedData/GetCaps/Free cycle
// individually (tools/crashtools/hidprobe32.exe), so it is not one bad device -
// it is a 2000-era DirectInput meeting a modern device set.
//
// This project has no use for DirectInput. Input is keyboard, mouse and the
// OpenXR host. So the cheapest correct answer is not to fix the enumeration but
// to skip it.
//
// HOW
//
// lithtech.exe imports DINPUT.dll by name, and Windows searches the
// executable's own directory before System32 (DINPUT is not a KnownDLL). A DLL
// of that name in game\ is therefore loaded instead of the system one. No
// injection, no patching of lithtech.exe, and it is reverted by deleting a
// file.
//
// MODES - chosen by the NOLFVR_DINPUT environment variable:
//
//   pass             forward everything to the real DINPUT.dll, unchanged.
//                    This is the CONTROL: with the proxy present and in this
//                    mode the game must behave exactly as it does without it.
//                    It did - same failure rate, and the log showed the
//                    forwarding - which is what made the other modes readable.
//
//   block            fail DirectInputCreate*. MEASURED 27 August: this does
//                    remove the access violation - the process exits with code
//                    0 instead of 0xC0000005 - but the engine will not run
//                    without DirectInput. It asks for version 0x700, is
//                    refused, falls back to 0x300, is refused, and quits. Kept
//                    because it is the experiment that proved the fault is
//                    reached through DirectInput and nothing else.
//
//   filter (DEFAULT) let creation succeed, then replace EnumDevices in the
//                    returned interface's vtable so the JOYSTICK sweep never
//                    runs. Keyboard and mouse enumerate normally.
//
//                    This is where the evidence points. In pass mode the log
//                    shows DirectInputCreateA(0x700) returning S_OK, so the
//                    crash is not in creation - it is in a later call through
//                    the interface, and the stack walk puts it inside DINPUT
//                    walking HID devices, which is the game-controller sweep.
//                    Filtering the callback would be too late: the damage is
//                    done inside the real EnumDevices, before any callback
//                    fires. So the joystick sweep must not be CALLED, not
//                    merely have its results discarded.
//
//                    MEASURED 27 August: 3 launches, 3 successes, no retries,
//                    all exiting cleanly. The same machine managed 0 in 18
//                    immediately before. It is the default because a fix that
//                    depends on an environment variable being set is a fix
//                    that can silently not be applied.
//
// Every run appends to game\logs\dinput-proxy.log, so which mode was actually
// in force is a matter of record rather than of memory - a mode that can only
// be set by an environment variable is a mode that can silently not be set.

#include <windows.h>
#include <unknwn.h>     // LPUNKNOWN, REFIID, REFCLSID - excluded by WIN32_LEAN_AND_MEAN
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

typedef HRESULT (WINAPI *PFN_CreateA)(HINSTANCE, DWORD, void**, LPUNKNOWN);
typedef HRESULT (WINAPI *PFN_CreateW)(HINSTANCE, DWORD, void**, LPUNKNOWN);
typedef HRESULT (WINAPI *PFN_CreateEx)(HINSTANCE, DWORD, REFIID, void**, LPUNKNOWN);
typedef HRESULT (WINAPI *PFN_NoArgs)(void);
typedef HRESULT (WINAPI *PFN_GetClass)(REFCLSID, REFIID, void**);

static HMODULE      g_real       = NULL;
static PFN_CreateA  g_CreateA    = NULL;
static PFN_CreateW  g_CreateW    = NULL;
static PFN_CreateEx g_CreateEx   = NULL;
static PFN_NoArgs   g_CanUnload  = NULL;
static PFN_GetClass g_GetClass   = NULL;
static bool         g_block      = false;
static bool         g_filter     = false;
// Declared up here with the other modes because Init() reads it, and Init()
// comes before the keyboard block that uses it.
static bool         g_bgKeyboard = true;
static char         g_logPath[MAX_PATH] = {0};

// DirectInput 7 device types, as passed to EnumDevices.
#define DI_DEVTYPE_DEVICE    0x00000001
#define DI_DEVTYPE_MOUSE     0x00000002
#define DI_DEVTYPE_KEYBOARD  0x00000003
#define DI_DEVTYPE_JOYSTICK  0x00000004

// COM methods are __stdcall with the interface pointer as the first argument.
typedef HRESULT (STDMETHODCALLTYPE *PFN_EnumDevices)(void *self, DWORD devType,
                                                     void *cb, void *ref, DWORD flags);

static PFN_EnumDevices g_realEnum   = NULL;
static bool            g_vtPatched  = false;

static void Log(const char *fmt, ...)
{
    if (!g_logPath[0]) return;
    FILE *f = fopen(g_logPath, "a");
    if (!f) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);

    fputc('\n', f);
    fclose(f);
}

static void Init(void)
{
    // The log goes beside the executable, not into the working directory - the
    // working directory is not reliably where anyone thinks it is.
    char exe[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    char *slash = strrchr(exe, '\\');
    if (slash) {
        *slash = 0;
        _snprintf(g_logPath, MAX_PATH - 1, "%s\\logs\\dinput-proxy.log", exe);
        char dir[MAX_PATH];
        _snprintf(dir, MAX_PATH - 1, "%s\\logs", exe);
        CreateDirectoryA(dir, NULL);
    }

    char mode[64] = {0};
    GetEnvironmentVariableA("NOLFVR_DINPUT", mode, sizeof(mode) - 1);
    // Filter is the default. "pass" and "block" have to be asked for by name,
    // and anything unrecognised falls through to filter rather than quietly
    // disabling the fix.
    g_block  = (_stricmp(mode, "block") == 0);
    g_filter = !g_block && (_stricmp(mode, "pass") != 0);

    // The background-keyboard fix is ON unless explicitly switched off: a fix
    // that needs an environment variable set is a fix that can silently not be
    // applied, which is the reasoning that made "filter" the default mode too.
    char bg[64] = {0};
    GetEnvironmentVariableA("NOLFVR_DINPUT_BG", bg, sizeof(bg) - 1);
    g_bgKeyboard = !(bg[0] == '0' && bg[1] == 0);


    // Load the REAL one by absolute path. LoadLibraryA("DINPUT.dll") would find
    // this proxy again and recurse.
    char sys[MAX_PATH] = {0};
    GetSystemDirectoryA(sys, MAX_PATH);       // SysWOW64 for a 32-bit process
    strncat(sys, "\\DINPUT.dll", MAX_PATH - strlen(sys) - 1);
    g_real = LoadLibraryA(sys);

    Log("---- proxy loaded, mode=%s ----",
        g_block ? "BLOCK" : (g_filter ? "FILTER" : "pass"));
    Log("real DINPUT: %s (%s)", sys, g_real ? "loaded" : "FAILED TO LOAD");

    if (g_real) {
        g_CreateA   = (PFN_CreateA) GetProcAddress(g_real, "DirectInputCreateA");
        g_CreateW   = (PFN_CreateW) GetProcAddress(g_real, "DirectInputCreateW");
        g_CreateEx  = (PFN_CreateEx)GetProcAddress(g_real, "DirectInputCreateEx");
        g_CanUnload = (PFN_NoArgs)  GetProcAddress(g_real, "DllCanUnloadNow");
        g_GetClass  = (PFN_GetClass)GetProcAddress(g_real, "DllGetClassObject");
        Log("forwards: CreateA=%p CreateW=%p CreateEx=%p",
            g_CreateA, g_CreateW, g_CreateEx);
    }
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);
        Init();
    }
    return TRUE;
}

// DIERR_NOTFOUND. A specific "there is nothing here" rather than a generic
// failure, because an engine is more likely to have a graceful path for it.
#define PROXY_REFUSE ((HRESULT)0x80070002L)

// --- filter mode ------------------------------------------------------------

// Replaces EnumDevices on the interface the engine was handed.
//
// The joystick sweep is never called. Everything else is forwarded untouched,
// including the "enumerate everything" case, which is split into the two
// classes we are willing to let DirectInput walk.
static HRESULT STDMETHODCALLTYPE ProxyEnumDevices(void *self, DWORD devType,
                                                  void *cb, void *ref, DWORD flags)
{
    if (devType == DI_DEVTYPE_JOYSTICK) {
        Log("EnumDevices(JOYSTICK) SKIPPED - this is the sweep that hangs");
        return S_OK;
    }

    if (devType == 0) {
        // 0 means "every class". Ask for the two safe ones by name instead.
        Log("EnumDevices(ALL) -> keyboard + mouse only, joystick skipped");
        HRESULT hr = g_realEnum(self, DI_DEVTYPE_KEYBOARD, cb, ref, flags);
        if (FAILED(hr)) Log("  keyboard enum failed 0x%08lx", hr);
        HRESULT hr2 = g_realEnum(self, DI_DEVTYPE_MOUSE, cb, ref, flags);
        if (FAILED(hr2)) Log("  mouse enum failed 0x%08lx", hr2);
        return SUCCEEDED(hr) ? hr2 : hr;
    }

    Log("EnumDevices(type=%lu) -> real", devType);
    return g_realEnum(self, devType, cb, ref, flags);
}

// Swap slot 4 of the interface's vtable. Patching the vtable rather than
// wrapping the object means QueryInterface, AddRef, Release and every other
// method still reach the real implementation - a wrapper would have to get all
// of them right, and getting QueryInterface wrong on a COM object the engine
// then casts is a worse bug than the one being fixed.
//
// Slot 4 is EnumDevices for IDirectInputA and every version derived from it:
// 0-2 are IUnknown, 3 is CreateDevice, 4 is EnumDevices.
static void PatchEnumDevices(void *pDI)
{
    if (!pDI || g_vtPatched || !g_filter) return;

    void **vt = *(void ***)pDI;
    if (!vt) { Log("PatchEnumDevices: no vtable"); return; }

    DWORD oldProt = 0;
    if (!VirtualProtect(&vt[4], sizeof(void *), PAGE_READWRITE, &oldProt)) {
        Log("PatchEnumDevices: VirtualProtect failed %lu", GetLastError());
        return;
    }

    g_realEnum = (PFN_EnumDevices)vt[4];
    vt[4] = (void *)ProxyEnumDevices;

    DWORD dummy = 0;
    VirtualProtect(&vt[4], sizeof(void *), oldProt, &dummy);

    g_vtPatched = true;
    Log("EnumDevices patched: real=%p proxy=%p", g_realEnum, ProxyEnumDevices);
}

// --- THE KEYBOARD MUST KEEP WORKING WHEN THE GAME IS NOT IN FRONT ----------
//
// the chat key (T) does nothing with a headset
// on. At the desk, with the game window focused and no mirror, it works - the
// tester typed mpmaphole and it skipped. So the bind, the message path and
// the text entry are all fine, and the only thing that differs in a headset is
// FOREGROUND: the host window is in front by design, the game window parks at
// the splash, and a DirectInput device acquired at FOREGROUND cooperative
// level returns nothing at all once its window is not the active one.
//
// The mirror key forwarding cannot help and never could: it uses PostMessageW,
// which puts a WINDOW MESSAGE in a queue, and DirectInput reads DEVICE STATE.
// Those two never meet. This is the other end of the same problem, and it is
// the end that can actually be fixed.
//
// KEYBOARD ONLY, deliberately. Forcing the MOUSE non-exclusive would change
// cursor capture in the menus, which is not broken and is not being asked
// about. The device pointers created for GUID_SysKeyboard are remembered and
// every other device passes through untouched.
//
// NOLFVR_DINPUT_BG=0 turns this off without a rebuild.

#define P_DISCL_EXCLUSIVE     0x00000001
#define P_DISCL_NONEXCLUSIVE  0x00000002
#define P_DISCL_FOREGROUND    0x00000004
#define P_DISCL_BACKGROUND    0x00000008
#define P_DISCL_NOWINKEY      0x00000010

// {6F1D2B61-D5A0-11CF-BFC7-444553540000}
static const GUID P_GUID_SysKeyboard =
    { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateDevice)(void *self, const GUID *guid,
                                                      void **out, LPUNKNOWN unk);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetCoop)(void *self, HWND hwnd, DWORD flags);

static PFN_CreateDevice g_realCreateDev = NULL;
static PFN_SetCoop      g_realSetCoop   = NULL;
static bool             g_cdPatched     = false;
static void            *g_kbDev[4]      = {0};
static int              g_kbCount       = 0;

static bool IsKeyboardDevice(void *dev)
{
    for (int i = 0; i < g_kbCount; ++i) if (g_kbDev[i] == dev) return true;
    return false;
}

static HRESULT STDMETHODCALLTYPE ProxySetCooperativeLevel(void *self, HWND hwnd, DWORD flags)
{
    if (!g_realSetCoop) return E_FAIL;
    if (!g_bgKeyboard || !IsKeyboardDevice(self))
        return g_realSetCoop(self, hwnd, flags);

    // BACKGROUND requires NONEXCLUSIVE for a keyboard; NOWINKEY requires
    // FOREGROUND, so it goes too. Anything else the game asked for is kept.
    const DWORD want = (flags & ~(P_DISCL_FOREGROUND | P_DISCL_EXCLUSIVE | P_DISCL_NOWINKEY))
                       | P_DISCL_BACKGROUND | P_DISCL_NONEXCLUSIVE;
    HRESULT hr = g_realSetCoop(self, hwnd, want);
    Log("keyboard SetCooperativeLevel(hwnd=%p) 0x%lx -> 0x%lx : %s",
        hwnd, flags, want,
        SUCCEEDED(hr) ? "FORCED TO BACKGROUND - keys work with the window behind"
                      : "FAILED, falling back to what the game asked for");
    if (FAILED(hr)) hr = g_realSetCoop(self, hwnd, flags);
    return hr;
}

// Slot 13 of IDirectInputDeviceA is SetCooperativeLevel (0-2 IUnknown, then
// GetCapabilities, EnumObjects, GetProperty, SetProperty, Acquire, Unacquire,
// GetDeviceState, GetDeviceData, SetDataFormat, SetEventNotification).
// Device2 and Device7 extend this interface, so the slot does not move.
static void PatchDeviceCoop(void *dev)
{
    if (!dev) return;
    void **vt = *(void ***)dev;
    if (!vt || vt[13] == (void *)ProxySetCooperativeLevel) return;

    DWORD oldProt = 0;
    if (!VirtualProtect(&vt[13], sizeof(void *), PAGE_READWRITE, &oldProt)) {
        Log("PatchDeviceCoop: VirtualProtect failed %lu"
            "  <- THE KEYBOARD FIX IS NOT IN FORCE", GetLastError());
        return;
    }
    g_realSetCoop = (PFN_SetCoop)vt[13];
    vt[13] = (void *)ProxySetCooperativeLevel;
    DWORD dummy = 0;
    VirtualProtect(&vt[13], sizeof(void *), oldProt, &dummy);
    Log("SetCooperativeLevel patched on the keyboard device: real=%p", g_realSetCoop);
}

static HRESULT STDMETHODCALLTYPE ProxyCreateDevice(void *self, const GUID *guid,
                                                   void **out, LPUNKNOWN unk)
{
    if (!g_realCreateDev) return E_FAIL;
    HRESULT hr = g_realCreateDev(self, guid, out, unk);
    if (SUCCEEDED(hr) && out && *out && guid &&
        memcmp(guid, &P_GUID_SysKeyboard, sizeof(GUID)) == 0)
    {
        if (g_kbCount < 4) g_kbDev[g_kbCount++] = *out;
        Log("CreateDevice(SysKeyboard) -> %p", *out);
        if (g_bgKeyboard) PatchDeviceCoop(*out);
        else Log("  NOLFVR_DINPUT_BG=0: left at the level the game asks for");
    }
    return hr;
}

// Slot 3 is CreateDevice, on the same interface whose slot 4 is EnumDevices.
static void PatchCreateDevice(void *pDI)
{
    if (!pDI || g_cdPatched || !g_filter) return;
    void **vt = *(void ***)pDI;
    if (!vt) return;

    DWORD oldProt = 0;
    if (!VirtualProtect(&vt[3], sizeof(void *), PAGE_READWRITE, &oldProt)) {
        Log("PatchCreateDevice: VirtualProtect failed %lu"
            "  <- THE KEYBOARD FIX IS NOT IN FORCE", GetLastError());
        return;
    }
    g_realCreateDev = (PFN_CreateDevice)vt[3];
    vt[3] = (void *)ProxyCreateDevice;
    DWORD dummy = 0;
    VirtualProtect(&vt[3], sizeof(void *), oldProt, &dummy);
    g_cdPatched = true;
    Log("CreateDevice patched: real=%p proxy=%p  (background keyboard %s)",
        g_realCreateDev, ProxyCreateDevice, g_bgKeyboard ? "ON" : "OFF");
}

static void PatchInterface(void *pDI)
{
    PatchEnumDevices(pDI);
    PatchCreateDevice(pDI);
}


extern "C" {

HRESULT WINAPI DirectInputCreateA(HINSTANCE h, DWORD ver, void **out, LPUNKNOWN unk)
{
    if (g_block) {
        Log("DirectInputCreateA(ver=0x%lx) REFUSED", ver);
        if (out) *out = NULL;
        return PROXY_REFUSE;
    }
    if (!g_CreateA) { Log("DirectInputCreateA: no forward"); return PROXY_REFUSE; }
    Log("DirectInputCreateA(ver=0x%lx) -> real", ver);
    HRESULT hr = g_CreateA(h, ver, out, unk);
    Log("DirectInputCreateA returned 0x%08lx", hr);
    if (SUCCEEDED(hr) && out) PatchInterface(*out);
    return hr;
}

HRESULT WINAPI DirectInputCreateW(HINSTANCE h, DWORD ver, void **out, LPUNKNOWN unk)
{
    if (g_block) {
        Log("DirectInputCreateW(ver=0x%lx) REFUSED", ver);
        if (out) *out = NULL;
        return PROXY_REFUSE;
    }
    if (!g_CreateW) { Log("DirectInputCreateW: no forward"); return PROXY_REFUSE; }
    Log("DirectInputCreateW(ver=0x%lx) -> real", ver);
    HRESULT hr = g_CreateW(h, ver, out, unk);
    Log("DirectInputCreateW returned 0x%08lx", hr);
    if (SUCCEEDED(hr) && out) PatchInterface(*out);
    return hr;
}

HRESULT WINAPI DirectInputCreateEx(HINSTANCE h, DWORD ver, REFIID iid, void **out, LPUNKNOWN unk)
{
    if (g_block) {
        Log("DirectInputCreateEx(ver=0x%lx) REFUSED", ver);
        if (out) *out = NULL;
        return PROXY_REFUSE;
    }
    if (!g_CreateEx) { Log("DirectInputCreateEx: no forward"); return PROXY_REFUSE; }
    Log("DirectInputCreateEx(ver=0x%lx) -> real", ver);
    HRESULT hr = g_CreateEx(h, ver, iid, out, unk);
    Log("DirectInputCreateEx returned 0x%08lx", hr);
    if (SUCCEEDED(hr) && out) PatchInterface(*out);
    return hr;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return g_CanUnload ? g_CanUnload() : S_FALSE;
}

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out)
{
    if (g_block) {
        Log("DllGetClassObject REFUSED");
        if (out) *out = NULL;
        return PROXY_REFUSE;
    }
    return g_GetClass ? g_GetClass(clsid, iid, out) : PROXY_REFUSE;
}

HRESULT WINAPI DllRegisterServer(void)   { return S_OK; }
HRESULT WINAPI DllUnregisterServer(void) { return S_OK; }

} // extern "C"
