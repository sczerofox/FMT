#Requires -Version 5.1
<#
.SYNOPSIS
    在 Windows 上用 Ninja + MSVC 配置并构建 FMT。

.DESCRIPTION
    本机不一定把 CMake / Ninja 放在 PATH 上，cl 也需要 vcvars 提供的
    INCLUDE / LIB。脚本按顺序处理：

      1. 用 vswhere 找到 Visual Studio Build Tools，导入 vcvars64 环境变量；
      2. 解析 cmake / ninja：先 PATH，再 Build Tools 自带的副本；
      3. 配置 -> 构建 ->（可选）跑测试。

.PARAMETER Test
    构建完成后运行 ctest。

.PARAMETER Fresh
    先删除构建目录再配置（切换构建类型时必须用，CMake 缓存不会自动切换）。

.EXAMPLE
    pwsh -File tools/build.ps1 -Test
#>
param(
    [switch]$Test,
    [switch]$Fresh
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root 'cmake-build-debug'

# ---- 1. MSVC 环境 ---------------------------------------------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw "找不到 vswhere.exe：$vswhere。请安装 Visual Studio Build Tools（含 C++ 工具集）。"
}

$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsPath) {
    throw 'vswhere 没有找到带 C++ 工具集的 Visual Studio 安装。'
}
$vsPath = $vsPath.Trim()

$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) {
    throw "找不到 vcvars64.bat：$vcvars"
}

Write-Host "MSVC 环境：$vsPath"
cmd.exe /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') {
        Set-Item -Path ('env:' + $matches[1]) -Value $matches[2]
    }
}

# ---- 2. cmake / ninja -----------------------------------------------------
$cmake = (Get-Command cmake.exe -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $cmake = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
$ninja = (Get-Command ninja.exe -ErrorAction SilentlyContinue).Source
if (-not $ninja) {
    $ninja = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
}
if (-not (Test-Path $cmake)) { throw "找不到 cmake.exe：$cmake" }
if (-not (Test-Path $ninja)) { throw "找不到 ninja.exe：$ninja" }

$env:PATH = (Split-Path $cmake) + ';' + (Split-Path $ninja) + ';' + $env:PATH
Write-Host "CMake ：$cmake"
Write-Host "Ninja ：$ninja"

# ---- 3. 配置 / 构建 / 测试 ------------------------------------------------
if ($Fresh -and (Test-Path $buildDir)) {
    Write-Host "删除构建目录：$buildDir"
    Remove-Item $buildDir -Recurse -Force
}

& $cmake -S $root -B $buildDir -G Ninja `
    -DCMAKE_BUILD_TYPE=Debug `
    -DBUILD_TESTING=ON `
    "-DCMAKE_MAKE_PROGRAM=$ninja"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $cmake --build $buildDir --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Test) {
    $ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
    & $ctest --test-dir $buildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host ''
Write-Host "产物：$(Join-Path $buildDir 'bin\fmt.exe')"
