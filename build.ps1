param (
    [switch]$BuildDll,
    [switch]$CheckOnly
)

$ErrorActionPreference = "Stop"

$WindhawkPath = "C:\Program Files\Windhawk"
$Clang = "$WindhawkPath\Compiler\bin\clang++.exe"
$Include = "$WindhawkPath\Compiler\include"
$LibDir = "$WindhawkPath\Engine\1.7.3\64"
$Source = "$PSScriptRoot\focus-peek.wh.cpp"
$OutputDll = "$PSScriptRoot\focus-peek.dll"

if (-not (Test-Path $Clang)) {
    Write-Error "Windhawk compiler not found at $Clang"
    exit 1
}

if ($BuildDll) {
    Write-Host "Compiling mod to dynamic link library (DLL)..."
    & $Clang -shared -o $OutputDll `
        -std=c++23 `
        -target x86_64-w64-mingw32 `
        -DUNICODE -D_UNICODE `
        -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -DNTDDI_VERSION=0x0A000008 `
        -D__USE_MINGW_ANSI_STDIO=0 `
        -DWH_MOD `
        -I $Include `
        $Source `
        -L $LibDir -lwindhawk -ldwmapi -lgdi32 -lcomctl32

    if ($LASTEXITCODE -eq 0) {
        Write-Host "Compilation finished successfully: $OutputDll"
    } else {
        Write-Error "Failed to compile DLL."
        exit $LASTEXITCODE
    }
} else {
    Write-Host "Verifying syntax and compatibility for x86_64..."
    & $Clang -fsyntax-only `
        -x c++ -std=c++23 `
        -target x86_64-w64-mingw32 `
        -DUNICODE -D_UNICODE `
        -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -DNTDDI_VERSION=0x0A000008 `
        -D__USE_MINGW_ANSI_STDIO=0 `
        -DWH_MOD -DWH_EDITING `
        -I $Include `
        $Source

    if ($LASTEXITCODE -ne 0) {
        Write-Error "x86_64 verification failed."
        exit $LASTEXITCODE
    }

    Write-Host "Verifying syntax and compatibility for i686 (32-bit)..."
    & $Clang -fsyntax-only `
        -x c++ -std=c++23 `
        -target i686-w64-mingw32 `
        -DUNICODE -D_UNICODE `
        -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -DNTDDI_VERSION=0x0A000008 `
        -D__USE_MINGW_ANSI_STDIO=0 `
        -DWH_MOD -DWH_EDITING `
        -I $Include `
        $Source

    if ($LASTEXITCODE -ne 0) {
        Write-Error "i686 verification failed."
        exit $LASTEXITCODE
    }

    Write-Host "Syntax verified successfully for both architectures (x86 and x64)."
}
