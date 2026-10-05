// ==WindhawkMod==
// @id              focus-peek
// @name            Focus Peek (No Transparency)
// @description     Brings the target window to focus on taskbar thumbnail hover without Aero Peek transparency via DWM Thumbnail Overlay
// @version         2.4.0
// @author          vak
// @github          https://github.com/Vakz03/Focus-Peek
// @include         explorer.exe
// @include         taskmgr.exe
// @include         windhawk.exe
// @compilerOptions -ldwmapi -lgdi32 -lcomctl32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Focus Peek (No Aero Peek Transparency)

Modifies taskbar hover preview behavior in Windows.

### Technical Details
- Hooks dwmapi.dll (ordinal 113) to completely suppress Aero Peek background fade and transparency.
- Projects the visual surface of the target window directly onto a dedicated overlay window via DwmRegisterThumbnail.
- Universal DWM Composition:
  - All windows (Spotify, Discord, VS Code, browsers, Notepad, Task Manager, Admin consoles, etc.) are projected by DWM without transparency onto the primary monitor.
  - For elevated processes included in the mod (Task Manager, Windhawk): When activation is confirmed via click, Explorer sends a filtered UIPI message (ChangeWindowMessageFilter) that the elevated process captures in its native message loop (GetMessage/PeekMessage) to relocate itself to the primary monitor with high integrity privileges.
- Multi-monitor: Projects the live preview onto the primary monitor regardless of which display the window resides on.
- Activation Confirmation: If the user clicks on the thumbnail or the preview to activate the window, it permanently moves to the primary monitor and receives focus. If the cursor is removed without clicking, the window remains in its original position.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- moveToPrimaryMonitor: true
  $name: Always show on primary monitor
  $description: Moves the preview to the primary monitor if the window is on another display, and keeps it there when clicked.
- previewMinimized: true
  $name: Preview minimized windows
  $description: Shows the window surface in the overlay even when minimized.
- blockShowDesktopPeek: false
  $name: Block transparency on Show Desktop
  $description: Suppresses transparency when hovering over the Show Desktop taskbar button.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <windhawk_api.h>

struct ModSettings {
    bool moveToPrimaryMonitor;
    bool previewMinimized;
    bool blockShowDesktopPeek;
} g_settings;

static bool g_bIsExplorerProcess = false;
static UINT g_msgFocusPeekActivate = 0;

// Modulo para procesos cliente elevados (taskmgr.exe, windhawk.exe)

typedef BOOL (WINAPI *pfnGetMessageW)(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax);
typedef BOOL (WINAPI *pfnPeekMessageW)(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg);

static pfnGetMessageW  pOriginalGetMessageW  = nullptr;
static pfnPeekMessageW pOriginalPeekMessageW = nullptr;

static void MoveSelfToPrimary(HWND hWndTarget) {
    if (!hWndTarget || !IsWindow(hWndTarget)) return;

    HWND hRoot = GetAncestor(hWndTarget, GA_ROOT);
    if (hRoot) hWndTarget = hRoot;

    HMONITOR hMonPrimary = MonitorFromWindow(NULL, MONITOR_DEFAULTTOPRIMARY);
    HMONITOR hMonTarget  = MonitorFromWindow(hWndTarget, MONITOR_DEFAULTTONEAREST);

    MONITORINFO miPrimary = { sizeof(miPrimary) };
    GetMonitorInfoW(hMonPrimary, &miPrimary);
    RECT rcWorkPrimary = miPrimary.rcWork;

    WINDOWPLACEMENT wp = { sizeof(wp) };
    GetWindowPlacement(hWndTarget, &wp);
    bool bWasMaximized = IsZoomed(hWndTarget) || (IsIconic(hWndTarget) && wp.showCmd == SW_SHOWMAXIMIZED);
    bool bWasMinimized = IsIconic(hWndTarget) != FALSE;

    if (hMonTarget != hMonPrimary) {
        if (bWasMaximized) {
            ShowWindow(hWndTarget, SW_RESTORE);
            SetWindowPos(
                hWndTarget, NULL,
                rcWorkPrimary.left, rcWorkPrimary.top,
                rcWorkPrimary.right - rcWorkPrimary.left,
                rcWorkPrimary.bottom - rcWorkPrimary.top,
                SWP_NOZORDER | SWP_NOACTIVATE
            );
            ShowWindow(hWndTarget, SW_MAXIMIZE);
        } else {
            MONITORINFO miTarget = { sizeof(miTarget) };
            GetMonitorInfoW(hMonTarget, &miTarget);
            RECT rcWorkTarget = miTarget.rcWork;

            RECT rcTarget = { 0 };
            if (bWasMinimized) {
                rcTarget = wp.rcNormalPosition;
            } else {
                GetWindowRect(hWndTarget, &rcTarget);
            }

            int relX = rcTarget.left - rcWorkTarget.left;
            int relY = rcTarget.top  - rcWorkTarget.top;
            int w = rcTarget.right - rcTarget.left;
            int h = rcTarget.bottom - rcTarget.top;

            int targetX = rcWorkPrimary.left + relX;
            int targetY = rcWorkPrimary.top  + relY;

            if (targetX + w > rcWorkPrimary.right)  targetX = rcWorkPrimary.right - w;
            if (targetX < rcWorkPrimary.left)       targetX = rcWorkPrimary.left;
            if (targetY + h > rcWorkPrimary.bottom) targetY = rcWorkPrimary.bottom - h;
            if (targetY < rcWorkPrimary.top)        targetY = rcWorkPrimary.top;

            if (bWasMinimized) {
                wp.rcNormalPosition.left   = targetX;
                wp.rcNormalPosition.top    = targetY;
                wp.rcNormalPosition.right  = targetX + w;
                wp.rcNormalPosition.bottom = targetY + h;
                SetWindowPlacement(hWndTarget, &wp);
                ShowWindow(hWndTarget, SW_RESTORE);
            } else {
                SetWindowPos(
                    hWndTarget, NULL,
                    targetX, targetY, w, h,
                    SWP_NOZORDER | SWP_ASYNCWINDOWPOS
                );
            }
        }
    } else {
        if (bWasMinimized) {
            ShowWindow(hWndTarget, SW_RESTORE);
        }
    }

    AllowSetForegroundWindow(ASFW_ANY);
    BringWindowToTop(hWndTarget);
    SetForegroundWindow(hWndTarget);
}

static BOOL WINAPI Hook_GetMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax) {
    BOOL res = pOriginalGetMessageW(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax);
    if (res > 0 && lpMsg && lpMsg->message == g_msgFocusPeekActivate) {
        Wh_Log(L"[Client] g_msgFocusPeekActivate caught in GetMessageW for %p", lpMsg->hwnd);
        MoveSelfToPrimary(lpMsg->hwnd);
    }
    return res;
}

static BOOL WINAPI Hook_PeekMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg) {
    BOOL res = pOriginalPeekMessageW(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg);
    if (res && lpMsg && lpMsg->message == g_msgFocusPeekActivate && (wRemoveMsg & PM_REMOVE)) {
        Wh_Log(L"[Client] g_msgFocusPeekActivate caught in PeekMessageW for %p", lpMsg->hwnd);
        MoveSelfToPrimary(lpMsg->hwnd);
    }
    return res;
}

// Modulo para explorer.exe

typedef HRESULT (WINAPI *pfnDwmActivateLivePreview)(
    BOOL  fActivate,
    HWND  hWndTarget,
    HWND  hWndTrigger,
    DWORD dwFlags,
    void* prcExclude
);

static pfnDwmActivateLivePreview pOriginalDwmActivateLivePreview = nullptr;

static const WCHAR OVERLAY_CLASS_NAME[]   = L"FocusPeekOverlayClass";
static const UINT_PTR TIMER_ID_ACTIVATION = 9001;

static HWND       g_hOverlay                 = nullptr;
static HTHUMBNAIL g_hThumbnail               = nullptr;
static HWND       g_hCurrentTarget           = nullptr;
static HWND       g_hPendingActivationTarget = nullptr;
static bool       g_bTargetWasOnSecondary    = false;

static void ActivateAndMoveTargetWindow(HWND hWndTarget);
static void HidePreviewOverlay();

static LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hWnd, &ps);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_LBUTTONDOWN: {
        Wh_Log(L"Direct click on overlay for window %p", g_hCurrentTarget);
        HWND hTarget = g_hCurrentTarget;
        HidePreviewOverlay();

        if (hTarget && IsWindow(hTarget)) {
            ActivateAndMoveTargetWindow(hTarget);
        }
        return 0;
    }

    case WM_TIMER: {
        if (wParam == TIMER_ID_ACTIVATION) {
            KillTimer(hWnd, TIMER_ID_ACTIVATION);
            HWND hTarget = g_hPendingActivationTarget;
            g_hPendingActivationTarget = nullptr;

            if (hTarget && IsWindow(hTarget)) {
                HWND hFg = GetForegroundWindow();
                HWND hRoot = GetAncestor(hFg, GA_ROOT);
                if (hFg == hTarget || hRoot == hTarget) {
                    Wh_Log(L"Activation verified (foreground=%p): moving to primary monitor", hTarget);
                    ActivateAndMoveTargetWindow(hTarget);
                } else {
                    Wh_Log(L"Timer: hover discarded without activation (foreground=%p, target=%p)", hFg, hTarget);
                }
            }
        }
        return 0;
    }

    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

static bool EnsureOverlayWindowCreated() {
    if (g_hOverlay && IsWindow(g_hOverlay)) {
        return true;
    }

    HINSTANCE hInstance = GetModuleHandleW(NULL);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = OverlayWndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = OVERLAY_CLASS_NAME;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;

    RegisterClassExW(&wc);

    g_hOverlay = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        OVERLAY_CLASS_NAME,
        L"FocusPeekOverlay",
        WS_POPUP,
        0, 0, 0, 0,
        NULL, NULL, hInstance, NULL
    );

    if (!g_hOverlay) {
        Wh_Log(L"Error creating overlay window: %lu", GetLastError());
        return false;
    }

    return true;
}

static void HidePreviewOverlay() {
    if (g_hThumbnail) {
        DwmUnregisterThumbnail(g_hThumbnail);
        g_hThumbnail = nullptr;
    }
    if (g_hOverlay && IsWindow(g_hOverlay)) {
        ShowWindow(g_hOverlay, SW_HIDE);
    }
}

static void ActivateAndMoveTargetWindow(HWND hWndTarget) {
    if (!hWndTarget || !IsWindow(hWndTarget)) {
        return;
    }

    Wh_Log(L"ActivateAndMoveTargetWindow: Activating window %p", hWndTarget);

    // Notificar al proceso cliente mediante mensaje registrado
    PostMessageW(hWndTarget, g_msgFocusPeekActivate, 0, 0);

    HMONITOR hMonPrimary = MonitorFromWindow(NULL, MONITOR_DEFAULTTOPRIMARY);
    HMONITOR hMonTarget  = MonitorFromWindow(hWndTarget, MONITOR_DEFAULTTONEAREST);
    bool bOnSecondary    = (hMonTarget != hMonPrimary);

    MONITORINFO miPrimary = { sizeof(miPrimary) };
    GetMonitorInfoW(hMonPrimary, &miPrimary);
    RECT rcWorkPrimary = miPrimary.rcWork;

    WINDOWPLACEMENT wp = { sizeof(wp) };
    GetWindowPlacement(hWndTarget, &wp);
    bool bWasMaximized = IsZoomed(hWndTarget) || (IsIconic(hWndTarget) && wp.showCmd == SW_SHOWMAXIMIZED);
    bool bWasMinimized = IsIconic(hWndTarget) != FALSE;

    if (bOnSecondary && g_settings.moveToPrimaryMonitor) {
        if (bWasMaximized) {
            ShowWindow(hWndTarget, SW_RESTORE);
            SetWindowPos(
                hWndTarget, NULL,
                rcWorkPrimary.left, rcWorkPrimary.top,
                rcWorkPrimary.right - rcWorkPrimary.left,
                rcWorkPrimary.bottom - rcWorkPrimary.top,
                SWP_NOZORDER | SWP_NOACTIVATE
            );
            ShowWindow(hWndTarget, SW_MAXIMIZE);
        } else {
            MONITORINFO miTarget = { sizeof(miTarget) };
            GetMonitorInfoW(hMonTarget, &miTarget);
            RECT rcWorkTarget = miTarget.rcWork;

            RECT rcTarget = { 0 };
            if (bWasMinimized) {
                rcTarget = wp.rcNormalPosition;
            } else {
                GetWindowRect(hWndTarget, &rcTarget);
            }

            int relX = rcTarget.left - rcWorkTarget.left;
            int relY = rcTarget.top  - rcWorkTarget.top;
            int w = rcTarget.right - rcTarget.left;
            int h = rcTarget.bottom - rcTarget.top;

            int targetX = rcWorkPrimary.left + relX;
            int targetY = rcWorkPrimary.top  + relY;

            if (targetX + w > rcWorkPrimary.right)  targetX = rcWorkPrimary.right - w;
            if (targetX < rcWorkPrimary.left)       targetX = rcWorkPrimary.left;
            if (targetY + h > rcWorkPrimary.bottom) targetY = rcWorkPrimary.bottom - h;
            if (targetY < rcWorkPrimary.top)        targetY = rcWorkPrimary.top;

            if (bWasMinimized) {
                wp.rcNormalPosition.left   = targetX;
                wp.rcNormalPosition.top    = targetY;
                wp.rcNormalPosition.right  = targetX + w;
                wp.rcNormalPosition.bottom = targetY + h;
                SetWindowPlacement(hWndTarget, &wp);
                ShowWindow(hWndTarget, SW_RESTORE);
            } else {
                SetWindowPos(
                    hWndTarget, NULL,
                    targetX, targetY, w, h,
                    SWP_NOZORDER | SWP_ASYNCWINDOWPOS
                );
            }
        }
    } else {
        if (bWasMinimized) {
            ShowWindow(hWndTarget, SW_RESTORE);
        }
    }

    AllowSetForegroundWindow(ASFW_ANY);
    BringWindowToTop(hWndTarget);
    SetForegroundWindow(hWndTarget);
}

static bool ShowPreviewOverlay(HWND hWndTarget) {
    if (!hWndTarget || !IsWindow(hWndTarget)) {
        return false;
    }

    if (!EnsureOverlayWindowCreated()) {
        return false;
    }

    if (g_hThumbnail) {
        DwmUnregisterThumbnail(g_hThumbnail);
        g_hThumbnail = nullptr;
    }

    HRESULT hr = DwmRegisterThumbnail(g_hOverlay, hWndTarget, &g_hThumbnail);
    if (FAILED(hr)) {
        Wh_Log(L"DwmRegisterThumbnail failed: 0x%08X for hWndTarget=%p", (unsigned int)hr, hWndTarget);
        return false;
    }

    SIZE srcSize = { 0 };
    DwmQueryThumbnailSourceSize(g_hThumbnail, &srcSize);

    HMONITOR hMonPrimary = MonitorFromWindow(NULL, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO miPrimary = { sizeof(miPrimary) };
    GetMonitorInfoW(hMonPrimary, &miPrimary);
    RECT rcWorkPrimary = miPrimary.rcWork;

    HMONITOR hMonTarget = MonitorFromWindow(hWndTarget, MONITOR_DEFAULTTONEAREST);
    g_bTargetWasOnSecondary = (hMonTarget != hMonPrimary);

    bool bMaximized = IsZoomed(hWndTarget) != FALSE;
    bool bIconic    = IsIconic(hWndTarget) != FALSE;
    WINDOWPLACEMENT wp = { sizeof(wp) };

    if (bIconic) {
        GetWindowPlacement(hWndTarget, &wp);
        if (wp.showCmd == SW_SHOWMAXIMIZED) {
            bMaximized = true;
        }
    }

    int targetX = 0;
    int targetY = 0;
    int targetW = 0;
    int targetH = 0;

    if (bMaximized) {
        targetX = rcWorkPrimary.left;
        targetY = rcWorkPrimary.top;
        targetW = rcWorkPrimary.right - rcWorkPrimary.left;
        targetH = rcWorkPrimary.bottom - rcWorkPrimary.top;
    } else {
        RECT rcTarget = { 0 };
        if (bIconic) {
            rcTarget = wp.rcNormalPosition;
        } else {
            HRESULT hrDwm = DwmGetWindowAttribute(hWndTarget, DWMWA_EXTENDED_FRAME_BOUNDS, &rcTarget, sizeof(rcTarget));
            if (FAILED(hrDwm) || (rcTarget.right == 0 && rcTarget.bottom == 0)) {
                GetWindowRect(hWndTarget, &rcTarget);
            }
        }

        int w = rcTarget.right - rcTarget.left;
        int h = rcTarget.bottom - rcTarget.top;

        if (w <= 0) w = srcSize.cx;
        if (h <= 0) h = srcSize.cy;
        if (w <= 0) w = 800;
        if (h <= 0) h = 600;

        int maxW = rcWorkPrimary.right - rcWorkPrimary.left;
        int maxH = rcWorkPrimary.bottom - rcWorkPrimary.top;
        if (w > maxW) w = maxW;
        if (h > maxH) h = maxH;

        if (g_bTargetWasOnSecondary && g_settings.moveToPrimaryMonitor) {
            MONITORINFO miTarget = { sizeof(miTarget) };
            GetMonitorInfoW(hMonTarget, &miTarget);
            RECT rcWorkTarget = miTarget.rcWork;

            int relX = rcTarget.left - rcWorkTarget.left;
            int relY = rcTarget.top  - rcWorkTarget.top;

            targetX = rcWorkPrimary.left + relX;
            targetY = rcWorkPrimary.top  + relY;

            if (targetX + w > rcWorkPrimary.right)  targetX = rcWorkPrimary.right - w;
            if (targetX < rcWorkPrimary.left)       targetX = rcWorkPrimary.left;
            if (targetY + h > rcWorkPrimary.bottom) targetY = rcWorkPrimary.bottom - h;
            if (targetY < rcWorkPrimary.top)        targetY = rcWorkPrimary.top;
        } else {
            targetX = rcTarget.left;
            targetY = rcTarget.top;
        }

        targetW = w;
        targetH = h;
    }

    DWORD cornerPref = bMaximized ? 1 : 2;
    DwmSetWindowAttribute(g_hOverlay, 33, &cornerPref, sizeof(cornerPref));

    DWM_THUMBNAIL_PROPERTIES props = { 0 };
    props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_OPACITY | DWM_TNP_VISIBLE;
    props.rcDestination.left   = 0;
    props.rcDestination.top    = 0;
    props.rcDestination.right  = targetW;
    props.rcDestination.bottom = targetH;
    props.opacity              = 255;
    props.fVisible             = TRUE;
    props.fSourceClientAreaOnly = FALSE;

    DwmUpdateThumbnailProperties(g_hThumbnail, &props);

    SetWindowPos(
        g_hOverlay,
        HWND_TOPMOST,
        targetX, targetY, targetW, targetH,
        SWP_NOACTIVATE | SWP_SHOWWINDOW
    );

    return true;
}

static HRESULT WINAPI Hook_DwmActivateLivePreview(
    BOOL  fActivate,
    HWND  hWndTarget,
    HWND  hWndTrigger,
    DWORD dwFlags,
    void* prcExclude
) {
    if (fActivate) {
        if (!hWndTarget || !IsWindow(hWndTarget)) {
            if (g_settings.blockShowDesktopPeek) {
                return S_OK;
            }
            return pOriginalDwmActivateLivePreview(fActivate, hWndTarget, hWndTrigger, dwFlags, prcExclude);
        }

        if (IsIconic(hWndTarget) && !g_settings.previewMinimized) {
            return S_OK;
        }

        g_hCurrentTarget = hWndTarget;

        Wh_Log(L"DwmActivateLivePreview: fActivate=1, hWndTarget=%p", hWndTarget);

        GetAsyncKeyState(VK_LBUTTON);

        if (!ShowPreviewOverlay(hWndTarget)) {
            return pOriginalDwmActivateLivePreview(fActivate, hWndTarget, hWndTrigger, dwFlags, prcExclude);
        }

        return S_OK;
    } else {
        Wh_Log(L"DwmActivateLivePreview: fActivate=0");

        HWND hTarget = g_hCurrentTarget;
        g_hCurrentTarget = nullptr;

        HidePreviewOverlay();

        if (hTarget && IsWindow(hTarget)) {
            SHORT mouseState = GetAsyncKeyState(VK_LBUTTON);
            bool bClicked = (mouseState & 0x8001) != 0;

            if (bClicked) {
                Wh_Log(L"DwmActivateLivePreview: Immediate click detected for target %p", hTarget);
                ActivateAndMoveTargetWindow(hTarget);
            } else {
                g_hPendingActivationTarget = hTarget;
                if (g_hOverlay && IsWindow(g_hOverlay)) {
                    SetTimer(g_hOverlay, TIMER_ID_ACTIVATION, 60, NULL);
                }
            }
        }

        if (!hWndTarget) {
            return pOriginalDwmActivateLivePreview(fActivate, hWndTarget, hWndTrigger, dwFlags, prcExclude);
        }

        return S_OK;
    }
}

// Inicializacion y ciclo de vida (Wh_ModInit / Wh_ModUninit)

static void LoadSettings() {
    g_settings.moveToPrimaryMonitor = Wh_GetIntSetting(L"moveToPrimaryMonitor") != 0;
    g_settings.previewMinimized     = Wh_GetIntSetting(L"previewMinimized") != 0;
    g_settings.blockShowDesktopPeek = Wh_GetIntSetting(L"blockShowDesktopPeek") != 0;
}

void Wh_ModSettingsChanged() {
    LoadSettings();
}

static bool DetectIfExplorerProcess() {
    WCHAR processPath[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, processPath, MAX_PATH);
    const WCHAR* pFileName = wcsrchr(processPath, L'\\');
    if (pFileName) {
        pFileName++;
    } else {
        pFileName = processPath;
    }
    return (_wcsicmp(pFileName, L"explorer.exe") == 0);
}

BOOL Wh_ModInit() {
    g_msgFocusPeekActivate = RegisterWindowMessageW(L"FocusPeek_Activate");
    g_bIsExplorerProcess   = DetectIfExplorerProcess();

    if (g_bIsExplorerProcess) {
        Wh_Log(L"Initializing Focus Peek v2.4 in explorer.exe...");
        LoadSettings();

        HMODULE hDwmApi = GetModuleHandleW(L"dwmapi.dll");
        if (!hDwmApi) {
            hDwmApi = LoadLibraryW(L"dwmapi.dll");
        }

        if (!hDwmApi) {
            Wh_Log(L"dwmapi.dll not available.");
            return FALSE;
        }

        FARPROC pDwmFunc = GetProcAddress(hDwmApi, MAKEINTRESOURCEA(113));
        if (!pDwmFunc) {
            pDwmFunc = GetProcAddress(hDwmApi, "DwmActivateLivePreview");
        }

        if (!pDwmFunc) {
            Wh_Log(L"Could not resolve ordinal 113 in dwmapi.dll.");
            return FALSE;
        }

        if (!Wh_SetFunctionHook((void*)pDwmFunc, (void*)Hook_DwmActivateLivePreview, (void**)&pOriginalDwmActivateLivePreview)) {
            Wh_Log(L"Error hooking DwmActivateLivePreview.");
            return FALSE;
        }

        Wh_Log(L"Focus Peek v2.4 (Explorer) initialized successfully.");
        return TRUE;
    } else {
        WCHAR processPath[MAX_PATH] = { 0 };
        GetModuleFileNameW(NULL, processPath, MAX_PATH);
        const WCHAR* pFileName = wcsrchr(processPath, L'\\');
        pFileName = pFileName ? (pFileName + 1) : processPath;

        Wh_Log(L"Initializing Focus Peek v2.4 in client process (%s)...", pFileName);

        // Permitir que el mensaje de activacion atraviese el filtro UIPI
        ChangeWindowMessageFilter(g_msgFocusPeekActivate, MSGFLT_ADD);

        HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
        if (hUser32) {
            FARPROC pGetMsg = GetProcAddress(hUser32, "GetMessageW");
            if (pGetMsg) {
                Wh_SetFunctionHook((void*)pGetMsg, (void*)Hook_GetMessageW, (void**)&pOriginalGetMessageW);
            }
            FARPROC pPeekMsg = GetProcAddress(hUser32, "PeekMessageW");
            if (pPeekMsg) {
                Wh_SetFunctionHook((void*)pPeekMsg, (void*)Hook_PeekMessageW, (void**)&pOriginalPeekMessageW);
            }
        }

        Wh_Log(L"Focus Peek v2.4 (Client) initialized successfully.");
        return TRUE;
    }
}

void Wh_ModUninit() {
    if (g_bIsExplorerProcess) {
        Wh_Log(L"Unloading Focus Peek from explorer.exe...");
        if (g_hOverlay && IsWindow(g_hOverlay)) {
            KillTimer(g_hOverlay, TIMER_ID_ACTIVATION);
        }
        HidePreviewOverlay();
        if (g_hOverlay && IsWindow(g_hOverlay)) {
            DestroyWindow(g_hOverlay);
            g_hOverlay = nullptr;
        }
        UnregisterClassW(OVERLAY_CLASS_NAME, GetModuleHandleW(NULL));
    } else {
        Wh_Log(L"Unloading Focus Peek from client process...");
    }
}
