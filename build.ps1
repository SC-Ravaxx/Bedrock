# Builds Bedrock on Windows in one go: the vcpkg packages it needs, the
# CMake configure and build, then the self-test.
#
#   .\build.ps1                 a normal build in build\
#   .\build.ps1 -DevTools       with the developer commands (and Tesseract)
#
# vcpkg is taken from $env:VCPKG_ROOT, else %USERPROFILE%\vcpkg, and cloned
# there if it is missing.
param([switch]$DevTools)

$ErrorActionPreference = "Stop"
$projectPath = $PSScriptRoot
$vcpkgPath = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { Join-Path $env:USERPROFILE "vcpkg" }

# cmake: PATH first, then the copy that comes with Visual Studio
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * -property installationPath
        $candidate = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        if (Test-Path $candidate) { $cmake = $candidate }
    }
}
if (-not $cmake) {
    Write-Host "cmake not found (neither on PATH nor in Visual Studio)." -ForegroundColor Red
    exit 1
}

if (-not (Test-Path (Join-Path $vcpkgPath "vcpkg.exe"))) {
    Write-Host "vcpkg not found - cloning it to $vcpkgPath" -ForegroundColor Yellow
    git clone https://github.com/microsoft/vcpkg $vcpkgPath
    & (Join-Path $vcpkgPath "bootstrap-vcpkg.bat")
}

$packages = @("opencv4[dnn]:x64-windows", "zstd:x64-windows")
if ($DevTools) { $packages += "tesseract:x64-windows" }
Write-Host "vcpkg: $($packages -join ', ')" -ForegroundColor Yellow
& (Join-Path $vcpkgPath "vcpkg.exe") install @packages

$buildPath = Join-Path $projectPath "build"
$toolchain = Join-Path $vcpkgPath "scripts\buildsystems\vcpkg.cmake"
$options = @("-S", $projectPath, "-B", $buildPath, "-A", "x64", "-DCMAKE_TOOLCHAIN_FILE=$toolchain")
if ($DevTools) { $options += "-DBEDROCK_DEV_TOOLS=ON" }
& $cmake @options
& $cmake --build $buildPath --config Release

$exe = Join-Path $buildPath "Release\Bedrock.exe"
Write-Host "Self-test..." -ForegroundColor Yellow
& $exe --selftest | Out-Host
if ($LASTEXITCODE -ne 0) {
    Write-Host "The self-test failed (exit code $LASTEXITCODE)." -ForegroundColor Red
    exit $LASTEXITCODE
}
Write-Host ""
Write-Host "Built: $exe" -ForegroundColor Green
