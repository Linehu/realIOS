@echo off
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"

REM ===== Image variant: dev / release =====
REM Priority: argument > QEMU_VARIANT env > interactive menu > default dev
REM Usage: run_qemu.bat [dev|release]
set "VARIANT="
if not "%~1"=="" set "VARIANT=%~1"
if not defined VARIANT if defined QEMU_VARIANT set "VARIANT=%QEMU_VARIANT%"
if not defined VARIANT (
    echo [qemu] Select image variant to boot:
    echo   1. dev      ^(realios-dev.img, kernel log + screen mirrored to this console^)
    echo   2. release  ^(realios-release.img, quiet boot^)
    set /p CHOICE="Choice [1-2, Enter=dev]: "
    if "!CHOICE!"=="2" set "VARIANT=release"
    if not defined VARIANT set "VARIANT=dev"
)
if "%VARIANT%"=="1" set "VARIANT=dev"
if "%VARIANT%"=="2" set "VARIANT=release"
if not "%VARIANT%"=="dev" if not "%VARIANT%"=="release" (
    echo [qemu] Unknown variant: %VARIANT% ^(expected dev / release^)
    exit /b 1
)

set "IMG=%SCRIPT_DIR%realios-%VARIANT%.img"
if not exist "%IMG%" (
    echo [qemu] Image not found: %IMG%
    echo [qemu] Run ..\build.bat first ^(or: build.bat -Variant %VARIANT%^)
    exit /b 1
)

REM QEMU_SERIAL_LOG=path : write serial to a file instead of stdio
set "SERIAL_ARGS=-serial stdio"
if defined QEMU_SERIAL_LOG (
    if exist "%QEMU_SERIAL_LOG%" del "%QEMU_SERIAL_LOG%"
    set "SERIAL_ARGS=-serial file:%QEMU_SERIAL_LOG%"
)

set "QEMU="
for %%Q in (qemu-system-x86_64.exe) do (
    for /f "delims=" %%P in ('where %%Q 2^>nul') do (
        if not defined QEMU set "QEMU=%%P"
    )
)
if not defined QEMU if exist "C:\Program Files\qemu\qemu-system-x86_64.exe" set "QEMU=C:\Program Files\qemu\qemu-system-x86_64.exe"
if not defined QEMU if exist "C:\msys64\ucrt64\bin\qemu-system-x86_64.exe" set "QEMU=C:\msys64\ucrt64\bin\qemu-system-x86_64.exe"
if not defined QEMU (
    echo [qemu] qemu-system-x86_64 was not found. Install QEMU or add it to PATH.
    exit /b 1
)

set "OVMF="
if exist "C:\Program Files\qemu\share\edk2-x86_64-code.fd" set "OVMF=C:\Program Files\qemu\share\edk2-x86_64-code.fd"
if not defined OVMF if exist "C:\Program Files\qemu\share\OVMF_CODE.fd" set "OVMF=C:\Program Files\qemu\share\OVMF_CODE.fd"
if not defined OVMF (
    echo [qemu] UEFI firmware ^(edk2-x86_64-code.fd / OVMF_CODE.fd^) was not found.
    exit /b 1
)

REM QEMU_ACCEL=whpx / tcg (default whpx), QEMU_MEM default 4G
if not defined QEMU_ACCEL set "QEMU_ACCEL=whpx"
if not defined QEMU_MEM set "QEMU_MEM=4G"

echo [qemu] Using: %QEMU%
echo [qemu] UEFI:  %OVMF%
echo [qemu] Image: %IMG%
echo [qemu] Accel: %QEMU_ACCEL%   RAM: %QEMU_MEM%
echo [qemu] F12 inside realIOS returns to the realinit menu.

REM OS disk on the q35 built-in AHCI (same driver path as real SATA); e1000 + user-mode net (QEMU serves DHCP)
"%QEMU%" -accel %QEMU_ACCEL% -machine q35 -m %QEMU_MEM% -smp 2 -cpu qemu64 %SERIAL_ARGS% ^
    -drive if=pflash,format=raw,readonly=on,file="%OVMF%" ^
    -drive if=none,id=osdisk,format=raw,file="%IMG%" -device ide-hd,drive=osdisk,bus=ide.0,bootindex=1 ^
    -netdev user,id=net0 -device e1000,netdev=net0 ^
    -vga std

endlocal
