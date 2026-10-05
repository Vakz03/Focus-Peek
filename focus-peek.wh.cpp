// ==WindhawkMod==
// @id              focus-peek
// @name            Focus Peek (No Transparency)
// @description     Brings the target window to focus on taskbar thumbnail hover without Aero Peek transparency via DWM Thumbnail Overlay
// @version         2.4.3
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
- Dedicated UI thread with message pump for the overlay window, isolating thumbnail rendering from Explorer worker threads.
- Foreground win-event hook and watchdog timer ensure the overlay immediately dismisses if another window gains focus (e.g. game scenes, popups, fullscreens) or if the cursor leaves the taskbar.
- Fullscreen and borderless window detection ensures proper aspect and rectangular geometry.
- Universal DWM Composition:
  - All windows (Spotify, Discord, VS Code, browsers, Notepad, Task Manager, Godot, Admin consoles, etc.) are projected by DWM without transparency onto the primary monitor.
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

struct TargetGeometry {
    bool isMaximized;
    int  x;
    int  y;
    int  w;
    int  h;
};

static bool IsWindowEffectivelyMaximized(HWND hWndTarget) {
    if (IsZoomed(hWndTarget)) return true;

    WINDOWPLACEMENT wp = { sizeof(wp) };
    if (GetWindowPlacement(hWndTarget, &wp)) {
        if (wp.showCmd == SW_SHOWMAXIMIZED || wp.showCmd == SW_MAXIMIZE) {
            return true;
        }
        if ((wp.showCmd == SW_SHOWMINIMIZED || wp.showCmd == SW_MINIMIZE) &&
            (wp.flags & WPF_RESTORETOMAXIMIZED)) {
            return true;
        }
    }
    return false;
}

static bool IsWindowFullscreenOrMaximized(HWND hWndTarget, HMONITOR hMonTarget) {
    if (IsWindowEffectivelyMaximized(hWndTarget)) return true;

    // Comprobar si la ventana cubre el monitor completo (estilo juego o app sin bordes)
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfoW(hMonTarget, &mi)) {
        RECT rc = { 0 };
        if (GetWindowRect(hWndTarget, &rc)) {
            int monW = mi.rcMonitor.right - mi.rcMonitor.left;
            int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;
            int winW = rc.right - rc.left;
            int winH = rc.bottom - rc.top;
            if (winW >= monW && winH >= monH) {
                return true;
            }
        }
    }
    return false;
}

static TargetGeometry CalculateTargetGeometry(HWND hWndTarget, SIZE fallbackSize) {
    TargetGeometry geo = { 0 };
    if (!hWndTarget || !IsWindow(hWndTarget)) return geo;

    HMONITOR hMonPrimary = MonitorFromWindow(NULL, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO miPrimary = { sizeof(miPrimary) };
    GetMonitorInfoW(hMonPrimary, &miPrimary);
    RECT rcWorkPrimary = miPrimary.rcWork;

    HMONITOR hMonTarget = MonitorFromWindow(hWndTarget, MONITOR_DEFAULTTONEAREST);
    MONITORINFO miTarget = { sizeof(miTarget) };
    GetMonitorInfoW(hMonTarget, &miTarget);
    RECT rcWorkTarget = miTarget.rcWork;

    bool bOnSecondary = (hMonTarget != hMonPrimary);

    geo.isMaximized = IsWindowFullscreenOrMaximized(hWndTarget, hMonTarget);

    if (geo.isMaximized) {
        RECT rcWork = (bOnSecondary && g_settings.moveToPrimaryMonitor) ? rcWorkPrimary : rcWorkTarget;
        geo.x = rcWork.left;
        geo.y = rcWork.top;
        geo.w = rcWork.right - rcWork.left;
        geo.h = rcWork.bottom - rcWork.top;
        return geo;
    }

    bool bIconic = IsIconic(hWndTarget) != FALSE;
    RECT rcTarget = { 0 };

    if (bIconic) {
        WINDOWPLACEMENT wp = { sizeof(wp) };
        GetWindowPlacement(hWndTarget, &wp);
        rcTarget = wp.rcNormalPosition;
    } else {
        HRESULT hrDwm = DwmGetWindowAttribute(hWndTarget, DWMWA_EXTENDED_FRAME_BOUNDS, &rcTarget, sizeof(rcTarget));
        if (FAILED(hrDwm) || (rcTarget.right == 0 && rcTarget.bottom == 0)) {
            GetWindowRect(hWndTarget, &rcTarget);
        }
    }

    int w = rcTarget.right - rcTarget.left;
    int h = rcTarget.bottom - rcTarget.top;

    if (w <= 0) w = fallbackSize.cx;
    if (h <= 0) h = fallbackSize.cy;
    if (w <= 0) w = 800;
    if (h <= 0) h = 600;

    RECT rcWorkBounds = (bOnSecondary && g_settings.moveToPrimaryMonitor) ? rcWorkPrimary : rcWorkTarget;
    int maxW = rcWorkBounds.right - rcWorkBounds.left;
    int maxH = rcWorkBounds.bottom - rcWorkBounds.top;
    if (w > maxW) w = maxW;
    if (h > maxH) h = maxH;

    int targetX = 0;
    int targetY = 0;

    if (bOnSecondary && g_settings.moveToPrimaryMonitor) {
        int relX = rcTarget.left - rcWorkTarget.left;
        int relY = rcTarget.top  - rcWorkTarget.top;

        targetX = rcWorkPrimary.left + relX;
        targetY = rcWorkPrimary.top  + relY;
    } else {
        targetX = rcTarget.left;
        targetY = rcTarget.top;
    }

    // Centrar en caso de que las coordenadas esten vacias (ej. apps suspendidas o minimizadas sin posicion)
    if (targetX == 0 && targetY == 0 && bIconic) {
        targetX = rcWorkBounds.left + (maxW - w) / 2;
        targetY = rcWorkBounds.top  + (maxH - h) / 2;
    }

    if (targetX + w > rcWorkBounds.right)  targetX = rcWorkBounds.right - w;
    if (targetX < rcWorkBounds.left)       targetX = rcWorkBounds.left;
    if (targetY + h > rcWorkBounds.bottom) targetY = rcWorkBounds.bottom - h;
    if (targetY < rcWorkBounds.top)        targetY = rcWorkBounds.top;

    geo.x = targetX;
    geo.y = targetY;
    geo.w = w;
    geo.h = h;
    return geo;
}

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

    TargetGeometry geo = CalculateTargetGeometry(hWndTarget, { 0, 0 });

    BOOL bTransitionsDisabled = TRUE;
    DwmSetWindowAttribute(hWndTarget, DWMWA_TRANSITIONS_FORCEDISABLED, &bTransitionsDisabled, sizeof(bTransitionsDisabled));

    if (hMonTarget != hMonPrimary && g_settings.moveToPrimaryMonitor) {
        if (geo.isMaximized) {
            WINDOWPLACEMENT wp = { sizeof(wp) };
            GetWindowPlacement(hWndTarget, &wp);
            wp.showCmd = SW_SHOWMAXIMIZED;
            wp.flags |= WPF_RESTORETOMAXIMIZED;
            wp.rcNormalPosition.left   = geo.x;
            wp.rcNormalPosition.top    = geo.y;
            wp.rcNormalPosition.right  = geo.x + geo.w;
            wp.rcNormalPosition.bottom = geo.y + geo.h;
            SetWindowPlacement(hWndTarget, &wp);
            ShowWindow(hWndTarget, SW_SHOWMAXIMIZED);
        } else {
            if (IsIconic(hWndTarget)) {
                WINDOWPLACEMENT wp = { sizeof(wp) };
                GetWindowPlacement(hWndTarget, &wp);
                wp.showCmd = SW_SHOWNORMAL;
                wp.rcNormalPosition.left   = geo.x;
                wp.rcNormalPosition.top    = geo.y;
                wp.rcNormalPosition.right  = geo.x + geo.w;
                wp.rcNormalPosition.bottom = geo.y + geo.h;
                SetWindowPlacement(hWndTarget, &wp);
                ShowWindow(hWndTarget, SW_RESTORE);
            } else {
                SetWindowPos(
                    hWndTarget, NULL,
                    geo.x, geo.y, geo.w, geo.h,
                    SWP_NOZORDER | SWP_ASYNCWINDOWPOS
                );
            }
        }
    } else {
        if (IsIconic(hWndTarget)) {
            if (geo.isMaximized) {
                ShowWindow(hWndTarget, SW_SHOWMAXIMIZED);
            } else {
                ShowWindow(hWndTarget, SW_RESTORE);
            }
        }
    }

    AllowSetForegroundWindow(ASFW_ANY);
    BringWindowToTop(hWndTarget);
    SetForegroundWindow(hWndTarget);

    bTransitionsDisabled = FALSE;
    DwmSetWindowAttribute(hWndTarget, DWMWA_TRANSITIONS_FORCEDISABLED, &bTransitionsDisabled, sizeof(bTransitionsDisabled));
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

static const WCHAR OVERLAY_CLASS_NAME[]        = L"FocusPeekOverlayClass";
static const UINT_PTR TIMER_ID_ACTIVATION      = 9001;
static const UINT_PTR TIMER_ID_HOVER_WATCHDOG  = 9002;

static HWND             g_hOverlay                     = nullptr;
static HTHUMBNAIL       g_hThumbnail                   = nullptr;
static HWND             g_hCurrentTarget               = nullptr;
static HWND             g_hPendingActivationTarget     = nullptr;
static bool             g_bTargetWasOnSecondary        = false;
static bool             g_bOriginalPreviewActive       = false;
static bool             g_bOverlayVisible              = false;
static bool             g_bTargetWasAlreadyForeground  = false;

static HANDLE           g_hOverlayThread               = NULL;
static DWORD            g_dwOverlayThreadId            = 0;
static HANDLE           g_hOverlayReadyEvent           = NULL;
static HWINEVENTHOOK    g_hWinEventHook                = NULL;
static CRITICAL_SECTION g_csOverlay;

static void ActivateAndMoveTargetWindow(HWND hWndTarget);
static void HidePreviewOverlay();

static void CALLBACK WinEventProc(
    HWINEVENTHOOK /*hWinEventHook*/,
    DWORD event,
    HWND hwnd,
    LONG idObject,
    LONG idChild,
    DWORD /*idEventThread*/,
    DWORD /*dwmsEventTime*/
) {
    if (event == EVENT_SYSTEM_FOREGROUND && idObject == OBJID_WINDOW && idChild == CHILDID_SELF) {
        if (!hwnd || hwnd == g_hOverlay) {
            return;
        }

        EnterCriticalSection(&g_csOverlay);
        bool bVisible = g_bOverlayVisible;
        HWND hTarget  = g_hCurrentTarget;
        LeaveCriticalSection(&g_csOverlay);

        if (bVisible) {
            HWND hRoot = GetAncestor(hwnd, GA_ROOT);
            if (hTarget && (hwnd == hTarget || hRoot == hTarget)) {
                // El usuario activo la ventana objetivo
                Wh_Log(L"WinEvent: Target window %p gained foreground", hTarget);
                ActivateAndMoveTargetWindow(hTarget);
            } else {
                // Otra ventana tomo el foco (ej. juego de Godot, ventana sin bordes, otra aplicacion)
                Wh_Log(L"WinEvent: Another window %p gained foreground, hiding overlay", hwnd);
            }
            HidePreviewOverlay();
        }
    }
}

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
            HidePreviewOverlay();
            return 0;
        }

        if (wParam == TIMER_ID_HOVER_WATCHDOG) {
            EnterCriticalSection(&g_csOverlay);
            bool bVis = g_bOverlayVisible;
            HWND hTarget = g_hCurrentTarget;
            LeaveCriticalSection(&g_csOverlay);

            if (!bVis) {
                KillTimer(hWnd, TIMER_ID_HOVER_WATCHDOG);
                return 0;
            }

            // 1. Verificar si la ventana objetivo sigue existiendo
            if (!hTarget || !IsWindow(hTarget)) {
                Wh_Log(L"Watchdog: Target window invalid, hiding overlay");
                HidePreviewOverlay();
                return 0;
            }

            // 2. Comprobar la posicion del raton
            POINT pt;
            if (GetCursorPos(&pt)) {
                HWND hMouseWnd = WindowFromPoint(pt);
                if (hMouseWnd) {
                    DWORD dwPid = 0;
                    GetWindowThreadProcessId(hMouseWnd, &dwPid);
                    // Si el cursor salio de Explorer (no esta en barra de tareas ni miniatura)
                    if (dwPid != GetCurrentProcessId()) {
                        HWND hFg = GetForegroundWindow();
                        HWND hRootFg = GetAncestor(hFg, GA_ROOT);
                        if (hFg != hTarget && hRootFg != hTarget) {
                            Wh_Log(L"Watchdog: Mouse left Explorer area (%p, pid=%lu), hiding overlay", hMouseWnd, dwPid);
                            HidePreviewOverlay();
                            return 0;
                        }
                    }
                }
            }
            return 0;
        }
        return 0;
    }

    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

static DWORD WINAPI OverlayThreadProc(LPVOID) {
    HINSTANCE hInstance = GetModuleHandleW(NULL);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = OverlayWndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = OVERLAY_CLASS_NAME;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;

    RegisterClassExW(&wc);

    g_hOverlay = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        OVERLAY_CLASS_NAME,
        L"FocusPeekOverlay",
        WS_POPUP,
        0, 0, 0, 0,
        NULL, NULL, hInstance, NULL
    );

    g_hWinEventHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND,
        EVENT_SYSTEM_FOREGROUND,
        NULL,
        WinEventProc,
        0, 0,
        WINEVENT_OUTOFCONTEXT
    );

    if (g_hOverlayReadyEvent) {
        SetEvent(g_hOverlayReadyEvent);
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hWinEventHook) {
        UnhookWinEvent(g_hWinEventHook);
        g_hWinEventHook = NULL;
    }

    EnterCriticalSection(&g_csOverlay);
    if (g_hThumbnail) {
        DwmUnregisterThumbnail(g_hThumbnail);
        g_hThumbnail = nullptr;
    }
    if (g_hOverlay && IsWindow(g_hOverlay)) {
        DestroyWindow(g_hOverlay);
        g_hOverlay = nullptr;
    }
    UnregisterClassW(OVERLAY_CLASS_NAME, hInstance);
    LeaveCriticalSection(&g_csOverlay);

    return 0;
}

static void HidePreviewOverlay() {
    EnterCriticalSection(&g_csOverlay);
    g_bOverlayVisible = false;
    g_hPendingActivationTarget = nullptr;
    if (g_hOverlay && IsWindow(g_hOverlay)) {
        KillTimer(g_hOverlay, TIMER_ID_ACTIVATION);
        KillTimer(g_hOverlay, TIMER_ID_HOVER_WATCHDOG);
        SetWindowPos(
            g_hOverlay,
            HWND_BOTTOM,
            0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_HIDEWINDOW
        );
    }
    if (g_hThumbnail) {
        DwmUnregisterThumbnail(g_hThumbnail);
        g_hThumbnail = nullptr;
    }
    LeaveCriticalSection(&g_csOverlay);
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

    TargetGeometry geo = CalculateTargetGeometry(hWndTarget, { 0, 0 });

    BOOL bTransitionsDisabled = TRUE;
    DwmSetWindowAttribute(hWndTarget, DWMWA_TRANSITIONS_FORCEDISABLED, &bTransitionsDisabled, sizeof(bTransitionsDisabled));

    if (bOnSecondary && g_settings.moveToPrimaryMonitor) {
        if (geo.isMaximized) {
            WINDOWPLACEMENT wp = { sizeof(wp) };
            GetWindowPlacement(hWndTarget, &wp);
            wp.showCmd = SW_SHOWMAXIMIZED;
            wp.flags |= WPF_RESTORETOMAXIMIZED;
            wp.rcNormalPosition.left   = geo.x;
            wp.rcNormalPosition.top    = geo.y;
            wp.rcNormalPosition.right  = geo.x + geo.w;
            wp.rcNormalPosition.bottom = geo.y + geo.h;
            SetWindowPlacement(hWndTarget, &wp);
            ShowWindow(hWndTarget, SW_SHOWMAXIMIZED);
        } else {
            if (IsIconic(hWndTarget)) {
                WINDOWPLACEMENT wp = { sizeof(wp) };
                GetWindowPlacement(hWndTarget, &wp);
                wp.showCmd = SW_SHOWNORMAL;
                wp.rcNormalPosition.left   = geo.x;
                wp.rcNormalPosition.top    = geo.y;
                wp.rcNormalPosition.right  = geo.x + geo.w;
                wp.rcNormalPosition.bottom = geo.y + geo.h;
                SetWindowPlacement(hWndTarget, &wp);
                ShowWindow(hWndTarget, SW_RESTORE);
            } else {
                SetWindowPos(
                    hWndTarget, NULL,
                    geo.x, geo.y, geo.w, geo.h,
                    SWP_NOZORDER | SWP_ASYNCWINDOWPOS
                );
            }
        }
    } else {
        if (IsIconic(hWndTarget)) {
            if (geo.isMaximized) {
                ShowWindow(hWndTarget, SW_SHOWMAXIMIZED);
            } else {
                ShowWindow(hWndTarget, SW_RESTORE);
            }
        }
    }

    AllowSetForegroundWindow(ASFW_ANY);
    BringWindowToTop(hWndTarget);
    SetForegroundWindow(hWndTarget);

    bTransitionsDisabled = FALSE;
    DwmSetWindowAttribute(hWndTarget, DWMWA_TRANSITIONS_FORCEDISABLED, &bTransitionsDisabled, sizeof(bTransitionsDisabled));
}

static bool ShowPreviewOverlay(HWND hWndTarget) {
    if (!hWndTarget || !IsWindow(hWndTarget)) {
        return false;
    }

    EnterCriticalSection(&g_csOverlay);

    if (!g_hOverlay || !IsWindow(g_hOverlay)) {
        LeaveCriticalSection(&g_csOverlay);
        return false;
    }

    if (g_hThumbnail) {
        DwmUnregisterThumbnail(g_hThumbnail);
        g_hThumbnail = nullptr;
    }

    HRESULT hr = DwmRegisterThumbnail(g_hOverlay, hWndTarget, &g_hThumbnail);
    if (FAILED(hr)) {
        Wh_Log(L"DwmRegisterThumbnail failed: 0x%08X for hWndTarget=%p", (unsigned int)hr, hWndTarget);
        LeaveCriticalSection(&g_csOverlay);
        return false;
    }

    SIZE srcSize = { 0 };
    DwmQueryThumbnailSourceSize(g_hThumbnail, &srcSize);

    HMONITOR hMonPrimary = MonitorFromWindow(NULL, MONITOR_DEFAULTTOPRIMARY);
    HMONITOR hMonTarget = MonitorFromWindow(hWndTarget, MONITOR_DEFAULTTONEAREST);
    g_bTargetWasOnSecondary = (hMonTarget != hMonPrimary);

    TargetGeometry geo = CalculateTargetGeometry(hWndTarget, srcSize);

    DWORD cornerPref = geo.isMaximized ? 1 : 2;
    DwmSetWindowAttribute(g_hOverlay, 33, &cornerPref, sizeof(cornerPref));

    DWM_THUMBNAIL_PROPERTIES props = { 0 };
    props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_OPACITY | DWM_TNP_VISIBLE;
    props.rcDestination.left   = 0;
    props.rcDestination.top    = 0;
    props.rcDestination.right  = geo.w;
    props.rcDestination.bottom = geo.h;
    props.opacity              = 255;
    props.fVisible             = TRUE;
    props.fSourceClientAreaOnly = FALSE;

    DwmUpdateThumbnailProperties(g_hThumbnail, &props);

    SetWindowPos(
        g_hOverlay,
        HWND_TOPMOST,
        geo.x, geo.y, geo.w, geo.h,
        SWP_NOACTIVATE | SWP_SHOWWINDOW
    );

    g_bOverlayVisible = true;
    SetTimer(g_hOverlay, TIMER_ID_HOVER_WATCHDOG, 100, NULL);

    LeaveCriticalSection(&g_csOverlay);
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
            g_bOriginalPreviewActive = true;
            return pOriginalDwmActivateLivePreview(fActivate, hWndTarget, hWndTrigger, dwFlags, prcExclude);
        }

        if (IsIconic(hWndTarget) && !g_settings.previewMinimized) {
            return S_OK;
        }

        HWND hCurFg = GetForegroundWindow();
        HWND hCurRoot = GetAncestor(hCurFg, GA_ROOT);
        g_bTargetWasAlreadyForeground = (hWndTarget && (hCurFg == hWndTarget || hCurRoot == hWndTarget));

        g_hCurrentTarget = hWndTarget;
        Wh_Log(L"DwmActivateLivePreview: fActivate=1, hWndTarget=%p (wasAlreadyFg=%d)", hWndTarget, g_bTargetWasAlreadyForeground ? 1 : 0);

        GetAsyncKeyState(VK_LBUTTON);

        if (!ShowPreviewOverlay(hWndTarget)) {
            g_bOriginalPreviewActive = true;
            return pOriginalDwmActivateLivePreview(fActivate, hWndTarget, hWndTrigger, dwFlags, prcExclude);
        }

        g_bOriginalPreviewActive = false;
        return S_OK;
    } else {
        Wh_Log(L"DwmActivateLivePreview: fActivate=0");

        HWND hTarget = g_hCurrentTarget;
        g_hCurrentTarget = nullptr;

        if (hTarget && IsWindow(hTarget)) {
            SHORT mouseState = GetAsyncKeyState(VK_LBUTTON);
            bool bClicked = (mouseState & 0x8001) != 0;

            if (bClicked) {
                Wh_Log(L"DwmActivateLivePreview: Immediate click detected for target %p", hTarget);
                ActivateAndMoveTargetWindow(hTarget);
                HidePreviewOverlay();
            } else if (g_bTargetWasAlreadyForeground) {
                // Si la ventana ya era de primer plano y no se hizo clic, solo fue un hover pasajero.
                // No forzar reactivacion para no alterar el z-order ni tapar ventanas hijas (ej. juego de Godot).
                Wh_Log(L"Hover ended for already-foreground window %p without click, hiding overlay", hTarget);
                HidePreviewOverlay();
            } else {
                g_hPendingActivationTarget = hTarget;
                if (g_hOverlay && IsWindow(g_hOverlay)) {
                    SetTimer(g_hOverlay, TIMER_ID_ACTIVATION, 50, NULL);
                } else {
                    HidePreviewOverlay();
                }
            }
        } else {
            HidePreviewOverlay();
        }

        if (g_bOriginalPreviewActive || !hWndTarget) {
            g_bOriginalPreviewActive = false;
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
    LoadSettings();

    if (g_bIsExplorerProcess) {
        Wh_Log(L"Initializing Focus Peek v2.4.3 in explorer.exe...");

        InitializeCriticalSection(&g_csOverlay);
        g_hOverlayReadyEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        g_hOverlayThread = CreateThread(NULL, 0, OverlayThreadProc, NULL, 0, &g_dwOverlayThreadId);
        if (g_hOverlayThread && g_hOverlayReadyEvent) {
            WaitForSingleObject(g_hOverlayReadyEvent, 2000);
        }

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

        Wh_Log(L"Focus Peek v2.4.3 (Explorer) initialized successfully.");
        return TRUE;
    } else {
        WCHAR processPath[MAX_PATH] = { 0 };
        GetModuleFileNameW(NULL, processPath, MAX_PATH);
        const WCHAR* pFileName = wcsrchr(processPath, L'\\');
        pFileName = pFileName ? (pFileName + 1) : processPath;

        Wh_Log(L"Initializing Focus Peek v2.4.3 in client process (%s)...", pFileName);

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

        Wh_Log(L"Focus Peek v2.4.3 (Client) initialized successfully.");
        return TRUE;
    }
}

void Wh_ModUninit() {
    if (g_bIsExplorerProcess) {
        Wh_Log(L"Unloading Focus Peek from explorer.exe...");

        if (g_dwOverlayThreadId) {
            PostThreadMessageW(g_dwOverlayThreadId, WM_QUIT, 0, 0);
            if (g_hOverlayThread) {
                WaitForSingleObject(g_hOverlayThread, 2000);
                CloseHandle(g_hOverlayThread);
                g_hOverlayThread = NULL;
            }
            g_dwOverlayThreadId = 0;
        }

        if (g_hOverlayReadyEvent) {
            CloseHandle(g_hOverlayReadyEvent);
            g_hOverlayReadyEvent = NULL;
        }

        DeleteCriticalSection(&g_csOverlay);
    } else {
        Wh_Log(L"Unloading Focus Peek from client process...");
    }
}
