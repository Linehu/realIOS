# 把 realIOS 的运行依赖放进 SYSTEM/trans/（打包输入，最终就是 ext4 上的 /trans）：
#   trans          trans-cli Linux 静态版（deaicup.com）
#   bin/busybox    静态 busybox（给 trans-cli 的 run_shell 当 sh）
#   certs/ca-certificates.crt
# 用法：powershell -ExecutionPolicy Bypass -File CODE\deps\fetch_deps.ps1 [-Force]
param([switch]$Force)
$ErrorActionPreference = 'Stop'

$Root = (Resolve-Path "$PSScriptRoot\..\..").Path
$Trans = Join-Path $Root 'SYSTEM\trans'
$Tmp = Join-Path $Root '.build_tmp'
New-Item -ItemType Directory -Force "$Trans\bin", "$Trans\certs", $Tmp | Out-Null

function To-Wsl([string]$p) {
    $full = [IO.Path]::GetFullPath($p)
    '/mnt/' + $full.Substring(0, 1).ToLower() + $full.Substring(2).Replace('\', '/')
}

# ---- trans-cli（WSL 里走不了 Windows 的代理，所以用 Windows curl 下载）----
$transOut = "$Trans\trans"
if ($Force -or -not (Test-Path $transOut)) {
    $dl = "$Tmp\trans-linux-amd64"
    if ($Force -or -not (Test-Path $dl)) {
        for ($i = 1; $i -le 30; $i++) {
            curl.exe -fL -C - --retry 5 -o $dl https://deaicup.com/trans-1/dist/trans-linux-amd64
            if ($LASTEXITCODE -eq 0) { break }
            Write-Host "[deps] trans 下载重试 $i（exit $LASTEXITCODE）"
            Start-Sleep 2
        }
        if ($LASTEXITCODE -ne 0) { throw 'trans-linux-amd64 下载失败' }
    }
    $magic = [IO.File]::ReadAllBytes($dl)[0..3]
    if ($magic[0] -ne 0x7f -or $magic[1] -ne 0x45) { throw "$dl 不是 ELF" }
    Copy-Item $dl $transOut -Force
    Write-Host "[deps] trans -> SYSTEM/trans/trans"
}

# ---- busybox：复用 Deshab 的静态 1.35（带 HTTPS wget），没有就让用户自己放 ----
$bbOut = "$Trans\bin\busybox"
if ($Force -or -not (Test-Path $bbOut)) {
    $cands = @(
        (Join-Path $Root '..\Deshab\CODE\linux\.cache\busybox'),
        (Join-Path $Root '..\Deshab\SYSTEMCOPY\OpenXJ380\third_party\busybox-prebuilt\busybox_amd64')
    )
    $src = $cands | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $src) { throw '找不到静态 busybox：请把 x86_64 静态 busybox 放到 SYSTEM/trans/bin/busybox' }
    Copy-Item $src $bbOut -Force
    Write-Host "[deps] busybox <- $src"
}

# ---- CA 证书：取 WSL 发行版自带的 ----
$certOut = "$Trans\certs\ca-certificates.crt"
if ($Force -or -not (Test-Path $certOut)) {
    $ErrorActionPreference = 'Continue'   # wsl 会往 stderr 打代理警告
    wsl -e cp -L /etc/ssl/certs/ca-certificates.crt (To-Wsl $certOut) 2>$null
    $ErrorActionPreference = 'Stop'
    if (-not (Test-Path $certOut)) { throw 'CA 证书复制失败（WSL 里装 ca-certificates）' }
    Write-Host "[deps] ca-certificates.crt"
}

Get-ChildItem -Recurse -File $Trans | ForEach-Object {
    '{0,10}  {1}' -f $_.Length, $_.FullName.Substring($Root.Length + 1)
}
