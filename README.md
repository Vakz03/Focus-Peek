# Focus-Peek

Windhawk mod that brings the target window into view when hovering over its taskbar thumbnail, completely suppressing Aero Peek background transparency and projecting the preview onto the primary monitor in multi-display setups using a zero-latency multi-process DWM Thumbnail Overlay architecture.

---

## Overview

Windows native taskbar hover previews are tightly coupled with Aero Peek: hovering over a thumbnail activates transparency on all background windows. Disabling Aero Peek in system settings also disables full-size window previews entirely. Furthermore, in multi-monitor setups, the native preview appears on whichever screen the target window currently resides, making tracking from the primary workstation display cumbersome.

Focus-Peek addresses these limitations directly at the compositor level:
1. Intercepts the native live preview request in `explorer.exe` (ordinal 113 of `dwmapi.dll`) and suppresses the call to `dwm.exe`, ensuring no background windows fade into transparency.
2. Leverages the Desktop Window Manager compositor API (`DwmRegisterThumbnail`) rendered onto a dedicated hardware-accelerated overlay window (`WS_POPUP`, `WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE`). This mirrors the DirectX/GDI surface of the target window instantaneously (0 ms) with GPU acceleration.
3. Universal projection and UIPI-aware relocation architecture:
   - **Universal preview across all windows (0 ms, without exception)**: `DwmRegisterThumbnail` calls into the Desktop Window Manager service (`dwm.exe`, running under system privilege). Explorer projects the visual surface of any window (Spotify, Discord, VS Code, Task Manager, elevated terminals, etc.) onto the primary monitor with zero integrity-level (UIPI) restrictions.
   - **Elevated processes upon click** (`taskmgr.exe`, `windhawk.exe`): If an elevated window resides on a secondary monitor and the user clicks to activate it, Explorer broadcasts a UIPI-allowed registered message (`ChangeWindowMessageFilter`). The elevated process intercepts this message directly within its native message pump (`GetMessage`/`PeekMessage`) and executes its own relocation to the primary monitor with high integrity level, avoiding `ERROR_ACCESS_DENIED`.
4. In multi-monitor environments, projects the live preview directly onto the primary monitor, scaling and fitting the window within the primary monitor's work area.
5. If the user **clicks** on the taskbar thumbnail or overlay to activate it, the window is permanently moved to the primary monitor and brought to the foreground.
6. If the user **moves the cursor away without clicking**, the overlay is destroyed immediately. Because the original window was never physically moved or altered in its Z-order (`HWND_TOPMOST`), the desktop layout remains entirely intact.

---

## Installation and Usage

### Via Windhawk Client

1. Open Windhawk.
2. Navigate to the development section (menu icon -> **Development** or **Create new mod**).
3. Set the mod ID to `focus-peek`.
4. Replace the template source code with the contents of `focus-peek.wh.cpp`.
5. Click **Compile Mod**, then click **Accept and Run**.

### Local Build via Terminal

The repository includes a PowerShell script to validate syntax or build the mod binary using the local Windhawk Clang compiler:

```powershell
# Syntax and SDK compatibility check (x86_64 and i686)
powershell -ExecutionPolicy Bypass -File .\build.ps1

# Build 64-bit dynamic link library (DLL)
powershell -ExecutionPolicy Bypass -File .\build.ps1 -BuildDll
```

---

## Configuration

The following settings can be configured directly from the Windhawk user interface after compiling the mod:

| Parameter | Default Value | Description |
| :--- | :---: | :--- |
| `moveToPrimaryMonitor` | `true` | Projects the preview onto the primary monitor and permanently moves the window there upon click. |
| `previewMinimized` | `true` | Projects the surface of minimized windows in the overlay without restoring them first. |
| `blockShowDesktopPeek` | `false` | Suppresses transparency when hovering over the Show Desktop taskbar button. |

---

## Technical Details

- **Zero Transparency**: Hooks ordinal 113 of `dwmapi.dll` (`DwmActivateLivePreview`) and returns `S_OK`, preventing the DWM compositor from ever triggering the background translucency shader.
- **No Z-Order Corruption**: Does not apply `HWND_TOPMOST` directly to target application windows, preventing windows from getting trapped behind other layers or breaking system focus hierarchies.
- **UIPI Architecture**: Inter-process communication between Explorer (Medium IL) and elevated system processes (High IL) uses registered messages permitted via process-level filters (`MSGFLT_ADD`).
- **Zero Input Hooks**: Does not use global low-level mouse hooks (`WH_MOUSE_LL`), ensuring zero CPU overhead, no latency, and complete explorer stability.
