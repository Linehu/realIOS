param(
    # dev：串口日志 + 屏幕镜像到串口（QEMU 调试）；release：安静启动（真机）；both = 两个都打
    [ValidateSet('dev', 'release', 'both')][string]$Variant = 'both',
    # 跳过内核编译（只改了 realboot / 配置 / SYSTEM 时用）
    [switch]$SkipKernel
)

$ErrorActionPreference = 'Stop'

$Root       = Split-Path -Parent $MyInvocation.MyCommand.Path
$SystemDir  = Join-Path $Root 'SYSTEM'
$IsoDir     = Join-Path $Root 'ISO'
$ConfigsDir = Join-Path $Root 'build\configs'
$BuildTmp   = Join-Path $Root '.build_tmp'
$Kernel     = Join-Path $BuildTmp 'vmlinuz.efi'
$BootEfi    = Join-Path $SystemDir 'EFI\BOOT\BOOTX64.EFI'

function ConvertTo-WslUnixPath([string]$WinPath) {
    $full = [System.IO.Path]::GetFullPath($WinPath)
    if ($full -match '^(?<d>[A-Za-z]):\\(?<rest>.*)$') {
        return '/mnt/' + $Matches['d'].ToLowerInvariant() + '/' + ($Matches['rest'] -replace '\\', '/')
    }
    throw "Cannot map Windows path to WSL: $WinPath"
}

# wsl 每次都会往 stderr 打一行 localhost 代理警告；只看退出码
function Invoke-Wsl([string]$What, [string[]]$WslArgs) {
    Write-Host "[build] $What"
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & wsl @WslArgs 2>&1 | ForEach-Object {
        $line = "$_"
        if ($line -notmatch 'localhost|^\s*$' -and $line -notmatch '^w s l') { Write-Host $line }
    }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $prev
    if ($code -ne 0) { throw "$What 失败（exit $code）" }
}

if (-not (Get-Command wsl -ErrorAction SilentlyContinue)) {
    throw '需要 WSL（Ubuntu）：内核、realinit、realboot 都在 WSL 里编译，打包需要 mkfs.ext4。'
}
New-Item -ItemType Directory -Force -Path $BuildTmp, $IsoDir | Out-Null
$wRoot = ConvertTo-WslUnixPath $Root

# ---- 1. 运行依赖：trans-cli / busybox / CA 证书 → SYSTEM/trans ----
& powershell -ExecutionPolicy Bypass -File (Join-Path $Root 'CODE\deps\fetch_deps.ps1')
if ($LASTEXITCODE -ne 0) { throw 'fetch_deps.ps1 失败' }

$wpaTar = Join-Path $BuildTmp 'wpa_supplicant-2.10.tar.gz'
if (-not (Test-Path $wpaTar)) {
    Write-Host '[build] downloading wpa_supplicant-2.10...'
    curl.exe -fL --retry 5 -o $wpaTar https://w1.fi/releases/wpa_supplicant-2.10.tar.gz
    if ($LASTEXITCODE -ne 0) { throw 'wpa_supplicant 源码下载失败' }
}
Invoke-Wsl 'Building wpa_supplicant...' @('-e', 'bash', "$wRoot/CODE/deps/build_wpa.sh", $wRoot)

# ---- 2. realinit（PID 1）----
Invoke-Wsl 'Building realinit...' @('-e', 'make', '-s', '-C', "$wRoot/CODE/init", '-f', 'MAKEFILE')

# ---- 3. 字体（GNU Unifont → RFNT）----
$FontOut = Join-Path $BuildTmp 'font.rfnt'
$FontSrc = Join-Path $Root 'CODE\font\unifont-16.0.04.hex.gz'
if (-not (Test-Path $FontOut) -or (Get-Item $FontSrc).LastWriteTime -gt (Get-Item $FontOut).LastWriteTime) {
    Invoke-Wsl 'Generating font.rfnt...' @('-e', 'python3', "$wRoot/CODE/font/mkfont.py",
        "$wRoot/CODE/font/unifont-16.0.04.hex.gz", "$wRoot/.build_tmp/font.rfnt")
}

# ---- 4. 内核（initramfs 编在里面）----
if (-not $SkipKernel -or -not (Test-Path $Kernel)) {
    Invoke-Wsl 'Building Linux kernel (this takes a while the first time)...' @(
        '-e', 'bash', "$wRoot/CODE/kernel/build_kernel.sh", $wRoot, "$wRoot/.build_tmp/vmlinuz.efi")
}

# ---- 5. realboot（内核嵌进 BOOTX64.EFI）----
Invoke-Wsl 'Building realboot (BOOTX64.EFI)...' @('-e', 'make', '-s', '-C', "$wRoot/CODE/boot", '-f', 'MAKEFILE',
    "KERNEL=$wRoot/.build_tmp/vmlinuz.efi", "OUT=$wRoot/SYSTEM/EFI/BOOT/BOOTX64.EFI")
if (-not (Test-Path $BootEfi)) { throw "realboot 没有产出 $BootEfi" }
Write-Host ("[build] BOOTX64.EFI: {0:N1} MiB" -f ((Get-Item $BootEfi).Length / 1MB))

# ---- 6. 打包：注入 build/configs/<variant>/boot.cfg → GPT(FAT32 ESP + ext4) ----
function Build-ImageVariant([string]$Name, [string]$ImagePath) {
    $cfg = Join-Path $ConfigsDir "$Name\boot.cfg"
    if (-not (Test-Path $cfg)) { throw "缺少 $cfg" }
    $dst = Join-Path $SystemDir 'realIOS'
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    Copy-Item -LiteralPath $cfg -Destination (Join-Path $dst 'boot.cfg') -Force
    $keyFile = Join-Path $dst 'apikey.txt'
    $keySrc = Join-Path $Root 'apikey.txt'
    if ($Name -eq 'dev' -and (Test-Path $keySrc)) {
        Copy-Item -LiteralPath $keySrc -Destination $keyFile -Force
        Write-Host "[build] Injected config variant=$Name (realIOS/boot.cfg + apikey.txt)"
    } else {
        if (Test-Path $keyFile) { Remove-Item -LiteralPath $keyFile -Force }
        Write-Host "[build] Injected config variant=$Name (realIOS/boot.cfg)"
    }
    $script = ConvertTo-WslUnixPath (Join-Path $Root 'CODE\pack\pack_system_image.sh')
    Invoke-Wsl "Packaging $Name image..." @('-u', 'root', '-e', 'bash', $script,
        (ConvertTo-WslUnixPath $SystemDir), (ConvertTo-WslUnixPath $ImagePath))
}

if ($Variant -in @('dev', 'both')) {
    Build-ImageVariant 'dev' (Join-Path $IsoDir 'realios-dev.img')
}
if ($Variant -in @('release', 'both')) {
    Build-ImageVariant 'release' (Join-Path $IsoDir 'realios-release.img')
}
# SYSTEM 保持 dev 配置（日常开发对象）
if ($Variant -eq 'both') {
    $dst = Join-Path $SystemDir 'realIOS'
    Copy-Item -LiteralPath (Join-Path $ConfigsDir 'dev\boot.cfg') -Destination (Join-Path $dst 'boot.cfg') -Force
    $keySrc = Join-Path $Root 'apikey.txt'
    $keyFile = Join-Path $dst 'apikey.txt'
    if (Test-Path $keySrc) {
        Copy-Item -LiteralPath $keySrc -Destination $keyFile -Force
    } elseif (Test-Path $keyFile) {
        Remove-Item -LiteralPath $keyFile -Force
    }
}

Get-ChildItem $IsoDir -Filter 'realios-*.img' | ForEach-Object {
    Write-Host ("[build] {0}  ({1:N0} MiB virtual)" -f $_.FullName, ($_.Length / 1MB))
}
Write-Host '[build] done. 运行 ISO\run_qemu.bat 启动；写 U 盘用 Rufus / dd 写入 realios-release.img。'
