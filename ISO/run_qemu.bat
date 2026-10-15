@echo off
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"

REM Usage: run_qemu.bat [dev|release]
set "VARIANT=%~1"
if not defined VARIANT set "VARIANT=%QEMU_VARIANT%"
if not defined VARIANT set "VARIANT=dev"
if "%VARIANT%"=="1" set "VARIANT=dev"
if "%VARIANT%"=="2" set "VARIANT=release"
if /I not "%VARIANT%"=="dev" if /I not "%VARIANT%"=="release" (
    echo [qemu] Unknown variant: %VARIANT%  ^(use: run_qemu.bat dev  or  run_qemu.bat release^)
    pause
    exit /b 1
)

set "IMG=%SCRIPT_DIR%realios-%VARIANT%.img"
if not exist "%IMG%" (
    echo [qemu] Image not found: %IMG%
    echo [qemu] Run ..\build.bat first
    pause
    exit /b 1
)

set "QEMU="
for /f "delims=" %%P in ('where qemu-system-x86_64.exe 2^>nul') do (
    if not defined QEMU set "QEMU=%%P"
)
if not defined QEMU if exist "C:\Program Files\qemu\qemu-system-x86_64.exe" set "QEMU=C:\Program Files\qemu\qemu-system-x86_64.exe"
if not defined QEMU (
    echo [qemu] qemu-system-x86_64 was not found.
    pause
    exit /b 1
)

set "OVMF="
if exist "C:\Program Files\qemu\share\edk2-x86_64-code.fd" set "OVMF=C:\Program Files\qemu\share\edk2-x86_64-code.fd"
if not defined OVMF if exist "C:\Program Files\qemu\share\OVMF_CODE.fd" set "OVMF=C:\Program Files\qemu\share\OVMF_CODE.fd"
if not defined OVMF (
    echo [qemu] UEFI firmware not found.
    pause
    exit /b 1
)

if not defined QEMU_MEM set "QEMU_MEM=2G"
set "LOG=%SCRIPT_DIR%qemu-last.log"

echo [qemu] Using: %QEMU%
echo [qemu] Image: %IMG%
echo [qemu] RAM:   %QEMU_MEM%
echo [qemu] 请等窗口出现（硬件加速失败会自动改用慢速模式）。
echo [qemu] 不要关这个黑窗口，关掉虚拟机也会退出。
echo [qemu] release: run_qemu.bat release

set "RC=1"

echo [qemu] try WHPX + gtk
"%QEMU%" -accel whpx -display gtk -machine q35 -m %QEMU_MEM% -smp 2 -cpu qemu64 -vga std -serial stdio -netdev user,id=net0 -device e1000,netdev=net0 -drive if=pflash,format=raw,readonly=on,file="%OVMF%" -drive if=none,id=osdisk,format=raw,file="%IMG%" -device ide-hd,drive=osdisk,bus=ide.0,bootindex=1 2>"%LOG%"
set "RC=!ERRORLEVEL!"
if "!RC!"=="0" goto :eof

echo [qemu] try TCG + gtk
"%QEMU%" -accel tcg -display gtk -machine q35 -m %QEMU_MEM% -smp 2 -cpu qemu64 -vga std -serial stdio -netdev user,id=net0 -device e1000,netdev=net0 -drive if=pflash,format=raw,readonly=on,file="%OVMF%" -drive if=none,id=osdisk,format=raw,file="%IMG%" -device ide-hd,drive=osdisk,bus=ide.0,bootindex=1 2>"%LOG%"
set "RC=!ERRORLEVEL!"
if "!RC!"=="0" goto :eof

echo [qemu] try TCG + sdl
"%QEMU%" -accel tcg -display sdl -machine q35 -m %QEMU_MEM% -smp 2 -cpu qemu64 -vga std -serial stdio -netdev user,id=net0 -device e1000,netdev=net0 -drive if=pflash,format=raw,readonly=on,file="%OVMF%" -drive if=none,id=osdisk,format=raw,file="%IMG%" -device ide-hd,drive=osdisk,bus=ide.0,bootindex=1 2>"%LOG%"
set "RC=!ERRORLEVEL!"
if "!RC!"=="0" goto :eof

echo [qemu] QEMU exited with error !RC!
echo [qemu] ----- %LOG% -----
type "%LOG%"
echo [qemu] --------------------
pause
exit /b !RC!
